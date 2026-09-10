/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Included only by command.c. Fragment depth exports may flush denormals and
 * transform NaNs even when sampling and bitcasts preserve their bits. Route
 * raw D32 samples through an integer MSAA image and maintenance8 copies.
 * Each recording owns one tile image, retained by its command allocator.
 */
struct vkd3d_tiled_copy_scratch_image
{
    struct vkd3d_tiled_copy_scratch_image *next;
    VkImage image;
    VkImageView view;
    struct vkd3d_device_memory_allocation memory;
};

static void vkd3d_tiled_copy_scratch_destroy(struct d3d12_device *device,
        struct vkd3d_tiled_copy_scratch_image *scratch)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VK_CALL(vkDestroyImageView(device->vk_device, scratch->view, NULL));
    VK_CALL(vkDestroyImage(device->vk_device, scratch->image, NULL));
    if (scratch->memory.vk_memory)
        vkd3d_free_device_memory(device, &scratch->memory);
    vkd3d_free(scratch);
}

static void vkd3d_tiled_copy_allocator_cleanup(struct d3d12_command_allocator *allocator)
{
    struct vkd3d_tiled_copy_scratch_image *scratch;
    while ((scratch = allocator->tiled_copy_images))
    {
        allocator->tiled_copy_images = scratch->next;
        vkd3d_tiled_copy_scratch_destroy(allocator->device, scratch);
    }
}

static HRESULT vkd3d_tiled_copy_scratch_create(struct d3d12_command_allocator *allocator,
        const struct d3d12_resource *texture, struct vkd3d_tiled_copy_scratch_image **out)
{
    struct d3d12_device *device = allocator->device;
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VkImageCreateInfo image = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkMemoryDedicatedAllocateInfo dedicated = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    VkImageViewCreateInfo view = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    VkBindImageMemoryInfo bind = {VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO};
    VkMemoryRequirements requirements;
    struct vkd3d_tiled_copy_scratch_image *scratch;
    HRESULT hr;
    VkResult vr;

    if (!(scratch = vkd3d_calloc(1, sizeof(*scratch))))
        return E_OUTOFMEMORY;
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = VK_FORMAT_R32_UINT;
    image.extent.width = texture->sparse.tile_shape.WidthInTexels;
    image.extent.height = texture->sparse.tile_shape.HeightInTexels;
    image.extent.depth = 1;
    image.mipLevels = image.arrayLayers = 1;
    image.samples = texture->desc.SampleDesc.Count;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
            VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if ((vr = VK_CALL(vkCreateImage(device->vk_device, &image, NULL, &scratch->image))) < 0)
    {
        hr = hresult_from_vk_result(vr);
        goto fail;
    }
    VK_CALL(vkGetImageMemoryRequirements(device->vk_device, scratch->image, &requirements));
    dedicated.image = scratch->image;
    if (FAILED(hr = vkd3d_allocate_device_memory(device, requirements.size, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            requirements.memoryTypeBits, &dedicated, true, &scratch->memory)))
        goto fail;
    bind.image = scratch->image;
    bind.memory = scratch->memory.vk_memory;
    if ((vr = VK_CALL(vkBindImageMemory2(device->vk_device, 1, &bind))) < 0)
    {
        hr = hresult_from_vk_result(vr);
        goto fail;
    }
    view.image = scratch->image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view.format = VK_FORMAT_R32_UINT;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = view.subresourceRange.layerCount = 1;
    if ((vr = VK_CALL(vkCreateImageView(device->vk_device, &view, NULL, &scratch->view))) < 0)
    {
        hr = hresult_from_vk_result(vr);
        goto fail;
    }
    scratch->next = allocator->tiled_copy_images;
    allocator->tiled_copy_images = scratch;
    *out = scratch;
    return S_OK;

fail:
    vkd3d_tiled_copy_scratch_destroy(device, scratch);
    return hr;
}

