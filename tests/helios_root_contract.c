/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Headless tests of private native-DDI engine operations. This executable is
 * not a substitute for Microsoft runtime + Helios native validation. */
#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_API
#include "vkd3d_private.h"
#include <stdio.h>
#include <time.h>
#include <unistd.h>

HRESULT helios_vkd3d_create_device(LUID luid, REFIID iid, void **device);

static unsigned int checks;
#define CHECK(test) do { checks++; if (!(test)) { fprintf(stderr, "FAIL:%u: %s\n", __LINE__, #test); exit(1); } } while (0)
#define HR(call) CHECK(SUCCEEDED(call))

static ID3D12Device *device;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *allocator;
static ID3D12GraphicsCommandList *list;
static ID3D12Fence *fence;
static uint64_t fence_value;

static ID3D12Resource *buffer(D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags,
        D3D12_RESOURCE_STATES state)
{
    D3D12_RESOURCE_DESC desc = {0};
    D3D12_HEAP_PROPERTIES props = {0};
    ID3D12Resource *resource = NULL;
    props.Type = heap;
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 65536;
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = flags;
    HR(ID3D12Device_CreateCommittedResource(device, &props, D3D12_HEAP_FLAG_NONE,
            &desc, state, NULL, &IID_ID3D12Resource, (void **)&resource));
    return resource;
}

static void submit(void)
{
    struct timespec start, now, delay = {0, 1000000};
    uint64_t completed;
    HR(ID3D12GraphicsCommandList_Close(list));
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
    HR(ID3D12CommandQueue_Signal(queue, fence, ++fence_value));
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;)
    {
        completed = ID3D12Fence_GetCompletedValue(fence);
        CHECK(completed != UINT64_MAX);
        if (completed >= fence_value) break;
        clock_gettime(CLOCK_MONOTONIC, &now);
        /* Exit without releasing potentially GPU-owned objects on failure. */
        CHECK(now.tv_sec - start.tv_sec < 30);
        nanosleep(&delay, NULL);
    }
}

static void reset(void)
{
    HR(ID3D12CommandAllocator_Reset(allocator));
    HR(ID3D12GraphicsCommandList_Reset(list, allocator, NULL));
}

static void barrier(ID3D12Resource *resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER barrier = {0};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &barrier);
}

static void check_output(ID3D12Resource *output, ID3D12Resource *readback,
        const uint32_t *expected, unsigned int count)
{
    D3D12_RANGE range = {0, count * sizeof(uint32_t)};
    uint32_t *data;
    unsigned int i;
    barrier(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, readback, 0, output, 0, range.End);
    barrier(output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    submit();
    HR(ID3D12Resource_Map(readback, 0, &range, (void **)&data));
    for (i = 0; i < count; i++)
    {
        if (data[i] != expected[i])
            fprintf(stderr, "word %u: got %u expected %u\n", i, data[i], expected[i]);
        CHECK(data[i] == expected[i]);
    }
    range.End = 0;
    ID3D12Resource_Unmap(readback, 0, &range);
    reset();
}

static ID3D12PipelineState *pipeline(ID3D12RootSignature *root, const char *path)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {0};
    ID3D12PipelineState *pipeline = NULL;
    FILE *file = fopen(path, "rb");
    long size;
    void *bytes;
    CHECK(file != NULL);
    CHECK(!fseek(file, 0, SEEK_END));
    size = ftell(file);
    CHECK(size > 0 && size < 1048576);
    rewind(file);
    CHECK((bytes = malloc(size)) != NULL);
    CHECK(fread(bytes, 1, size, file) == (size_t)size);
    fclose(file);
    desc.pRootSignature = root;
    desc.CS.pShaderBytecode = bytes;
    desc.CS.BytecodeLength = size;
    HR(ID3D12Device_CreateComputePipelineState(device, &desc, &IID_ID3D12PipelineState, (void **)&pipeline));
    free(bytes);
    return pipeline;
}

