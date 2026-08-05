/*
 * Copyright 2018 Józef Kucia for CodeWeavers
 * Copyright 2020 Joshua Ashton for Valve Software
 * Copyright 2023 Hans-Kristian Arntzen for Valve Corporation
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
 *
 */

/* Helios addition: the debug-control facility, MOVED VERBATIM out of main.c
 * (:840-1045).  Not a rewrite -- one implementation, in a smaller object.
 *
 * Why it had to move.  libvkd3d calls five of these predicates unconditionally
 * from device.c, command.c, resource.c and state.c:
 *
 *     vkd3d_debug_control_is_test_suite
 *     vkd3d_debug_control_explode_on_vvl_error
 *     vkd3d_debug_control_has_out_of_spec_test_behavior
 *     vkd3d_debug_control_get_behavior_flags
 *     vkd3d_debug_control_mute_message_id
 *
 * They were defined in main.c, which is ALSO the only object in the engine that
 * references CreateDXGIFactory1 (d3d12_get_adapter, :383/:406).  The Helios
 * static arm (helios_d3d12_static, DECISIONS.md D4) omits main.c precisely so
 * that dxgi import cannot be generated -- and thereby lost all five predicates
 * with it: 19 unresolved externals at the first attempt to link the archive
 * into the D12-G1 probe.  A shared_library force-links every object it is
 * handed, so the DLL arm never noticed; an archive member is pulled only when
 * referenced, so the static arm did.
 *
 * Splitting is the fix rather than restating the five in the Helios target,
 * because the predicates read state that ONLY the COM vtbl below writes.  Two
 * implementations would mean the DLL arm's IVKD3DDebugControlInterface silently
 * not reaching the static arm's statics -- a difference in behaviour between the
 * conformance arm and the shipping arm, which is exactly what D12-G1 exists to
 * rule out.
 *
 * Nothing here touches DXGI, and the whole file is reachable from libvkd3d
 * without D3D12GetInterface.  In helios_umd12.dll the COM interface is
 * unreachable (Helios never calls D3D12GetInterface -- ARCHITECTURE.md §12 rule
 * 15), so every predicate answers from zeroed state: not under test, no
 * explode-on-VVL-error, no muted VUIDs, no out-of-spec behaviour, no behaviour
 * flags.  That is the same answer a shipping d3d12core.dll gives an app that
 * never asks for CLSID_VKD3DDebugControl.
 */

#define VKD3D_DBG_CHANNEL VKD3D_DBG_CHANNEL_API

#define VK_NO_PROTOTYPES
#ifdef _WIN32
#include "vkd3d_win32.h"
#endif
#include "vkd3d.h"
#include "vkd3d_atomic.h"
#include "vkd3d_debug.h"
#include "vkd3d_threads.h"
#include "vkd3d_core_interface.h"

#include "debug.h"
#include "debug_control.h"

#include <string.h>

static pthread_mutex_t vkd3d_debug_control_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t vkd3d_debug_control_is_running_under_test;
static uint32_t vkd3d_debug_control_explode_on_error;
static uint32_t vkd3d_debug_control_out_of_spec_behavior[VKD3D_DEBUG_CONTROL_OUT_OF_SPEC_BEHAVIOR_COUNT];
static uint32_t vkd3d_debug_control_mute_validation_global_counter;
static uint32_t vkd3d_debug_control_behavior_flags;

static struct vkd3d_debug_control_muted_vuid
{
    char vuid[8];
    char explanation[248];
} vkd3d_debug_control_muted_vuids[32];
static unsigned int vkd3d_debug_control_muted_vuid_count;

bool vkd3d_debug_control_is_test_suite(void)
{
    return vkd3d_atomic_uint32_load_explicit(&vkd3d_debug_control_is_running_under_test, vkd3d_memory_order_relaxed) != 0;
}

