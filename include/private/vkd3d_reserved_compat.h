/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Versioned, local results from tools/vulkan_sparse_behavior_probe.c. This is
 * a compatibility policy cache, never a feature-level or completion override.
 * Both producers and readers include this file; bump VERSION when tests change.
 */
#ifndef VKD3D_RESERVED_COMPAT_H
#define VKD3D_RESERVED_COMPAT_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <wchar.h>
#endif

#define VKD3D_SPARSE_PROBE_VERSION 2u
#define VKD3D_SPARSE_PROBE_PATH_SIZE 1024u
#define VKD3D_SPARSE_PROBE_UNKNOWN 0u
#define VKD3D_SPARSE_PROBE_PASS 1u
#define VKD3D_SPARSE_PROBE_FAIL 2u

/* Only these color4 cases have a probe implementation. Other combinations
 * remain unverified. These are VkFormat values, not vendor/device identifiers. */
static const uint32_t vkd3d_sparse_probe_formats[] = {13, 74, 98, 101, 37, 43, 44};

struct vkd3d_sparse_probe_key
{
    uint8_t device_uuid[16], driver_uuid[16], pipeline_uuid[16];
    uint32_t api_version, driver_version, vendor_id, device_id;
    uint32_t format, samples;
    uint8_t device_luid[8], layer_driver_uuid[16];
    uint32_t luid_valid, layer_driver_id, layer_driver_version, icd_hash_lo, icd_hash_hi;
};

/* Venus's public driverVersion/UUID describe Mesa, not the renderer driver.
 * Include the underlying driver, guest adapter epoch and loaded ICD bytes too.
 * This fingerprint detects changes; it is not an authentication mechanism. */
static inline int vkd3d_sparse_probe_key_init(struct vkd3d_sparse_probe_key *key,
        PFN_vkGetPhysicalDeviceProperties2 get_properties, VkPhysicalDevice gpu, uint32_t format,
        int maintenance7)
{
    VkPhysicalDeviceProperties2 props = {0};
    VkPhysicalDeviceDriverProperties driver = {0};
    VkPhysicalDeviceIDProperties id = {0};
    VkPhysicalDeviceLayeredApiPropertiesListKHR list = {0};
    VkPhysicalDeviceLayeredApiPropertiesKHR api = {0};
    VkPhysicalDeviceLayeredApiVulkanPropertiesKHR layer = {0};
    VkPhysicalDeviceIDProperties layer_id = {0};
    VkPhysicalDeviceDriverProperties layer_driver = {0};
    memset(key, 0, sizeof(*key));
    props.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2; props.pNext = &driver;
    driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES; driver.pNext = &id;
    id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;
    get_properties(gpu, &props);
    memcpy(key->device_uuid, id.deviceUUID, 16); memcpy(key->driver_uuid, id.driverUUID, 16);
    memcpy(key->pipeline_uuid, props.properties.pipelineCacheUUID, 16);
    if (id.deviceLUIDValid) memcpy(key->device_luid, id.deviceLUID, 8);
    key->luid_valid = id.deviceLUIDValid;
    key->api_version = props.properties.apiVersion; key->driver_version = props.properties.driverVersion;
    key->vendor_id = props.properties.vendorID; key->device_id = props.properties.deviceID;
    key->format = format; key->samples = 4;
    if (driver.driverID != VK_DRIVER_ID_MESA_VENUS) return 1;
    if (!maintenance7) return 0;
    list.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LAYERED_API_PROPERTIES_LIST_KHR;
    list.layeredApiCount = 1; list.pLayeredApis = &api;
    api.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LAYERED_API_PROPERTIES_KHR; api.pNext = &layer;
    layer.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_LAYERED_API_VULKAN_PROPERTIES_KHR;
    layer.properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2; layer.properties.pNext = &layer_id;
    layer_id.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES; layer_id.pNext = &layer_driver;
    layer_driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
    props.pNext = &list;
    get_properties(gpu, &props);
    if (!list.layeredApiCount || api.layeredAPI != VK_PHYSICAL_DEVICE_LAYERED_API_VULKAN_KHR ||
            !layer_driver.driverID || !memcmp(layer_id.driverUUID, (uint8_t[16]){0}, 16)) return 0;
    memcpy(key->layer_driver_uuid, layer_id.driverUUID, 16);
    key->layer_driver_id = layer_driver.driverID;
    key->layer_driver_version = layer.properties.properties.driverVersion;
#ifdef _WIN32
    {
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
        MODULEENTRY32W module = {0};
        uint64_t hash = UINT64_C(14695981039346656037);
        unsigned int matches = 0;
        unsigned char data[16384];
        size_t n, i;
        FILE *file;
        module.dwSize = sizeof(module);
        if (snapshot == INVALID_HANDLE_VALUE) return 0;
        if (Module32FirstW(snapshot, &module)) do
        {
            if (_wcsnicmp(module.szModule, L"vulkan_virtio", 13)) continue;
            matches++;
            if (!(file = _wfopen(module.szExePath, L"rb"))) { matches += 2; break; }
            while ((n = fread(data, 1, sizeof(data), file)))
                for (i = 0; i < n; i++) hash = (hash ^ data[i]) * UINT64_C(1099511628211);
            if (ferror(file)) matches += 2;
            fclose(file);
        } while (Module32NextW(snapshot, &module));
        CloseHandle(snapshot);
        if (matches != 1) return 0;
        key->icd_hash_lo = (uint32_t)hash; key->icd_hash_hi = (uint32_t)(hash >> 32);
    }
#endif
    return 1;
}

