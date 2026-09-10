/*
 * Copyright 2016-2017 Józef Kucia for CodeWeavers
 * Copyright 2020-2021 Philip Rebohle for Valve Corporation
 * Copyright 2020-2021 Joshua Ashton for Valve Corporation
 * Copyright 2020-2021 Hans-Kristian Arntzen for Valve Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_API
#include "d3d12_crosstest.h"

void test_primitive_restart_list_topology_stream_output(void)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc;
    ID3D12Resource *counter_buffer, *so_buffer;
    ID3D12GraphicsCommandList *command_list;
    D3D12_STREAM_OUTPUT_BUFFER_VIEW sobv;
    struct test_context_desc desc;
    ID3D12Resource *index_buffer;
    struct resource_readback rb;
    struct test_context context;
    D3D12_INDEX_BUFFER_VIEW ibv;
    ID3D12CommandQueue *queue;
    const struct vec4 *data;
    ID3D12Device *device;
    uint32_t counter;
    unsigned int i;
    HRESULT hr;

    static const D3D12_SO_DECLARATION_ENTRY so_declaration[] =
    {
        {0, "SV_Position", 0, 0, 4, 0},
    };
    static const struct vec4 expected_output[] =
    {
        /* Strip */
        { 2000.0f, 2000.0f, 2000.0f, 2000.0f },
        { 3000.0f, 3000.0f, 3000.0f, 3000.0f },
        { 4000.0f, 4000.0f, 4000.0f, 4000.0f },

        /* List */
        { 0.0f, 0.0f, 0.0f, 0.0f },
        { 1.0f, 1.0f, 1.0f, 1.0f },
        { -1.0f, -1.0f, -1.0f, -1.0f },
        { 9.0f, 9.0f, 9.0f, 9.0f },
        { -1.0f, -1.0f, -1.0f, -1.0f },
        { -1.0f, -1.0f, -1.0f, -1.0f },
        { 2000.0f, 2000.0f, 2000.0f, 2000.0f },
        { 3000.0f, 3000.0f, 3000.0f, 3000.0f },
        { 4000.0f, 4000.0f, 4000.0f, 4000.0f },

        /* Strip */
        { 2000.0f, 2000.0f, 2000.0f, 2000.0f },
        { 3000.0f, 3000.0f, 3000.0f, 3000.0f },
        { 4000.0f, 4000.0f, 4000.0f, 4000.0f },
    };
    static const uint32_t index_data[] = { 0, 1, UINT32_MAX, 9, UINT32_MAX, UINT32_MAX, 2000, 3000, 4000 };
    static const UINT strides[] = { 16 };

    static const DWORD vs_code[] =
    {
#if 0
        float4 main(uint vid : SV_VertexID) : SV_Position
        {
            if (vid == ~0u)
                return float4(-1, -1, -1, -1);
            else
                return float4(vid, vid, vid, vid);
        }
#endif
        0x43425844, 0x59eaaf80, 0xf7ab5160, 0xf0ce6da4, 0x82ce289b, 0x00000001, 0x00000140, 0x00000003,
        0x0000002c, 0x00000060, 0x00000094, 0x4e475349, 0x0000002c, 0x00000001, 0x00000008, 0x00000020,
        0x00000000, 0x00000006, 0x00000001, 0x00000000, 0x00000101, 0x565f5653, 0x65747265, 0x00444978,
        0x4e47534f, 0x0000002c, 0x00000001, 0x00000008, 0x00000020, 0x00000000, 0x00000001, 0x00000003,
        0x00000000, 0x0000000f, 0x505f5653, 0x7469736f, 0x006e6f69, 0x58454853, 0x000000a4, 0x00010050,
        0x00000029, 0x0100086a, 0x04000060, 0x00101012, 0x00000000, 0x00000006, 0x04000067, 0x001020f2,
        0x00000000, 0x00000001, 0x02000068, 0x00000001, 0x07000020, 0x00100012, 0x00000000, 0x0010100a,
        0x00000000, 0x00004001, 0xffffffff, 0x0304001f, 0x0010000a, 0x00000000, 0x08000036, 0x001020f2,
        0x00000000, 0x00004002, 0xbf800000, 0xbf800000, 0xbf800000, 0xbf800000, 0x0100003e, 0x01000012,
        0x05000056, 0x001020f2, 0x00000000, 0x00101006, 0x00000000, 0x0100003e, 0x01000015, 0x0100003e,
    };

    static const D3D12_SHADER_BYTECODE vs = SHADER_BYTECODE(vs_code);

    memset(&desc, 0, sizeof(desc));
    desc.root_signature_flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;
    desc.no_pipeline = true;
    if (!init_test_context(&context, &desc))
        return;

    device = context.device;
    command_list = context.list;
    queue = context.queue;

    init_pipeline_state_desc(&pso_desc, context.root_signature, 0, &vs, NULL, NULL);
    pso_desc.StreamOutput.NumEntries = ARRAY_SIZE(so_declaration);
    pso_desc.StreamOutput.pSODeclaration = so_declaration;
    pso_desc.StreamOutput.pBufferStrides = strides;
    pso_desc.StreamOutput.NumStrides = ARRAY_SIZE(strides);
    pso_desc.StreamOutput.RasterizedStream = D3D12_SO_NO_RASTERIZED_STREAM;
    pso_desc.IBStripCutValue = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_0xFFFFFFFF;
    hr = ID3D12Device_CreateGraphicsPipelineState(device, &pso_desc,
            &IID_ID3D12PipelineState, (void **)&context.pipeline_state);
    ok(SUCCEEDED(hr), "Failed to create PSO, hr #%x.\n", (int)hr);

    counter_buffer = create_default_buffer(device, 32,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_STREAM_OUT);
    so_buffer = create_default_buffer(device, 4096,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_STREAM_OUT);
    index_buffer = create_upload_buffer(device, sizeof(index_data), index_data);
    sobv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(so_buffer);
    sobv.SizeInBytes = 4096;
    sobv.BufferFilledSizeLocation = ID3D12Resource_GetGPUVirtualAddress(counter_buffer);

    ibv.Format = DXGI_FORMAT_R32_UINT;
    ibv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(index_buffer);
    ibv.SizeInBytes = sizeof(index_data);

    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_SOSetTargets(command_list, 0, 1, &sobv);
    ID3D12GraphicsCommandList_IASetIndexBuffer(command_list, &ibv);

    /* Primitive restart state only applies to strip primitives. */
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D12GraphicsCommandList_DrawIndexedInstanced(command_list, ARRAY_SIZE(index_data), 1,
              0, 0, 0);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawIndexedInstanced(command_list, ARRAY_SIZE(index_data), 1,
                                                   0, 0, 0);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D12GraphicsCommandList_DrawIndexedInstanced(command_list, ARRAY_SIZE(index_data), 1,
                                                   0, 0, 0);

    transition_resource_state(command_list, counter_buffer,
              D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition_resource_state(command_list, so_buffer,
              D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);

    get_buffer_readback_with_command_list(counter_buffer, DXGI_FORMAT_R32_UINT, &rb, queue, command_list);
    counter = get_readback_uint(&rb, 0, 0, 0);

    ok(counter == sizeof(expected_output), "Got unexpected counter %u, expected %u.\n",
            counter, (unsigned int)sizeof(expected_output));

    release_resource_readback(&rb);
    reset_command_list(command_list, context.allocator);
    get_buffer_readback_with_command_list(so_buffer, DXGI_FORMAT_UNKNOWN, &rb, queue, command_list);
    for (i = 0; i < ARRAY_SIZE(expected_output); ++i)
    {
        const struct vec4 *expected = &expected_output[i];
        data = get_readback_vec4(&rb, i, 0);

        ok(compare_vec4(data, expected, 1),
                "Got {%.8e, %.8e, %.8e, %.8e}, expected {%.8e, %.8e, %.8e, %.8e}.\n",
                data->x, data->y, data->z, data->w, expected->x, expected->y, expected->z, expected->w);
    }
    release_resource_readback(&rb);

    ID3D12Resource_Release(index_buffer);
    ID3D12Resource_Release(counter_buffer);
    ID3D12Resource_Release(so_buffer);
    destroy_test_context(&context);
}

