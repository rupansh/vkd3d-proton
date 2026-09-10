#version 450
#extension GL_GOOGLE_include_directive : require
#include "tiled_copy.h"
void main()
{
    if (!enabled())
        discard;
    uvec2 p = uvec2(gl_FragCoord.xy) - args.origin;
    uint offset = ((p.y * args.tile_width + p.x) * args.samples + uint(gl_SampleID)) * args.bytes_per_sample;
    uint value = load_bytes(offset, args.bytes_per_sample);
    gl_FragDepth = args.bytes_per_sample == 2 ? float(value) / 65535.0 : uintBitsToFloat(value);
}
