/* Native Windows entry point for the inherited feature-level conformance tests.
 * Uses the system D3D12 runtime and the exact Helios adapter. No experimental
 * shader-model enablement, app-local runtime or engine capability override.
 * SHA256 attribution is enforced before any test records GPU work. */
#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_API
#define INITGUID
#define VKD3D_TEST_DECLARE_MAIN
#include "d3d12_crosstest.h"
#include <tlhelp32.h>
#include <wincrypt.h>

void test_rasterizer_ordered_views_dxbc(void);
void test_rasterizer_ordered_views_dxil(void);
void test_conservative_rasterization_dxbc(void);
void test_conservative_rasterization_dxil(void);
void test_tir_outputs_dxbc(void);
void test_tir_outputs_dxil(void);
void test_tir_invalid_dxbc(void);
void test_tir_invalid_dxil(void);
void test_tir_one_sample_dxbc(void);
void test_tir_one_sample_dxil(void);
void test_tir_mixed_samples_dxbc(void);
void test_tir_mixed_samples_dxil(void);

void test_query_pipeline_statistics_continuation(void);
void test_query_pipeline_statistics_continuation_ia(void);
void test_query_dgc_compute_dxbc(void);
void test_query_dgc_compute_dxil(void);

void test_format_support(void);
void test_multisample_quality_levels(void);
void test_typed_buffer_uav(void);
void test_typed_uav_store(void);
void test_uav_load(void);
void test_cs_uav_store(void);
void test_uav_counters(void);
void test_decrement_uav_counter(void);
void test_atomic_instructions_dxbc(void);
void test_atomic_instructions_dxil(void);
void test_sample_instructions(void);
void test_sample_c_lz(void);
void test_multisample_array_texture(void);
void test_geometry_shader_dxbc(void);
void test_geometry_shader_dxil(void);
void test_quad_tessellation_dxbc(void);
void test_quad_tessellation_dxil(void);
void test_line_tessellation_dxbc(void);
void test_line_tessellation_dxil(void);
void test_typed_buffers_many_objects_dxbc(void);
void test_typed_buffers_many_objects_dxil(void);
void test_sampler_border_color(void);
void test_filter_reduction_dxbc(void);
void test_filter_reduction_dxil(void);
void test_typed_srv_uav_cast(void);

static IDXGIAdapter1 *native_adapter;
static PFN_D3D12_CREATE_DEVICE native_create_device;

static void native_require(bool condition, const char *reason)
{
    if (!condition)
    {
        fprintf(stderr, "FAIL,native-features,%s\n", reason);
        fflush(stdout);
        fflush(stderr);
        /* No owner unwind can infer GPU retirement from a failed precondition. */
        ExitProcess(1);
    }
}

static bool module_hash_matches(const WCHAR *path, const char *expected)
{
    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    BYTE buffer[16384], digest[32];
    DWORD count, size = sizeof(digest);
    char actual[65];
    HANDLE file;
    bool valid = false;
    unsigned int i;

    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    if (!CryptAcquireContextW(&provider, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) ||
            !CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash))
        goto done;
    for (;;)
    {
        if (!ReadFile(file, buffer, sizeof(buffer), &count, NULL))
            goto done;
        if (!count)
            break;
        if (!CryptHashData(hash, buffer, count, 0))
            goto done;
    }
    if (!CryptGetHashParam(hash, HP_HASHVAL, digest, &size, 0) || size != sizeof(digest))
        goto done;
    for (i = 0; i < sizeof(digest); i++)
        sprintf(actual + 2 * i, "%02x", digest[i]);
    fprintf(stderr, "UMD12_SHA256,%s\n", actual);
    valid = !_stricmp(expected, actual);
done:
    if (hash)
        CryptDestroyHash(hash);
    if (provider)
        CryptReleaseContext(provider, 0);
    CloseHandle(file);
    return valid;
}

