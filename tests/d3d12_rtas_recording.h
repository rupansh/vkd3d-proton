/* Engine containment of malformed CPU-visible AS addresses. Never submit an
 * invalid list. These are not portable invalid-application D3D12 expectations;
 * the native runtime can reject such calls before reaching the Helios DDI. */
void test_raytracing_recording_rejection(void)
{
    static const struct
    {
        const char *name;
        unsigned int operation, operand;
        uint64_t address;
    } cases[] =
    {
        {"clone-unaligned-source", 1, 0, 1},
        {"clone-null-source", 1, 0, 0},
        {"clone-unmapped-source", 1, 0, UINT64_C(0x1000000000000000)},
        {"clone-self", 1, 2, 0},
        {"compact-self", 2, 2, 0},
        {"serialize-self", 3, 2, 0},
        {"build-null-destination", 0, 1, 0},
        {"build-unaligned-destination", 0, 1, 1},
        {"build-unmapped-destination", 0, 1, UINT64_C(0x1000000000000000)},
        {"update-null-source", 4, 0, 0},
        {"update-unaligned-source", 4, 0, 1},
        {"update-unmapped-source", 4, 0, UINT64_C(0x1000000000000000)},
        {"query-unaligned-source", 5, 0, 1},
    };
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build, valid;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO sizes;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC query;
    D3D12_GPU_VIRTUAL_ADDRESS src, dst, bad;
    struct raytracing_test_context context;
    ID3D12Resource *source, *destination, *scratch, *output;
    D3D12_HEAP_DESC heap_desc;
    ID3D12Heap *heap;
    unsigned int i;
    HRESULT hr;

    for (i = 0; i < ARRAY_SIZE(cases); i++)
    {
        vkd3d_test_set_context("%s", cases[i].name);
        if (!init_raytracing_test_context(&context, D3D12_RAYTRACING_TIER_1_0))
            return;
        memset(&valid, 0, sizeof(valid));
        valid.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        valid.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        valid.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE |
                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
        ID3D12Device5_GetRaytracingAccelerationStructurePrebuildInfo(context.device5, &valid.Inputs, &sizes);
        memset(&heap_desc, 0, sizeof(heap_desc));
        heap_desc.SizeInBytes = 2 * align(sizes.ResultDataMaxSizeInBytes, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT);
        heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
        hr = ID3D12Device_CreateHeap(context.context.device, &heap_desc, &IID_ID3D12Heap, (void **)&heap);
        rtas_serialization_require(SUCCEEDED(hr), "create recording AS heap separate from scratch");
        source = create_placed_buffer(context.context.device, heap, 0, max(256, sizes.ResultDataMaxSizeInBytes),
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        destination = create_placed_buffer(context.context.device, heap, heap_desc.SizeInBytes / 2,
                max(256, sizes.ResultDataMaxSizeInBytes),
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        scratch = create_default_buffer(context.context.device,
                max(256, max(sizes.ScratchDataSizeInBytes, sizes.UpdateScratchDataSizeInBytes)),
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        output = create_default_buffer(context.context.device, 256, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        src = ID3D12Resource_GetGPUVirtualAddress(source);
        dst = ID3D12Resource_GetGPUVirtualAddress(destination);
        valid.DestAccelerationStructureData = src;
        valid.ScratchAccelerationStructureData = ID3D12Resource_GetGPUVirtualAddress(scratch);
        /* A valid batched prefix must survive rollback of an incomplete entry,
         * even though Close must reject the entire list and it is never sent. */
        ID3D12GraphicsCommandList4_BuildRaytracingAccelerationStructure(context.list4, &valid, 0, NULL);
        bad = cases[i].address == 1 ? src + 1 : cases[i].address;
        if (cases[i].operation == 0 || cases[i].operation == 4)
        {
            build = valid;
            build.DestAccelerationStructureData = dst;
            if (cases[i].operation == 4)
            {
                build.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
                build.SourceAccelerationStructureData = bad;
            }
            else
                build.DestAccelerationStructureData = bad;
            ID3D12GraphicsCommandList4_BuildRaytracingAccelerationStructure(context.list4, &build, 0, NULL);
        }
        else if (cases[i].operation == 5)
        {
            uav_barrier(context.context.list, source);
            query.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_CURRENT_SIZE;
            query.DestBuffer = ID3D12Resource_GetGPUVirtualAddress(output);
            ID3D12GraphicsCommandList4_EmitRaytracingAccelerationStructurePostbuildInfo(context.list4, &query, 1, &bad);
        }
        else
        {
            uav_barrier(context.context.list, source);
            ID3D12GraphicsCommandList4_CopyRaytracingAccelerationStructure(context.list4,
                    cases[i].operand == 2 ? src : dst, cases[i].operand == 2 ? src : bad,
                    cases[i].operation == 1 ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE :
                    cases[i].operation == 2 ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT :
                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE);
        }
        hr = ID3D12GraphicsCommandList_Close(context.context.list);
        ok(hr == E_INVALIDARG, "Malformed AS recording returned %#x, expected E_INVALIDARG.\n", (unsigned int)hr);
        ok(ID3D12Device_GetDeviceRemovedReason(context.context.device) == S_OK,
                "Recording refusal unexpectedly removed the device.\n");
        trace("RTAS_RECORDING_REJECTION,%s,close=%#x,not_submitted\n", cases[i].name, (unsigned int)hr);
        /* No commands were submitted; releasing this backing cannot race GPU
         * access from the rejected list. Use the fixture's ordinary teardown. */
        ID3D12Resource_Release(output);
        ID3D12Resource_Release(scratch);
        ID3D12Resource_Release(destination);
        ID3D12Resource_Release(source);
        ID3D12Heap_Release(heap);
        destroy_raytracing_test_context(&context);
        if (hr != E_INVALIDARG)
            break;
    }
    vkd3d_test_set_context(NULL);
}
