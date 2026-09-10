/* Pipeline queries must agree with actual shader side effects, including
 * GPU-produced control data, padded arguments, predication and closed replay. */
static void test_query_dgc_compute_internal(bool dxil)
{
    enum { COMMANDS = 4101, MAX_STRIDE = 24, ARG_OFFSET = 32 };
    static const struct { uint32_t count; uint64_t predicate; D3D12_PREDICATION_OP op; } cases[] = {
        {0, 1, D3D12_PREDICATION_OP_EQUAL_ZERO},
        {1, 1, D3D12_PREDICATION_OP_EQUAL_ZERO},
        {70, UINT64_C(1) << 40, D3D12_PREDICATION_OP_EQUAL_ZERO},
        {UINT32_MAX, UINT64_C(1) << 40, D3D12_PREDICATION_OP_EQUAL_ZERO},
        {4099, 0, D3D12_PREDICATION_OP_EQUAL_ZERO},
        {83, 0, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO},
        {COMMANDS, 1, D3D12_PREDICATION_OP_NOT_EQUAL_ZERO},
    };
    const uint32_t initial[4] = {0x12345678, 0, 0, 0x87654321};
    D3D12_ROOT_PARAMETER parameters[2] = {{0}};
    D3D12_ROOT_SIGNATURE_DESC root_desc = {2, parameters};
    D3D12_INDIRECT_ARGUMENT_DESC tokens[2] = {{0}};
    D3D12_COMMAND_SIGNATURE_DESC signature_desc = {0, 2, tokens};
    D3D12_QUERY_HEAP_DESC heap_desc = {D3D12_QUERY_HEAP_TYPE_PIPELINE_STATISTICS, 2};
    D3D12_QUERY_DATA_PIPELINE_STATISTICS *statistics;
    ID3D12Resource *argument_source, *arguments, *control_source, *control;
    ID3D12Resource *reset_source, *output, *readback, *query_readback;
    ID3D12CommandSignature *signature;
    ID3D12QueryHeap *heap;
    struct test_context context;
    struct test_context_desc context_desc = {0};
    D3D12_RANGE no_read = {0, 0};
    uint32_t *mapped, *data;
    uint64_t expected[2];
    unsigned int padded, with_count, iteration, i, j, stride, count;
    HRESULT hr;

#include "shaders/query/headers/dgc_statistics.h"

    context_desc.no_render_target = true;
    context_desc.no_root_signature = true;
    context_desc.no_pipeline = true;
    for (padded = 0; padded < 2; padded++)
    for (with_count = 0; with_count < 2; with_count++)
    {
        /* Pipeline statistics queries are valid only on a DIRECT list, even
         * when every application command in the scope is compute. */
        if (!init_test_context(&context, &context_desc))
            return;
        if (dxil && !context_supports_dxil(&context))
        {
            destroy_test_context(&context);
            return;
        }
        stride = padded ? MAX_STRIDE : 16;
        data = calloc(1, ARG_OFFSET + COMMANDS * stride);
        assert_that(!!data, "Argument allocation failed.\n");
        for (i = 0; i < COMMANDS; i++)
        {
            uint32_t *a = data + (ARG_OFFSET + i * stride) / 4;
            a[0] = i % 2;
            a[1] = i % 5 ? 1 + i % 2 : 0;
            a[2] = 1 + i % 3;
            a[3] = 2;
        }
        argument_source = create_upload_buffer(context.device, ARG_OFFSET + COMMANDS * stride, data);
        free(data);
        arguments = create_default_buffer(context.device, ARG_OFFSET + COMMANDS * stride, 0, D3D12_RESOURCE_STATE_COPY_DEST);
        control_source = create_upload_buffer(context.device, 32, NULL);
        control = create_default_buffer(context.device, 32, 0, D3D12_RESOURCE_STATE_COPY_DEST);
        reset_source = create_upload_buffer(context.device, sizeof(initial), initial);
        output = create_default_buffer(context.device, sizeof(initial), D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_DEST);
        readback = create_readback_buffer(context.device, sizeof(initial));
        query_readback = create_readback_buffer(context.device, 2 * sizeof(*statistics));
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[1].Constants.Num32BitValues = 1;
        hr = create_root_signature(context.device, &root_desc, &context.root_signature);
        assert_that(hr == S_OK, "Root signature failed, hr %#x.\n", (int)hr);
        context.pipeline_state = create_compute_pipeline_state(context.device, context.root_signature,
                dxil ? dgc_statistics_dxil : dgc_statistics_dxbc);
        tokens[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
        tokens[0].Constant.RootParameterIndex = 1;
        tokens[0].Constant.Num32BitValuesToSet = 1;
        tokens[1].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        signature_desc.ByteStride = stride;
        hr = ID3D12Device_CreateCommandSignature(context.device, &signature_desc, context.root_signature,
                &IID_ID3D12CommandSignature, (void **)&signature);
        assert_that(hr == S_OK, "Command signature failed, hr %#x.\n", (int)hr);
        hr = ID3D12Device_CreateQueryHeap(context.device, &heap_desc, &IID_ID3D12QueryHeap, (void **)&heap);
        assert_that(hr == S_OK, "Query heap failed, hr %#x.\n", (int)hr);

        for (iteration = 0; iteration < ARRAY_SIZE(cases); iteration++)
        {
            bool execute = (cases[iteration].predicate != 0) == (cases[iteration].op == D3D12_PREDICATION_OP_EQUAL_ZERO);
            vkd3d_test_set_context("padded %u, count buffer %u, replay %u", padded, with_count, iteration);
            /* The predicate operation is immutable command-list state. Replay
             * the same recording for each group of equal operations. */
            if (!iteration || cases[iteration].op != cases[iteration - 1].op)
            {
                if (iteration)
                    reset_command_list(context.list, context.allocator);
                ID3D12GraphicsCommandList_CopyBufferRegion(context.list, arguments, 0, argument_source, 0,
                        ARG_OFFSET + COMMANDS * stride);
                ID3D12GraphicsCommandList_CopyBufferRegion(context.list, control, 0, control_source, 0, 32);
                ID3D12GraphicsCommandList_CopyBufferRegion(context.list, output, 0, reset_source, 0, sizeof(initial));
                transition_resource_state(context.list, arguments, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
                transition_resource_state(context.list, control, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
                transition_resource_state(context.list, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                ID3D12GraphicsCommandList_SetComputeRootSignature(context.list, context.root_signature);
                ID3D12GraphicsCommandList_SetPipelineState(context.list, context.pipeline_state);
                ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(context.list, 0,
                        ID3D12Resource_GetGPUVirtualAddress(output) + 4);
                ID3D12GraphicsCommandList_SetPredication(context.list, control, 16, cases[iteration].op);
                ID3D12GraphicsCommandList_BeginQuery(context.list, heap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
                ID3D12GraphicsCommandList_BeginQuery(context.list, heap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 1);
                ID3D12GraphicsCommandList_ExecuteIndirect(context.list, signature, COMMANDS, arguments, ARG_OFFSET,
                        with_count ? control : NULL, with_count ? 8 : 0);
                ID3D12GraphicsCommandList_EndQuery(context.list, heap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 1);
                ID3D12GraphicsCommandList_ResolveQueryData(context.list, heap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 1, 1,
                        query_readback, sizeof(*statistics));
                ID3D12GraphicsCommandList_SetPredication(context.list, NULL, 0, D3D12_PREDICATION_OP_EQUAL_ZERO);
                uav_barrier(context.list, output);
                ID3D12GraphicsCommandList_SetComputeRoot32BitConstant(context.list, 1, 0, 0);
                ID3D12GraphicsCommandList_Dispatch(context.list, 1, 1, 1);
                ID3D12GraphicsCommandList_EndQuery(context.list, heap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0);
                ID3D12GraphicsCommandList_ResolveQueryData(context.list, heap, D3D12_QUERY_TYPE_PIPELINE_STATISTICS, 0, 1, query_readback, 0);
                transition_resource_state(context.list, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
                ID3D12GraphicsCommandList_CopyBufferRegion(context.list, readback, 0, output, 0, sizeof(initial));
                transition_resource_state(context.list, output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
                transition_resource_state(context.list, arguments, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COPY_DEST);
                transition_resource_state(context.list, control, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT, D3D12_RESOURCE_STATE_COPY_DEST);
                hr = ID3D12GraphicsCommandList_Close(context.list);
                assert_that(hr == S_OK, "Close failed, hr %#x.\n", (int)hr);
                if (FAILED(hr))
                    break;
            }

            hr = ID3D12Resource_Map(control_source, 0, &no_read, (void **)&mapped);
            assert_that(hr == S_OK, "Map control failed, hr %#x.\n", (int)hr);
            memset(mapped, 0, 32);
            mapped[2] = cases[iteration].count;
            memcpy(mapped + 4, &cases[iteration].predicate, 8);
            ID3D12Resource_Unmap(control_source, 0, NULL);
            exec_command_list(context.queue, context.list);
            wait_queue_idle(context.device, context.queue);

            expected[0] = expected[1] = 0;
            count = with_count ? min(COMMANDS, cases[iteration].count) : COMMANDS;
            for (i = 0; execute && i < count; i++)
                expected[i % 2] += 24 * (i % 5 ? 1 + i % 2 : 0) * (1 + i % 3) * 2;
            hr = ID3D12Resource_Map(query_readback, 0, NULL, (void **)&statistics);
            assert_that(hr == S_OK, "Map queries failed, hr %#x.\n", (int)hr);
            for (i = 0; i < 2; i++)
            {
                const uint64_t *fields = (const uint64_t *)&statistics[i];
                ok(statistics[i].CSInvocations == expected[0] + expected[1] + (i ? 0 : 24),
                        "Query %u returned %"PRIu64" invocations; expected %"PRIu64".\n", i,
                        statistics[i].CSInvocations, expected[0] + expected[1] + (i ? 0 : 24));
                for (j = 0; j < 10; j++)
                    ok(!fields[j], "Query %u: inactive field %u returned %"PRIu64".\n", i, j, fields[j]);
            }
            ID3D12Resource_Unmap(query_readback, 0, &no_read);
            hr = ID3D12Resource_Map(readback, 0, NULL, (void **)&mapped);
            assert_that(hr == S_OK, "Map counters failed, hr %#x.\n", (int)hr);
            ok(mapped[0] == initial[0] && mapped[3] == initial[3], "Output guards changed.\n");
            ok(mapped[1] == expected[0] + 24 && mapped[2] == expected[1],
                    "Shader counters %u/%u, expected %"PRIu64"/%"PRIu64".\n",
                    mapped[1], mapped[2], expected[0] + 24, expected[1]);
            printf("DGC_QUERY_READBACK,%u,%u,%u,%u,%u\n", dxil, padded, with_count, iteration, mapped[1] + mapped[2]);
            ID3D12Resource_Unmap(readback, 0, &no_read);
#ifdef _WIN32
            {
                extern unsigned int helios_native_debug_errors(void *device);
                ok(!helios_native_debug_errors(context.device), "Native debug layer reported an error.\n");
            }
#endif
        }
        vkd3d_test_set_context(NULL);
        ID3D12QueryHeap_Release(heap);
        ID3D12CommandSignature_Release(signature);
        ID3D12Resource_Release(query_readback);
        ID3D12Resource_Release(readback);
        ID3D12Resource_Release(output);
        ID3D12Resource_Release(reset_source);
        ID3D12Resource_Release(control);
        ID3D12Resource_Release(control_source);
        ID3D12Resource_Release(arguments);
        ID3D12Resource_Release(argument_source);
        destroy_test_context(&context);
    }
}

void test_query_dgc_compute_dxbc(void) { test_query_dgc_compute_internal(false); }
void test_query_dgc_compute_dxil(void) { test_query_dgc_compute_internal(true); }
