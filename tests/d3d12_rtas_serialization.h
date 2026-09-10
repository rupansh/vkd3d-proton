/* Shared by the ray-dispatch fixture below. A TLAS is restored and serialized
 * again before any referenced BLAS restore is even recorded. The same closed
 * TLAS command list is replayed with a new GPU pointer-table producer. */
static void rtas_serialization_require(bool value, const char *what)
{
    ok(value, "%s.\n", what);
    if (!value)
    {
        fprintf(stderr, "Fatal RTAS serialization precondition: %s\n", what);
        exit(1);
    }
}

static void rtas_serialization_execute(struct raytracing_test_context *context, ID3D12GraphicsCommandList *list)
{
    ID3D12Fence *fence;
    HANDLE event;
    HRESULT hr;

    hr = ID3D12Device_CreateFence(context->context.device, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
    rtas_serialization_require(SUCCEEDED(hr), "create serialization completion fence");
    event = create_event();
    rtas_serialization_require(!!event, "create serialization completion event");
    ID3D12CommandQueue_ExecuteCommandLists(context->context.queue, 1, (ID3D12CommandList **)&list);
    hr = ID3D12CommandQueue_Signal(context->context.queue, fence, 1);
    rtas_serialization_require(SUCCEEDED(hr), "signal serialization completion fence");
    hr = ID3D12Fence_SetEventOnCompletion(fence, 1, event);
    rtas_serialization_require(SUCCEEDED(hr), "register serialization completion event");
    rtas_serialization_require(wait_event(event, 30000) == 0, "serialization completed within 30 seconds");
    rtas_serialization_require(ID3D12Fence_GetCompletedValue(fence) == 1, "authenticated serialization completion value");
    destroy_event(event);
    ID3D12Fence_Release(fence);
}

/* Record before the first producer. Replays observe TLAS -> BLAS -> TLAS at
 * one VA, including a query destination at an 8-byte (not 256-byte) offset. */
struct rtas_query_replay
{
    ID3D12CommandAllocator *allocator;
    ID3D12GraphicsCommandList4 *list;
    ID3D12Resource *output, *sentinel, *serialized, *readback;
};

static void rtas_query_replay_init(struct raytracing_test_context *context,
        struct rtas_query_replay *query, D3D12_GPU_VIRTUAL_ADDRESS source, uint64_t serialized_size)
{
    ID3D12Device *device = context->context.device;
    D3D12_COMMAND_LIST_TYPE type = ID3D12GraphicsCommandList_GetType(context->context.list);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC desc;
    D3D12_GPU_VIRTUAL_ADDRESS addresses[3] = {source, source, source};
    uint64_t sentinel[8];
    ID3D12GraphicsCommandList *list;
    unsigned int i;
    HRESULT hr;

    for (i = 0; i < ARRAY_SIZE(sentinel); i++)
        sentinel[i] = UINT64_C(0xdecafbad12345678);
    query->sentinel = create_upload_buffer(device, sizeof(sentinel), sentinel);
    query->output = create_default_buffer(device, sizeof(sentinel),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    query->serialized = create_default_buffer(device, serialized_size,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    query->readback = create_readback_buffer(device, sizeof(sentinel) +
            sizeof(D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER));
    hr = ID3D12Device_CreateCommandAllocator(device, type, &IID_ID3D12CommandAllocator, (void **)&query->allocator);
    rtas_serialization_require(SUCCEEDED(hr), "create query replay allocator");
    hr = ID3D12Device_CreateCommandList(device, 0, type, query->allocator, NULL,
            &IID_ID3D12GraphicsCommandList4, (void **)&query->list);
    rtas_serialization_require(SUCCEEDED(hr), "create query replay list");
    list = (ID3D12GraphicsCommandList *)query->list;
    ID3D12GraphicsCommandList_CopyBufferRegion(list, query->output, 0, query->sentinel, 0, sizeof(sentinel));
    transition_resource_state(list, query->output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    desc.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION;
    desc.DestBuffer = ID3D12Resource_GetGPUVirtualAddress(query->output) + 8;
    ID3D12GraphicsCommandList4_EmitRaytracingAccelerationStructurePostbuildInfo(query->list, &desc, 3, addresses);
    transition_resource_state(list, query->output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, query->readback, 0, query->output, 0, sizeof(sentinel));
    transition_resource_state(list, query->output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    /* Independently serialize the current object to cross-check both query
     * fields against GPU-produced data, not a remembered CPU size/type. */
    ID3D12GraphicsCommandList4_CopyRaytracingAccelerationStructure(query->list,
            ID3D12Resource_GetGPUVirtualAddress(query->serialized), source,
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE);
    transition_resource_state(list, query->serialized, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_CopyBufferRegion(list, query->readback, sizeof(sentinel), query->serialized, 0,
            sizeof(D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER));
    transition_resource_state(list, query->serialized, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    hr = ID3D12GraphicsCommandList_Close(list);
    rtas_serialization_require(SUCCEEDED(hr), "close query before any AS producer is recorded");
}

static void rtas_query_replay_check(struct raytracing_test_context *context,
        struct rtas_query_replay *query, uint64_t expected_count)
{
    D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER header;
    uint64_t *words;
    unsigned int i;
    HRESULT hr;

    rtas_serialization_execute(context, (ID3D12GraphicsCommandList *)query->list);
    hr = ID3D12Resource_Map(query->readback, 0, NULL, (void **)&words);
    rtas_serialization_require(SUCCEEDED(hr), "map query replay results");
    memcpy(&header, words + 8, sizeof(header));
    ok(words[0] == UINT64_C(0xdecafbad12345678) && words[7] == UINT64_C(0xdecafbad12345678),
            "Query output guards changed.\n");
    ok(header.NumBottomLevelAccelerationStructurePointersAfterHeader == expected_count,
            "Serialized object does not have expected reference count %u.\n", (unsigned int)expected_count);
    for (i = 0; i < 3; i++)
    {
        ok(words[1 + 2 * i] == header.SerializedSizeInBytesIncludingHeader,
                "Query %u size differs from current serialized header.\n", i);
        ok(words[2 + 2 * i] == expected_count,
                "Query %u pointer count differs from current AS type.\n", i);
    }
    ID3D12Resource_Unmap(query->readback, 0, NULL);
    trace("RTAS_QUERY_REPLAY,references=%u,queries=3,offset=8,guards=pass\n", (unsigned int)expected_count);
}

static void rtas_query_replay_cleanup(struct rtas_query_replay *query)
{
    ID3D12GraphicsCommandList4_Release(query->list);
    ID3D12CommandAllocator_Release(query->allocator);
    ID3D12Resource_Release(query->output);
    ID3D12Resource_Release(query->sentinel);
    ID3D12Resource_Release(query->serialized);
    ID3D12Resource_Release(query->readback);
}

static void test_rtas_serialization_roundtrip(struct raytracing_test_context *context,
        struct test_rt_geometry *geometry, ID3D12Resource **restored)
{
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC query;
    D3D12_SERIALIZED_RAYTRACING_ACCELERATION_STRUCTURE_HEADER *header;
    D3D12_GPU_VIRTUAL_ADDRESS addresses[7], *pointers, *expected;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION_DESC sizes[7];
    ID3D12Resource *original[7], *serialized[7], *first[6];
    ID3D12Resource *query_buffer, *patch, *verify, *verify_readback;
    ID3D12Resource *packed;
    D3D12_GPU_VIRTUAL_ADDRESS packed_va;
    ID3D12CommandAllocator *replay_allocator;
    ID3D12GraphicsCommandList4 *replay;
    ID3D12GraphicsCommandList *list = context->context.list;
    ID3D12Device *device = context->context.device;
    struct resource_readback readback;
    struct rtas_query_replay query_replay;
    uint64_t pointer_count, n, max_serialized_size = 0, max_as_size = 0, packed_stride;
    unsigned int i, j, pass;
    void *mapped;
    HRESULT hr;

    for (i = 0; i < 3; i++)
    {
        original[i] = geometry->bottom_acceleration_structures_tri[i];
        original[i + 3] = geometry->bottom_acceleration_structures_aabb[i];
    }
    original[6] = geometry->top_acceleration_structures[2];
    for (i = 0; i < 7; i++)
    {
        addresses[i] = ID3D12Resource_GetGPUVirtualAddress(original[i]);
        max_as_size = max(max_as_size, ID3D12Resource_GetDesc(original[i]).Width);
    }
    query_buffer = create_default_buffer(device, sizeof(sizes), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    query.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_SERIALIZATION;
    query.DestBuffer = ID3D12Resource_GetGPUVirtualAddress(query_buffer);
    ID3D12GraphicsCommandList4_EmitRaytracingAccelerationStructurePostbuildInfo(context->list4, &query, 7, addresses);
    transition_resource_state(list, query_buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_buffer_readback_with_command_list(query_buffer, DXGI_FORMAT_UNKNOWN, &readback, context->context.queue, list);
    memcpy(sizes, readback.data, sizeof(sizes));
    release_resource_readback(&readback);
    reset_command_list(list, context->context.allocator);
    /* Three AS slots in one resource, at nonzero 256-byte offsets. The slots
     * reuse the same VAs for triangles, AABBs and a TLAS. Clone and compact run
     * in both address directions before serialization and the existing later
     * ray-hit checks. No AS data is inspected as an ordinary buffer. */
    packed_stride = align(max_as_size, 256);
    packed = create_default_buffer(device, 256 + 3 * packed_stride,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
    packed_va = ID3D12Resource_GetGPUVirtualAddress(packed) + 256;
    transition_resource_state(list, query_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    for (i = 0; i < 7; i++)
    {
        rtas_serialization_require(sizes[i].SerializedSizeInBytes >= sizeof(*header), "valid serialized AS size");
        max_serialized_size = max(max_serialized_size, sizes[i].SerializedSizeInBytes);
        serialized[i] = create_default_buffer(device, align(sizes[i].SerializedSizeInBytes, 256),
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        restored[i] = create_default_buffer(device, max_as_size + 256,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        if (i < 6)
            first[i] = create_default_buffer(device, ID3D12Resource_GetDesc(original[i]).Width + 256,
                    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE);
        ID3D12GraphicsCommandList4_CopyRaytracingAccelerationStructure(context->list4,
                packed_va + 2 * packed_stride, addresses[i], D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE);
        uav_barrier(list, packed);
        ID3D12GraphicsCommandList4_CopyRaytracingAccelerationStructure(context->list4,
                packed_va, packed_va + 2 * packed_stride, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);
        uav_barrier(list, packed);
        ID3D12GraphicsCommandList4_CopyRaytracingAccelerationStructure(context->list4,
                packed_va + packed_stride, packed_va, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_CLONE);
        uav_barrier(list, packed);
        for (j = 0; j < 2; j++)
        {
            D3D12_GPU_VIRTUAL_ADDRESS copied_va = packed_va + packed_stride;
            query.InfoType = j ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE :
                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_CURRENT_SIZE;
            query.DestBuffer = ID3D12Resource_GetGPUVirtualAddress(query_buffer) + i * 16 + j * 8;
            ID3D12GraphicsCommandList4_EmitRaytracingAccelerationStructurePostbuildInfo(context->list4, &query, 1, &copied_va);
        }
        ID3D12GraphicsCommandList4_CopyRaytracingAccelerationStructure(context->list4,
                ID3D12Resource_GetGPUVirtualAddress(serialized[i]), packed_va + packed_stride,
                D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE);
        uav_barrier(list, packed);
        transition_resource_state(list, serialized[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                i == 6 ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    transition_resource_state(list, query_buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    get_buffer_readback_with_command_list(query_buffer, DXGI_FORMAT_UNKNOWN, &readback, context->context.queue, list);
    for (i = 0; i < 7; i++)
    {
        const uint64_t *actual = (const uint64_t *)readback.data + 2 * i;
        ok(actual[0] > 0 && actual[0] <= packed_stride, "Packed AS %u current extent exceeds its slot.\n", i);
        ok(actual[1] > 0 && actual[1] <= packed_stride, "Packed AS %u compacted extent exceeds its slot.\n", i);
        trace("RTAS_PACKED_COPY,index=%u,base=%#"PRIx64",stride=%"PRIu64",current=%"PRIu64",compact=%"PRIu64"\n",
                i, packed_va, packed_stride, actual[0], actual[1]);
    }
    release_resource_readback(&readback);
    reset_command_list(list, context->context.allocator);
    get_buffer_readback_with_command_list(serialized[6], DXGI_FORMAT_UNKNOWN, &readback, context->context.queue, list);
    header = readback.data;
    pointer_count = header->NumBottomLevelAccelerationStructurePointersAfterHeader;
    rtas_serialization_require(pointer_count > 0 && pointer_count <=
            (sizes[6].SerializedSizeInBytes - sizeof(*header)) / sizeof(uint64_t), "bounded TLAS pointer count");
    pointers = malloc(pointer_count * sizeof(*pointers));
    expected = malloc(pointer_count * sizeof(*expected));
    rtas_serialization_require(pointers && expected, "allocate expected TLAS pointers");
    memcpy(pointers, header + 1, pointer_count * sizeof(*pointers));
    release_resource_readback(&readback);
    reset_command_list(list, context->context.allocator);
    transition_resource_state(list, serialized[6], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    hr = ID3D12GraphicsCommandList_Close(list);
    rtas_serialization_require(SUCCEEDED(hr), "close setup for replay");
    rtas_serialization_execute(context, list);

    rtas_query_replay_init(context, &query_replay,
            ID3D12Resource_GetGPUVirtualAddress(restored[6]) + 256, align(max_serialized_size, 256));
    patch = create_upload_buffer(device, pointer_count * sizeof(*pointers), NULL);
    verify = create_default_buffer(device, align(sizes[6].SerializedSizeInBytes, 256),
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    verify_readback = create_readback_buffer(device, sizes[6].SerializedSizeInBytes);
    hr = ID3D12Device_CreateCommandAllocator(device, ID3D12GraphicsCommandList_GetType(list),
            &IID_ID3D12CommandAllocator, (void **)&replay_allocator);
    rtas_serialization_require(SUCCEEDED(hr), "create replay allocator");
    hr = ID3D12Device_CreateCommandList(device, 0, ID3D12GraphicsCommandList_GetType(list), replay_allocator, NULL,
            &IID_ID3D12GraphicsCommandList4, (void **)&replay);
    rtas_serialization_require(SUCCEEDED(hr), "create TLAS-only replay list");
    ID3D12GraphicsCommandList4_CopyBufferRegion(replay, serialized[6], sizeof(*header), patch, 0,
            pointer_count * sizeof(*pointers));
    transition_resource_state((ID3D12GraphicsCommandList *)replay, serialized[6],
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12GraphicsCommandList4_CopyRaytracingAccelerationStructure(replay,
            ID3D12Resource_GetGPUVirtualAddress(restored[6]) + 256,
            ID3D12Resource_GetGPUVirtualAddress(serialized[6]), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE);
    uav_barrier((ID3D12GraphicsCommandList *)replay, restored[6]);
    ID3D12GraphicsCommandList4_CopyRaytracingAccelerationStructure(replay,
            ID3D12Resource_GetGPUVirtualAddress(verify), ID3D12Resource_GetGPUVirtualAddress(restored[6]) + 256,
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_SERIALIZE);
    transition_resource_state((ID3D12GraphicsCommandList *)replay, verify,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList4_CopyBufferRegion(replay, verify_readback, 0, verify, 0, sizes[6].SerializedSizeInBytes);
    transition_resource_state((ID3D12GraphicsCommandList *)replay, verify,
            D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    transition_resource_state((ID3D12GraphicsCommandList *)replay, serialized[6],
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    hr = ID3D12GraphicsCommandList4_Close(replay);
    rtas_serialization_require(SUCCEEDED(hr), "close TLAS-only replay list");
    for (pass = 0; pass < 2; pass++)
    {
        for (n = 0; n < pointer_count; n++)
        {
            expected[n] = 0;
            if (!pointers[n])
                continue;
            for (j = 0; j < 6 && pointers[n] != addresses[j]; j++) {}
            rtas_serialization_require(j < 6, "serialized pointer belongs to original BLAS set");
            expected[n] = ID3D12Resource_GetGPUVirtualAddress(pass ? restored[j] : first[j]) + 256;
        }
        hr = ID3D12Resource_Map(patch, 0, NULL, &mapped);
        rtas_serialization_require(SUCCEEDED(hr), "map pointer-table producer");
        memcpy(mapped, expected, pointer_count * sizeof(*expected));
        ID3D12Resource_Unmap(patch, 0, NULL);
        rtas_serialization_execute(context, (ID3D12GraphicsCommandList *)replay);
        hr = ID3D12Resource_Map(verify_readback, 0, NULL, &mapped);
        rtas_serialization_require(SUCCEEDED(hr), "map TLAS before BLAS contents exist");
        header = mapped;
        rtas_serialization_require(header->NumBottomLevelAccelerationStructurePointersAfterHeader == pointer_count,
                "TLAS pointer count survives restore");
        for (n = 0; n < pointer_count; n++)
            ok(((uint64_t *)(header + 1))[n] == expected[n], "Replay %u TLAS pointer %u mismatch.\n", pass, (unsigned int)n);
        ID3D12Resource_Unmap(verify_readback, 0, NULL);
        trace("RTAS_TLAS_FIRST,pass=%u,references=%u,offset=256,BLAS_commands_not_recorded\n", pass, (unsigned int)pointer_count);
        rtas_query_replay_check(context, &query_replay, pointer_count);
        /* Only now record these BLAS restores. This distinguishes the test from
         * a same-list ordering test where future recording already made views. */
        reset_command_list(list, context->context.allocator);
        for (i = 0; i < 6; i++)
            ID3D12GraphicsCommandList4_CopyRaytracingAccelerationStructure(context->list4,
                    ID3D12Resource_GetGPUVirtualAddress(pass ? restored[i] : first[i]) + 256,
                    ID3D12Resource_GetGPUVirtualAddress(serialized[i]),
                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE);
        uav_barrier(list, NULL);
        hr = ID3D12GraphicsCommandList_Close(list);
        rtas_serialization_require(SUCCEEDED(hr), "close later BLAS restores");
        rtas_serialization_execute(context, list);
        if (!pass)
        {
            /* Change the type solely through GPU deserialization, then replay
             * the exact query list recorded before the initial TLAS restore.
             * The next TLAS-only replay changes this same VA back to a TLAS. */
            reset_command_list(list, context->context.allocator);
            ID3D12GraphicsCommandList4_CopyRaytracingAccelerationStructure(context->list4,
                    ID3D12Resource_GetGPUVirtualAddress(restored[6]) + 256,
                    ID3D12Resource_GetGPUVirtualAddress(serialized[0]),
                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_DESERIALIZE);
            uav_barrier(list, restored[6]);
            hr = ID3D12GraphicsCommandList_Close(list);
            rtas_serialization_require(SUCCEEDED(hr), "close TLAS-to-BLAS overwrite");
            rtas_serialization_execute(context, list);
            rtas_query_replay_check(context, &query_replay, 0);
        }
    }
    reset_command_list(list, context->context.allocator);
    rtas_query_replay_cleanup(&query_replay);
    ID3D12GraphicsCommandList4_Release(replay);
    ID3D12CommandAllocator_Release(replay_allocator);
    ID3D12Resource_Release(patch);
    ID3D12Resource_Release(verify);
    ID3D12Resource_Release(verify_readback);
    ID3D12Resource_Release(query_buffer);
    ID3D12Resource_Release(packed);
    for (i = 0; i < 7; i++)
    {
        ID3D12Resource_Release(serialized[i]);
        if (i < 6)
            ID3D12Resource_Release(first[i]);
    }
    free(expected);
    free(pointers);
}