bool vkd3d_debug_control_explode_on_vvl_error(void)
{
    return vkd3d_atomic_uint32_load_explicit(&vkd3d_debug_control_explode_on_error, vkd3d_memory_order_relaxed) != 0;
}

bool vkd3d_debug_control_has_out_of_spec_test_behavior(VKD3D_DEBUG_CONTROL_OUT_OF_SPEC_BEHAVIOR behavior)
{
    if (behavior >= VKD3D_DEBUG_CONTROL_OUT_OF_SPEC_BEHAVIOR_COUNT)
        return false;
    return vkd3d_atomic_uint32_load_explicit(&vkd3d_debug_control_out_of_spec_behavior[behavior], vkd3d_memory_order_relaxed) != 0;
}

VKD3D_DEBUG_CONTROL_BEHAVIOR_FLAGS vkd3d_debug_control_get_behavior_flags(void)
{
    return vkd3d_atomic_uint32_load_explicit(&vkd3d_debug_control_behavior_flags, vkd3d_memory_order_relaxed);
}

bool vkd3d_debug_control_mute_message_id(const char *vuid)
{
    const struct vkd3d_debug_control_muted_vuid *entry;
    bool ret = false;
    unsigned int i;
    if (vkd3d_atomic_uint32_load_explicit(&vkd3d_debug_control_mute_validation_global_counter, vkd3d_memory_order_relaxed))
        return true;

    pthread_mutex_lock(&vkd3d_debug_control_lock);
    for (i = 0; i < vkd3d_debug_control_muted_vuid_count && !ret; i++)
    {
        entry = &vkd3d_debug_control_muted_vuids[i];

        if (strstr(vuid, entry->vuid))
        {
            ret = true;
            if (entry->explanation[0])
                INFO("Muted %s: %s\n", vuid, entry->explanation);
            else
                WARN("Muted %s.\n", vuid);
        }
    }
    pthread_mutex_unlock(&vkd3d_debug_control_lock);
    return ret;
}

