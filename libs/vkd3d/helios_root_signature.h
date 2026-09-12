/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef VKD3D_HELIOS_ROOT_SIGNATURE_H
#define VKD3D_HELIOS_ROOT_SIGNATURE_H

/* The native runtime may append instrumentation to an application's 64 DWORD
 * root signature. This is private driver capacity, not a raised API limit.
 * Keep all root masks wide, including temporary masks and indirect paths. */
#define VKD3D_ROOT_SIGNATURE_MAX_COST 128u
#ifdef __SIZEOF_INT128__
typedef unsigned __int128 vkd3d_root_mask_t;
#elif defined(__BITINT_MAXWIDTH__) && __BITINT_MAXWIDTH__ >= 128
/* Clang's 32-bit Windows target supports _BitInt, but not __int128. */
typedef unsigned _BitInt(128) vkd3d_root_mask_t;
#else
#error A 128-bit integer type is required for native root-signature masks.
#endif

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
