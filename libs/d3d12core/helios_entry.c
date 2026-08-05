/*
 * Copyright 2026 Helios vGPU
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

/* Helios addition — DXGI-free entry points for the Helios D3D12 user-mode display
 * driver (helios_umd12.dll).
 *
 * d3d12core's own device path, d3d12core_CreateDeviceFromFactory (main.c:643,
 * reached from d3d12core_CreateDevice at :742 via the exported D3D12GetInterface),
 * resolves the adapter through d3d12_get_adapter (:375) -> CreateDXGIFactory1
 * (:383, :406).  A WDDM user-mode driver sits BELOW DXGI and implements the DXGI
 * DDI; it must not depend on dxgi.dll, and a UMD that loads dxgi during device
 * creation risks re-entering the adapter enumeration that loads the UMD.  These
 * two exports exist precisely so that path is never entered.
 *
 * (The exported D3D12CreateDevice is NOT in this module at all — it is
 * libs/d3d12/main.c:143, in the separate thin d3d12.dll target, which Helios
 * neither builds into helios_vkd3d.dll nor ever loads.)
 *
 * The second export is the root-signature serializer.  d3d12umddi delivers root
 * signatures to the driver already parsed (D3D12DDI_ROOT_SIGNATURE), while
 * ID3D12Device::CreateRootSignature wants a serialized DXBC RTS0 blob, so the UMD
 * must re-serialize.  vkd3d_serialize_root_signature (include/vkd3d.h:129) is
 * linked into this DLL from the static libvkd3d but is exported from no DLL, and
 * d3d12core_SerializeRootSignature (main.c:757) is reachable only as a vtable
 * method behind D3D12GetInterface — i.e. through the interface being avoided.
 */

#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_API

#define VK_NO_PROTOTYPES
#ifdef _WIN32
#include "vkd3d_win32.h"
#endif
#include "vkd3d_sonames.h"
#include "vkd3d.h"
#include "vkd3d_debug.h"
#include "vkd3d_threads.h"

#include "debug.h"

#if defined(__WINE__) || !defined(_WIN32)
#define DLLEXPORT __attribute__((visibility("default")))
#include <dlfcn.h>
#else
#define DLLEXPORT
#endif

static pthread_once_t helios_vulkan_once = PTHREAD_ONCE_INIT;
static PFN_vkGetInstanceProcAddr helios_vk_gipa;

#ifdef _WIN32
static HMODULE helios_vulkan_module;
#else
static void *helios_vulkan_module;
#endif

/* vkd3d_init_vk_global_procs (device.c:461-468) returns E_INVALIDARG when
 * pfn_vkGetInstanceProcAddr is NULL, so the "if set to NULL, libvkd3d loads
 * libvulkan" comment in include/vkd3d.h:68 does not hold for vkd3d-proton: the
 * caller must supply the entry point.  This mirrors load_modules_once
 * (main.c:319-364) without the wineopenxr half, which only feeds VR instance
 * extensions a display driver has no use for. */
static void helios_load_vulkan_once(void)
{
#ifdef _WIN32
    /* Prefer winevulkan directly, as upstream does, to bypass third-party
     * overlays that hook the Vulkan loader.  On the Helios guest this name is
     * absent and vulkan-1.dll is the one that resolves. */
    static const char * const vulkan_dllnames[] =
    {
        "winevulkan.dll",
        "vulkan-1.dll",
    };
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(vulkan_dllnames); i++)
    {
        helios_vulkan_module = LoadLibraryA(vulkan_dllnames[i]);
        if (!helios_vulkan_module)
            continue;

        helios_vk_gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(
                helios_vulkan_module, "vkGetInstanceProcAddr");
        if (helios_vk_gipa)
            break;

        FreeLibrary(helios_vulkan_module);
        helios_vulkan_module = NULL;
    }