static HRESULT STDMETHODCALLTYPE vkd3d_debug_control_SetRunningUnderTest(IVKD3DDebugControlInterface *iface)
{
    (void)iface;
    vkd3d_atomic_uint32_store_explicit(&vkd3d_debug_control_is_running_under_test, 1, vkd3d_memory_order_relaxed);
    INFO("Running in test suite.\n");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE vkd3d_debug_control_SetExplodeOnValidationError(
        IVKD3DDebugControlInterface *iface, BOOL enable)
{
    (void)iface;
    vkd3d_atomic_uint32_store_explicit(&vkd3d_debug_control_explode_on_error, enable, vkd3d_memory_order_relaxed);
    if (enable)
        INFO("Enabling explode-on-VVL-error test mode.\n");
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE vkd3d_debug_control_MuteValidationGlobal(IVKD3DDebugControlInterface *iface)
{
    (void)iface;
    vkd3d_atomic_uint32_increment(&vkd3d_debug_control_mute_validation_global_counter, vkd3d_memory_order_relaxed);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE vkd3d_debug_control_UnmuteValidationGlobal(IVKD3DDebugControlInterface *iface)
{
    (void)iface;
    vkd3d_atomic_uint32_decrement(&vkd3d_debug_control_mute_validation_global_counter, vkd3d_memory_order_relaxed);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE vkd3d_debug_control_MuteValidationMessageID(
        IVKD3DDebugControlInterface *iface, const char *vuid, const char *explanation)
{
    struct vkd3d_debug_control_muted_vuid *entry;
    HRESULT hr = S_OK;
    unsigned int i;
    (void)iface;

    pthread_mutex_lock(&vkd3d_debug_control_lock);

    for (i = 0; i < vkd3d_debug_control_muted_vuid_count; i++)
    {
        entry = &vkd3d_debug_control_muted_vuids[i];

        /* Ignore duplicates, it is possible for apps to initialize d3d12
         * multiple times while keeping d3d12core loaded. */
        if (!strncmp(entry->vuid, vuid, ARRAY_SIZE(entry->vuid)))
        {
            hr = S_FALSE;
            goto out;
        }
    }

    if (vkd3d_debug_control_muted_vuid_count == ARRAY_SIZE(vkd3d_debug_control_muted_vuids))
    {
        hr = E_OUTOFMEMORY;
        goto out;
    }

    entry = &vkd3d_debug_control_muted_vuids[vkd3d_debug_control_muted_vuid_count++];
    strncpy(entry->vuid, vuid, ARRAY_SIZE(entry->vuid) - 1u);

    if (explanation)
        strncpy(entry->explanation, explanation, ARRAY_SIZE(entry->explanation) - 1u);

out:
    pthread_mutex_unlock(&vkd3d_debug_control_lock);
    return hr;
}

static HRESULT STDMETHODCALLTYPE vkd3d_debug_control_UnmuteValidationMessageID(
        IVKD3DDebugControlInterface *iface, const char *vuid)
{
    struct vkd3d_debug_control_muted_vuid *entry, *last;
    unsigned int i;
    (void)iface;

    pthread_mutex_lock(&vkd3d_debug_control_lock);

    for (i = 0; i < vkd3d_debug_control_muted_vuid_count; i++)
    {
        entry = &vkd3d_debug_control_muted_vuids[i];

        if (strncmp(entry->vuid, vuid, ARRAY_SIZE(entry->vuid)) == 0)
        {
            last = &vkd3d_debug_control_muted_vuids[--vkd3d_debug_control_muted_vuid_count];

            memcpy(entry, last, sizeof(*last));
            memset(last, 0, sizeof(*last));

            pthread_mutex_unlock(&vkd3d_debug_control_lock);
            return S_OK;
        }
    }

    pthread_mutex_unlock(&vkd3d_debug_control_lock);
    return E_INVALIDARG;
}

static HRESULT STDMETHODCALLTYPE vkd3d_debug_control_SetOutOfSpecTestBehavior(
        IVKD3DDebugControlInterface *iface, VKD3D_DEBUG_CONTROL_OUT_OF_SPEC_BEHAVIOR behavior, BOOL enable)
{
    (void)iface;

    if (behavior < VKD3D_DEBUG_CONTROL_OUT_OF_SPEC_BEHAVIOR_COUNT)
    {
        vkd3d_atomic_uint32_store_explicit(
                &vkd3d_debug_control_out_of_spec_behavior[behavior], enable,
                vkd3d_memory_order_relaxed);
        return S_OK;
    }
    else
    {
        return E_INVALIDARG;
    }
}

static HRESULT STDMETHODCALLTYPE vkd3d_debug_control_SetBehaviorFlags(
        IVKD3DDebugControlInterface *iface, VKD3D_DEBUG_CONTROL_BEHAVIOR_FLAGS behavior)
{
    (void)iface;
    vkd3d_atomic_uint32_store_explicit(&vkd3d_debug_control_behavior_flags, behavior, vkd3d_memory_order_relaxed);
    return S_OK;
}

static CONST_VTBL struct IVKD3DDebugControlInterfaceVtbl vkd3d_debug_control_vtbl =
{
    vkd3d_debug_control_SetRunningUnderTest,
    vkd3d_debug_control_SetExplodeOnValidationError,
    vkd3d_debug_control_MuteValidationGlobal,
    vkd3d_debug_control_UnmuteValidationGlobal,
    vkd3d_debug_control_MuteValidationMessageID,
    vkd3d_debug_control_UnmuteValidationMessageID,
    vkd3d_debug_control_SetOutOfSpecTestBehavior,
    vkd3d_debug_control_SetBehaviorFlags,
};

/* No longer `static`: main.c hands this out from d3d12core_D3D12GetInterface
 * and now lives in a different translation unit.  Declared in debug_control.h. */
const struct IVKD3DDebugControlInterface vkd3d_debug_control_instance =
{
    .lpVtbl = &vkd3d_debug_control_vtbl,
};

