/* CPU descriptors shared by prebuild sizing and build recording. Negative
 * cases never reach a GPU queue; the high-stride scene is a separate GPU test. */
void test_raytracing_build_inputs(void)
{
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info, array_info, pointer_info;
    D3D12_RAYTRACING_GEOMETRY_DESC geometry[17];
    const D3D12_RAYTRACING_GEOMETRY_DESC *pointers[17];
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs, empty = {0};
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC build = {0};
    struct raytracing_test_context context;
    ID3D12Resource *scratch, *destination;
    unsigned int i, j, variant;
    HRESULT hr;

    for (i = 0; i < 16; ++i)
    {
        vkd3d_test_set_context("Invalid input %u", i);
        if (!init_raytracing_test_context(&context, D3D12_RAYTRACING_TIER_1_0))
            return;
        memset(geometry, 0, sizeof(geometry));
        memset(pointers, 0, sizeof(pointers));
        memset(&inputs, 0, sizeof(inputs));
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.NumDescs = 1;
        inputs.pGeometryDescs = geometry;
        geometry[0].Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
        geometry[0].AABBs.AABBCount = 1;
        geometry[0].AABBs.AABBs.StartAddress = 1; /* prebuild dummy, never submitted */
        geometry[0].AABBs.AABBs.StrideInBytes = 24;
        switch (i)
        {
            case 0: geometry[0].Type = (D3D12_RAYTRACING_GEOMETRY_TYPE)99; break;
            case 1: inputs.Type = (D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE)99; break;
            case 2: inputs.DescsLayout = (D3D12_ELEMENTS_LAYOUT)99; break;
            case 3: inputs.pGeometryDescs = NULL; break;
            case 4: inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS;
                inputs.ppGeometryDescs = NULL; break;
            case 5: inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS;
                inputs.ppGeometryDescs = pointers; break;
            case 6: inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
                inputs.NumDescs = D3D12_RAYTRACING_MAX_INSTANCES_PER_TOP_LEVEL_ACCELERATION_STRUCTURE + 1; break;
            case 7: inputs.NumDescs = D3D12_RAYTRACING_MAX_GEOMETRIES_PER_BOTTOM_LEVEL_ACCELERATION_STRUCTURE + 1; break;
            case 8: geometry[0].AABBs.AABBCount = ((UINT64)1 << 32) + 1; break;
            case 9: inputs.NumDescs = 2;
                geometry[0].AABBs.AABBCount = D3D12_RAYTRACING_MAX_PRIMITIVES_PER_BOTTOM_LEVEL_ACCELERATION_STRUCTURE / 2 + 1;
                geometry[1] = geometry[0]; break;
            case 10: memset(geometry, 0, sizeof(geometry));
                geometry[0].Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                geometry[0].Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
                geometry[0].Triangles.VertexCount = UINT32_MAX;
                geometry[0].Triangles.VertexBuffer.StrideInBytes = 12; break;
            case 11: inputs.Flags = (D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS)0x80000000u; break;
            case 12: inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD |
                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE; break;
            case 13: geometry[0].Flags = (D3D12_RAYTRACING_GEOMETRY_FLAGS)0x80; break;
            case 14: break; /* null outer description */
            case 15: inputs.NumDescs = ARRAY_SIZE(geometry);
                for (j = 1; j < ARRAY_SIZE(geometry); ++j) geometry[j] = geometry[0];
                geometry[16].Type = (D3D12_RAYTRACING_GEOMETRY_TYPE)99; break;
        }
        memset(&info, 0xa5, sizeof(info));
        ID3D12Device5_GetRaytracingAccelerationStructurePrebuildInfo(context.device5, i == 14 ? NULL : &inputs, &info);
        ok(!info.ResultDataMaxSizeInBytes && !info.ScratchDataSizeInBytes && !info.UpdateScratchDataSizeInBytes,
                "Failed prebuild left sizes %#llx, %#llx, %#llx.\n",
                (unsigned long long)info.ResultDataMaxSizeInBytes,
                (unsigned long long)info.ScratchDataSizeInBytes,
                (unsigned long long)info.UpdateScratchDataSizeInBytes);
        /* The old engine fails the first, non-crashing prebuild case. Stop there
         * rather than feeding its unchecked null/oversized arrays into Vulkan. */
        if (info.ResultDataMaxSizeInBytes || info.ScratchDataSizeInBytes || info.UpdateScratchDataSizeInBytes)
        {
            destroy_raytracing_test_context(&context);
            vkd3d_test_set_context(NULL);
            return;
        }

        empty.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        empty.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        ID3D12Device5_GetRaytracingAccelerationStructurePrebuildInfo(context.device5, &empty, &info);
        destination = create_default_buffer(context.context.device, info.ResultDataMaxSizeInBytes,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        scratch = create_default_buffer(context.context.device, max(info.ScratchDataSizeInBytes, 256),
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        build.Inputs = empty;
        build.DestAccelerationStructureData = ID3D12Resource_GetGPUVirtualAddress(destination);
        build.ScratchAccelerationStructureData = ID3D12Resource_GetGPUVirtualAddress(scratch);
        ID3D12GraphicsCommandList4_BuildRaytracingAccelerationStructure(context.list4, &build, 0, NULL);
        build.Inputs = inputs;
        ID3D12GraphicsCommandList4_BuildRaytracingAccelerationStructure(context.list4, i == 14 ? NULL : &build, 0, NULL);
        hr = ID3D12GraphicsCommandList_Close(context.context.list);
        ok(hr == E_INVALIDARG, "Invalid build closed with %#x.\n", (unsigned int)hr);
        ok(ID3D12Device_GetDeviceRemovedReason(context.context.device) == S_OK, "Recording rejection removed device.\n");
        trace("RTAS_INPUT_REJECTION,case=%u,zero_prebuild,close=%#x,not_submitted\n", i, (unsigned int)hr);
        ID3D12Resource_Release(scratch);
        ID3D12Resource_Release(destination);
        destroy_raytracing_test_context(&context);
    }
    vkd3d_test_set_context(NULL);

    if (!init_raytracing_test_context(&context, D3D12_RAYTRACING_TIER_1_0))
        return;
    /* 17 geometries exercise the allocation path beyond the 16-entry stack.
     * Compare array and pointer layouts with identical geometry properties.
     * Dummy GPU addresses include unaligned and near-overflow values: prebuild
     * may only distinguish zero/nonzero, never dereference or range-check them. */
    for (variant = 0; variant < 4; ++variant)
    {
        memset(&inputs, 0, sizeof(inputs));
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        inputs.Flags = variant & 1 ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE : 0;
        inputs.NumDescs = ARRAY_SIZE(geometry);
        for (i = 0; i < ARRAY_SIZE(geometry); ++i)
        {
            memset(&geometry[i], 0, sizeof(geometry[i]));
            geometry[i].Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
            geometry[i].AABBs.AABBCount = 1 + i;
            geometry[i].AABBs.AABBs.StartAddress = variant & 2 ? UINT64_MAX : 1;
            geometry[i].AABBs.AABBs.StrideInBytes = (variant & 2 ? (UINT64)0xabcdef01 << 32 : 0) | 24;
            pointers[i] = &geometry[i];
        }
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.pGeometryDescs = geometry;
        ID3D12Device5_GetRaytracingAccelerationStructurePrebuildInfo(context.device5, &inputs, &array_info);
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY_OF_POINTERS;
        inputs.ppGeometryDescs = pointers;
        ID3D12Device5_GetRaytracingAccelerationStructurePrebuildInfo(context.device5, &inputs, &pointer_info);
        ok(array_info.ResultDataMaxSizeInBytes && array_info.ScratchDataSizeInBytes, "Valid prebuild has no storage.\n");
        ok(!memcmp(&array_info, &pointer_info, sizeof(array_info)), "CPU layouts changed prebuild requirements.\n");
        if (!(variant & 1)) ok(!array_info.UpdateScratchDataSizeInBytes, "Non-update build reports update storage.\n");
    }
    /* A valid pending build must remain intact when the next call has a null
     * postbuild array. The invalid list is closed but never submitted. */
    ID3D12Device5_GetRaytracingAccelerationStructurePrebuildInfo(context.device5, &empty, &info);
    destination = create_default_buffer(context.context.device, info.ResultDataMaxSizeInBytes,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    scratch = create_default_buffer(context.context.device, max(info.ScratchDataSizeInBytes, 256),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    build.Inputs = empty;
    build.DestAccelerationStructureData = ID3D12Resource_GetGPUVirtualAddress(destination);
    build.ScratchAccelerationStructureData = ID3D12Resource_GetGPUVirtualAddress(scratch);
    ID3D12GraphicsCommandList4_BuildRaytracingAccelerationStructure(context.list4, &build, 0, NULL);
    ID3D12GraphicsCommandList4_BuildRaytracingAccelerationStructure(context.list4, &build, 1, NULL);
    hr = ID3D12GraphicsCommandList_Close(context.context.list);
    ok(hr == E_INVALIDARG, "Null postbuild array closed with %#x.\n", (unsigned int)hr);
    ok(ID3D12Device_GetDeviceRemovedReason(context.context.device) == S_OK, "Null postbuild array removed device.\n");
    trace("RTAS_INPUT_REJECTION,null_postbuild_array,close=%#x,not_submitted\n", (unsigned int)hr);
    ID3D12Resource_Release(scratch);
    ID3D12Resource_Release(destination);
    destroy_raytracing_test_context(&context);
}

void test_raytracing_prebuild_allocation_failure(void)
{
#ifndef _WIN32
    void (*arm)(unsigned int) = dlsym(RTLD_DEFAULT, "helios_test_prebuild_alloc_arm");
    void (*finish)(unsigned int *, unsigned int *, unsigned int *) =
            dlsym(RTLD_DEFAULT, "helios_test_prebuild_alloc_finish");
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO baseline, info;
    D3D12_RAYTRACING_GEOMETRY_DESC geometry[17] = {{0}};
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = {0};
    struct raytracing_test_context context;
    unsigned int i, calls, failures, live;

    if (!arm || !finish)
    {
        skip("Requires explicit LD_PRELOAD of librtas-prebuild-alloc-fault.so.\n");
        return;
    }
    if (!init_raytracing_test_context(&context, D3D12_RAYTRACING_TIER_1_0))
        return;
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs = ARRAY_SIZE(geometry);
    inputs.pGeometryDescs = geometry;
    for (i = 0; i < ARRAY_SIZE(geometry); ++i)
    {
        geometry[i].Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
        geometry[i].AABBs.AABBCount = 1;
        geometry[i].AABBs.AABBs.StartAddress = 1;
        geometry[i].AABBs.AABBs.StrideInBytes = 24;
    }
    ID3D12Device5_GetRaytracingAccelerationStructurePrebuildInfo(context.device5, &inputs, &baseline);
    ok(baseline.ResultDataMaxSizeInBytes && baseline.ScratchDataSizeInBytes, "Baseline prebuild failed.\n");
    for (i = 0; i < 3; ++i)
    {
        memset(&info, 0xa5, sizeof(info));
        arm(i);
        ID3D12Device5_GetRaytracingAccelerationStructurePrebuildInfo(context.device5, &inputs, &info);
        finish(&calls, &failures, &live);
        ok(calls == 3 && failures == 1, "Injection %u reached %u calls, %u failures.\n", i, calls, failures);
        ok(!live, "Injection %u leaked %u sibling allocations.\n", i, live);
        ok(!info.ResultDataMaxSizeInBytes && !info.ScratchDataSizeInBytes && !info.UpdateScratchDataSizeInBytes,
                "Injection %u left nonzero prebuild output.\n", i);
        ok(ID3D12Device_GetDeviceRemovedReason(context.context.device) == S_OK, "OOM removed the device.\n");
        ID3D12Device5_GetRaytracingAccelerationStructurePrebuildInfo(context.device5, &inputs, &info);
        ok(!memcmp(&info, &baseline, sizeof(info)), "Retry after injection %u changed storage requirements.\n", i);
        trace("RTAS_PREBUILD_OOM,index=%u,calls=%u,failures=%u,live=%u\n", i, calls, failures, live);
    }
    destroy_raytracing_test_context(&context);
#else
    skip("Prebuild allocator fault injection requires the Linux glibc test interposer.\n");
#endif
}
