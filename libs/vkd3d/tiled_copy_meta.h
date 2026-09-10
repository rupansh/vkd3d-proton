/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Included by meta.c to reuse its pipeline builders. Pipelines are lazy and
 * owned by the device; callers hold the device through their command list. */

static void vkd3d_tiled_copy_destroy_pipeline(struct d3d12_device *device,
        struct vkd3d_tiled_copy_pipeline *pipeline)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    VK_CALL(vkDestroyPipeline(device->vk_device, pipeline->pipeline, NULL));
    VK_CALL(vkDestroyPipelineLayout(device->vk_device, pipeline->layout, NULL));
    VK_CALL(vkDestroyDescriptorSetLayout(device->vk_device, pipeline->set_layout, NULL));
    memset(pipeline, 0, sizeof(*pipeline));
}

static void vkd3d_tiled_copy_cleanup(struct d3d12_device *device)
{
    struct vkd3d_tiled_copy_ops *ops = &device->meta_ops.tiled_copy;
    unsigned int i, j;
    for (i = 0; i < ARRAY_SIZE(ops->compute); i++)
        vkd3d_tiled_copy_destroy_pipeline(device, &ops->compute[i]);
    for (i = 0; i < ARRAY_SIZE(ops->depth); i++)
        for (j = 0; j < ARRAY_SIZE(ops->depth[i]); j++)
            vkd3d_tiled_copy_destroy_pipeline(device, &ops->depth[i][j]);
    vkd3d_tiled_copy_destroy_pipeline(device, &ops->rows);
}

HRESULT vkd3d_tiled_copy_rows_get_pipeline(struct d3d12_device *device,
        struct vkd3d_tiled_copy_pipeline *pipeline)
{
    struct vkd3d_tiled_copy_pipeline *cached = &device->meta_ops.tiled_copy.rows, candidate = {0};
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(struct vkd3d_tiled_copy_rows_args)};
    VkResult vr;
    int rc;

    if (!device->device_info.vulkan_1_2_features.storageBuffer8BitAccess ||
            !device->device_info.vulkan_1_2_features.shaderInt8)
        return E_NOTIMPL;
    if ((rc = pthread_mutex_lock(&device->mutex)))
        return hresult_from_errno(rc);
    if (cached->pipeline)
    {
        *pipeline = *cached;
        pthread_mutex_unlock(&device->mutex);
        return S_OK;
    }
    if ((vr = vkd3d_meta_create_pipeline_layout(device, 0, NULL, &push, &candidate.layout)) == VK_SUCCESS)
        vr = vkd3d_meta_create_compute_pipeline(device, sizeof(cs_tiled_copy_rows), cs_tiled_copy_rows,
                candidate.layout, NULL, false, NULL, &candidate.pipeline);
    if (vr == VK_SUCCESS)
        *pipeline = *cached = candidate;
    else
    {
        vkd3d_tiled_copy_destroy_pipeline(device, &candidate);
        WARN("Predicated CopyTiles row pipeline creation failed, vr %d.\n", vr);
    }
    pthread_mutex_unlock(&device->mutex);
    return hresult_from_vk_result(vr);
}

HRESULT vkd3d_tiled_copy_get_pipeline(struct d3d12_device *device,
        enum vkd3d_tiled_copy_kind kind, unsigned int bytes, unsigned int samples,
        struct vkd3d_tiled_copy_pipeline *pipeline)
{
    const struct vkd3d_vk_device_procs *vk_procs = &device->vk_procs;
    struct vkd3d_tiled_copy_ops *ops = &device->meta_ops.tiled_copy;
    struct vkd3d_tiled_copy_pipeline *cached, candidate = {0};
    VkPipelineDepthStencilStateCreateInfo ds = {0};
    VkDescriptorSetLayoutBinding binding = {0};
    VkPushConstantRange push_range;
    VkShaderModule module = VK_NULL_HANDLE;
    unsigned int sample_index;
    const uint32_t *code;
    size_t code_size;
    VkResult vr;
    int rc;