static void verify_native_modules(void)
{
    WCHAR system[MAX_PATH], expected_path[MAX_PATH];
    bool runtime = false, core = false, dxgi = false, icd = false, valid = true;
    const char *expected = getenv("HELIOS_NATIVE_EXPECTED_UMD12_SHA256");
    MODULEENTRY32W module = {0};
    unsigned int umds = 0;
    HANDLE snapshot;
    BOOL next;

    native_require(expected && strlen(expected) == 64, "expected UMD12 SHA256 is required");
    native_require(GetSystemDirectoryW(system, ARRAY_SIZE(system)) != 0, "system directory unavailable");
    snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    native_require(snapshot != INVALID_HANDLE_VALUE, "module snapshot failed");
    module.dwSize = sizeof(module);
    for (next = Module32FirstW(snapshot, &module); next; next = Module32NextW(snapshot, &module))
    {
        bool *system_module = !_wcsicmp(module.szModule, L"d3d12.dll") ? &runtime :
                !_wcsicmp(module.szModule, L"D3D12Core.dll") ? &core :
                !_wcsicmp(module.szModule, L"dxgi.dll") ? &dxgi : NULL;
        bool umd = !_wcsnicmp(module.szModule, L"helios_umd12", 12);
        bool venus = !_wcsnicmp(module.szModule, L"vulkan_virtio", 13);
        if (system_module || umd || venus || !_wcsicmp(module.szModule, L"d3d12SDKLayers.dll"))
            fprintf(stderr, "MODULE,%ls,%ls\n", module.szModule, module.szExePath);
        if (system_module)
        {
            *system_module = true;
            _snwprintf(expected_path, ARRAY_SIZE(expected_path), L"%ls\\%ls", system, module.szModule);
            expected_path[ARRAY_SIZE(expected_path) - 1] = 0;
            valid &= !_wcsicmp(expected_path, module.szExePath);
        }
        if (umd)
        {
            umds++;
            valid &= module_hash_matches(module.szExePath, expected);
        }
        icd |= venus;
        valid &= _wcsicmp(module.szModule, L"helios_vkd3d.dll") && _wcsicmp(module.szModule, L"d3d10warp.dll");
    }
    CloseHandle(snapshot);
    native_require(valid && runtime && core && dxgi && icd && umds == 1, "native runtime/Helios UMD/ICD identity mismatch");
    fflush(stderr);
}

static HRESULT WINAPI create_native_device(IUnknown *adapter, D3D_FEATURE_LEVEL level, REFIID iid, void **out)
{
    HRESULT hr;

    native_require(!adapter, "selected tests must use the pinned Helios adapter");
    hr = native_create_device((IUnknown *)native_adapter, level, iid, out);
    native_require(SUCCEEDED(hr), "native device creation failed");
    verify_native_modules();
    return hr;
}

static void test_helios_tir_attachment_creation(void)
{
    /* D3D11.3 functional spec 3.5.6: inherited by D3D12 FL11_1+.
     * Forced >1 permits single-sample RTVs; forced 1 permits MSAA RTVs.
     * These are creation checks, not coverage/blending/occlusion validation. */
    static const unsigned int forced_counts[] = {0, 1, 4, 8, 16, 1};
    static const unsigned int output_counts[] = {1, 1, 1, 1, 1, 4};
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(forced_counts); i++)
    {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc;
        D3D12_ROOT_SIGNATURE_DESC root_desc = {0};
        ID3D12PipelineState *pso = NULL;
        ID3D12RootSignature *root;
        ID3D12Device *device;
        HRESULT hr;

        device = create_device();
        native_require(device != NULL, "TIR device creation failed");
        hr = create_root_signature(device, &root_desc, &root);
        native_require(SUCCEEDED(hr), "TIR empty root signature failed");
        init_pipeline_state_desc(&pso_desc, root, DXGI_FORMAT_R8G8B8A8_UNORM, NULL, NULL, NULL);
        pso_desc.RasterizerState.ForcedSampleCount = forced_counts[i];
        pso_desc.SampleDesc.Count = output_counts[i];
        hr = ID3D12Device_CreateGraphicsPipelineState(device, &pso_desc, &IID_ID3D12PipelineState, (void **)&pso);
        printf("TIR_CREATION,forced=%u,attachment_samples=%u,hr=%08x\n", forced_counts[i], output_counts[i], (unsigned int)hr);
        ok(hr == S_OK, "Legal TIR PSO forced %u, output samples %u failed, hr %#x.\n",
                forced_counts[i], output_counts[i], (unsigned int)hr);
        if (pso)
            ID3D12PipelineState_Release(pso);
        ID3D12RootSignature_Release(root);
        ID3D12Device_Release(device);
    }
}

