/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Engine ownership checks; native runtime acceptance is a separate probe. */
#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_API
#include "vkd3d_private.h"
#include <stdio.h>

HRESULT helios_vkd3d_create_device(LUID luid, REFIID iid, void **device);
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %u: %s\n", __LINE__, #x); _Exit(1); } } while (0)

int main(void)
{
    ID3D12CommandAllocator *allocator;
    ID3D12GraphicsCommandList *list;
    ID3D12Resource *upload, *readback;
    ID3D12CommandQueue *queue;
    ID3D12Fence *gate, *done;
    ID3D12Device *device;
    ID3D12CommandList *lists[1];
    struct d3d12_command_allocator *internal;
    D3D12_COMMAND_QUEUE_DESC queue_desc = {0};
    D3D12_HEAP_PROPERTIES heap = {0};
    D3D12_RESOURCE_DESC desc = {0};
    D3D12_RANGE empty = {0}, range = {0, 4096};
    uint32_t *mapped;
    size_t saved_pending;
    LUID luid = {0};
    unsigned int i;

    CHECK(SUCCEEDED(helios_vkd3d_create_device(luid, &IID_ID3D12Device, (void **)&device)));
    CHECK(SUCCEEDED(ID3D12Device_CreateCommandQueue(device, &queue_desc, &IID_ID3D12CommandQueue, (void **)&queue)));
    CHECK(SUCCEEDED(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void **)&allocator)));
    CHECK(SUCCEEDED(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
            &IID_ID3D12GraphicsCommandList, (void **)&list)));
    CHECK(helios_vkd3d_try_reset_command_allocator(NULL) == E_INVALIDARG);
    CHECK(helios_vkd3d_try_reset_command_allocator(allocator) == E_FAIL);
    CHECK(SUCCEEDED(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&gate)));
    CHECK(SUCCEEDED(ID3D12Device_CreateFence(device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&done)));
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    heap.CreationNodeMask = heap.VisibleNodeMask = 1;
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 4096; desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    CHECK(SUCCEEDED(ID3D12Device_CreateCommittedResource(device, &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, NULL, &IID_ID3D12Resource, (void **)&upload)));
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    CHECK(SUCCEEDED(ID3D12Device_CreateCommittedResource(device, &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&readback)));
    CHECK(SUCCEEDED(ID3D12Resource_Map(upload, 0, &empty, (void **)&mapped)));
    for (i = 0; i < 1024; i++) mapped[i] = 0x12340000u ^ i;
    ID3D12Resource_Unmap(upload, 0, NULL);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, 0, upload, 0, 4096);
    CHECK(SUCCEEDED(ID3D12GraphicsCommandList_Close(list)));
    CHECK(SUCCEEDED(ID3D12CommandQueue_Wait(queue, gate, 1)));
    lists[0] = (ID3D12CommandList *)list;
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    CHECK(SUCCEEDED(ID3D12CommandQueue_Signal(queue, done, 1)));
    CHECK(helios_vkd3d_try_reset_command_allocator(allocator) == S_FALSE);
    CHECK(ID3D12Fence_GetCompletedValue(done) == 0);
    CHECK(SUCCEEDED(ID3D12Fence_Signal(gate, 1)));
    CHECK(SUCCEEDED(ID3D12Fence_SetEventOnCompletion(done, 1, NULL)));
    CHECK(ID3D12Fence_GetCompletedValue(done) == 1);
    CHECK(helios_vkd3d_try_reset_command_allocator(allocator) == S_OK);
    CHECK(SUCCEEDED(ID3D12Resource_Map(readback, 0, &range, (void **)&mapped)));
    for (i = 0; i < 1024; i++) CHECK(mapped[i] == (0x12340000u ^ i));
    ID3D12Resource_Unmap(readback, 0, &empty);

    /* Exercise the reserve overflow failure before memcpy/pool reset. Restore
     * the synthetic metadata before destruction; no fake GPU completion. */
    internal = CONTAINING_RECORD(allocator, struct d3d12_command_allocator, ID3D12CommandAllocator_iface);
    saved_pending = internal->primary_pool.pending.command_buffer_count;
    CHECK(internal->primary_pool.recycled.command_buffer_count > 0 ||
            SUCCEEDED(ID3D12GraphicsCommandList_Reset(list, allocator, NULL)));
    if (internal->current_command_list && internal->current_command_list->is_recording)
        CHECK(SUCCEEDED(ID3D12GraphicsCommandList_Close(list)));
    CHECK(helios_vkd3d_try_reset_command_allocator(allocator) == S_OK);
    /* Need one recycled handle so the sum overflows independently of malloc. */
    CHECK(internal->primary_pool.recycled.command_buffer_count > 0);
    internal->primary_pool.pending.command_buffer_count = SIZE_MAX;
    CHECK(helios_vkd3d_try_reset_command_allocator(allocator) == E_OUTOFMEMORY);
    internal->primary_pool.pending.command_buffer_count = saved_pending;
    CHECK(helios_vkd3d_try_reset_command_allocator(allocator) == S_OK);
    CHECK(SUCCEEDED(ID3D12Device_GetDeviceRemovedReason(device)));
    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(allocator);
    CHECK(SUCCEEDED(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_BUNDLE,
            &IID_ID3D12CommandAllocator, (void **)&allocator)));
    CHECK(helios_vkd3d_try_reset_command_allocator(allocator) == S_OK);
    CHECK(SUCCEEDED(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_BUNDLE, allocator, NULL,
            &IID_ID3D12GraphicsCommandList, (void **)&list)));
    CHECK(helios_vkd3d_try_reset_command_allocator(allocator) == E_FAIL);
    CHECK(SUCCEEDED(ID3D12GraphicsCommandList_Close(list)));
    CHECK(helios_vkd3d_try_reset_command_allocator(allocator) == S_OK);
    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(allocator);
    CHECK(SUCCEEDED(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void **)&allocator)));
    CHECK(SUCCEEDED(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, NULL,
            &IID_ID3D12GraphicsCommandList, (void **)&list)));
    internal = CONTAINING_RECORD(allocator, struct d3d12_command_allocator, ID3D12CommandAllocator_iface);
    saved_pending = internal->primary_pool.pending.command_buffer_count;
    internal->primary_pool.pending.command_buffer_count = SIZE_MAX;
    CHECK(ID3D12GraphicsCommandList_Close(list) == E_OUTOFMEMORY);
    internal->primary_pool.pending.command_buffer_count = saved_pending;
    CHECK(ID3D12GraphicsCommandList_Reset(list, allocator, NULL) == E_OUTOFMEMORY);
    CHECK(helios_vkd3d_try_reset_command_allocator(allocator) == S_OK);
    CHECK(SUCCEEDED(ID3D12Device_GetDeviceRemovedReason(device)));
    ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(allocator);
    ID3D12Resource_Release(upload); ID3D12Resource_Release(readback);
    ID3D12Fence_Release(gate); ID3D12Fence_Release(done);
    ID3D12CommandQueue_Release(queue); ID3D12Device_Release(device);
    puts("PASS: recording refusal, pending ownership, authenticated retirement, 1024 GPU words, reserve overflow, bundle layout, sticky OOM Close");
    return 0;
}