    if (kind >= VKD3D_TILED_COPY_KIND_COUNT || samples <= 1 || samples > 64 || !is_power_of_two(samples) ||
            vkd3d_tiled_copy_uint_format(bytes) == DXGI_FORMAT_UNKNOWN)
        return E_INVALIDARG;
    if (!device->device_info.vulkan_1_2_features.storageBuffer8BitAccess ||
            !device->device_info.vulkan_1_2_features.shaderInt8)
        return E_NOTIMPL;
    if (kind == VKD3D_TILED_COPY_WRITE_COLOR &&
            (!device->device_info.features2.features.shaderStorageImageMultisample ||
             !device->device_info.features2.features.shaderStorageImageWriteWithoutFormat))
        return E_NOTIMPL;
    if (kind == VKD3D_TILED_COPY_WRITE_DEPTH &&
            (bytes != 2 && bytes != 4))
        return E_INVALIDARG;
    if (kind == VKD3D_TILED_COPY_WRITE_DEPTH && bytes == 4 &&
            (!device->vk_info.EXT_depth_range_unrestricted || !device->vk_info.EXT_depth_clamp_zero_one))
        return E_NOTIMPL;

    sample_index = vkd3d_bitmask_iter32(&samples);
    samples = 1u << sample_index;
    cached = kind == VKD3D_TILED_COPY_WRITE_DEPTH ? &ops->depth[bytes == 4][sample_index] : &ops->compute[kind];
    if ((rc = pthread_mutex_lock(&device->mutex)))
        return hresult_from_errno(rc);
    if (cached->pipeline)
    {
        *pipeline = *cached;
        pthread_mutex_unlock(&device->mutex);
        return S_OK;
    }

    push_range.stageFlags = kind == VKD3D_TILED_COPY_WRITE_DEPTH ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(struct vkd3d_tiled_copy_args);
    if (kind != VKD3D_TILED_COPY_WRITE_DEPTH)
    {
        binding.binding = 0;
        binding.descriptorType = kind == VKD3D_TILED_COPY_WRITE_COLOR ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        binding.descriptorCount = 1;
        binding.stageFlags = push_range.stageFlags;
        if ((vr = vkd3d_meta_create_descriptor_set_layout(device, 1, &binding, false, &candidate.set_layout)) < 0)
            goto done;
    }
    if ((vr = vkd3d_meta_create_pipeline_layout(device, !!candidate.set_layout,
            &candidate.set_layout, &push_range, &candidate.layout)) < 0)
        goto done;

    if (kind == VKD3D_TILED_COPY_WRITE_DEPTH)
    {
        if ((vr = vkd3d_meta_create_shader_module(device, SPIRV_CODE(fs_tiled_copy_depth), &module)) < 0)
            goto done;
        ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        ds.depthTestEnable = VK_TRUE;
        ds.depthWriteEnable = VK_TRUE;
        ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
        ds.maxDepthBounds = 1.0f;
        vr = vkd3d_meta_create_graphics_pipeline(&device->meta_ops, candidate.layout, VK_FORMAT_UNDEFINED,
                bytes == 2 ? VK_FORMAT_D16_UNORM : VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT,
                VK_NULL_HANDLE, module, samples, &ds, 0, NULL, NULL, false, false, &candidate.pipeline);
    }
    else
    {
        if (kind == VKD3D_TILED_COPY_WRITE_COLOR)
        {
            code = cs_tiled_copy_write;
            code_size = sizeof(cs_tiled_copy_write);
        }
        else if (kind == VKD3D_TILED_COPY_READ_COLOR)
        {
            code = cs_tiled_copy_read;
            code_size = sizeof(cs_tiled_copy_read);
        }
        else
        {
            code = cs_tiled_copy_depth;
            code_size = sizeof(cs_tiled_copy_depth);
        }
        vr = vkd3d_meta_create_compute_pipeline(device, code_size, code, candidate.layout,
                NULL, false, NULL, &candidate.pipeline);
    }

done:
    VK_CALL(vkDestroyShaderModule(device->vk_device, module, NULL));
    if (vr == VK_SUCCESS)
    {
        *pipeline = *cached = candidate;
    }
    else
    {
        vkd3d_tiled_copy_destroy_pipeline(device, &candidate);
        WARN("MSAA CopyTiles pipeline creation failed, vr %d.\n", vr);
    }
    pthread_mutex_unlock(&device->mutex);
    return hresult_from_vk_result(vr);
}