static HRESULT create_stream_output_pipeline_stream(ID3D12Device *device,
        const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc, bool physical, ID3D12PipelineState **pipeline)
{
    struct
    {
        union d3d12_root_signature_subobject root;
        union d3d12_shader_bytecode_subobject vs;
        union d3d12_stream_output_subobject so;
        union d3d12_primitive_topology_subobject topology;
        union d3d12_depth_stencil_subobject depth_stencil;
        union d3d12_rasterizer_subobject rasterizer;
        union d3d12_sample_desc_subobject sample;
        union d3d12_cached_pso_subobject cached;
    } stream =
    {
        {{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE, desc->pRootSignature}},
        {{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS, desc->VS}},
        {{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_STREAM_OUTPUT, desc->StreamOutput}},
        {{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY, desc->PrimitiveTopologyType}},
        {{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DEPTH_STENCIL, desc->DepthStencilState}},
        {{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_RASTERIZER, desc->RasterizerState}},
        {{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_SAMPLE_DESC, desc->SampleDesc}},
        {{D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CACHED_PSO, desc->CachedPSO}},
    };
    D3D12_PIPELINE_STATE_STREAM_DESC stream_desc = {sizeof(stream), &stream};
    ID3D12Device2 *device2;
    HRESULT hr;

    if (physical)
    {
#ifndef _WIN32
        HRESULT (*create_native_so)(ID3D12Device *, const D3D12_PIPELINE_STATE_STREAM_DESC *, ID3D12PipelineState **);
        Dl_info module_info;
        void *module;
        /* Use the same loaded engine that owns device. Linking another static
         * copy here would not validate the native bridge's actual factory.
         * The loader uses RTLD_LOCAL, so RTLD_DEFAULT cannot find this symbol. */
        if (!dladdr((void *)device->lpVtbl->CreateGraphicsPipelineState, &module_info) ||
                !(module = dlopen(module_info.dli_fname, RTLD_NOW | RTLD_NOLOAD)))
            return E_NOINTERFACE;
        create_native_so = dlsym(module, "helios_vkd3d_create_stream_output_pipeline");
        hr = create_native_so ? create_native_so(device, &stream_desc, pipeline) : E_NOINTERFACE;
        dlclose(module);
        return hr;
#else
        return E_NOTIMPL;
#endif
    }
    if (FAILED(hr = ID3D12Device_QueryInterface(device, &IID_ID3D12Device2, (void **)&device2)))
        return hr;
    hr = ID3D12Device2_CreatePipelineState(device2, &stream_desc, &IID_ID3D12PipelineState, (void **)pipeline);
    ID3D12Device2_Release(device2);
    return hr;
}