struct vkd3d_sparse_probe_record
{
    uint32_t version, size;
    struct vkd3d_sparse_probe_key key;
    uint32_t status, completed_phases, checked_pixels, bad_pixels;
    uint32_t checksum;
};

static inline uint32_t vkd3d_sparse_probe_checksum(const struct vkd3d_sparse_probe_record *record)
{
    const uint8_t *p = (const uint8_t *)record;
    uint32_t hash = 2166136261u;
    size_t i;
    for (i = 0; i < offsetof(struct vkd3d_sparse_probe_record, checksum); i++)
        hash = (hash ^ p[i]) * 16777619u;
    return hash;
}

static inline int vkd3d_sparse_probe_path(char *path, size_t size, const struct vkd3d_sparse_probe_key *key)
{
    const char *base;
    char uuid[65];
    static const char hex[] = "0123456789abcdef";
    unsigned int i;
    int n;
    for (i = 0; i < 16; i++)
    {
        uuid[2 * i] = hex[key->device_uuid[i] >> 4];
        uuid[2 * i + 1] = hex[key->device_uuid[i] & 15];
        uuid[32 + 2 * i] = hex[key->driver_uuid[i] >> 4];
        uuid[33 + 2 * i] = hex[key->driver_uuid[i] & 15];
    }
    uuid[64] = 0;
#ifdef _WIN32
    base = getenv("PROGRAMDATA");
    if (!base || !*base) return 0;
    n = snprintf(path, size, "%s/Helios/sparse-probe-v%u/%s-%u-%u.bin", base,
            VKD3D_SPARSE_PROBE_VERSION, uuid, key->format, key->samples);
#else
    base = getenv("XDG_CACHE_HOME");
    if (base && *base)
        n = snprintf(path, size, "%s/helios/sparse-probe-v%u/%s-%u-%u.bin", base,
                VKD3D_SPARSE_PROBE_VERSION, uuid, key->format, key->samples);
    else
    {
        base = getenv("HOME");
        if (!base || !*base) return 0;
        n = snprintf(path, size, "%s/.cache/helios/sparse-probe-v%u/%s-%u-%u.bin", base,
                VKD3D_SPARSE_PROBE_VERSION, uuid, key->format, key->samples);
    }
#endif
    return n >= 0 && (size_t)n < size;
}

static inline uint32_t vkd3d_sparse_probe_read(const struct vkd3d_sparse_probe_key *key)
{
    struct vkd3d_sparse_probe_record record;
    char path[VKD3D_SPARSE_PROBE_PATH_SIZE];
    FILE *file;
    int valid;
    if (!vkd3d_sparse_probe_path(path, sizeof(path), key) || !(file = fopen(path, "rb")))
        return VKD3D_SPARSE_PROBE_UNKNOWN;
    valid = fread(&record, 1, sizeof(record), file) == sizeof(record) && fgetc(file) == EOF && !ferror(file);
    fclose(file);
    if (!valid || record.version != VKD3D_SPARSE_PROBE_VERSION || record.size != sizeof(record) ||
            memcmp(&record.key, key, sizeof(*key)) || record.checksum != vkd3d_sparse_probe_checksum(&record))
        return VKD3D_SPARSE_PROBE_UNKNOWN;
    /* A query, partial run or failed control can never certify sparse backing. */
    if (record.status == VKD3D_SPARSE_PROBE_PASS && record.completed_phases == 7 &&
            record.checked_pixels && !record.bad_pixels)
        return VKD3D_SPARSE_PROBE_PASS;
    return record.status == VKD3D_SPARSE_PROBE_FAIL ? VKD3D_SPARSE_PROBE_FAIL : VKD3D_SPARSE_PROBE_UNKNOWN;
}
#endif
