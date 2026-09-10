/* SPDX-License-Identifier: LGPL-2.1-or-later
 * MSAA and predicated CopyTiles shader implementation. No capability override. */
#ifndef VKD3D_TILED_COPY_H
#define VKD3D_TILED_COPY_H

struct vkd3d_tiled_copy_args
{
    VkDeviceAddress buffer_va;
    VkDeviceAddress predicate_va;
    uint32_t origin_x, origin_y;
    uint32_t width, height;
    uint32_t tile_width, layer;
    uint32_t samples, bytes_per_sample;
};

/* Raw single-sample tile rows. Both buffers use the standard tile pitch;
 * edge holes and bytes outside the requested offset are never overwritten. */
struct vkd3d_tiled_copy_rows_args
{
    VkDeviceAddress src_va, dst_va, predicate_va;
    uint32_t row_pitch, slice_pitch, row_bytes, rows, depth, padding;
};

enum vkd3d_tiled_copy_kind
{
    VKD3D_TILED_COPY_READ_COLOR,
    VKD3D_TILED_COPY_WRITE_COLOR,
    VKD3D_TILED_COPY_READ_DEPTH,
    VKD3D_TILED_COPY_WRITE_DEPTH,
    VKD3D_TILED_COPY_KIND_COUNT,
};

struct vkd3d_tiled_copy_pipeline
{
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkPipeline pipeline;
};

struct vkd3d_tiled_copy_ops
{
    struct vkd3d_tiled_copy_pipeline compute[3];
    struct vkd3d_tiled_copy_pipeline depth[2][7];
    struct vkd3d_tiled_copy_pipeline rows;
};

static inline DXGI_FORMAT vkd3d_tiled_copy_uint_format(unsigned int bytes)
{
    switch (bytes)
    {
        case 1: return DXGI_FORMAT_R8_UINT;
        case 2: return DXGI_FORMAT_R16_UINT;
        case 4: return DXGI_FORMAT_R32_UINT;
        case 8: return DXGI_FORMAT_R32G32_UINT;
        case 16: return DXGI_FORMAT_R32G32B32A32_UINT;
        default: return DXGI_FORMAT_UNKNOWN;
    }
}

HRESULT vkd3d_tiled_copy_get_pipeline(struct d3d12_device *device,
        enum vkd3d_tiled_copy_kind kind, unsigned int bytes, unsigned int samples,
        struct vkd3d_tiled_copy_pipeline *pipeline);

HRESULT vkd3d_tiled_copy_rows_get_pipeline(struct d3d12_device *device,
        struct vkd3d_tiled_copy_pipeline *pipeline);

#endif
