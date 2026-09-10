/* Target-independent rasterization readback, query and replay contracts.
 * The native entry point pins the adapter, runtime and UMD before these run. */
#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_API
#include "d3d12_crosstest.h"
#include "shaders/tir/headers/helios_tir_vs_dxbc.h"
#include "shaders/tir/headers/helios_tir_vs_dxil.h"
#include "shaders/tir/headers/helios_tir_ps_dxbc.h"
#include "shaders/tir/headers/helios_tir_ps_dxil.h"
#include "shaders/tir/headers/helios_tir_mask_dxbc.h"
#include "shaders/tir/headers/helios_tir_mask_dxil.h"
#include "shaders/tir/headers/helios_tir_cs_dxbc.h"
#include "shaders/tir/headers/helios_tir_cs_dxil.h"
#include "shaders/tir/headers/helios_tir_single_cs_dxbc.h"
#include "shaders/tir/headers/helios_tir_single_cs_dxil.h"

static void tir_require(HRESULT hr, const char *operation)
{
    assert_that(SUCCEEDED(hr), "%s failed, hr %#x.\n", operation, (unsigned int)hr);
}

static void tir_submit(struct test_context *context)
{
    ID3D12Fence *fence;
    tir_require(ID3D12Device_CreateFence(context->device, 0, D3D12_FENCE_FLAG_NONE,
            &IID_ID3D12Fence, (void **)&fence), "CreateFence");
    exec_command_list(context->queue, context->list);
    tir_require(ID3D12CommandQueue_Signal(context->queue, fence, 1), "queue signal");
#ifdef _WIN32
    {
        HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
        assert_that(event != NULL, "Fence event creation failed.\n");
        tir_require(ID3D12Fence_SetEventOnCompletion(fence, 1, event), "fence completion");
        assert_that(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0, "TIR GPU completion timed out.\n");
        CloseHandle(event);
    }
#else
    wait_queue_idle(context->device, context->queue);
#endif
    assert_that(ID3D12Fence_GetCompletedValue(fence) == 1, "TIR has no authenticated completion.\n");
    ID3D12Fence_Release(fence);
#ifdef _WIN32
    {
        extern unsigned int helios_native_debug_errors(void *device);
        ok(!helios_native_debug_errors(context->device), "Native debug layer reported an error.\n");
    }
#endif
}

static unsigned int tir_pixel_primitives(unsigned int x, unsigned int y, bool conservative)
{
    if (!conservative)
        return x < 7 && y < 5;
    if (x >= 8 || y >= 6)
        return 0;
    /* Conservative rasterization includes both triangles along their shared
     * diagonal. Count intersections independently; ordinary top-left rules
     * do not eliminate this intentional overlap. */
    return (max((float)y, 0.25f) <= 0.25f + (min(x + 1.0f, 7.25f) - 0.25f) * 5.0f / 7.0f) +
           (min(y + 1.0f, 5.25f) >= 0.25f + (max((float)x, 0.25f) - 0.25f) * 5.0f / 7.0f);
}

/* D3D standard sample positions in sixteenths relative to pixel center.
 * The two triangles form [0.25,7.25) x [0.25,5.25); count their coverage
 * separately because pixel-frequency shading can execute once per primitive. */
static const int tir_positions[3][16][2] =
{
    {{-2,-6},{6,-2},{-6,2},{2,6}},
    {{1,-3},{-1,3},{5,1},{-3,-5},{-5,5},{-7,-1},{3,7},{7,-7}},
    {{1,1},{-1,-3},{-3,2},{4,-1},{-5,-2},{2,5},{5,3},{3,-5},
     {-2,6},{0,-7},{-4,-6},{-6,4},{-8,0},{7,-4},{6,7},{-7,-8}},
};

static unsigned int tir_primitives(unsigned int x, unsigned int y, bool conservative,
        unsigned int forced, uint32_t *coverage, unsigned int *covered_samples)
{
    unsigned int i, count = 0;
    uint32_t masks[2] = {0};
    if (forced == 1 || conservative)
    {
        count = tir_pixel_primitives(x, y, conservative);
        *coverage = count ? (1u << forced) - 1 : 0;
        *covered_samples = count * forced;
        return count;
    }
    for (i = 0; i < forced; i++)
    {
        const int *position = tir_positions[forced == 4 ? 0 : forced == 8 ? 1 : 2][i];
        int sx = 16 * x + 8 + position[0], sy = 16 * y + 8 + position[1];
        if (sx >= 4 && sx < 116 && sy >= 4 && sy < 84)
        {
            masks[7 * (sy - 4) <= 5 * (sx - 4)] |= 1u << i;
            count++;
        }
    }
    *coverage = masks[0] | masks[1];
    *covered_samples = count;
    return !!masks[0] + !!masks[1];
}