static void test_vertex_shader_stream_output(bool use_dxil, bool partial, bool user_output,
        bool physical, unsigned int semantic_collision)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc;
    ID3D12Resource *counter_buffer, *so_buffer;
    ID3D12GraphicsCommandList *command_list;
    D3D12_STREAM_OUTPUT_BUFFER_VIEW sobv;
    ID3D12Resource *upload_buffer;
    struct test_context_desc desc;
    struct resource_readback rb;
    struct test_context context;
    ID3D12CommandQueue *queue;
    unsigned int counter, i;
    const struct vec4 *data;
    ID3D12Device *device;
    HRESULT hr;

    static const D3D12_SO_DECLARATION_ENTRY so_declaration[] =
    {
        {0, "SV_Position", 0, 0, 4, 0},
    };
    static const D3D12_SO_DECLARATION_ENTRY partial_declaration[] =
    {
        {0, "SV_Position", 0, 1, 2, 0},
        {0, NULL, 0, 0, 1, 0},
        {0, "SV_Position", 0, 0, 1, 0},
    };
    static const D3D12_SO_DECLARATION_ENTRY user_declaration[] =
    {
        {0, "UV_TEXCOORD", 0, 1, 1, 0},
        {0, NULL, 0, 0, 1, 0},
        {0, "UV_TEXCOORD", 0, 0, 1, 0},
        {0, NULL, 0, 0, 1, 0},
    };
    static const D3D12_SO_DECLARATION_ENTRY physical_declaration[] =
    {
        {0, "__HELIOS_DDI_SO_REGISTER", 0, 1, 2, 0},
        {0, NULL, 0, 0, 1, 0},
        {0, "__HELIOS_DDI_SO_REGISTER", 0, 0, 1, 0},
    };
    static const D3D12_SO_DECLARATION_ENTRY collision_declaration[] =
    {
        {0, "__HELIOS_DDI_SO_REGISTER", 7, 1, 2, 0},
        {0, NULL, 0, 0, 1, 0},
        {0, "__HELIOS_DDI_SO_REGISTER", 7, 0, 1, 0},
    };
#include "shaders/sparse/headers/texture_feedback_vs.h"
#include "shaders/pso/headers/stream_output_marker_semantic.h"
    static const struct vec4 expected_output[] =
    {
        {-1.0f, 1.0f, 0.0f, 1.0f},
        { 3.0f, 1.0f, 0.0f, 1.0f},
        {-1.0f,-3.0f, 0.0f, 1.0f},
    };
    unsigned int strides[] = {16};

    memset(&desc, 0, sizeof(desc));
    desc.root_signature_flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;
    desc.no_pipeline = true;
    if (!init_test_context(&context, &desc))
        return;

    if (use_dxil && !context_supports_dxil(&context))
    {
        destroy_test_context(&context);
        return;
    }

    device = context.device;
    command_list = context.list;
    queue = context.queue;

    if (use_dxil)
        init_pipeline_state_desc_dxil(&pso_desc, context.root_signature, 0, NULL, NULL, NULL);
    else
        init_pipeline_state_desc(&pso_desc, context.root_signature, 0, NULL, NULL, NULL);
    if (user_output)
        pso_desc.VS = use_dxil ? texture_feedback_vs_dxil : texture_feedback_vs_dxbc;
    pso_desc.StreamOutput.NumEntries = partial ? ARRAY_SIZE(partial_declaration) : ARRAY_SIZE(so_declaration);
    pso_desc.StreamOutput.pSODeclaration = partial ? partial_declaration : so_declaration;
    if (user_output)
    {
        pso_desc.StreamOutput.NumEntries = ARRAY_SIZE(user_declaration);
        pso_desc.StreamOutput.pSODeclaration = user_declaration;
    }
    else if (physical)
        pso_desc.StreamOutput.pSODeclaration = physical_declaration;
    else if (semantic_collision)
    {
        pso_desc.VS = stream_output_marker_semantic_dxil;
        pso_desc.StreamOutput.pSODeclaration = collision_declaration;
    }
    pso_desc.StreamOutput.pBufferStrides = strides;
    pso_desc.StreamOutput.NumStrides = ARRAY_SIZE(strides);
    pso_desc.StreamOutput.RasterizedStream = D3D12_SO_NO_RASTERIZED_STREAM;
    if (physical || semantic_collision == 2)
        hr = create_stream_output_pipeline_stream(device, &pso_desc, physical, &context.pipeline_state);
    else
        hr = ID3D12Device_CreateGraphicsPipelineState(device, &pso_desc,
                &IID_ID3D12PipelineState, (void **)&context.pipeline_state);
    if (hr == E_NOTIMPL)
    {
        skip("Stream output is not supported.\n");
        destroy_test_context(&context);
        return;
    }
    ok(hr == S_OK, "Failed to create graphics pipeline state, hr %#x.\n", (int)hr);
    if (FAILED(hr))
    {
        destroy_test_context(&context);
        return;
    }
    if (semantic_collision)
    {
        ID3DBlob *cached;
        hr = ID3D12PipelineState_GetCachedBlob(context.pipeline_state, &cached);
        ok(hr == S_OK, "Failed to get cached SO pipeline, hr %#x.\n", (int)hr);
        if (FAILED(hr))
        {
            destroy_test_context(&context);
            return;
        }
        ID3D12PipelineState_Release(context.pipeline_state);
        context.pipeline_state = NULL;
        pso_desc.CachedPSO.pCachedBlob = ID3D10Blob_GetBufferPointer(cached);
        pso_desc.CachedPSO.CachedBlobSizeInBytes = ID3D10Blob_GetBufferSize(cached);
        if (semantic_collision == 2)
            hr = create_stream_output_pipeline_stream(device, &pso_desc, false, &context.pipeline_state);
        else
            hr = ID3D12Device_CreateGraphicsPipelineState(device, &pso_desc,
                    &IID_ID3D12PipelineState, (void **)&context.pipeline_state);
        ID3D10Blob_Release(cached);
        ok(hr == S_OK, "Failed to recreate cached SO pipeline, hr %#x.\n", (int)hr);
        if (FAILED(hr))
        {
            destroy_test_context(&context);
            return;
        }
    }

    counter = 0;
    upload_buffer = create_upload_buffer(device, sizeof(counter), &counter);

    counter_buffer = create_default_buffer(device, 32,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    so_buffer = create_default_buffer(device, 1024,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_STREAM_OUT);
    sobv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(so_buffer);
    sobv.SizeInBytes = 1024;
    sobv.BufferFilledSizeLocation = ID3D12Resource_GetGPUVirtualAddress(counter_buffer);

    ID3D12GraphicsCommandList_CopyBufferRegion(command_list, counter_buffer, 0,
            upload_buffer, 0, sizeof(counter));

    transition_resource_state(command_list, counter_buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);

    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_SOSetTargets(command_list, 0, 1, &sobv);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);
    /* A NULL array unbinds the requested slot count. The buffer has ample
     * capacity; another draw must not append to the former target. */
    ID3D12GraphicsCommandList_SOSetTargets(command_list, 0, 1, NULL);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 3, 0);


    transition_resource_state(command_list, counter_buffer,
            D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition_resource_state(command_list, so_buffer,
            D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);

    get_buffer_readback_with_command_list(counter_buffer, DXGI_FORMAT_R32_UINT, &rb, queue, command_list);
    counter = get_readback_uint(&rb, 0, 0, 0);
    ok(counter == 3 * sizeof(struct vec4), "Got unexpected counter %u.\n", counter);
    release_resource_readback(&rb);
    reset_command_list(command_list, context.allocator);
    get_buffer_readback_with_command_list(so_buffer, DXGI_FORMAT_UNKNOWN, &rb, queue, command_list);
    for (i = 0; i < ARRAY_SIZE(expected_output); ++i)
    {
        const struct vec4 *expected = &expected_output[i];
        data = get_readback_vec4(&rb, i, 0);
        if (user_output)
        {
            ok(compare_float(data->x, (1.0f - expected->y) / 2.0f, 1) &&
                    compare_float(data->z, (expected->x + 1.0f) / 2.0f, 1),
                    "User partial capture %u: got {%g, gap, %g, gap}.\n", i, data->x, data->z);
        }
        else
        if (partial)
        {
            ok(compare_float(data->x, expected->y, 1) && compare_float(data->y, expected->z, 1)
                    && compare_float(data->w, expected->x, 1),
                    "Partial capture %u: got {%g, %g, gap, %g}, expected {%g, %g, gap, %g}.\n",
                    i, data->x, data->y, data->w, expected->y, expected->z, expected->x);
        }
        else
            ok(compare_vec4(data, expected, 1),
                    "Got {%.8e, %.8e, %.8e, %.8e}, expected {%.8e, %.8e, %.8e, %.8e}.\n",
                    data->x, data->y, data->z, data->w, expected->x, expected->y, expected->z, expected->w);
    }
    release_resource_readback(&rb);

    ID3D12Resource_Release(counter_buffer);
    ID3D12Resource_Release(upload_buffer);
    ID3D12Resource_Release(so_buffer);
    destroy_test_context(&context);
}

