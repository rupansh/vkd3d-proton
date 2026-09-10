/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Included by resource.c. Isolated compatibility policy for reserved images.
 * Upstream base: resource.c at 35bdee1435c94f8c3548725fcb046595b263bd7e.
 * No vendor/device/driver allowlist. Real sparse color4 requires an identity-
 * matched completed behavior probe. See docs/dx12/SPARSE_COMPATIBILITY.md.
 */

void vkd3d_reserved_compat_init(struct d3d12_device *device)
{
    struct vkd3d_sparse_probe_key key = {0};
    unsigned int i;
    STATIC_ASSERT(sizeof(struct vkd3d_sparse_probe_key) == 116);
    STATIC_ASSERT(sizeof(struct vkd3d_sparse_probe_record) == 144);
    if (!vkd3d_sparse_probe_key_init(&key, device->vkd3d_instance->vk_procs.vkGetPhysicalDeviceProperties2,
            device->vk_physical_device, 0, device->vk_info.KHR_maintenance7))
    {
        WARN("ReservedCompatProbe: incomplete stack identity; color4 backing remains committed.\n");
        return;
    }
    for (i = 0; i < ARRAY_SIZE(vkd3d_sparse_probe_formats); i++)
    {
        key.format = vkd3d_sparse_probe_formats[i];
        device->reserved_compat_probe_status[i] = vkd3d_sparse_probe_read(&key);
        INFO("ReservedCompatProbe: format=%u samples=4 status=%u (0=unverified,1=pass,2=fail).\n",
                key.format, device->reserved_compat_probe_status[i]);
    }
}

static bool vkd3d_reserved_compat_required(const struct d3d12_device *device,
        const struct vkd3d_format *format, unsigned int samples)
{
    unsigned int i;
    if (samples != 4 || format->vk_aspect_mask != VK_IMAGE_ASPECT_COLOR_BIT)
        return false;
    for (i = 0; i < ARRAY_SIZE(vkd3d_sparse_probe_formats); i++)
        if ((uint32_t)format->vk_format == vkd3d_sparse_probe_formats[i])
            return device->reserved_compat_probe_status[i] != VKD3D_SPARSE_PROBE_PASS;
    return true;
}

VkSampleCountFlags vkd3d_reserved_compat_sample_counts(const struct vkd3d_format *format)
{
    unsigned int bytes = format->byte_count * format->block_byte_count;

    /* Logical 2D reserved images can use the committed fallback. This is
     * capability of that backing, not evidence of sparse residency or aliasing.
     * Match the synthetic tile geometry and the D3D tiled format restrictions:
     * single aspect, power-of-two blocks, 1x or 4x, no 128-bit MSAA tiles. */
    if (format->is_emulated || !is_power_of_two(format->vk_aspect_mask) ||
            !is_power_of_two(bytes) || bytes > 16)
        return 0;
    return format->supported_sample_counts & (VK_SAMPLE_COUNT_1_BIT |
            (bytes <= 8 && format->block_width == 1 && format->block_height == 1 ? VK_SAMPLE_COUNT_4_BIT : 0));
}

void vkd3d_reserved_compat_note_mapping(struct d3d12_resource *resource, bool copy)
{
    uint32_t *counter = copy ? &resource->device->reserved_compat_copy_mappings :
            &resource->device->reserved_compat_update_mappings;
    uint32_t n = vkd3d_atomic_uint32_increment(counter, vkd3d_memory_order_relaxed);
    /* Count every call, log powers of two to keep compatibility chatter bounded. */
    if (!(n & (n - 1)))
        WARN("ReservedCompat%sMappings=%u: committed image; mapping, unmapping and alias changes ignored.\n",
                copy ? "Copy" : "Update", n);
}

static HRESULT d3d12_resource_create_reserved_fallback(struct d3d12_device *device,
        const D3D12_RESOURCE_DESC1 *desc, D3D12_RESOURCE_STATES initial_state,
        const D3D12_CLEAR_VALUE *optimized_clear_value,
        UINT num_castable_formats, const DXGI_FORMAT *castable_formats, struct d3d12_resource **resource)
{
    D3D12_HEAP_PROPERTIES heap_props = {0};
    struct d3d12_resource *object;
    uint32_t n;
    HRESULT hr;
    heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(hr = d3d12_resource_create_committed_flags(device, desc, &heap_props,
            D3D12_HEAP_FLAG_CREATE_NOT_ZEROED, initial_state, optimized_clear_value,
            num_castable_formats, castable_formats, NULL, VKD3D_RESOURCE_RESERVED_COMPAT, &object)))
        return hr;
    if (FAILED(hr = d3d12_resource_init_sparse_info(object, device, &object->sparse)))
    {
        d3d12_resource_destroy_and_release_device(object, device);
        return hr;
    }
    n = vkd3d_atomic_uint32_increment(&device->reserved_compat_resources, vkd3d_memory_order_relaxed);
    WARN("ReservedCompatResources=%u: format=%u samples=%u; full committed backing, synthetic tiling, no sparse aliases.\n",
            n, object->format->vk_format, desc->SampleDesc.Count);
    *resource = object;
    return S_OK;
}