static void test_tir(bool dxil, unsigned int forced)
{
    static const unsigned int output_samples[] = {2, 4, 8, 16};
    static const struct {uint32_t mask, mode; bool conservative, alpha;} cases[] =
    {
        {~0u, 0}, {7, 0}, {2, 0}, {0, 0}, {0x80000000u, 0},
        {~0u, 1}, {7, 1}, {2, 1}, {~0u, 2}, {~0u, 3},
        {~0u, 4}, {~0u, 5}, {7, 5}, {7, 0, true},
        {~0u, 6}, {~0u, 7}, {~0u, 8}, {~0u, 1, false, true}, {~0u, 2, false, true},
    };
    const unsigned int width = 16, height = 8, pixels = width * height;
    const float clear[4] = {1, 2, 4, 8};
    const D3D12_SHADER_BYTECODE vs = dxil ? shader_bytecode(helios_tir_vs_dxil, sizeof(helios_tir_vs_dxil)) :
            shader_bytecode(helios_tir_vs_dxbc, sizeof(helios_tir_vs_dxbc));
    const D3D12_SHADER_BYTECODE ps = dxil ? shader_bytecode(helios_tir_ps_dxil, sizeof(helios_tir_ps_dxil)) :
            shader_bytecode(helios_tir_ps_dxbc, sizeof(helios_tir_ps_dxbc));
    const D3D12_SHADER_BYTECODE mask_ps = dxil ? shader_bytecode(helios_tir_mask_dxil, sizeof(helios_tir_mask_dxil)) :
            shader_bytecode(helios_tir_mask_dxbc, sizeof(helios_tir_mask_dxbc));
    const D3D12_SHADER_BYTECODE cs = forced != 1 ? (dxil ?
            shader_bytecode(helios_tir_single_cs_dxil, sizeof(helios_tir_single_cs_dxil)) :
            shader_bytecode(helios_tir_single_cs_dxbc, sizeof(helios_tir_single_cs_dxbc))) : dxil ? shader_bytecode(helios_tir_cs_dxil, sizeof(helios_tir_cs_dxil)) :
            shader_bytecode(helios_tir_cs_dxbc, sizeof(helios_tir_cs_dxbc));
    unsigned int sample_index, case_index, replay, x, y, s, c;

    for (sample_index = 0; sample_index < (forced == 1 ? ARRAY_SIZE(output_samples) : 1); sample_index++)
    {
        unsigned int samples = forced == 1 ? output_samples[sample_index] : 1;
        unsigned int data_bytes = 16 * pixels * (1 + samples), query_offset = data_bytes + 64;
        unsigned int total_bytes = query_offset + 32;
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS quality = {DXGI_FORMAT_R32G32B32A32_FLOAT, samples};
        D3D12_ROOT_SIGNATURE_DESC root_desc = {0};
        D3D12_ROOT_PARAMETER parameters[3] = {{0}};
        D3D12_DESCRIPTOR_RANGE range = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {0};
        D3D12_QUERY_HEAP_DESC query_desc = {D3D12_QUERY_HEAP_TYPE_OCCLUSION, 4, 0};
        D3D12_COMPUTE_PIPELINE_STATE_DESC compute_desc = {0};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc;
        ID3D12Resource *data, *initial, *readback;
        ID3D12DescriptorHeap *srv_heap;
        ID3D12PipelineState *compute, *pso, *mask_variant;
        ID3D12QueryHeap *queries;
        struct test_context_desc context_desc = {0};
        struct test_context context;
        D3D12_RANGE read_range = {0, total_bytes}, write_range = {0, 0};
        void *mapped, *initial_data = calloc(1, total_bytes);
        ID3D12Device *caps_device = create_device();
        assert_that(caps_device && initial_data, "TIR test setup failed.\n");
        tir_require(ID3D12Device_CheckFeatureSupport(caps_device, D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS,
                &quality, sizeof(quality)), "sample capabilities");
        ID3D12Device_Release(caps_device);
        if (!quality.NumQualityLevels)
        {
            ok(samples > 4, "Required %u-sample float target unsupported.\n", samples);
            printf("TIR_OUTPUT_UNSUPPORTED,samples=%u,format=RGBA32_FLOAT\n", samples);
            free(initial_data);
            continue;
        }

        context_desc.rt_width = width;
        context_desc.rt_height = height;
        context_desc.rt_format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        context_desc.sample_desc.Count = samples;
        context_desc.no_root_signature = context_desc.no_pipeline = true;
        assert_that(init_test_context(&context, &context_desc), "TIR context failed.\n");
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants.Num32BitValues = 8;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        parameters[1].Descriptor.ShaderRegister = 1;
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[2].DescriptorTable.NumDescriptorRanges = 1;
        parameters[2].DescriptorTable.pDescriptorRanges = &range;
        root_desc.NumParameters = ARRAY_SIZE(parameters);
        root_desc.pParameters = parameters;
        tir_require(create_root_signature(context.device, &root_desc, &context.root_signature), "root signature");
        compute_desc.pRootSignature = context.root_signature;
        compute_desc.CS = cs;
        tir_require(ID3D12Device_CreateComputePipelineState(context.device, &compute_desc,
                &IID_ID3D12PipelineState, (void **)&compute), "readback compute pipeline");
        data = create_default_buffer(context.device, total_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_COPY_DEST);
        memset((char *)initial_data + data_bytes, 0xcd, total_bytes - data_bytes);
        initial = create_upload_buffer(context.device, total_bytes, initial_data);
        free(initial_data);
        readback = create_readback_buffer(context.device, total_bytes);
        srv_heap = create_gpu_descriptor_heap(context.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1);
        srv.Format = context_desc.rt_format;
        srv.ViewDimension = samples == 1 ? D3D12_SRV_DIMENSION_TEXTURE2D : D3D12_SRV_DIMENSION_TEXTURE2DMS;
        if (samples == 1) srv.Texture2D.MipLevels = 1;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        ID3D12Device_CreateShaderResourceView(context.device, context.render_target, &srv,
                ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(srv_heap));
        tir_require(ID3D12Device_CreateQueryHeap(context.device, &query_desc, &IID_ID3D12QueryHeap,
                (void **)&queries), "occlusion heap");

        for (case_index = 0; case_index < (forced == 1 ? 14 : ARRAY_SIZE(cases)); case_index++)
        {
            struct {float rectangle[4]; uint32_t mode, samples, width, height;} args =
                    {{0.25f, 0.25f, 7.25f, 5.25f}, cases[case_index].mode, samples, width, height};
            unsigned int covered_width = 8;
            unsigned int covered_height = 6;
            uint32_t output_mask = cases[case_index].mask & ((1u << samples) - 1);
            bool mask_shader = args.mode == 1 || args.mode == 2;
            const D3D12_SHADER_BYTECODE *fragment = mask_shader ? &mask_ps : &ps;
            uint64_t expected_count, raster_count = 0;
            uint64_t query_values[4];

            for (y = 0; y < covered_height; y++) for (x = 0; x < covered_width; x++)
            {
                uint32_t coverage;
                unsigned int covered_samples;
                tir_primitives(x, y, cases[case_index].conservative, forced, &coverage, &covered_samples);
                raster_count += covered_samples;
            }
            expected_count = output_mask ? raster_count : 0;

            vkd3d_test_set_context("%s forced %u samples %u case %u", dxil ? "DXIL" : "DXBC", forced, samples, case_index);
            init_pipeline_state_desc(&pso_desc, context.root_signature, context_desc.rt_format, &vs, fragment, NULL);
            pso_desc.SampleDesc.Count = samples;
            pso_desc.SampleMask = cases[case_index].mask;
            pso_desc.RasterizerState.ForcedSampleCount = forced;
            pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pso_desc.RasterizerState.ConservativeRaster = cases[case_index].conservative ?
                    D3D12_CONSERVATIVE_RASTERIZATION_MODE_ON : D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
            pso_desc.BlendState.AlphaToCoverageEnable = args.mode >= 4 || cases[case_index].alpha;
            pso_desc.BlendState.RenderTarget[0].BlendEnable = true;
            pso_desc.BlendState.RenderTarget[0].SrcBlend = pso_desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
            pso_desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
            pso_desc.BlendState.RenderTarget[0].SrcBlendAlpha = pso_desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
            pso_desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
            tir_require(ID3D12Device_CreateGraphicsPipelineState(context.device, &pso_desc,
                    &IID_ID3D12PipelineState, (void **)&pso), "forced-sample pipeline");
            pso_desc.SampleMask = 7;
            tir_require(ID3D12Device_CreateGraphicsPipelineState(context.device, &pso_desc,
                    &IID_ID3D12PipelineState, (void **)&mask_variant), "mask-change pipeline");

            ID3D12GraphicsCommandList_CopyBufferRegion(context.list, data, 0, initial, 0, total_bytes);
            transition_resource_state(context.list, data, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            ID3D12GraphicsCommandList_ClearRenderTargetView(context.list, context.rtv, clear, 0, NULL);
            ID3D12GraphicsCommandList_OMSetRenderTargets(context.list, 1, &context.rtv, false, NULL);
            ID3D12GraphicsCommandList_SetGraphicsRootSignature(context.list, context.root_signature);
            ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(context.list, 0, 8, &args, 0);
            ID3D12GraphicsCommandList_SetGraphicsRootUnorderedAccessView(context.list, 1, ID3D12Resource_GetGPUVirtualAddress(data));
            ID3D12GraphicsCommandList_RSSetViewports(context.list, 1, &context.viewport);
            ID3D12GraphicsCommandList_RSSetScissorRects(context.list, 1, &context.scissor_rect);
            ID3D12GraphicsCommandList_IASetPrimitiveTopology(context.list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D12GraphicsCommandList_SetPipelineState(context.list, pso);
            ID3D12GraphicsCommandList_BeginQuery(context.list, queries, D3D12_QUERY_TYPE_OCCLUSION, 0);
            ID3D12GraphicsCommandList_DrawInstanced(context.list, 6, 1, 0, 0);
            ID3D12GraphicsCommandList_DrawInstanced(context.list, 6, 1, 0, 0);
            ID3D12GraphicsCommandList_EndQuery(context.list, queries, D3D12_QUERY_TYPE_OCCLUSION, 0);
            ID3D12GraphicsCommandList_BeginQuery(context.list, queries, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 1);
            ID3D12GraphicsCommandList_DrawInstanced(context.list, 6, 1, 0, 0);
            ID3D12GraphicsCommandList_EndQuery(context.list, queries, D3D12_QUERY_TYPE_BINARY_OCCLUSION, 1);
            transition_resource_state(context.list, context.render_target, D3D12_RESOURCE_STATE_RENDER_TARGET,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            uav_barrier(context.list, data);
            ID3D12GraphicsCommandList_SetPipelineState(context.list, compute);
            ID3D12GraphicsCommandList_SetDescriptorHeaps(context.list, 1, &srv_heap);
            ID3D12GraphicsCommandList_SetComputeRootSignature(context.list, context.root_signature);
            ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(context.list, 0, 8, &args, 0);
            ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(context.list, 1, ID3D12Resource_GetGPUVirtualAddress(data));
            ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(context.list, 2,
                    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(srv_heap));
            ID3D12GraphicsCommandList_Dispatch(context.list, 2, 1, 1);
            transition_resource_state(context.list, data, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            ID3D12GraphicsCommandList_CopyBufferRegion(context.list, readback, 0, data, 0, total_bytes);
            transition_resource_state(context.list, data, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            transition_resource_state(context.list, context.render_target, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    D3D12_RESOURCE_STATE_RENDER_TARGET);
            ID3D12GraphicsCommandList_SetPipelineState(context.list, pso);
            ID3D12GraphicsCommandList_BeginQuery(context.list, queries, D3D12_QUERY_TYPE_OCCLUSION, 2);
            ID3D12GraphicsCommandList_DrawInstanced(context.list, 6, 1, 0, 0);
            ID3D12GraphicsCommandList_SetPipelineState(context.list, mask_variant);
            ID3D12GraphicsCommandList_DrawInstanced(context.list, 6, 1, 0, 0);
            ID3D12GraphicsCommandList_EndQuery(context.list, queries, D3D12_QUERY_TYPE_OCCLUSION, 2);
            /* Resolve after a shader/PSO transition; guard bytes surround output. */
            ID3D12GraphicsCommandList_ResolveQueryData(context.list, queries, D3D12_QUERY_TYPE_OCCLUSION,
                    0, 1, readback, query_offset);
            ID3D12GraphicsCommandList_ResolveQueryData(context.list, queries, D3D12_QUERY_TYPE_BINARY_OCCLUSION,
                    1, 1, readback, query_offset + 8);
            ID3D12GraphicsCommandList_ResolveQueryData(context.list, queries, D3D12_QUERY_TYPE_OCCLUSION,
                    2, 1, readback, query_offset + 16);
            transition_resource_state(context.list, data, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            tir_require(ID3D12GraphicsCommandList_Close(context.list), "close TIR command list");

            for (replay = 0; replay < 2; replay++)
            {
                tir_submit(&context);
                tir_require(ID3D12Resource_Map(readback, 0, &read_range, &mapped), "map TIR result");
                memcpy(query_values, (char *)mapped + query_offset, sizeof(query_values));
                ok(query_values[0] == 2 * expected_count, "Replay %u occlusion %#"PRIx64", expected %#"PRIx64".\n",
                        replay, query_values[0], 2 * expected_count);
                ok(query_values[1] == !!expected_count, "Binary query %#"PRIx64", expected %u.\n", query_values[1], !!expected_count);
                ok(query_values[2] == expected_count + raster_count,
                        "Mixed-mask query %#"PRIx64", expected %#"PRIx64".\n", query_values[2], expected_count + raster_count);
                ok(query_values[3] == UINT64_C(0xcdcdcdcdcdcdcdcd), "Query tail guard modified.\n");
                for (x = data_bytes; x < query_offset; x++)
                    ok(((uint8_t *)mapped)[x] == 0xcd, "Readback guard %u modified.\n", x);
                for (y = 0; y < height; y++) for (x = 0; x < width; x++)
                {
                    uint32_t input_coverage;
                    unsigned int covered_samples;
                    unsigned int primitives = tir_primitives(x, y, cases[case_index].conservative,
                            forced, &input_coverage, &covered_samples);
                    bool covered = primitives != 0;
                    unsigned int expected_invocations = output_mask ? 3 * primitives : 0;
                    uint32_t *pixel = (uint32_t *)mapped + 4 * (y * width + x);
                    float interpolated;
                    memcpy(&interpolated, pixel + 3, sizeof(interpolated));
                    ok(pixel[0] == expected_invocations, "Pixel %u,%u invocations %u, expected %u.\n", x, y, pixel[0], expected_invocations);
                    ok(pixel[1] == (expected_invocations ? input_coverage : 0), "Pixel %u,%u input coverage %#x.\n", x, y, pixel[1]);
                    ok(pixel[2] == (expected_invocations ? forced : 0), "Pixel %u,%u raster sample count %u.\n", x, y, pixel[2]);
                    ok(fabsf(interpolated - (expected_invocations ? x + 0.5f + (forced == 1 ? 0 :
                            tir_positions[forced == 4 ? 0 : forced == 8 ? 1 : 2][0][0] / 16.0f) : 0)) < 0.0001f,
                            "Pixel %u,%u pull interpolation %.9g.\n", x, y, interpolated);
                    for (s = 0; s < samples; s++)
                    {
                        uint32_t coverage = args.mode == 2 ? 0 : mask_shader ? ((x + y) & 1 ? 1 : 10) : ~0u;
                        bool written = covered && (output_mask & coverage & (1u << s)) &&
                                !(args.mode == 3 && (x & 1)) && args.mode != 4 && args.mode != 6 && args.mode != 8;
                        float *color = (float *)mapped + 4 * pixels + 4 * (samples * (y * width + x) + s);
                        for (c = 0; c < 4; c++)
                        {
                            float increment = c < 3 ? 0.25f * (c + 1) : args.mode == 5 ? 1 : args.mode == 7 ? 0.75f : 0.5f;
                            float expected = clear[c] + (written ? 3 * primitives * increment : 0);
                            ok(color[c] == expected, "Pixel %u,%u sample %u component %u got %.9g, expected %.9g.\n",
                                    x, y, s, c, color[c], expected);
                        }
                    }
                }
                ID3D12Resource_Unmap(readback, 0, &write_range);
                printf("TIR_READBACK,%s,forced=%u,samples=%u,case=%u,replay=%u,occlusion=%"PRIu64",binary=%"PRIu64",mixed=%"PRIu64"\n",
                        dxil ? "DXIL" : "DXBC", forced, samples, case_index, replay, query_values[0], query_values[1], query_values[2]);
            }
            reset_command_list(context.list, context.allocator);
            ID3D12PipelineState_Release(mask_variant);
            ID3D12PipelineState_Release(pso);
        }
        ID3D12QueryHeap_Release(queries);
        ID3D12PipelineState_Release(compute);
        ID3D12DescriptorHeap_Release(srv_heap);
        ID3D12Resource_Release(initial);
        ID3D12Resource_Release(data);
        ID3D12Resource_Release(readback);
        destroy_test_context(&context);
    }
    vkd3d_test_set_context(NULL);
}

void test_tir_one_sample_dxbc(void) { test_tir(false, 1); }
void test_tir_one_sample_dxil(void) { test_tir(true, 1); }

void test_tir_mixed_samples_dxbc(void) { test_tir(false, 4); test_tir(false, 8); test_tir(false, 16); }
void test_tir_mixed_samples_dxil(void) { test_tir(true, 4); test_tir(true, 8); test_tir(true, 16); }

#include "shaders/tir/headers/helios_tir_mrt_dxbc.h"
#include "shaders/tir/headers/helios_tir_mrt_dxil.h"
#include "shaders/tir/headers/helios_tir_uint_dxbc.h"
#include "shaders/tir/headers/helios_tir_uint_dxil.h"
#include "shaders/tir/headers/helios_tir_sample_dxbc.h"
#include "shaders/tir/headers/helios_tir_sample_dxil.h"
#include "shaders/tir/headers/helios_tir_depth_dxbc.h"
#include "shaders/tir/headers/helios_tir_depth_dxil.h"

static void test_tir_outputs(bool dxil)
{
    const D3D12_SHADER_BYTECODE vs = dxil ? shader_bytecode(helios_tir_vs_dxil, sizeof(helios_tir_vs_dxil)) :
            shader_bytecode(helios_tir_vs_dxbc, sizeof(helios_tir_vs_dxbc));
    unsigned int integer, forced, target, x, y, c, replay;
    const float clear[4] = {16, 32, 64, 128};
    const float increment[2][4] = {{1, 2, 3, 4}, {8, 16, 32, 64}};
    for (integer = 0; integer < 2; integer++)
    {
        D3D12_SHADER_BYTECODE ps = integer ? (dxil ? shader_bytecode(helios_tir_uint_dxil, sizeof(helios_tir_uint_dxil)) :
                shader_bytecode(helios_tir_uint_dxbc, sizeof(helios_tir_uint_dxbc))) :
                (dxil ? shader_bytecode(helios_tir_mrt_dxil, sizeof(helios_tir_mrt_dxil)) :
                shader_bytecode(helios_tir_mrt_dxbc, sizeof(helios_tir_mrt_dxbc)));
        D3D12_ROOT_SIGNATURE_DESC root_desc = {0};
        D3D12_ROOT_PARAMETER parameter = {0};
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc;
        D3D12_RANGE read_range = {0, 4096}, write_range = {0, 0};
        ID3D12DescriptorHeap *heap;
        ID3D12Resource *targets[2], *readback;
        ID3D12PipelineState *pso;
        D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2];
        struct test_context_desc context_desc = {0};
        struct test_context context;
        struct {float rectangle[4]; uint32_t mode, samples, width, height;} args =
                {{0.25f, 0.25f, 7.25f, 5.25f}, 0, 1, 16, 8};
        context_desc.rt_width = 16;
        context_desc.rt_height = 8;
        context_desc.rt_format = integer ? DXGI_FORMAT_R32G32B32A32_UINT : DXGI_FORMAT_R32G32B32A32_FLOAT;
        context_desc.no_root_signature = context_desc.no_pipeline = context_desc.no_render_target = true;
        assert_that(init_test_context(&context, &context_desc), "TIR MRT context failed.\n");
        set_viewport(&context.viewport, 0, 0, 16, 8, 0, 1);
        set_rect(&context.scissor_rect, 0, 0, 16, 8);
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameter.Constants.Num32BitValues = 8;
        root_desc.NumParameters = 1;
        root_desc.pParameters = &parameter;
        tir_require(create_root_signature(context.device, &root_desc, &context.root_signature), "MRT root");
        heap = create_cpu_descriptor_heap(context.device, D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 2);
        for (target = 0; target < 2; target++)
        {
            rtvs[target] = get_cpu_rtv_handle(&context, heap, target);
            create_render_target(&context, &context_desc, &targets[target], &rtvs[target]);
        }
        readback = create_readback_buffer(context.device, 4096);
        for (forced = 4; forced <= 16; forced *= 2)
        {
            vkd3d_test_set_context("%s MRT forced %u integer %u", dxil ? "DXIL" : "DXBC", forced, integer);
            init_pipeline_state_desc(&pso_desc, context.root_signature, context_desc.rt_format, &vs, &ps, NULL);
            pso_desc.NumRenderTargets = 2;
            pso_desc.RTVFormats[1] = context_desc.rt_format;
            pso_desc.RasterizerState.ForcedSampleCount = forced;
            pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pso_desc.BlendState.IndependentBlendEnable = !integer;
            for (target = 0; target < 2; target++)
            {
                D3D12_RENDER_TARGET_BLEND_DESC *blend = &pso_desc.BlendState.RenderTarget[target];
                blend->LogicOpEnable = integer;
                blend->LogicOp = D3D12_LOGIC_OP_XOR;
                blend->BlendEnable = !integer;
                blend->SrcBlend = blend->DestBlend = blend->SrcBlendAlpha = blend->DestBlendAlpha = D3D12_BLEND_ONE;
                blend->BlendOp = blend->BlendOpAlpha = target ? D3D12_BLEND_OP_REV_SUBTRACT : D3D12_BLEND_OP_ADD;
                blend->RenderTargetWriteMask = target && !integer ? 5 : 15;
            }
            tir_require(ID3D12Device_CreateGraphicsPipelineState(context.device, &pso_desc,
                    &IID_ID3D12PipelineState, (void **)&pso), "MRT TIR pipeline");
            for (target = 0; target < 2; target++)
                ID3D12GraphicsCommandList_ClearRenderTargetView(context.list, rtvs[target], clear, 0, NULL);
            ID3D12GraphicsCommandList_OMSetRenderTargets(context.list, 2, rtvs, false, NULL);
            ID3D12GraphicsCommandList_SetGraphicsRootSignature(context.list, context.root_signature);
            ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(context.list, 0, 8, &args, 0);
            ID3D12GraphicsCommandList_RSSetViewports(context.list, 1, &context.viewport);
            ID3D12GraphicsCommandList_RSSetScissorRects(context.list, 1, &context.scissor_rect);
            ID3D12GraphicsCommandList_IASetPrimitiveTopology(context.list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D12GraphicsCommandList_SetPipelineState(context.list, pso);
            ID3D12GraphicsCommandList_DrawInstanced(context.list, 6, 1, 0, 0);
            for (target = 0; target < 2; target++)
            {
                D3D12_TEXTURE_COPY_LOCATION src = {0}, dst = {0};
                src.pResource = targets[target];
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.pResource = readback;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint.Offset = target * 2048;
                dst.PlacedFootprint.Footprint.Format = context_desc.rt_format;
                dst.PlacedFootprint.Footprint.Width = 16;
                dst.PlacedFootprint.Footprint.Height = 8;
                dst.PlacedFootprint.Footprint.Depth = 1;
                dst.PlacedFootprint.Footprint.RowPitch = 256;
                transition_resource_state(context.list, targets[target], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
                ID3D12GraphicsCommandList_CopyTextureRegion(context.list, &dst, 0, 0, 0, &src, NULL);
                transition_resource_state(context.list, targets[target], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            }
            tir_require(ID3D12GraphicsCommandList_Close(context.list), "MRT close");
            for (replay = 0; replay < 2; replay++)
            {
                void *mapped;
                tir_submit(&context);
                tir_require(ID3D12Resource_Map(readback, 0, &read_range, &mapped), "MRT map");
                for (y = 0; y < 8; y++) for (x = 0; x < 16; x++)
                {
                    uint32_t coverage;
                    unsigned int covered_samples, primitives = tir_primitives(x, y, false, forced, &coverage, &covered_samples);
                    for (target = 0; target < 2; target++) for (c = 0; c < 4; c++)
                    {
                        unsigned int index = target * 512 + y * 64 + x * 4 + c;
                        if (integer)
                        {
                            uint32_t expected = (uint32_t)clear[c] ^ (primitives & 1 ? (uint32_t)increment[target][c] : 0);
                            ok(((uint32_t *)mapped)[index] == expected, "MRT%u pixel %u,%u component %u got %u, expected %u.\n",
                                    target, x, y, c, ((uint32_t *)mapped)[index], expected);
                        }
                        else
                        {
                            float expected = clear[c] + (target ? (c & 1 ? 0 : -1) : 1) * increment[target][c] * primitives;
                            ok(((float *)mapped)[index] == expected, "MRT%u pixel %u,%u component %u got %.9g, expected %.9g.\n",
                                    target, x, y, c, ((float *)mapped)[index], expected);
                        }
                    }
                }
                ID3D12Resource_Unmap(readback, 0, &write_range);
                printf("TIR_MRT,%s,forced=%u,integer=%u,replay=%u\n", dxil ? "DXIL" : "DXBC", forced, integer, replay);
            }
            reset_command_list(context.list, context.allocator);
            ID3D12PipelineState_Release(pso);
        }
        ID3D12Resource_Release(readback);
        for (target = 0; target < 2; target++) ID3D12Resource_Release(targets[target]);
        ID3D12DescriptorHeap_Release(heap);
        destroy_test_context(&context);
    }
    vkd3d_test_set_context(NULL);
}

static void test_tir_invalid(bool dxil)
{
    ID3D12Device *device = create_device();
    ID3D12RootSignature *root;
    D3D12_ROOT_SIGNATURE_DESC root_desc = {0};
    unsigned int i;
    tir_require(create_root_signature(device, &root_desc, &root), "invalid-case root");
    for (i = 0; i < 7; i++)
    {
        ID3D12PipelineState *pso = NULL;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
        D3D12_SHADER_BYTECODE ps;
        HRESULT hr;
        init_pipeline_state_desc(&desc, root, DXGI_FORMAT_R8G8B8A8_UNORM, NULL, NULL, NULL);
        desc.RasterizerState.ForcedSampleCount = i == 0 ? 2 : i == 1 ? 32 : 4;
        if (i == 2) desc.SampleDesc.Count = 4;
        if (i == 3) {desc.DepthStencilState.DepthEnable = true; desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;}
        if (i == 4) {desc.DepthStencilState.StencilEnable = true; desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;}
        if (i >= 5)
        {
            ps = i == 5 ? (dxil ? shader_bytecode(helios_tir_sample_dxil, sizeof(helios_tir_sample_dxil)) :
                    shader_bytecode(helios_tir_sample_dxbc, sizeof(helios_tir_sample_dxbc))) :
                    (dxil ? shader_bytecode(helios_tir_depth_dxil, sizeof(helios_tir_depth_dxil)) :
                    shader_bytecode(helios_tir_depth_dxbc, sizeof(helios_tir_depth_dxbc)));
            desc.PS = ps;
        }
        hr = ID3D12Device_CreateGraphicsPipelineState(device, &desc, &IID_ID3D12PipelineState, (void **)&pso);
        ok(hr == E_INVALIDARG && !pso, "Invalid TIR case %u returned %#x, object %p.\n", i, (unsigned int)hr, pso);
        if (pso) ID3D12PipelineState_Release(pso);
    }
    ok(ID3D12Device_GetDeviceRemovedReason(device) == S_OK, "Invalid PSO removed the device.\n");
    ID3D12RootSignature_Release(root);
    ID3D12Device_Release(device);
}

void test_tir_outputs_dxbc(void) { test_tir_outputs(false); }
void test_tir_outputs_dxil(void) { test_tir_outputs(true); }
void test_tir_invalid_dxbc(void) { test_tir_invalid(false); }
void test_tir_invalid_dxil(void) { test_tir_invalid(true); }