int main(int argc, char **argv)
{
    D3D12_DESCRIPTOR_RANGE1 range = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0,
            D3D12_DESCRIPTOR_RANGE_FLAG_DESCRIPTORS_STATIC_KEEPING_BUFFER_BOUNDS_CHECKS, 0};
    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {0};
    D3D12_ROOT_PARAMETER1 params[129] = {{0}};
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1,
            D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {0};
    D3D12_COMMAND_QUEUE_DESC queue_desc = {0};
    D3D12_INDIRECT_ARGUMENT_DESC indirect_args[2] = {{0}};
    D3D12_COMMAND_SIGNATURE_DESC indirect_desc = {16, 2, indirect_args, 0};
    ID3D12CommandSignature *signature;
    ID3D12Resource *output, *readback, *arguments, *cbv;
    ID3D12RootSignature *root, *bad = NULL;
    ID3D12DescriptorHeap *heap;
    ID3D12PipelineState *pso;
    struct d3d12_command_list *internal;
    struct d3d12_root_signature *internal_root;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu;
    D3D12_RANGE empty = {0, 0};
    uint32_t expected[4], *mapped, value;
    ID3DBlob *blob = NULL;
    unsigned int i, replay;
    LUID luid = {0};
    CHECK(argc == 3);
    HR(helios_vkd3d_create_device(luid, &IID_ID3D12Device, (void **)&device));
    HR(ID3D12Device_CreateCommandQueue(device, &queue_desc, &IID_ID3D12CommandQueue, (void **)&queue));
    HR(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_DIRECT,
            &IID_ID3D12CommandAllocator, (void **)&allocator));
    HR(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator,
            NULL, &IID_ID3D12GraphicsCommandList, (void **)&list));
    HR(ID3D12Device_CreateFence(device, 0, 0, &IID_ID3D12Fence, (void **)&fence));
    output = buffer(D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    readback = buffer(D3D12_HEAP_TYPE_READBACK, 0, D3D12_RESOURCE_STATE_COPY_DEST);
    arguments = buffer(D3D12_HEAP_TYPE_UPLOAD, 0, D3D12_RESOURCE_STATE_GENERIC_READ);
    cbv = buffer(D3D12_HEAP_TYPE_UPLOAD, 0, D3D12_RESOURCE_STATE_GENERIC_READ);
    HR(ID3D12Device_CreateDescriptorHeap(device, &heap_desc, &IID_ID3D12DescriptorHeap, (void **)&heap));
    cpu = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
    gpu = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heap);
    uav.Format = DXGI_FORMAT_R32_TYPELESS;
    uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = 16384;
    uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    ID3D12Device_CreateUnorderedAccessView(device, output, NULL, &uav, cpu);
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_2;
    desc.Desc_1_2.NumParameters = 128;
    desc.Desc_1_2.pParameters = params;
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 1;
    params[0].DescriptorTable.pDescriptorRanges = &range;
    for (i = 1; i < ARRAY_SIZE(params); i++)
    {
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[i].Constants.ShaderRegister = i - 1;
        params[i].Constants.Num32BitValues = 1;
    }
    HR(helios_vkd3d_create_root_signature(device, 0, &desc, &root));
    internal_root = impl_from_ID3D12RootSignature(root);
    CHECK(internal_root->parameter_count == 128);
    CHECK(internal_root->root_constant_mask & vkd3d_root_mask_bit(127));
    CHECK(internal_root->compute.flags & VKD3D_ROOT_SIGNATURE_USE_PUSH_CONSTANT_UNIFORM_BLOCK);
    HR(vkd3d_serialize_versioned_root_signature(&desc, &blob, NULL));
    CHECK(ID3D12Device_CreateRootSignature(device, 0, ID3D10Blob_GetBufferPointer(blob),
            ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **)&bad) == E_INVALIDARG);
    CHECK(bad == NULL);
    ID3D10Blob_Release(blob);
    desc.Desc_1_2.NumParameters = 129;
    CHECK(helios_vkd3d_create_root_signature(device, 0, &desc, &bad) == E_INVALIDARG && !bad);
    desc.Desc_1_2.NumParameters = 128;
    pso = pipeline(root, argv[1]);
    ID3D12GraphicsCommandList_SetPipelineState(list, pso);
    ID3D12GraphicsCommandList_SetComputeRootSignature(list, root);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 0, gpu);
    for (i = 1; i < 128; i++)
        ID3D12GraphicsCommandList_SetComputeRoot32BitConstant(list, i, i * 19, 0);
    ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 1);
    expected[0] = 19; expected[1] = 63 * 19; expected[2] = 64 * 19; expected[3] = 127 * 19;
    check_output(output, readback, expected, 4);

    ID3D12GraphicsCommandList_SetPipelineState(list, pso);
    ID3D12GraphicsCommandList_SetComputeRootSignature(list, root);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
    for (i = 1; i < 128; i++) ID3D12GraphicsCommandList_SetComputeRoot32BitConstant(list, i, ~i, 0);
    HR(helios_vkd3d_clear_root_arguments(list));
    internal = CONTAINING_RECORD(list, struct d3d12_command_list, ID3D12GraphicsCommandList_iface);
    CHECK(internal->state == impl_from_ID3D12PipelineState(pso));
    CHECK(internal->compute_bindings.root_signature == internal_root);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 0, gpu);
    ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 1);
    memset(expected, 0, sizeof(expected));
    check_output(output, readback, expected, 4);

    indirect_args[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
    indirect_args[0].Constant.RootParameterIndex = 127;
    indirect_args[0].Constant.Num32BitValuesToSet = 1;
    indirect_args[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    HR(ID3D12Device_CreateCommandSignature(device, &indirect_desc, root, &IID_ID3D12CommandSignature, (void **)&signature));
    for (replay = 0; replay < 3; replay++)
    {
        HR(ID3D12Resource_Map(arguments, 0, &empty, (void **)&mapped));
        mapped[0] = 1000 + replay; mapped[1] = mapped[2] = mapped[3] = 1;
        ID3D12Resource_Unmap(arguments, 0, NULL);
        ID3D12GraphicsCommandList_SetPipelineState(list, pso);
        ID3D12GraphicsCommandList_SetComputeRootSignature(list, root);
        ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
        ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 0, gpu);
        for (i = 1; i < 128; i++) ID3D12GraphicsCommandList_SetComputeRoot32BitConstant(list, i, 0, 0);
        ID3D12GraphicsCommandList_ExecuteIndirect(list, signature, 1, arguments, 0, NULL, 0);
        expected[3] = 1000 + replay;
        check_output(output, readback, expected, 4);
    }
    ID3D12CommandSignature_Release(signature);

    /* A bundle's private clear must not become a replay-time clear of its
     * caller's inherited root arguments. No shader dereferences a NULL view. */
    {
        ID3D12CommandAllocator *bundle_allocator;
        ID3D12GraphicsCommandList *bundle;
        HR(ID3D12Device_CreateCommandAllocator(device, D3D12_COMMAND_LIST_TYPE_BUNDLE,
                &IID_ID3D12CommandAllocator, (void **)&bundle_allocator));
        HR(ID3D12Device_CreateCommandList(device, 0, D3D12_COMMAND_LIST_TYPE_BUNDLE, bundle_allocator,
                NULL, &IID_ID3D12GraphicsCommandList, (void **)&bundle));
        HR(helios_vkd3d_clear_root_arguments(bundle));
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(bundle, root);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstant(bundle, 127, 73, 0);
        HR(ID3D12GraphicsCommandList_Close(bundle));
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, root);
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstant(list, 1, 91, 0);
        ID3D12GraphicsCommandList_ExecuteBundle(list, bundle);
        CHECK(internal->graphics_bindings.root_constants[0] == 91);
        CHECK(internal->graphics_bindings.root_constants[126] == 73);
        submit(); reset();
        ID3D12GraphicsCommandList_Release(bundle);
        ID3D12CommandAllocator_Release(bundle_allocator);
    }
    ID3D12PipelineState_Release(pso);
    ID3D12RootSignature_Release(root);

    /* Move the descriptor table to bit 127 of its separate mask. */
    for (i = 0; i < 127; i++)
    {
        memset(&params[i], 0, sizeof(params[i]));
        params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[i].Constants.ShaderRegister = i;
        params[i].Constants.Num32BitValues = 1;
    }
    params[127].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[127].DescriptorTable.NumDescriptorRanges = 1;
    params[127].DescriptorTable.pDescriptorRanges = &range;
    HR(helios_vkd3d_create_root_signature(device, 0, &desc, &root));
    CHECK(impl_from_ID3D12RootSignature(root)->descriptor_table_mask == vkd3d_root_mask_bit(127));
    pso = pipeline(root, argv[1]);
    ID3D12GraphicsCommandList_SetPipelineState(list, pso);
    ID3D12GraphicsCommandList_SetComputeRootSignature(list, root);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &heap);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(list, 127, gpu);
    for (i = 0; i < 127; i++) ID3D12GraphicsCommandList_SetComputeRoot32BitConstant(list, i, 17 * (i + 1), 0);
    ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 1);
    expected[0] = 17; expected[1] = 63 * 17; expected[2] = 64 * 17; expected[3] = 127 * 17;
    check_output(output, readback, expected, 4);
    ID3D12PipelineState_Release(pso);
    ID3D12RootSignature_Release(root);

    /* A root CBV above index 63, at the complete 128 DWORD cost. */
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    params[0].Descriptor.ShaderRegister = 0;
    params[0].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    for (i = 1; i < 124; i++) params[i].Constants.ShaderRegister = 200 + i;
    params[124].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[124].Descriptor.ShaderRegister = 0;
    params[124].Descriptor.RegisterSpace = 0;
    params[124].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_STATIC_WHILE_SET_AT_EXECUTE;
    params[125].Constants.ShaderRegister = 1;
    desc.Desc_1_2.NumParameters = 126;
    HR(helios_vkd3d_create_root_signature(device, 0, &desc, &root));
    CHECK(impl_from_ID3D12RootSignature(root)->root_descriptor_raw_va_mask & vkd3d_root_mask_bit(124));
    pso = pipeline(root, argv[2]);
    HR(ID3D12Resource_Map(cbv, 0, &empty, (void **)&mapped));
    *mapped = 0x12345678;
    ID3D12Resource_Unmap(cbv, 0, NULL);
    ID3D12GraphicsCommandList_SetPipelineState(list, pso);
    ID3D12GraphicsCommandList_SetComputeRootSignature(list, root);
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(list, 0, ID3D12Resource_GetGPUVirtualAddress(output));
    ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(list, 124, ID3D12Resource_GetGPUVirtualAddress(cbv));
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstant(list, 125, 42, 0);
    ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 1);
    expected[0] = 0x12345678; expected[1] = 42;
    check_output(output, readback, expected, 2);
    /* Re-recorded values cannot leak through a private clear. Inspect the
     * address instead of dereferencing a NULL root view (undefined at API). */
    ID3D12GraphicsCommandList_SetComputeRootSignature(list, root);
    ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(list, 124, ID3D12Resource_GetGPUVirtualAddress(cbv));
    HR(helios_vkd3d_clear_root_arguments(list));
    value = 0;
    for (i = 0; i < VKD3D_ROOT_SIGNATURE_MAX_COST; i++) value |= internal->compute_bindings.root_constants[i];
    CHECK(value == 0);
    submit();
    ID3D12PipelineState_Release(pso);
    ID3D12RootSignature_Release(root);

    /* 64 raw root descriptors consume the complete 512-byte upload. */
    reset();
    memset(params, 0, sizeof(params));
    for (i = 0; i < 64; i++)
    {
        params[i].ParameterType = i == 63 ? D3D12_ROOT_PARAMETER_TYPE_UAV : D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[i].Descriptor.ShaderRegister = i == 63 ? 0 : i;
        params[i].Descriptor.Flags = D3D12_ROOT_DESCRIPTOR_FLAG_DATA_VOLATILE;
    }
    desc.Desc_1_2.NumParameters = 64;
    HR(helios_vkd3d_create_root_signature(device, 0, &desc, &root));
    pso = pipeline(root, argv[2]);
    HR(ID3D12Resource_Map(cbv, 0, &empty, (void **)&mapped));
    mapped[64] = 51;
    ID3D12Resource_Unmap(cbv, 0, NULL);
    ID3D12GraphicsCommandList_SetPipelineState(list, pso);
    ID3D12GraphicsCommandList_SetComputeRootSignature(list, root);
    for (i = 0; i < 63; i++) ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(list, i,
            ID3D12Resource_GetGPUVirtualAddress(cbv) + (i == 1 ? 256 : 0));
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(list, 63, ID3D12Resource_GetGPUVirtualAddress(output));
    ID3D12GraphicsCommandList_Dispatch(list, 1, 1, 1);
    expected[0] = 0x12345678; expected[1] = 51;
    check_output(output, readback, expected, 2);
    ID3D12GraphicsCommandList_SetComputeRootSignature(list, root);
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstant(list, 128, 1, 0);
    CHECK(ID3D12GraphicsCommandList_Close(list) == E_INVALIDARG);
    CHECK(ID3D12GraphicsCommandList_Reset(list, allocator, NULL) == E_INVALIDARG);
    ID3D12PipelineState_Release(pso);
    ID3D12RootSignature_Release(root);
    ID3D12Resource_Release(cbv); ID3D12Resource_Release(arguments);
    ID3D12Resource_Release(readback); ID3D12Resource_Release(output);
    ID3D12DescriptorHeap_Release(heap); ID3D12GraphicsCommandList_Release(list);
    ID3D12CommandAllocator_Release(allocator); ID3D12CommandQueue_Release(queue);
    ID3D12Fence_Release(fence); ID3D12Device_Release(device);
    printf("PASS private root contract: %u checks; native Windows validation separate\n", checks);
    return 0;
}
