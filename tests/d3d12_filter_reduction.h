/* Tier-2 min/max filtering is behavior, not just successful sampler creation.
 * Check both descriptor samplers and static samplers, including zero-weight
 * texels/mips, which must not contribute to a min/max reduction. */
static void test_filter_reduction_internal(bool dxil)
{
    static const float mip0[] = {-4.0f, 2.0f, 8.0f, 18.0f}, mip1[] = {-8.0f};
    static const D3D12_SUBRESOURCE_DATA data[] = {{mip0, 8, 16}, {mip1, 4, 4}};
    static const struct
    {
        D3D12_FILTER filter;
        float expected[5];
    } cases[] = {
        {D3D12_FILTER_MIN_MAG_MIP_LINEAR, {6.0f, -1.0f, -8.0f, -4.0f, 2.0f}},
        {D3D12_FILTER_MINIMUM_MIN_MAG_MIP_LINEAR, {-4.0f, -8.0f, -8.0f, -4.0f, 2.0f}},
        {D3D12_FILTER_MAXIMUM_MIN_MAG_MIP_LINEAR, {18.0f, 18.0f, -8.0f, -4.0f, 2.0f}},
    };
    D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1},
            {D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1}};
    D3D12_ROOT_PARAMETER parameters[2] = {{0}};
    D3D12_ROOT_SIGNATURE_DESC root_desc = {2, parameters};
    D3D12_STATIC_SAMPLER_DESC static_desc = {0};
    D3D12_SAMPLER_DESC sampler_desc = {0};
    struct test_context_desc desc = {0};
    struct resource_readback readback;
    struct test_context context;
    ID3D12DescriptorHeap *heaps[2];
    ID3D12RootSignature *root;
    ID3D12PipelineState *pipeline;
    ID3D12Resource *texture;
    unsigned int stat, c, i;
    HRESULT hr;

#include "shaders/descriptors/headers/filter_reduction.h"

    desc.no_root_signature = true;
    desc.no_pipeline = true;
    desc.rt_width = 5;
    desc.rt_height = 1;
    desc.rt_format = DXGI_FORMAT_R32_FLOAT;
    if (!init_test_context(&context, &desc))
        return;
    if ((dxil && !context_supports_dxil(&context)) || !is_min_max_filtering_supported(context.device))
    {
        skip("DXIL or min/max filtering unsupported.\n");
        destroy_test_context(&context);
        return;
    }
    for (i = 0; i < 2; i++)
    {
        parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[i].DescriptorTable.NumDescriptorRanges = 1;
        parameters[i].DescriptorTable.pDescriptorRanges = &ranges[i];
        parameters[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    heaps[0] = create_gpu_descriptor_heap(context.device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1);
    heaps[1] = create_gpu_descriptor_heap(context.device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1);
    texture = create_default_texture2d(context.device, 2, 2, 1, 2, DXGI_FORMAT_R32_FLOAT,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    upload_texture_data(texture, data, 2, context.queue, context.list);
    reset_command_list(context.list, context.allocator);
    transition_resource_state(context.list, texture, D3D12_RESOURCE_STATE_COPY_DEST,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ID3D12Device_CreateShaderResourceView(context.device, texture, NULL,
            ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heaps[0]));

    for (stat = 0; stat < 2; stat++)
    for (c = 0; c < ARRAY_SIZE(cases); c++)
    {
        vkd3d_test_set_context("static %u, reduction %u", stat, c);
        sampler_desc.Filter = cases[c].filter;
        sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler_desc.MaxLOD = 1.0f;
        sampler_desc.MaxAnisotropy = 1;
        sampler_desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        ID3D12Device_CreateSampler(context.device, &sampler_desc,
                ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heaps[1]));
        static_desc.Filter = cases[c].filter;
        static_desc.AddressU = static_desc.AddressV = static_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        static_desc.MaxLOD = 1.0f;
        static_desc.MaxAnisotropy = 1;
        static_desc.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
        static_desc.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        root_desc.NumParameters = stat ? 1 : 2;
        root_desc.NumStaticSamplers = stat;
        root_desc.pStaticSamplers = stat ? &static_desc : NULL;
        hr = create_root_signature(context.device, &root_desc, &root);
        assert_that(hr == S_OK, "Create root failed, hr %#x.\n", (int)hr);
        /* The native runtime requires one shader binary model across stages. */
        pipeline = dxil ? create_pipeline_state_dxil(context.device, root, DXGI_FORMAT_R32_FLOAT,
                NULL, &filter_reduction_dxil, NULL) : create_pipeline_state(context.device, root,
                DXGI_FORMAT_R32_FLOAT, NULL, &filter_reduction_dxbc, NULL);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(context.list, root);
        ID3D12GraphicsCommandList_SetPipelineState(context.list, pipeline);
        ID3D12GraphicsCommandList_SetDescriptorHeaps(context.list, 2, heaps);
        ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(context.list, 0,
                ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heaps[0]));
        if (!stat)
            ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(context.list, 1,
                    ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(heaps[1]));
        ID3D12GraphicsCommandList_OMSetRenderTargets(context.list, 1, &context.rtv, false, NULL);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(context.list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_RSSetViewports(context.list, 1, &context.viewport);
        ID3D12GraphicsCommandList_RSSetScissorRects(context.list, 1, &context.scissor_rect);
        ID3D12GraphicsCommandList_DrawInstanced(context.list, 3, 1, 0, 0);
        transition_resource_state(context.list, context.render_target,
                D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        get_texture_readback_with_command_list(context.render_target, 0, &readback, context.queue, context.list);
        for (i = 0; i < 5; i++)
        {
            float value = get_readback_float(&readback, i, 0);
            ok(value == cases[c].expected[i], "Pixel %u: %g, expected %g.\n", i, value, cases[c].expected[i]);
            printf("FILTER_REDUCTION_READBACK,%u,%u,%u,%u,%g\n", dxil, stat, c, i, value);
        }
#ifdef _WIN32
        {
            extern unsigned int helios_native_debug_errors(void *device);
            ok(!helios_native_debug_errors(context.device), "Native debug layer reported an error.\n");
        }
#endif
        release_resource_readback(&readback);
        ID3D12PipelineState_Release(pipeline);
        ID3D12RootSignature_Release(root);
        reset_command_list(context.list, context.allocator);
        transition_resource_state(context.list, context.render_target,
                D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    vkd3d_test_set_context(NULL);
    ID3D12Resource_Release(texture);
    ID3D12DescriptorHeap_Release(heaps[0]);
    ID3D12DescriptorHeap_Release(heaps[1]);
    destroy_test_context(&context);
}

void test_filter_reduction_dxbc(void) { test_filter_reduction_internal(false); }
void test_filter_reduction_dxil(void) { test_filter_reduction_internal(true); }
