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
#include "vkd3d.h"
#include "vkd3d_debug.h"

#include <stdlib.h>
#include <string.h>

#include "debug.h"

#if defined(__WINE__) || !defined(_WIN32)
#define DLLEXPORT __attribute__((visibility("default")))
#include <dlfcn.h>
#else
#define DLLEXPORT
#endif

static VkPhysicalDevice helios_find_physical_device(VkInstance instance,
        PFN_vkGetInstanceProcAddr gipa, LUID adapter_luid)
{
    PFN_vkEnumeratePhysicalDevices enumerate_physical_devices;
    PFN_vkGetPhysicalDeviceProperties2 get_properties2;
    VkPhysicalDevice *physical_devices = NULL;
    VkPhysicalDevice match = VK_NULL_HANDLE;
    uint32_t count = 0;
    VkResult vr;

    enumerate_physical_devices = (PFN_vkEnumeratePhysicalDevices)
            gipa(instance, "vkEnumeratePhysicalDevices");
    get_properties2 = (PFN_vkGetPhysicalDeviceProperties2)
            gipa(instance, "vkGetPhysicalDeviceProperties2");
    if (!enumerate_physical_devices || !get_properties2)
        return VK_NULL_HANDLE;
    if ((vr = enumerate_physical_devices(instance, &count, NULL)) != VK_SUCCESS ||
            !count || count > 64)
        return VK_NULL_HANDLE;
    if (!(physical_devices = calloc(count, sizeof(*physical_devices))))
        return VK_NULL_HANDLE;
    if ((vr = enumerate_physical_devices(instance, &count, physical_devices)) != VK_SUCCESS)
        goto done;

    for (uint32_t i = 0; i < count; ++i)
    {
        VkPhysicalDeviceIDProperties id_properties;
        VkPhysicalDeviceProperties2 properties;

        memset(&id_properties, 0, sizeof(id_properties));
        id_properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
        memset(&properties, 0, sizeof(properties));
        properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties.pNext = &id_properties;
        get_properties2(physical_devices[i], &properties);
        if (!id_properties.deviceLUIDValid ||
                memcmp(id_properties.deviceLUID, &adapter_luid, VK_LUID_SIZE))
            continue;
        if (match)
        {
            ERR("More than one A5 physical device matched LUID %08x:%08x.\n",
                    (unsigned int)adapter_luid.HighPart,
                    (unsigned int)adapter_luid.LowPart);
            match = VK_NULL_HANDLE;
            goto done;
        }
        match = physical_devices[i];
    }

done:
    free(physical_devices);
    return match;
}

DLLEXPORT HRESULT helios_vkd3d_create_device(VkInstance vk_instance,
        PFN_vkGetInstanceProcAddr gipa, void *expected_vk_module,
        LUID adapter_luid, void *outer_context,
        HRESULT (*outer_allocation_create)(void *, uint64_t, uint32_t, uint32_t,
                struct HeliosResourceAssociationV1 *),
        HRESULT (*outer_allocation_teardown_begin)(void *, uint64_t, uint64_t),
        HRESULT (*outer_allocation_begin)(void *, uint64_t, uint64_t, void **),
        HRESULT (*outer_allocation_finish)(void *, void *, VkResult),
        HRESULT (*outer_allocation_retire)(void *, uint64_t, uint64_t, VkResult),
        REFIID iid, void **device)
{
    struct vkd3d_instance_create_info instance_create_info;
    struct vkd3d_device_create_info device_create_info;
    struct vkd3d_instance *instance = NULL;
    VkPhysicalDevice physical_device;
    HRESULT hr;

    TRACE("adapter_luid %08x:%08x, iid %s, device %p.\n",
            (unsigned int)adapter_luid.HighPart, (unsigned int)adapter_luid.LowPart,
            debugstr_guid(iid), device);

    if (!device || !vk_instance || !gipa || !expected_vk_module || !outer_context ||
            !outer_allocation_create || !outer_allocation_teardown_begin ||
            !outer_allocation_begin || !outer_allocation_finish ||
            !outer_allocation_retire ||
            (!adapter_luid.LowPart && !adapter_luid.HighPart))
        return E_INVALIDARG;

    memset(&instance_create_info, 0, sizeof(instance_create_info));
    instance_create_info.pfn_vkGetInstanceProcAddr = gipa;
    instance_create_info.vk_instance = vk_instance;
    instance_create_info.expected_vk_module = expected_vk_module;
    instance_create_info.helios_record_only = true;
    if (FAILED(hr = vkd3d_create_instance(&instance_create_info, &instance)))
        return hr;

    physical_device = helios_find_physical_device(vk_instance, gipa, adapter_luid);
    if (!physical_device)
    {
        ERR("A5 instance has no unique physical device for LUID %08x:%08x.\n",
                (unsigned int)adapter_luid.HighPart,
                (unsigned int)adapter_luid.LowPart);
        vkd3d_instance_decref(instance);
        return E_FAIL;
    }

    memset(&device_create_info, 0, sizeof(device_create_info));
    device_create_info.minimum_feature_level = D3D_FEATURE_LEVEL_11_0;
    device_create_info.instance = instance;
    device_create_info.vk_physical_device = physical_device;
    device_create_info.parent = NULL;   /* deliberately NOT an IDXGIAdapter */
    device_create_info.adapter_luid = adapter_luid;
    device_create_info.independent = true;
    device_create_info.helios_outer_context = outer_context;
    device_create_info.helios_outer_allocation_create = outer_allocation_create;
    device_create_info.helios_outer_allocation_teardown_begin =
            outer_allocation_teardown_begin;
    device_create_info.helios_outer_allocation_begin = outer_allocation_begin;
    device_create_info.helios_outer_allocation_finish = outer_allocation_finish;
    device_create_info.helios_outer_allocation_retire = outer_allocation_retire;

    hr = vkd3d_create_device(&device_create_info, iid, device);
    vkd3d_instance_decref(instance);
    return hr;
}

DLLEXPORT HRESULT helios_vkd3d_serialize_root_signature(
        const D3D12_ROOT_SIGNATURE_DESC *desc, D3D_ROOT_SIGNATURE_VERSION version,
        ID3DBlob **blob, ID3DBlob **error_blob)
{
    TRACE("desc %p, version %#x, blob %p, error_blob %p.\n", desc, version, blob, error_blob);

    return vkd3d_serialize_root_signature(desc, version, blob, error_blob);
}