#else
    helios_vulkan_module = dlopen(SONAME_LIBVULKAN, RTLD_LAZY);
    if (helios_vulkan_module)
        helios_vk_gipa = (PFN_vkGetInstanceProcAddr)dlsym(helios_vulkan_module, "vkGetInstanceProcAddr");
#endif
}

DLLEXPORT HRESULT helios_vkd3d_create_device(LUID adapter_luid, REFIID iid, void **device)
{
    /* The same lists d3d12core uses (main.c:574-593, :659-670), so that the
     * device this export creates is configured exactly like the one vkd3d's own
     * conformance suite creates — that equivalence is what makes the D12-G1
     * engine gate predictive of the shipping path. */
    static const char * const instance_extensions[] =
    {
        VK_KHR_SURFACE_EXTENSION_NAME,
#ifdef _WIN32
        VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
#endif
    };

    static const char * const optional_instance_extensions[] =
    {
        VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME,
        VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME,
        VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME,
    };

    static const char * const device_extensions[] =
    {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };

    static const char * const optional_device_extensions[] =
    {
        VK_KHR_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
        VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME,
    };

    struct vkd3d_instance_create_info instance_create_info;
    struct vkd3d_device_create_info device_create_info;

    TRACE("adapter_luid %08x:%08x, iid %s, device %p.\n",
            (unsigned int)adapter_luid.HighPart, (unsigned int)adapter_luid.LowPart,
            debugstr_guid(iid), device);

    if (!device)
        return E_INVALIDARG;

    pthread_once(&helios_vulkan_once, helios_load_vulkan_once);
    if (!helios_vk_gipa)
    {
        ERR("Failed to load Vulkan library.\n");
        return E_FAIL;
    }

    memset(&instance_create_info, 0, sizeof(instance_create_info));
    instance_create_info.pfn_vkGetInstanceProcAddr = helios_vk_gipa;
    instance_create_info.instance_extensions = instance_extensions;
    instance_create_info.instance_extension_count = ARRAY_SIZE(instance_extensions);
    instance_create_info.optional_instance_extensions = optional_instance_extensions;
    instance_create_info.optional_instance_extension_count = ARRAY_SIZE(optional_instance_extensions);

    memset(&device_create_info, 0, sizeof(device_create_info));
    device_create_info.minimum_feature_level = D3D_FEATURE_LEVEL_11_0;
    device_create_info.instance = NULL;
    device_create_info.instance_create_info = &instance_create_info;
    /* VK_NULL_HANDLE delegates selection to vkd3d_select_physical_device
     * (device.c:3491-3573), which honours VKD3D_FILTER_DEVICE_NAME and otherwise
     * prefers DISCRETE > INTEGRATED > physical_devices[0].  That is correct on a
     * single-GPU guest but it is NOT LUID matching: if a second Vulkan device
     * ever appears in the guest, chain VkPhysicalDeviceIDProperties here and
     * match deviceLUID against adapter_luid first, the way
     * d3d12_find_physical_device (main.c:446-566) does at :498-532. */
    device_create_info.vk_physical_device = VK_NULL_HANDLE;
    device_create_info.device_extensions = device_extensions;
    device_create_info.device_extension_count = ARRAY_SIZE(device_extensions);
    device_create_info.optional_device_extensions = optional_device_extensions;
    device_create_info.optional_device_extension_count = ARRAY_SIZE(optional_device_extensions);
    device_create_info.parent = NULL;   /* deliberately NOT an IDXGIAdapter */
    device_create_info.adapter_luid = adapter_luid;

    return vkd3d_create_device(&device_create_info, iid, device);
}

DLLEXPORT HRESULT helios_vkd3d_serialize_root_signature(
        const D3D12_ROOT_SIGNATURE_DESC *desc, D3D_ROOT_SIGNATURE_VERSION version,
        ID3DBlob **blob, ID3DBlob **error_blob)
{
    TRACE("desc %p, version %#x, blob %p, error_blob %p.\n", desc, version, blob, error_blob);

    return vkd3d_serialize_root_signature(desc, version, blob, error_blob);
}