START_TEST(helios_native_features)
{
    static const char *forbidden[] = {"VKD3D_FEATURE_LEVEL", "VKD3D_SHADER_MODEL", "VKD3D_SHADER_OVERRIDE",
            "D3D12SDKPath", "D3D12SDKVersion", "VKD3D_TEST_EXCLUDE", "VKD3D_TEST_PLATFORM", "VKD3D_TEST_BUG"};
    IDXGIFactory4 *factory;
    ID3D12Debug *debug;
    DWORD session;
    unsigned int i;

    (void)argv;
    native_require(argc == 1, "select a test using VKD3D_TEST_MATCH; command-line overrides are forbidden");
    native_require(ProcessIdToSessionId(GetCurrentProcessId(), &session) && session, "interactive scheduled task required");
    fprintf(stderr, "PROCESS,%lu,session,%lu\n", GetCurrentProcessId(), session);
    for (i = 0; i < ARRAY_SIZE(forbidden); i++)
        native_require(!getenv(forbidden[i]), forbidden[i]);
    native_require(getenv("HELIOS_WSI_ASYNC_PRESENT") && !strcmp(getenv("HELIOS_WSI_ASYNC_PRESENT"), "1"), "async present must stay enabled");
    puts("Native fence completion: authenticated wire retirement; feedback-shadow workaround removed.");

    native_create_device = get_d3d12_pfn(D3D12CreateDevice);
    pfn_D3D12CreateDevice = create_native_device;
    pfn_D3D12EnableExperimentalFeatures = NULL;
    pfn_D3D12GetDebugInterface = get_d3d12_pfn(D3D12GetDebugInterface);
    pfn_D3D12GetInterface = get_d3d12_pfn(D3D12GetInterface);
    pfn_D3D12CreateVersionedRootSignatureDeserializer = get_d3d12_pfn(D3D12CreateVersionedRootSignatureDeserializer);
    pfn_D3D12SerializeVersionedRootSignature = get_d3d12_pfn(D3D12SerializeVersionedRootSignature);
    native_require(native_create_device && pfn_D3D12GetDebugInterface, "native runtime entry points unavailable");
    native_require(SUCCEEDED(pfn_D3D12GetDebugInterface(&IID_ID3D12Debug, (void **)&debug)), "native debug layer required");
    ID3D12Debug_EnableDebugLayer(debug);
    ID3D12Debug_Release(debug);
    native_require(SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory4, (void **)&factory)), "DXGI factory failed");
    for (i = 0;; i++)
    {
        DXGI_ADAPTER_DESC1 desc;
        IDXGIAdapter1 *candidate;
        HRESULT hr = IDXGIFactory4_EnumAdapters1(factory, i, &candidate);
        if (hr == DXGI_ERROR_NOT_FOUND)
            break;
        native_require(SUCCEEDED(hr), "adapter enumeration failed");
        native_require(SUCCEEDED(IDXGIAdapter1_GetDesc1(candidate, &desc)), "adapter description failed");
        if (desc.VendorId == 0x1af4 && desc.DeviceId == 0x1050 && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
        {
            native_require(!native_adapter, "ambiguous Helios adapters");
            native_adapter = candidate;
            fprintf(stderr, "ADAPTER,%04x,%04x,%08lx:%08lx,%ls\n", desc.VendorId, desc.DeviceId,
                    (unsigned long)desc.AdapterLuid.HighPart, desc.AdapterLuid.LowPart, desc.Description);
        }
        else
            IDXGIAdapter1_Release(candidate);
    }
    IDXGIFactory4_Release(factory);
    native_require(native_adapter != NULL, "Helios adapter unavailable");
    /* This is the requested level passed to the real native creation API. */
    vkd3d_device_feature_level = D3D_FEATURE_LEVEL_12_1;
    vkd3d_set_running_in_test_suite();
    run_test(test_format_support);
    run_test(test_multisample_quality_levels);
    run_test(test_typed_buffer_uav);
    run_test(test_typed_uav_store);
    run_test(test_uav_load);
    run_test(test_cs_uav_store);
    run_test(test_uav_counters);
    run_test(test_decrement_uav_counter);
    run_test(test_atomic_instructions_dxbc);
    run_test(test_atomic_instructions_dxil);
    run_test(test_sample_instructions);
    run_test(test_sample_c_lz);
    run_test(test_multisample_array_texture);
    run_test(test_geometry_shader_dxbc);
    run_test(test_geometry_shader_dxil);
    run_test(test_quad_tessellation_dxbc);
    run_test(test_quad_tessellation_dxil);
    run_test(test_line_tessellation_dxbc);
    run_test(test_line_tessellation_dxil);
    run_test(test_typed_buffers_many_objects_dxbc);
    run_test(test_typed_buffers_many_objects_dxil);
    run_test(test_sampler_border_color);
    run_test(test_filter_reduction_dxbc);
    run_test(test_filter_reduction_dxil);
    run_test(test_typed_srv_uav_cast);
    run_test(test_query_pipeline_statistics_continuation);
    run_test(test_query_pipeline_statistics_continuation_ia);
    run_test(test_query_dgc_compute_dxbc);
    run_test(test_query_dgc_compute_dxil);
    run_test(test_rasterizer_ordered_views_dxbc);
    run_test(test_rasterizer_ordered_views_dxil);
    run_test(test_conservative_rasterization_dxbc);
    run_test(test_conservative_rasterization_dxil);
    run_test(test_helios_tir_attachment_creation);
    run_test(test_tir_outputs_dxbc);
    run_test(test_tir_outputs_dxil);
    run_test(test_tir_invalid_dxbc);
    run_test(test_tir_invalid_dxil);
    run_test(test_tir_one_sample_dxbc);
    run_test(test_tir_one_sample_dxil);
    run_test(test_tir_mixed_samples_dxbc);
    run_test(test_tir_mixed_samples_dxil);
    native_require(vkd3d_test_state.success_count && !vkd3d_test_state.skip_count &&
            !vkd3d_test_state.todo_count && !vkd3d_test_state.todo_success_count && !vkd3d_test_state.bug_count,
            "selected native coverage was skipped or graded as expected failure");
    IDXGIAdapter1_Release(native_adapter);
}
