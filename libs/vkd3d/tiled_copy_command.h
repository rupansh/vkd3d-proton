/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Included by command.c; uses allocator-owned views and queue continuations. */

static void d3d12_command_list_copy_tiles_predicated(struct d3d12_command_list *list,
        struct d3d12_resource *tiled, const D3D12_TILED_RESOURCE_COORDINATE *coord,
        const D3D12_TILE_REGION_SIZE *size, struct d3d12_resource *linear,
        uint64_t offset, bool to_buffer)
{
    const struct vkd3d_vk_device_procs *vk_procs = &list->device->vk_procs;
    bool image = d3d12_resource_is_texture(tiled);
    const struct vkd3d_format *format = tiled->format;
    const D3D12_TILE_SHAPE *shape = &tiled->sparse.tile_shape;
    struct vkd3d_tiled_copy_pipeline pipeline;
    struct vkd3d_tiled_copy_rows_args args = {0};
    struct vkd3d_scratch_allocation scratch = {0};
    VkMemoryBarrier2 memory = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    VkImageMemoryBarrier2 image_barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    VkDependencyInfo dependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    VkBufferImageCopy2 region = {VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
    VkCopyImageToBufferInfo2 read = {VK_STRUCTURE_TYPE_COPY_IMAGE_TO_BUFFER_INFO_2};
    VkCopyBufferToImageInfo2 write = {VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2};
    VkDeviceAddress linear_va;
    unsigned int i, tile;
    HRESULT hr;

    STATIC_ASSERT(sizeof(args) == 48);
    if (list->cmd.active_non_inline_running_queries)
    {
        d3d12_command_list_record_error(list, E_NOTIMPL);
        d3d12_command_list_mark_as_invalid(list, "Predicated CopyTiles requires virtualized scoped queries.\n");
        return;
    }
    if (FAILED(hr = vkd3d_tiled_copy_rows_get_pipeline(list->device, &pipeline)))
    {
        d3d12_command_list_record_error(list, hr);
        d3d12_command_list_mark_as_invalid(list, "Predicated CopyTiles row pipeline unavailable, hr %#x.\n", (unsigned int)hr);
        return;
    }
    if (!d3d12_command_list_require_queue_flags(list, VK_QUEUE_COMPUTE_BIT) ||
            (image && !d3d12_command_list_require_depth_stencil_copy_queue(list, format->vk_aspect_mask, false)))
        return;
    if (image && FAILED(hr = d3d12_command_allocator_allocate_scratch_memory_result(list->allocator,
            VKD3D_SCRATCH_POOL_KIND_DEVICE_STORAGE, VKD3D_TILE_SIZE, 256, ~0u, &scratch)))
    {
        d3d12_command_list_record_error(list, hr);
        return;
    }
    d3d12_command_list_end_current_render_pass(list, true);
    d3d12_command_list_update_conditional_rendering_state(list, true);
    d3d12_command_list_track_resource_usage(list, tiled, true);
    d3d12_command_list_track_resource_usage(list, linear, true);

    /* D3D exposes copy states, but the predicate-controlled byte transfer is
     * compute. Include both domains and protect scratch reuse / sparse aliases.
     * The source predicate is the GPU snapshot from SetPredication. */
    memory.srcStageMask = memory.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    memory.srcAccessMask = memory.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    dependency.memoryBarrierCount = 1;
    dependency.pMemoryBarriers = &memory;
    if (image)
    {
        image_barrier.srcStageMask = image_barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        image_barrier.srcAccessMask = image_barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        image_barrier.srcQueueFamilyIndex = image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        image_barrier.oldLayout = tiled->common_layout;
        image_barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        image_barrier.image = tiled->res.vk_image;
        image_barrier.subresourceRange.aspectMask = format->vk_aspect_mask;
        image_barrier.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
        image_barrier.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &image_barrier;
    }
    VK_CALL(vkCmdPipelineBarrier2(list->cmd.vk_command_buffer, &dependency));
    dependency.imageMemoryBarrierCount = 0;
    d3d12_command_list_reset_transfer_waw_tracking(list);
    VK_CALL(vkCmdBindPipeline(list->cmd.vk_command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline.pipeline));
    args.predicate_va = list->predication.va;
    args.row_pitch = args.slice_pitch = args.row_bytes = VKD3D_TILE_SIZE;
    args.rows = args.depth = 1;

    for (i = 0; i < size->NumTiles; i++)
    {
        if (i)
            VK_CALL(vkCmdPipelineBarrier2(list->cmd.vk_command_buffer, &dependency));
        tile = vkd3d_get_tile_index_from_region(&tiled->sparse, coord, size, i);
        linear_va = linear->res.va + offset + (VkDeviceSize)i * VKD3D_TILE_SIZE;
        if (image)
        {
            const struct d3d12_sparse_image_region *tile_region = &tiled->sparse.tiles[tile].image;
            region.bufferOffset = scratch.offset;
            region.bufferRowLength = shape->WidthInTexels;
            region.bufferImageHeight = shape->HeightInTexels;
            region.imageSubresource = vk_subresource_layers_from_subresource(&tile_region->subresource);
            region.imageOffset = tile_region->offset;
            region.imageExtent = tile_region->extent;
            read.srcImage = tiled->res.vk_image;
            read.srcImageLayout = VK_IMAGE_LAYOUT_GENERAL;
            read.dstBuffer = scratch.buffer;
            read.regionCount = 1;
            read.pRegions = &region;
            /* Transfer preserves BC and raw depth bits. For buffer -> image,
             * snapshot the destination so false predication preserves its bytes.
             * Only valid edge rows are read, patched and written back. */
            VK_CALL(vkCmdCopyImageToBuffer2(list->cmd.vk_command_buffer, &read));
            VK_CALL(vkCmdPipelineBarrier2(list->cmd.vk_command_buffer, &dependency));
            args.row_pitch = shape->WidthInTexels / format->block_width * format->byte_count * format->block_byte_count;
            args.slice_pitch = args.row_pitch * (shape->HeightInTexels / format->block_height);
            args.row_bytes = align(region.imageExtent.width, format->block_width) /
                    format->block_width * format->byte_count * format->block_byte_count;
            args.rows = align(region.imageExtent.height, format->block_height) / format->block_height;
            args.depth = region.imageExtent.depth;
            args.src_va = to_buffer ? scratch.va : linear_va;
            args.dst_va = to_buffer ? linear_va : scratch.va;
        }
        else
        {
            args.src_va = to_buffer ? tiled->res.va + (VkDeviceSize)tile * VKD3D_TILE_SIZE : linear_va;
            args.dst_va = to_buffer ? linear_va : tiled->res.va + (VkDeviceSize)tile * VKD3D_TILE_SIZE;
        }
        VK_CALL(vkCmdPushConstants(list->cmd.vk_command_buffer, pipeline.layout,
                VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(args), &args));
        VK_CALL(vkCmdDispatch(list->cmd.vk_command_buffer, VKD3D_TILE_SIZE / (4 * 64), 1, 1));
        if (image && !to_buffer)
        {
            VK_CALL(vkCmdPipelineBarrier2(list->cmd.vk_command_buffer, &dependency));
            write.srcBuffer = scratch.buffer;
            write.dstImage = tiled->res.vk_image;
            write.dstImageLayout = VK_IMAGE_LAYOUT_GENERAL;
            write.regionCount = 1;
            write.pRegions = &region;
            VK_CALL(vkCmdCopyBufferToImage2(list->cmd.vk_command_buffer, &write));
        }
    }
    if (image)
    {
        image_barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        image_barrier.newLayout = tiled->common_layout;
        dependency.imageMemoryBarrierCount = 1;
    }
    memory.dstStageMask |= VK_PIPELINE_STAGE_2_HOST_BIT;
    VK_CALL(vkCmdPipelineBarrier2(list->cmd.vk_command_buffer, &dependency));
    d3d12_command_list_invalidate_all_state(list);
    list->descriptor_heap.buffers.global_heap_dirty = true;
    d3d12_command_list_update_conditional_rendering_state(list, false);
    VKD3D_BREADCRUMB_COMMAND(COPY_TILES);
}

#include "tiled_copy_depth.h"

static void d3d12_command_list_copy_tiles_msaa(struct d3d12_command_list *list,
        struct d3d12_resource *texture, const D3D12_TILED_RESOURCE_COORDINATE *coord,
        const D3D12_TILE_REGION_SIZE *size, struct d3d12_resource *buffer,
        uint64_t offset, bool to_buffer)
{
    const struct vkd3d_vk_device_procs *vk_procs = &list->device->vk_procs;
    bool depth = texture->format->vk_aspect_mask == VK_IMAGE_ASPECT_DEPTH_BIT;
    bool graphics = depth && !to_buffer;
    struct vkd3d_tiled_copy_pipeline pipeline;
    struct vkd3d_tiled_copy_args args = {0};
    struct vkd3d_texture_view_desc view_desc = {0};
    struct vkd3d_view *view = NULL;
    VkImageMemoryBarrier2 image_barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    VkMemoryBarrier2 memory_barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    VkDependencyInfo dependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    VkDescriptorImageInfo image_info = {0};
    VkWriteDescriptorSet write = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    VkRenderingAttachmentInfo attachment = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo rendering = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    VkPipelineBindPoint bind_point = graphics ? VK_PIPELINE_BIND_POINT_GRAPHICS : VK_PIPELINE_BIND_POINT_COMPUTE;
    VkShaderStageFlags shader_stages = graphics ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_COMPUTE_BIT;
    enum vkd3d_tiled_copy_kind kind = depth ? (to_buffer ? VKD3D_TILED_COPY_READ_DEPTH : VKD3D_TILED_COPY_WRITE_DEPTH) :
            (to_buffer ? VKD3D_TILED_COPY_READ_COLOR : VKD3D_TILED_COPY_WRITE_COLOR);
    VkViewport viewport;
    unsigned int i;
    HRESULT hr;

    STATIC_ASSERT(sizeof(struct vkd3d_tiled_copy_args) == 48);
    if (list->cmd.active_non_inline_running_queries)
    {
        /* The current native no-DGC path virtualizes pipeline statistics.
         * Unvirtualized queries cannot contain internal draw/dispatch work. */
        d3d12_command_list_record_error(list, E_NOTIMPL);
        d3d12_command_list_mark_as_invalid(list, "MSAA CopyTiles requires virtualized scoped queries.\n");
        return;
    }
    if (texture->desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            (depth && texture->format->vk_format != VK_FORMAT_D16_UNORM && texture->format->vk_format != VK_FORMAT_D32_SFLOAT))
    {
        d3d12_command_list_mark_as_invalid(list, "MSAA CopyTiles requires a supported single-aspect 2D format.\n");
        return;
    }
    if (depth && texture->format->vk_format == VK_FORMAT_D32_SFLOAT)
    {
        d3d12_command_list_copy_tiles_d32(list, texture, coord, size, buffer, offset, to_buffer);
        return;
    }
    if (FAILED(hr = vkd3d_tiled_copy_get_pipeline(list->device, kind, texture->format->byte_count,
            texture->desc.SampleDesc.Count, &pipeline)))
    {
        d3d12_command_list_record_error(list, hr);
        d3d12_command_list_mark_as_invalid(list, "MSAA CopyTiles pipeline unavailable, hr %#x.\n", (unsigned int)hr);
        return;
    }

    view_desc.image = texture->res.vk_image;
    view_desc.view_type = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view_desc.format = depth ? texture->format : vkd3d_get_format(list->device,
            vkd3d_tiled_copy_uint_format(texture->format->byte_count), false);
    view_desc.miplevel_count = 1;
    view_desc.layer_count = texture->desc.DepthOrArraySize;
    view_desc.aspect_mask = texture->format->vk_aspect_mask;
    view_desc.image_usage = graphics ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT :
            (to_buffer ? VK_IMAGE_USAGE_SAMPLED_BIT : VK_IMAGE_USAGE_STORAGE_BIT);
    if (!vkd3d_create_texture_view(list->device, &view_desc, &view))
    {
        d3d12_command_list_record_error(list, E_FAIL);
        d3d12_command_list_mark_as_invalid(list, "MSAA CopyTiles image view creation failed.\n");
        return;
    }
    if (!d3d12_command_allocator_add_view(list->allocator, view))
    {
        vkd3d_view_decref(view, list->device);
        d3d12_command_list_record_error(list, E_OUTOFMEMORY);
        return;
    }
    /* The allocator now holds a reference until its GPU use is complete. */
    vkd3d_view_decref(view, list->device);

    if (!d3d12_command_list_require_queue_flags(list, graphics ? VK_QUEUE_GRAPHICS_BIT : VK_QUEUE_COMPUTE_BIT))
        return;
    d3d12_command_list_end_current_render_pass(list, true);
    d3d12_command_list_update_conditional_rendering_state(list, true);
    d3d12_command_list_track_resource_usage(list, texture, true);
    d3d12_command_list_track_resource_usage(list, buffer, true);

    /* The API states are COPY_SOURCE/COPY_DEST, whose ordinary barriers target
     * transfers. The internal sample accesses additionally need shader/depth
     * visibility, including the GPU snapshot made by SetPredication. */
    image_barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    image_barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    image_barrier.dstStageMask = graphics ? VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT :
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    image_barrier.dstAccessMask = graphics ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT :
            (to_buffer ? VK_ACCESS_2_SHADER_READ_BIT : VK_ACCESS_2_SHADER_WRITE_BIT);
    image_barrier.oldLayout = texture->common_layout;
    image_barrier.newLayout = d3d12_resource_pick_layout(texture, graphics ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL :
            (to_buffer ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL));
    image_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.image = texture->res.vk_image;
    image_barrier.subresourceRange.aspectMask = texture->format->vk_aspect_mask;
    image_barrier.subresourceRange.levelCount = 1;
    image_barrier.subresourceRange.layerCount = texture->desc.DepthOrArraySize;
    memory_barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    memory_barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    memory_barrier.dstStageMask = graphics ? VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    memory_barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
    dependency.memoryBarrierCount = dependency.imageMemoryBarrierCount = 1;
    dependency.pMemoryBarriers = &memory_barrier;
    dependency.pImageMemoryBarriers = &image_barrier;
    VK_CALL(vkCmdPipelineBarrier2(list->cmd.vk_command_buffer, &dependency));
    d3d12_command_list_reset_transfer_waw_tracking(list);

    VK_CALL(vkCmdBindPipeline(list->cmd.vk_command_buffer, bind_point, pipeline.pipeline));
    if (!graphics)
    {
        image_info.imageView = view->vk_image_view;
        image_info.imageLayout = image_barrier.newLayout;
        write.descriptorType = to_buffer ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.descriptorCount = 1;
        write.pImageInfo = &image_info;
        VK_CALL(vkCmdPushDescriptorSetKHR(list->cmd.vk_command_buffer, bind_point, pipeline.layout, 0, 1, &write));
    }
    else
    {
        attachment.imageView = view->vk_image_view;
        attachment.imageLayout = image_barrier.newLayout;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        rendering.pDepthAttachment = &attachment;
        rendering.layerCount = texture->desc.DepthOrArraySize;
        rendering.flags = d3d12_device_get_rendering_flags(list->device);
    }

    args.predicate_va = list->predication.va;
    args.samples = texture->desc.SampleDesc.Count;
    args.bytes_per_sample = texture->format->byte_count;
    args.tile_width = texture->sparse.tile_shape.WidthInTexels;
    for (i = 0; i < size->NumTiles; i++)
    {
        unsigned int tile = vkd3d_get_tile_index_from_region(&texture->sparse, coord, size, i);
        const struct d3d12_sparse_image_region *region = &texture->sparse.tiles[tile].image;
        args.buffer_va = buffer->res.va + offset + (uint64_t)i * VKD3D_TILE_SIZE;
        args.origin_x = region->offset.x;
        args.origin_y = region->offset.y;
        args.width = region->extent.width;
        args.height = region->extent.height;
        args.layer = region->subresource.arrayLayer;
        VK_CALL(vkCmdPushConstants(list->cmd.vk_command_buffer, pipeline.layout, shader_stages, 0, sizeof(args), &args));
        if (graphics)
        {
            rendering.renderArea.offset.x = args.origin_x;
            rendering.renderArea.offset.y = args.origin_y;
            rendering.renderArea.extent.width = args.width;
            rendering.renderArea.extent.height = args.height;
            viewport.x = args.origin_x;
            viewport.y = args.origin_y;
            viewport.width = args.width;
            viewport.height = args.height;
            viewport.minDepth = 0.0f;
            viewport.maxDepth = 1.0f;
            VK_CALL(vkCmdSetViewport(list->cmd.vk_command_buffer, 0, 1, &viewport));
            VK_CALL(vkCmdSetScissor(list->cmd.vk_command_buffer, 0, 1, &rendering.renderArea));
            VK_CALL(vkCmdBeginRendering(list->cmd.vk_command_buffer, &rendering));
            VK_CALL(vkCmdDraw(list->cmd.vk_command_buffer, 3, 1, 0, args.layer));
            VK_CALL(vkCmdEndRendering(list->cmd.vk_command_buffer));
        }
        else if (to_buffer)
            VK_CALL(vkCmdDispatch(list->cmd.vk_command_buffer, VKD3D_TILE_SIZE / (4 * 64), 1, 1));
        else
            VK_CALL(vkCmdDispatch(list->cmd.vk_command_buffer, (args.width + 7) / 8, (args.height + 7) / 8, args.samples));

        /* Tile mappings can alias backing. Preserve copy order even between
         * distinct virtual tiles of the same resource. No host wait is needed. */
        if (i + 1 < size->NumTiles)
        {
            VkMemoryBarrier2 tile_barrier = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            VkDependencyInfo tile_dependency = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            tile_barrier.srcStageMask = tile_barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            tile_barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
            tile_barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
            tile_dependency.memoryBarrierCount = 1;
            tile_dependency.pMemoryBarriers = &tile_barrier;
            VK_CALL(vkCmdPipelineBarrier2(list->cmd.vk_command_buffer, &tile_dependency));
        }
    }

    image_barrier.srcStageMask = image_barrier.dstStageMask;
    image_barrier.srcAccessMask = image_barrier.dstAccessMask;
    image_barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    image_barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    image_barrier.oldLayout = image_barrier.newLayout;
    image_barrier.newLayout = texture->common_layout;
    memory_barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    memory_barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    memory_barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT;
    memory_barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    VK_CALL(vkCmdPipelineBarrier2(list->cmd.vk_command_buffer, &dependency));
    d3d12_command_list_invalidate_all_state(list);
    list->descriptor_heap.buffers.global_heap_dirty = true;
    d3d12_command_list_update_conditional_rendering_state(list, false);
}
