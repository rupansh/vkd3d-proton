/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef VKD3D_HELIOS_ROOT_SIGNATURE_H
#define VKD3D_HELIOS_ROOT_SIGNATURE_H

/* The native runtime may append instrumentation to an application's 64 DWORD
 * root signature. This is private driver capacity, not a raised API limit.
 * GCC and Clang (including the Windows clang-cl build) provide this integer
 * type. Keep all root masks wide, including temporary masks and indirect paths. */
#define VKD3D_ROOT_SIGNATURE_MAX_COST 128u
typedef unsigned __int128 vkd3d_root_mask_t;

static inline vkd3d_root_mask_t vkd3d_root_mask_bit(unsigned int index)
{
    assert(index < VKD3D_ROOT_SIGNATURE_MAX_COST);
    return (vkd3d_root_mask_t)1 << index;
}

static inline unsigned int vkd3d_root_mask_iter(vkd3d_root_mask_t *mask)
{
    uint64_t low = (uint64_t)*mask;
    uint64_t high = (uint64_t)(*mask >> 64);
    unsigned int index;

    assert(*mask);
    index = low ? vkd3d_bitmask_iter64(&low) : 64 + vkd3d_bitmask_iter64(&high);
    *mask &= *mask - 1;
    return index;
}

#endif
