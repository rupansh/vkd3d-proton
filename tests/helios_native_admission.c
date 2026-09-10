/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Backing admission checks, not native Windows runtime conformance. */
#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_API
#include "vkd3d_private.h"
#include <stdio.h>

HRESULT helios_vkd3d_create_device(LUID luid, REFIID iid, void **device);

int main(int argc, char **argv)
{
    ID3D12Device *device = NULL;
    LUID luid = {0};
    HRESULT hr, expected;
    unsigned int failures = 0;

    if (argc != 2 || (strcmp(argv[1], "supported") && strcmp(argv[1], "unsupported")))
        return 2;
    expected = !strcmp(argv[1], "supported") ? S_OK : DXGI_ERROR_UNSUPPORTED;
    if (FAILED(hr = helios_vkd3d_create_device(luid, &IID_ID3D12Device, (void **)&device)))
    {
        fprintf(stderr, "Engine creation failed: %#x.\n", (unsigned int)hr);
        return 1;
    }
    hr = helios_vkd3d_validate_native_feature_level(device, D3D_FEATURE_LEVEL_12_1);
    if (hr != expected)
    {
        fprintf(stderr, "Admission %#x, expected %#x.\n", (unsigned int)hr, (unsigned int)expected);
        failures++;
    }
    failures += helios_vkd3d_validate_native_feature_level(NULL, D3D_FEATURE_LEVEL_12_1) != E_INVALIDARG;
    failures += helios_vkd3d_validate_native_feature_level(device, D3D_FEATURE_LEVEL_12_2) != E_INVALIDARG;
    /* Releasing a refused engine must be safe without a native device owner. */
    ID3D12Device_Release(device);
    printf("%s: native backing admission %s; 3 checks, %u failures.\n",
            failures ? "FAIL" : "PASS", argv[1], failures);
    return failures ? 1 : 0;
}
