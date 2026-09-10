/* Included by command.c after helios_producer.h. Native DDI mappings are
 * prepared synchronously, but their mapping effects wait for exact runtime
 * admission on the existing submission worker. */

static bool vkd3d_helios_tile_region(const struct d3d12_sparse_info *sparse,
        const D3D12_TILED_RESOURCE_COORDINATE *coord, const D3D12_TILE_REGION_SIZE *size,
        uint32_t *count)
{
    const D3D12_SUBRESOURCE_TILING *tiling;
    uint64_t base, volume;
    if (coord->Subresource >= sparse->tiling_count)
        return false;
    tiling = &sparse->tilings[coord->Subresource];
    if (tiling->StartTileIndexInOverallResource == VKD3D_INVALID_TILE_INDEX)
    {
        if (size->UseBox || coord->Y || coord->Z ||
                coord->X >= sparse->packed_mips.NumTilesForPackedMips)
            return false;
        base = (uint64_t)sparse->packed_mips.StartTileIndexInOverallResource + coord->X;
    }
    else
    {
        if (coord->X >= tiling->WidthInTiles || coord->Y >= tiling->HeightInTiles ||
                coord->Z >= tiling->DepthInTiles)
            return false;
        base = (uint64_t)tiling->StartTileIndexInOverallResource + coord->X +
                (uint64_t)tiling->WidthInTiles * (coord->Y + (uint64_t)tiling->HeightInTiles * coord->Z);
    }
    if (size->UseBox)
    {
        volume = (uint64_t)size->Width * size->Height * size->Depth;
        if (!volume || volume > UINT32_MAX ||
                size->Width > tiling->WidthInTiles - coord->X ||
                size->Height > tiling->HeightInTiles - coord->Y ||
                size->Depth > tiling->DepthInTiles - coord->Z)
            return false;
        /* NumTiles is ignored for a box, as specified by the API/DDI. */
        *count = volume;
    }
    else
    {
        if (base + size->NumTiles > sparse->tile_count)
            return false;
        *count = size->NumTiles;
    }
    return true;
}

static HRESULT vkd3d_helios_commit_sparse(struct d3d12_command_queue *queue,
        struct d3d12_command_queue_submission *sub, HANDLE admission,
        uint32_t *ctx, uint32_t *value, uint64_t *cookie)
{
    d3d12_resource_incref(sub->bind_sparse.dst_resource);
    if (sub->bind_sparse.src_resource)
        d3d12_resource_incref(sub->bind_sparse.src_resource);
    if (sub->bind_sparse.helios_heap)
        d3d12_heap_incref(sub->bind_sparse.helios_heap);
    if (vkd3d_helios_commit_execute(queue, sub, admission, ctx, value, cookie))
        return S_OK;
    d3d12_resource_decref(sub->bind_sparse.dst_resource);
    if (sub->bind_sparse.src_resource)
        d3d12_resource_decref(sub->bind_sparse.src_resource);
    if (sub->bind_sparse.helios_heap)
        d3d12_heap_decref(sub->bind_sparse.helios_heap);
    vkd3d_free(sub->bind_sparse.bind_infos);
    return E_FAIL;
}