void test_index_buffer_edge_case_stream_output(void)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc;
    ID3D12Resource *counter_buffer, *so_buffer;
    ID3D12GraphicsCommandList *command_list;
    D3D12_STREAM_OUTPUT_BUFFER_VIEW sobv;
    ID3D12Resource *upload_buffer;
    struct test_context_desc desc;
    ID3D12Resource *index_buffer;
    struct resource_readback rb;
    D3D12_INDEX_BUFFER_VIEW ibv;
    struct test_context context;
    ID3D12CommandQueue *queue;
    unsigned int counter, i;
    const struct vec4 *data;
    ID3D12Device *device;
    HRESULT hr;

    static const D3D12_SO_DECLARATION_ENTRY so_declaration[] =
    {
        {0, "SV_Position", 0, 0, 4, 0},
    };
    static const struct vec4 expected_output[] =
    {
        {-1.0f, 1.0f, 0.0f, 1.0f},
        { 3.0f, 1.0f, 0.0f, 1.0f},
        {-1.0f,-3.0f, 0.0f, 1.0f},
        {-1.0f, 1.0f, 0.0f, 1.0f}, /* For the case where we are rendering with NULL index. The first vertex is always picked. */
        {-1.0f, 1.0f, 0.0f, 1.0f},
        {-1.0f, 1.0f, 0.0f, 1.0f},
        {-1.0f, 1.0f, 0.0f, 1.0f}, /* For DrawInstanced with NULL index buffer, which should work just fine. */
        { 3.0f, 1.0f, 0.0f, 1.0f},
        {-1.0f,-3.0f, 0.0f, 1.0f},
        {-1.0f, 1.0f, 0.0f, 1.0f}, /* For the case where we are rendering with NULL GPU VA for index. */
        {-1.0f, 1.0f, 0.0f, 1.0f},
        {-1.0f, 1.0f, 0.0f, 1.0f},
        {-1.0f,-3.0f, 0.0f, 1.0f}, /* For the case where we are rendering with UNKNOWN index format. It is actually R16_UINT. */
        { 3.0f, 1.0f, 0.0f, 1.0f},
        {-1.0f, 1.0f, 0.0f, 1.0f},
    };
    unsigned int strides[] = {16};
    static const uint16_t index_data[3] = { 2, 1, 0 };

    memset(&desc, 0, sizeof(desc));
    desc.root_signature_flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT;
    desc.no_pipeline = true;
    if (!init_test_context(&context, &desc))
        return;

    device = context.device;
    command_list = context.list;
    queue = context.queue;

    index_buffer = create_upload_buffer(device, sizeof(index_data), index_data);

    init_pipeline_state_desc(&pso_desc, context.root_signature, 0, NULL, NULL, NULL);
    pso_desc.StreamOutput.NumEntries = ARRAY_SIZE(so_declaration);
    pso_desc.StreamOutput.pSODeclaration = so_declaration;
    pso_desc.StreamOutput.pBufferStrides = strides;
    pso_desc.StreamOutput.NumStrides = ARRAY_SIZE(strides);
    pso_desc.StreamOutput.RasterizedStream = D3D12_SO_NO_RASTERIZED_STREAM;
    hr = ID3D12Device_CreateGraphicsPipelineState(device, &pso_desc,
            &IID_ID3D12PipelineState, (void **)&context.pipeline_state);
    if (hr == E_NOTIMPL)
    {
        skip("Stream output is not supported.\n");
        destroy_test_context(&context);
        return;
    }
    ok(hr == S_OK, "Failed to create graphics pipeline state, hr %#x.\n", (int)hr);

    counter = 0;
    upload_buffer = create_upload_buffer(device, sizeof(counter), &counter);

    counter_buffer = create_default_buffer(device, 32,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    so_buffer = create_default_buffer(device, 1024,
            D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_STREAM_OUT);
    sobv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(so_buffer);
    sobv.SizeInBytes = 1024;
    sobv.BufferFilledSizeLocation = ID3D12Resource_GetGPUVirtualAddress(counter_buffer);

    ID3D12GraphicsCommandList_CopyBufferRegion(command_list, counter_buffer, 0,
            upload_buffer, 0, sizeof(counter));

    transition_resource_state(command_list, counter_buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);

    ID3D12GraphicsCommandList_RSSetScissorRects(command_list, 1, &context.scissor_rect);
    ID3D12GraphicsCommandList_RSSetViewports(command_list, 1, &context.viewport);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(command_list, context.root_signature);
    ID3D12GraphicsCommandList_SetPipelineState(command_list, context.pipeline_state);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(command_list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_SOSetTargets(command_list, 0, 1, &sobv);
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    /* Should render all 0 indices. */
    ID3D12GraphicsCommandList_IASetIndexBuffer(command_list, NULL);
    ID3D12GraphicsCommandList_DrawIndexedInstanced(command_list, 3, 1, 1, 1, 0);

    /* Should still render. */
    ID3D12GraphicsCommandList_DrawInstanced(command_list, 3, 1, 0, 0);

    ibv.BufferLocation = 0;
    ibv.Format = DXGI_FORMAT_R32_UINT;
    ibv.SizeInBytes = 0;

    /* Should render all 0 indices. */
    ID3D12GraphicsCommandList_IASetIndexBuffer(command_list, &ibv);
    ID3D12GraphicsCommandList_DrawIndexedInstanced(command_list, 3, 1, 2, 1, 0);

    /* This is supposed to be illegal, but works anyways. UNKNOWN is R16_UINT on AMD at least. */
    ibv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(index_buffer);
    ibv.Format = DXGI_FORMAT_UNKNOWN;
    ibv.SizeInBytes = sizeof(index_data);
    ID3D12GraphicsCommandList_IASetIndexBuffer(command_list, &ibv);
    ID3D12GraphicsCommandList_DrawIndexedInstanced(command_list, 3, 1, 0, 0, 0);

    transition_resource_state(command_list, counter_buffer,
            D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition_resource_state(command_list, so_buffer,
            D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);

    get_buffer_readback_with_command_list(counter_buffer, DXGI_FORMAT_R32_UINT, &rb, queue, command_list);
    counter = get_readback_uint(&rb, 0, 0, 0);
    ok(counter == 15 * sizeof(struct vec4), "Got unexpected counter %u.\n", counter);
    release_resource_readback(&rb);
    reset_command_list(command_list, context.allocator);
    get_buffer_readback_with_command_list(so_buffer, DXGI_FORMAT_UNKNOWN, &rb, queue, command_list);
    for (i = 0; i < ARRAY_SIZE(expected_output); ++i)
    {
        const struct vec4 *expected = &expected_output[i];
        data = get_readback_vec4(&rb, i, 0);
        ok(compare_vec4(data, expected, 1),
                "Got {%.8e, %.8e, %.8e, %.8e}, expected {%.8e, %.8e, %.8e, %.8e}.\n",
                data->x, data->y, data->z, data->w, expected->x, expected->y, expected->z, expected->w);
    }
    release_resource_readback(&rb);

    ID3D12Resource_Release(counter_buffer);
    ID3D12Resource_Release(upload_buffer);
    ID3D12Resource_Release(so_buffer);
    ID3D12Resource_Release(index_buffer);
    destroy_test_context(&context);
}

void test_vertex_shader_stream_output_dxbc(void)
{
    test_vertex_shader_stream_output(false, false, false, false, 0);
}

void test_vertex_shader_stream_output_dxil(void)
{
    test_vertex_shader_stream_output(true, false, false, false, 0);
}

void test_vertex_shader_stream_output_partial_dxil(void)
{
    test_vertex_shader_stream_output(true, true, false, false, 0);
}

void test_vertex_shader_stream_output_partial_dxbc(void)
{
    test_vertex_shader_stream_output(false, true, false, false, 0);
}

void test_vertex_shader_stream_output_partial_user_dxil(void)
{
    test_vertex_shader_stream_output(true, true, true, false, 0);
}

void test_vertex_shader_stream_output_partial_user_dxbc(void)
{
    test_vertex_shader_stream_output(false, true, true, false, 0);
}

void test_vertex_shader_stream_output_partial_physical_dxil(void)
{
#ifdef _WIN32
    skip("Private native-DDI SO factory is tested in the Linux engine harness; native Windows uses the Helios SO probe.\n");
#else
    test_vertex_shader_stream_output(true, true, false, true, 0);
#endif
}

void test_vertex_shader_stream_output_marker_semantic_dxil(void)
{
    test_vertex_shader_stream_output(true, true, false, false, 1);
}

void test_vertex_shader_stream_output_marker_semantic_stream_dxil(void)
{
    test_vertex_shader_stream_output(true, true, false, false, 2);
}

static void test_null_stream_output_targets(bool use_dxil)
{
    const unsigned int counter_heap_size = 4 * 1024 * 1024;
    const unsigned int counter_base = counter_heap_size - 16;
    static const unsigned int tested_strides[] = {4, 8, 16, 32};
    static const struct vec4 positions[] =
    {
        {-1.0f, 1.0f, 0.0f, 1.0f},
        { 3.0f, 1.0f, 0.0f, 1.0f},
        {-1.0f,-3.0f, 0.0f, 1.0f},
    };
    static const D3D12_INPUT_ELEMENT_DESC input =
        {"POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
#include "shaders/sparse/headers/texture_feedback_vs.h"
#include "shaders/pso/headers/vs_mismatch.h"
#include "shaders/pso/headers/stream_output_two_streams.h"
    D3D12_SO_DECLARATION_ENTRY entries[2];
    D3D12_STREAM_OUTPUT_BUFFER_VIEW views[2], limited;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc;
    D3D12_QUERY_HEAP_DESC query_desc;
    D3D12_HEAP_DESC heap_desc;
    D3D12_VERTEX_BUFFER_VIEW vbv;
    ID3D12Resource *buffers[2], *counters, *upload, *observations, *vertices;
    ID3D12GraphicsCommandList *list;
    ID3D12PipelineState *pso;
    ID3D12Heap *counter_heap;
    ID3D12QueryHeap *queries;
    struct test_context_desc desc;
    struct test_context context;
    struct resource_readback rb;
    unsigned int stride_index, mode, multi, slot, word, vertex, component, sequence;
    unsigned int initial[2], blocked[2], resumed[2], total[2], strides[2];
    uint32_t initial_data[256];
    uint64_t value, expected;
    const float *floats;
    float expected_float;
    HRESULT hr;

    memset(&desc, 0, sizeof(desc));
    desc.no_pipeline = true;
    desc.root_signature_flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_STREAM_OUTPUT |
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    if (!init_test_context(&context, &desc))
        return;
    if (use_dxil && !context_supports_dxil(&context))
    {
        destroy_test_context(&context);
        return;
    }

    list = context.list;
    for (multi = 0; multi <= (unsigned int)use_dxil; ++multi)
    for (stride_index = 0; stride_index < ARRAY_SIZE(tested_strides); ++stride_index)
    for (mode = 0; mode < (multi ? 4u : 5u); ++mode)
    {
        /* Declared but unbound slots are full buffers (D3D11.3 14.6), so they
         * block every buffer in that stream, but never an independent stream.
         * Small strides expose finite dummy-buffer capacity; stride 32 is the
         * control. Modes exercise initial NULL, explicit NULL, zero size with
         * ignored invalid addresses, and an actual buffer with one slot left.
         * The last mode omits slot 0 from the declaration altogether: its data
         * and counter must remain untouched whether bound or unbound. */
        vkd3d_test_set_context("%s, stride %u, mode %u", multi ? "two streams" : "one stream",
                tested_strides[stride_index], mode);
        strides[0] = tested_strides[stride_index];
        strides[1] = multi ? 8 : 16;
        memset(entries, 0, sizeof(entries));
        entries[0].SemanticName = multi ? "ARG" : "UV_TEXCOORD";
        entries[0].ComponentCount = 1;
        entries[1].Stream = multi;
        entries[1].SemanticName = multi ? "ARG" : "SV_Position";
        entries[1].SemanticIndex = multi;
        entries[1].ComponentCount = multi ? 2 : 4;
        entries[1].OutputSlot = 1;
        init_pipeline_state_desc(&pso_desc, context.root_signature, DXGI_FORMAT_UNKNOWN,
                multi ? &vs_mismatch_dxil : use_dxil ? &texture_feedback_vs_dxil : &texture_feedback_vs_dxbc,
                NULL, NULL);
        memset(&pso_desc.PS, 0, sizeof(pso_desc.PS));
        pso_desc.NumRenderTargets = 0;
        pso_desc.PrimitiveTopologyType = multi ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE :
                D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        if (multi)
        {
            pso_desc.GS = stream_output_two_streams_dxil;
            pso_desc.InputLayout.NumElements = 1;
            pso_desc.InputLayout.pInputElementDescs = &input;
        }
        pso_desc.StreamOutput.pSODeclaration = entries + (mode == 4);
        pso_desc.StreamOutput.NumEntries = ARRAY_SIZE(entries) - (mode == 4);
        pso_desc.StreamOutput.pBufferStrides = strides;
        pso_desc.StreamOutput.NumStrides = ARRAY_SIZE(strides);
        pso_desc.StreamOutput.RasterizedStream = D3D12_SO_NO_RASTERIZED_STREAM;
        hr = ID3D12Device_CreateGraphicsPipelineState(context.device, &pso_desc,
                &IID_ID3D12PipelineState, (void **)&pso);
        ok(hr == S_OK, "Failed to create stream-output PSO, hr %#x.\n", (int)hr);
        if (FAILED(hr))
            continue;

        memset(initial_data, 0xcd, sizeof(initial_data));
        initial_data[1] = initial_data[3] = 0;
        if (mode == 4)
            initial_data[1] = 124;
        upload = create_upload_buffer(context.device, sizeof(initial_data), initial_data);
        vertices = create_upload_buffer(context.device, sizeof(positions), positions);
        /* BufferFilledSize is 32 bits. The last counter occupies exactly the
         * heap's last four bytes, so allocation padding cannot conceal an
         * incorrect eight-byte range requirement. Each counter has a preceding
         * sentinel, including one immediately after the first counter. */
        memset(&heap_desc, 0, sizeof(heap_desc));
        heap_desc.SizeInBytes = counter_heap_size;
        heap_desc.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        heap_desc.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
        hr = ID3D12Device_CreateHeap(context.device, &heap_desc, &IID_ID3D12Heap, (void **)&counter_heap);
        ok(hr == S_OK, "Failed to create counter heap, hr %#x.\n", (int)hr);
        counters = create_placed_buffer(context.device, counter_heap, 0, counter_heap_size,
                D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
        observations = create_default_buffer(context.device, 640, D3D12_RESOURCE_FLAG_NONE,
                D3D12_RESOURCE_STATE_COPY_DEST);
        memset(&query_desc, 0, sizeof(query_desc));
        query_desc.Type = D3D12_QUERY_HEAP_TYPE_SO_STATISTICS;
        query_desc.Count = 4;
        hr = ID3D12Device_CreateQueryHeap(context.device, &query_desc, &IID_ID3D12QueryHeap, (void **)&queries);
        ok(hr == S_OK, "Failed to create query heap, hr %#x.\n", (int)hr);
        for (slot = 0; slot < 2; ++slot)
        {
            buffers[slot] = create_default_buffer(context.device, 256, D3D12_RESOURCE_FLAG_NONE,
                    D3D12_RESOURCE_STATE_COPY_DEST);
            ID3D12GraphicsCommandList_CopyBufferRegion(list, buffers[slot], 0, upload, 16, 256);
            transition_resource_state(list, buffers[slot], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
            views[slot].BufferLocation = ID3D12Resource_GetGPUVirtualAddress(buffers[slot]);
            views[slot].SizeInBytes = 256;
            views[slot].BufferFilledSizeLocation = ID3D12Resource_GetGPUVirtualAddress(counters) + counter_base + slot * 8 + 4;
            initial[slot] = mode ? (slot && multi ? 1 : 3) : 0;
            blocked[slot] = slot && multi ? 1 : mode == 3 ? 1 : 0;
            resumed[slot] = slot && multi ? 1 : 3;
            if (mode == 4)
            {
                initial[slot] = resumed[slot] = slot ? 3 : 0;
                blocked[slot] = slot ? 3 : 0;
            }
            total[slot] = initial[slot] + blocked[slot] + resumed[slot];
        }
        ID3D12GraphicsCommandList_CopyBufferRegion(list, counters, counter_base, upload, 0, 16);
        transition_resource_state(list, counters, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_STREAM_OUT);
        ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &context.scissor_rect);
        ID3D12GraphicsCommandList_RSSetViewports(list, 1, &context.viewport);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, context.root_signature);
        ID3D12GraphicsCommandList_SetPipelineState(list, pso);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(list,
                multi ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST : D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
        if (multi)
        {
            vbv.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(vertices);
            vbv.SizeInBytes = sizeof(positions);
            vbv.StrideInBytes = sizeof(positions[0]);
            ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vbv);
        }
        if (mode)
        {
            ID3D12GraphicsCommandList_SOSetTargets(list, 0, 2, views);
            ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
        }
        else
            ID3D12GraphicsCommandList_SOSetTargets(list, 1, 1, &views[1]);

        if (mode == 1 || mode == 4)
            ID3D12GraphicsCommandList_SOSetTargets(list, 0, 1, NULL);
        else if (mode == 2)
        {
            limited.BufferLocation = 1;
            limited.SizeInBytes = 0;
            limited.BufferFilledSizeLocation = UINT64_MAX;
            ID3D12GraphicsCommandList_SOSetTargets(list, 0, 1, &limited);
        }
        else if (mode == 3)
        {
            limited = views[0];
            limited.SizeInBytes = (initial[0] + 1) * strides[0];
            ID3D12GraphicsCommandList_SOSetTargets(list, 0, 1, &limited);
        }
        for (slot = 0; slot <= multi; ++slot)
            ID3D12GraphicsCommandList_BeginQuery(list, queries, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 + slot, slot);
        ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
        for (slot = 0; slot <= multi; ++slot)
        {
            ID3D12GraphicsCommandList_EndQuery(list, queries, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 + slot, slot);
            ID3D12GraphicsCommandList_ResolveQueryData(list, queries, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 + slot,
                    slot, 1, observations, 544 + slot * 16);
        }
        transition_resource_state(list, counters, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ID3D12GraphicsCommandList_CopyBufferRegion(list, observations, 528, counters, counter_base, 16);
        transition_resource_state(list, counters, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_STREAM_OUT);

        /* The other logical target remains bound. Rebind only slot 0 and verify
         * that the saved counters survive query/copy render-pass boundaries. */
        ID3D12GraphicsCommandList_SOSetTargets(list, 0, 1, &views[0]);
        for (slot = 0; slot <= multi; ++slot)
            ID3D12GraphicsCommandList_BeginQuery(list, queries, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 + slot, 2 + slot);
        ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
        for (slot = 0; slot <= multi; ++slot)
        {
            ID3D12GraphicsCommandList_EndQuery(list, queries, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 + slot, 2 + slot);
            ID3D12GraphicsCommandList_ResolveQueryData(list, queries, D3D12_QUERY_TYPE_SO_STATISTICS_STREAM0 + slot,
                    2 + slot, 1, observations, 576 + slot * 16);
        }
        ID3D12GraphicsCommandList_SOSetTargets(list, 0, 2, NULL);
        transition_resource_state(list, counters, D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ID3D12GraphicsCommandList_CopyBufferRegion(list, observations, 512, counters, counter_base, 16);
        for (slot = 0; slot < 2; ++slot)
        {
            transition_resource_state(list, buffers[slot], D3D12_RESOURCE_STATE_STREAM_OUT, D3D12_RESOURCE_STATE_COPY_SOURCE);
            ID3D12GraphicsCommandList_CopyBufferRegion(list, observations, slot * 256, buffers[slot], 0, 256);
        }
        transition_resource_state(list, observations, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        get_buffer_readback_with_command_list(observations, DXGI_FORMAT_UNKNOWN, &rb, context.queue, list);
        for (slot = 0; slot < 2; ++slot)
        {
            value = get_readback_uint(&rb, 129 + slot * 2, 0, 0);
            expected = mode == 4 && !slot ? 124 : total[slot] * strides[slot];
            ok(value == expected, "Slot %u final counter %"PRIu64", expected %"PRIu64".\n", slot, value, expected);
            value = get_readback_uint(&rb, 133 + slot * 2, 0, 0);
            expected = mode == 4 && !slot ? 124 : (initial[slot] + blocked[slot]) * strides[slot];
            ok(value == expected, "Slot %u limited counter %"PRIu64", expected %"PRIu64".\n", slot, value, expected);
            ok(get_readback_uint(&rb, 128 + slot * 2, 0, 0) == 0xcdcdcdcd &&
                    get_readback_uint(&rb, 132 + slot * 2, 0, 0) == 0xcdcdcdcd,
                    "Slot %u counter guard was overwritten.\n", slot);
            for (word = 0; word < 64; ++word)
            {
                vertex = word / (strides[slot] / 4);
                component = word % (strides[slot] / 4);
                if (vertex >= total[slot] || component >= (slot ? multi ? 2 : 4 : 1))
                {
                    ok(get_readback_uint(&rb, slot * 64 + word, 0, 0) == 0xcdcdcdcd,
                            "Slot %u padding/tail word %u overwritten.\n", slot, word);
                    continue;
                }
                sequence = vertex < initial[slot] ? vertex : vertex < initial[slot] + blocked[slot] ?
                        vertex - initial[slot] : vertex - initial[slot] - blocked[slot];
                floats = (const float *)&positions[sequence];
                expected_float = multi ? slot ? 4.0f + component : 1.0f :
                        slot ? floats[component] : sequence == 1 ? 2.0f : 0.0f;
                ok(get_readback_float(&rb, slot * 64 + word, 0) == expected_float,
                        "Slot %u data word %u got %g, expected %g.\n", slot, word,
                        get_readback_float(&rb, slot * 64 + word, 0), expected_float);
            }
        }
        for (slot = 0; slot <= multi; ++slot)
        {
            unsigned int written = mode == 4 ? blocked[1] : blocked[slot];
            unsigned int needed = mode == 4 ? resumed[1] : resumed[slot];
            value = get_readback_uint64(&rb, 68 + slot * 2, 0);
            ok(value == written, "Stream %u limited written %"PRIu64", expected %u.\n", slot, value, written);
            value = get_readback_uint64(&rb, 69 + slot * 2, 0);
            ok(value == needed, "Stream %u limited needed %"PRIu64", expected %u.\n", slot, value, needed);
            value = get_readback_uint64(&rb, 72 + slot * 2, 0);
            ok(value == needed, "Stream %u resumed written %"PRIu64", expected %u.\n", slot, value, needed);
            value = get_readback_uint64(&rb, 73 + slot * 2, 0);
            ok(value == needed, "Stream %u resumed needed %"PRIu64", expected %u.\n", slot, value, needed);
        }
        release_resource_readback(&rb);
        reset_command_list(list, context.allocator);
        ID3D12QueryHeap_Release(queries);
        ID3D12Resource_Release(observations);
        ID3D12Resource_Release(counters);
        ID3D12Heap_Release(counter_heap);
        ID3D12Resource_Release(buffers[0]);
        ID3D12Resource_Release(buffers[1]);
        ID3D12Resource_Release(upload);
        ID3D12Resource_Release(vertices);
        ID3D12PipelineState_Release(pso);
    }
    vkd3d_test_set_context(NULL);
    destroy_test_context(&context);
}

void test_null_stream_output_targets_dxbc(void)
{
    test_null_stream_output_targets(false);
}

void test_null_stream_output_targets_dxil(void)
{
    test_null_stream_output_targets(true);
}