static void d3d12_command_list_copy_tiles_d32(struct d3d12_command_list *list,
        struct d3d12_resource *texture, const D3D12_TILED_RESOURCE_COORDINATE *coord,
        const D3D12_TILE_REGION_SIZE *size, struct d3d12_resource *buffer, uint64_t offset, bool to_buffer)
{
    const struct vkd3d_vk_device_procs *vk_procs = &list->device->vk_procs;
    struct vkd3d_tiled_copy_scratch_image *scratch;
    struct vkd3d_tiled_copy_pipeline pipeline;
    struct vkd3d_tiled_copy_args args = {0};
    VkMemoryBarrier2 memory = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    VkImageMemoryBarrier2 images[2] = {{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}, {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}};
    VkDependencyInfo dependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    VkDescriptorImageInfo image_info = {0};
    VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    VkImageCopy2 region = {VK_STRUCTURE_TYPE_IMAGE_COPY_2};
    VkCopyImageInfo2 copy = {VK_STRUCTURE_TYPE_COPY_IMAGE_INFO_2};
    unsigned int i;
    HRESULT hr;

    if (!list->device->device_info.maintenance_8_features.maintenance8 || !list->device->vk_info.EXT_depth_range_unrestricted)
    {
        /* STUB: without a raw color/depth transfer, fragment writes are lossy
         * for D32. Ordinary committed depth rendering remains available. */
        d3d12_command_list_record_error(list, E_NOTIMPL);
        d3d12_command_list_mark_as_invalid(list, "Raw D32 MSAA CopyTiles requires maintenance8 and unrestricted depth copies.\n");
        return;
    }
    if (FAILED(hr = vkd3d_tiled_copy_get_pipeline(list->device,
            to_buffer ? VKD3D_TILED_COPY_READ_COLOR : VKD3D_TILED_COPY_WRITE_COLOR,
            4, texture->desc.SampleDesc.Count, &pipeline)))
        goto fail;
    /* MSAA depth/color image copies require graphics, including readback.
     * The existing ordered continuation keeps direct/compute/copy API queues. */
    if (!d3d12_command_list_require_queue_flags(list, VK_QUEUE_GRAPHICS_BIT))
        return;
    if (FAILED(hr = vkd3d_tiled_copy_scratch_create(list->allocator, texture, &scratch)))
        goto fail;
    d3d12_command_list_end_current_render_pass(list, true);
    d3d12_command_list_update_conditional_rendering_state(list, true);
    d3d12_command_list_track_resource_usage(list, texture, true);
    d3d12_command_list_track_resource_usage(list, buffer, true);

    memory.srcStageMask = memory.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    memory.srcAccessMask = memory.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    for (i = 0; i < 2; i++)
    {
        images[i].srcStageMask = images[i].dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        images[i].srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        images[i].dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        images[i].oldLayout = i ? VK_IMAGE_LAYOUT_UNDEFINED : texture->common_layout;
        images[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        images[i].srcQueueFamilyIndex = images[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        images[i].image = i ? scratch->image : texture->res.vk_image;
        images[i].subresourceRange.aspectMask = i ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        images[i].subresourceRange.levelCount = 1;
        images[i].subresourceRange.layerCount = i ? 1 : texture->desc.DepthOrArraySize;
    }
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &memory;
    dependency.imageMemoryBarrierCount = 2;
    dependency.pImageMemoryBarriers = images;
    VK_CALL(vkCmdPipelineBarrier2(list->cmd.vk_command_buffer, &dependency));
    dependency.imageMemoryBarrierCount = 0;
    d3d12_command_list_reset_transfer_waw_tracking(list);

    image_info.imageView = scratch->view;
    image_info.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    write.descriptorType = to_buffer ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.descriptorCount = 1;
    write.pImageInfo = &image_info;
    VK_CALL(vkCmdBindPipeline(list->cmd.vk_command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline));
    VK_CALL(vkCmdPushDescriptorSetKHR(list->cmd.vk_command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.layout, 0, 1, &write));
    args.predicate_va = list->predication.va;
    args.samples = texture->desc.SampleDesc.Count;
    args.bytes_per_sample = 4;
    args.tile_width = texture->sparse.tile_shape.WidthInTexels;
    copy.srcImageLayout = copy.dstImageLayout = VK_IMAGE_LAYOUT_GENERAL;
    copy.regionCount = 1;
    copy.pRegions = &region;
    region.srcSubresource.layerCount = region.dstSubresource.layerCount = 1;
    for (i = 0; i < size->NumTiles; i++)
    {
        unsigned int tile = vkd3d_get_tile_index_from_region(&texture->sparse, coord, size, i);
        const struct d3d12_sparse_image_region *tile_region = &texture->sparse.tiles[tile].image;
        args.buffer_va = buffer->res.va + offset + (uint64_t)i * VKD3D_TILE_SIZE;
        args.width = tile_region->extent.width;
        args.height = tile_region->extent.height;
        region.extent = tile_region->extent;
        /* A false write predicate preserves the original depth bits. Snapshot
         * the destination before conditionally patching its integer samples;
         * the following transfer then writes either patched or original data. */
        if (to_buffer || args.predicate_va)
        {
            copy.srcImage = texture->res.vk_image;
            copy.dstImage = scratch->image;
            region.srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_DEPTH_BIT,
                    tile_region->subresource.mipLevel, tile_region->subresource.arrayLayer, 1};
            region.srcOffset = tile_region->offset;
            region.dstSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.dstOffset = (VkOffset3D){0, 0, 0};
            VK_CALL(vkCmdCopyImage2(list->cmd.vk_command_buffer, &copy));
            VK_CALL(vkCmdPipelineBarrier2(list->cmd.vk_command_buffer, &dependency));
        }
        VK_CALL(vkCmdPushConstants(list->cmd.vk_command_buffer, pipeline.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(args), &args));
        if (to_buffer)
            VK_CALL(vkCmdDispatch(list->cmd.vk_command_buffer, VKD3D_TILE_SIZE / (4 * 64), 1, 1));
        else
            VK_CALL(vkCmdDispatch(list->cmd.vk_command_buffer, (args.width + 7) / 8, (args.height + 7) / 8, args.samples));
        VK_CALL(vkCmdPipelineBarrier2(list->cmd.vk_command_buffer, &dependency));
        if (!to_buffer)
        {
            copy.srcImage = scratch->image;
            copy.dstImage = texture->res.vk_image;
            region.srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.srcOffset = (VkOffset3D){0, 0, 0};
            region.dstSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_DEPTH_BIT,
                    tile_region->subresource.mipLevel, tile_region->subresource.arrayLayer, 1};
            region.dstOffset = tile_region->offset;
            VK_CALL(vkCmdCopyImage2(list->cmd.vk_command_buffer, &copy));
            /* Serialize scratch reuse and aliased virtual tile destinations. */
            VK_CALL(vkCmdPipelineBarrier2(list->cmd.vk_command_buffer, &dependency));
        }
    }
    images[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    images[0].newLayout = texture->common_layout;
    dependency.imageMemoryBarrierCount = 1;
    memory.dstStageMask |= VK_PIPELINE_STAGE_2_HOST_BIT;
    memory.dstAccessMask |= VK_ACCESS_2_HOST_READ_BIT;
    VK_CALL(vkCmdPipelineBarrier2(list->cmd.vk_command_buffer, &dependency));
    d3d12_command_list_invalidate_all_state(list);
    list->descriptor_heap.buffers.global_heap_dirty = true;
    d3d12_command_list_update_conditional_rendering_state(list, false);
    return;

fail:
    d3d12_command_list_record_error(list, hr);
    d3d12_command_list_mark_as_invalid(list, "Raw D32 MSAA CopyTiles setup failed, hr %#x.\n", (unsigned int)hr);
}