HRESULT helios_vkd3d_update_tile_mappings(ID3D12CommandQueue *iface, ID3D12Resource *resource,
        UINT region_count, const D3D12_TILED_RESOURCE_COORDINATE *coords,
        const D3D12_TILE_REGION_SIZE *sizes, ID3D12Heap *heap, UINT range_count,
        const D3D12_TILE_RANGE_FLAGS *range_flags, const UINT *offsets, const UINT *counts,
        D3D12_TILE_MAPPING_FLAGS flags, HANDLE admission,
        uint32_t *ctx, uint32_t *value, uint64_t *cookie)
{
    struct d3d12_command_queue_submission sub = {0};
    struct d3d12_command_queue *queue;
    struct d3d12_resource *res;
    struct d3d12_heap *memory_heap;
    D3D12_TILED_RESOURCE_COORDINATE coord = {0};
    D3D12_TILE_REGION_SIZE size = {0};
    D3D12_TILE_RANGE_FLAGS range_flag;
    struct vkd3d_sparse_memory_bind *bind;
    uint32_t i, j, region_tiles, range = 0, range_tile = 0, range_size, tile, index;
    uint32_t *bound_tiles = NULL;
    uint64_t total = 0, range_total = 0, offset;
    size_t capacity = 0;
    HRESULT hr = E_INVALIDARG;

    if (!iface || !resource || !admission || !ctx || !value || !cookie ||
            flags & ~D3D12_TILE_MAPPING_FLAG_NO_HAZARD || (!coords && region_count > 1))
        return E_INVALIDARG;
    queue = impl_from_ID3D12CommandQueue(iface);
    res = impl_from_ID3D12Resource(resource);
    memory_heap = heap ? impl_from_ID3D12Heap(heap) : NULL;
    if (res->device != queue->device || !d3d12_resource_is_tiled(res) ||
            (memory_heap && (memory_heap->device != queue->device ||
                !memory_heap->allocation.device_allocation.vk_memory)))
        return E_INVALIDARG;
    if (FAILED(hr = d3d12_device_removed_reason(queue->device)))
        return hr;
    hr = E_INVALIDARG;
    for (i = 0; i < region_count; i++)
    {
        if (coords) coord = coords[i];
        if (sizes) size = sizes[i];
        else size.NumTiles = coords ? 1 : res->sparse.tile_count;
        if (!vkd3d_helios_tile_region(&res->sparse, &coord, &size, &region_tiles))
            return E_INVALIDARG;
        total += region_tiles;
    }
    /* The API's optional count array defaults to one tile for multiple ranges,
     * and to all addressed tiles for a single range. */
    if (total > UINT32_MAX)
        return E_OUTOFMEMORY;
    for (i = 0; i < range_count; i++)
    {
        range_size = counts ? counts[i] : range_count == 1 ? total : 1;
        range_flag = range_flags ? range_flags[i] : D3D12_TILE_RANGE_FLAG_NONE;
        if (range_flag != D3D12_TILE_RANGE_FLAG_NONE && range_flag != D3D12_TILE_RANGE_FLAG_NULL &&
                range_flag != D3D12_TILE_RANGE_FLAG_SKIP && range_flag != D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE)
            return E_INVALIDARG;
        if (range_size && (range_flag == D3D12_TILE_RANGE_FLAG_NONE ||
                range_flag == D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE))
        {
            offset = offsets ? offsets[i] : 0;
            offset += range_flag == D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE ? 1 : range_size;
            if (!memory_heap || offset > memory_heap->desc.SizeInBytes / VKD3D_TILE_SIZE)
                return E_INVALIDARG;
        }
        range_total += range_size;
    }
    if (range_total != total)
        return E_INVALIDARG;

    sub.type = VKD3D_SUBMISSION_BIND_SPARSE;
    sub.bind_sparse.mode = VKD3D_SPARSE_MEMORY_BIND_MODE_UPDATE;
    sub.bind_sparse.dst_resource = res;
    sub.bind_sparse.helios_heap = memory_heap;
    if (res->flags & VKD3D_RESOURCE_RESERVED_COMPAT)
    {
        /* STUB: owner-authorized committed fallback has no mutable tile map.
         * Still validate, retain, admit and GPU-signal this ordered operation. */
        vkd3d_reserved_compat_note_mapping(res, false);
        return vkd3d_helios_commit_sparse(queue, &sub, admission, ctx, value, cookie);
    }
    if (total && !(bound_tiles = vkd3d_malloc(res->sparse.tile_count * sizeof(*bound_tiles))))
        return E_OUTOFMEMORY;
    if (bound_tiles)
        memset(bound_tiles, 0xff, res->sparse.tile_count * sizeof(*bound_tiles));
    memset(&coord, 0, sizeof(coord));
    memset(&size, 0, sizeof(size));
    for (i = 0; i < region_count; i++)
    {
        if (coords) coord = coords[i];
        if (sizes) size = sizes[i];
        else size.NumTiles = coords ? 1 : res->sparse.tile_count;
        vkd3d_helios_tile_region(&res->sparse, &coord, &size, &region_tiles);
        for (j = 0; j < region_tiles; j++, range_tile++)
        {
            while (range < range_count)
            {
                range_size = counts ? counts[range] : range_count == 1 ? total : 1;
                if (range_tile < range_size) break;
                range_tile = 0;
                range++;
            }
            if (range == range_count) goto fail;
            range_flag = range_flags ? range_flags[range] : D3D12_TILE_RANGE_FLAG_NONE;
            if (range_flag == D3D12_TILE_RANGE_FLAG_SKIP) continue;
            tile = vkd3d_get_tile_index_from_region(&res->sparse, &coord, &size, j);
            if (tile == VKD3D_INVALID_TILE_INDEX) goto fail;
            index = bound_tiles[tile];
            if (index == VKD3D_INVALID_TILE_INDEX)
            {
                if (!vkd3d_array_reserve((void **)&sub.bind_sparse.bind_infos, &capacity,
                        sub.bind_sparse.bind_count + 1, sizeof(*sub.bind_sparse.bind_infos)))
                {
                    hr = E_OUTOFMEMORY;
                    goto fail;
                }
                bound_tiles[tile] = index = sub.bind_sparse.bind_count++;
            }
            bind = &sub.bind_sparse.bind_infos[index];
            memset(bind, 0, sizeof(*bind));
            bind->dst_tile = tile;
            if (range_flag != D3D12_TILE_RANGE_FLAG_NULL)
            {
                offset = offsets ? offsets[range] : 0;
                if (range_flag != D3D12_TILE_RANGE_FLAG_REUSE_SINGLE_TILE) offset += range_tile;
                bind->helios_heap = memory_heap;
                bind->vk_memory = memory_heap->allocation.device_allocation.vk_memory;
                bind->vk_offset = memory_heap->allocation.offset + offset * VKD3D_TILE_SIZE;
            }
        }
    }
    vkd3d_free(bound_tiles);
    return vkd3d_helios_commit_sparse(queue, &sub, admission, ctx, value, cookie);
fail:
    vkd3d_free(bound_tiles);
    vkd3d_free(sub.bind_sparse.bind_infos);
    return hr;
}

