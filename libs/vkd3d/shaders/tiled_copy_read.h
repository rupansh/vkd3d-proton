/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "tiled_copy.h"
#extension GL_EXT_samplerless_texture_functions : require
layout(local_size_x = 64) in;
#ifdef TILED_DEPTH
layout(binding = 0) uniform texture2DMSArray src;
#else
layout(binding = 0) uniform utexture2DMSArray src;
#endif
void main()
{
    /* One invocation owns a DWORD, including R8/R16 samples. This avoids
     * overlapping subword writes and keeps byte-offset readback legal. */
    uint offset = gl_GlobalInvocationID.x * 4;
    if (offset >= 65536 || !enabled())
        return;
    uint sample_bytes = args.bytes_per_sample;
    uint value = 0, valid_bytes = 0;
    for (uint i = 0; i < max(1u, 4 / sample_bytes); ++i)
    {
        uint sample_offset = offset + i * sample_bytes;
        uint pixel = sample_offset / (sample_bytes * args.samples);
        uvec2 p = uvec2(pixel % args.tile_width, pixel / args.tile_width);
        if (any(greaterThanEqual(p, args.extent)))
            continue;
        ivec3 coord = ivec3(p + args.origin, args.layer);
        uint sample_index = (sample_offset / sample_bytes) % args.samples;
#ifdef TILED_DEPTH
        float depth = texelFetch(src, coord, int(sample_index)).x;
        uint bits = sample_bytes == 2 ? uint(roundEven(depth * 65535.0)) : floatBitsToUint(depth);
#else
        uint bits = texelFetch(src, coord, int(sample_index))[(offset % sample_bytes) / 4];
#endif
        value |= bits << (i * sample_bytes * 8);
        valid_bytes |= ((1u << min(4u, sample_bytes)) - 1u) << (i * sample_bytes);
    }
    if (valid_bytes == 15)
        store_word(offset, value);
    else
        for (uint i = 0; i < 4; ++i)
            if ((valid_bytes & (1u << i)) != 0)
                Bytes(args.buffer_va + offset).data[i] = uint8_t(value >> (8 * i));
}