HRESULT helios_vkd3d_copy_tile_mappings(ID3D12CommandQueue *iface, ID3D12Resource *dst,
        const D3D12_TILED_RESOURCE_COORDINATE *dst_coord, ID3D12Resource *src,
        const D3D12_TILED_RESOURCE_COORDINATE *src_coord, const D3D12_TILE_REGION_SIZE *size,
        D3D12_TILE_MAPPING_FLAGS flags, HANDLE admission,
        uint32_t *ctx, uint32_t *value, uint64_t *cookie)
{
    struct d3d12_command_queue_submission sub = {0};
    struct d3d12_command_queue *queue;
    struct d3d12_resource *dst_res, *src_res;
    uint32_t count, src_count, i;
    HRESULT hr;
    if (!iface || !dst || !src || !dst_coord || !src_coord || !size ||
            !admission || !ctx || !value || !cookie || flags & ~D3D12_TILE_MAPPING_FLAG_NO_HAZARD)
        return E_INVALIDARG;
    queue = impl_from_ID3D12CommandQueue(iface);
    dst_res = impl_from_ID3D12Resource(dst);
    src_res = impl_from_ID3D12Resource(src);
    if (dst_res->device != queue->device || src_res->device != queue->device ||
            !d3d12_resource_is_tiled(dst_res) || !d3d12_resource_is_tiled(src_res) ||
            !vkd3d_helios_tile_region(&dst_res->sparse, dst_coord, size, &count) ||
            !vkd3d_helios_tile_region(&src_res->sparse, src_coord, size, &src_count) || count != src_count)
        return E_INVALIDARG;
    if (FAILED(hr = d3d12_device_removed_reason(queue->device)))
        return hr;
    sub.type = VKD3D_SUBMISSION_BIND_SPARSE;
    sub.bind_sparse.mode = VKD3D_SPARSE_MEMORY_BIND_MODE_COPY;
    sub.bind_sparse.dst_resource = dst_res;
    sub.bind_sparse.src_resource = src_res;
    if ((dst_res->flags | src_res->flags) & VKD3D_RESOURCE_RESERVED_COMPAT)
    {
        /* STUB: no aliases into or out of a committed fallback. In particular,
         * never turn its synthetic metadata into a real sparse unbind. */
        vkd3d_reserved_compat_note_mapping(dst_res, true);
        return vkd3d_helios_commit_sparse(queue, &sub, admission, ctx, value, cookie);
    }
    sub.bind_sparse.bind_count = count;
    if (count && !(sub.bind_sparse.bind_infos = vkd3d_calloc(count, sizeof(*sub.bind_sparse.bind_infos))))
        return E_OUTOFMEMORY;
    for (i = 0; i < count; i++)
    {
        sub.bind_sparse.bind_infos[i].dst_tile = vkd3d_get_tile_index_from_region(&dst_res->sparse, dst_coord, size, i);
        sub.bind_sparse.bind_infos[i].src_tile = vkd3d_get_tile_index_from_region(&src_res->sparse, src_coord, size, i);
    }
    /* Source mappings are snapshotted by compaction only after admission and
     * before mutating any destination tile, including overlapping self-copy. */
    return vkd3d_helios_commit_sparse(queue, &sub, admission, ctx, value, cookie);
}

static bool vkd3d_helios_sparse_ready(struct vkd3d_helios_execution *op)
{
#ifdef _WIN32
    return !op || WaitForSingleObject(op->admission_event, 0) == WAIT_OBJECT_0;
#else
    return !op;
#endif
}

static void vkd3d_helios_flush_sparse(struct d3d12_command_queue *queue)
{
    d3d12_command_queue_flush_bind_sparse(queue);
    if (queue->helios_sparse_completion)
    {
        vkd3d_helios_execution_signal(queue, queue->helios_sparse_completion);
        vkd3d_helios_execution_free(queue->helios_sparse_completion);
        queue->helios_sparse_completion = NULL;
    }
}
