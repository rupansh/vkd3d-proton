/* SPDX-License-Identifier: LGPL-2.1-or-later */
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require
#extension GL_EXT_shader_explicit_arithmetic_types_int8 : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_8bit_storage : require

layout(buffer_reference, scalar, buffer_reference_align = 4) buffer Words { uint data[]; };
layout(buffer_reference, scalar, buffer_reference_align = 1) buffer Bytes { uint8_t data[]; };
layout(push_constant, scalar) uniform Args
{
    uint64_t buffer_va;
    uint64_t predicate_va;
    uvec2 origin;
    uvec2 extent;
    uint tile_width;
    uint layer;
    uint samples;
    uint bytes_per_sample;
} args;

bool enabled()
{
    return args.predicate_va == uint64_t(0) || Words(args.predicate_va).data[0] != 0;
}

uint load_bytes(uint offset, uint count)
{
    uint64_t address = args.buffer_va + offset;
    if (count == 4 && (address & uint64_t(3)) == uint64_t(0))
        return Words(address).data[0];
    uint value = 0;
    for (uint i = 0; i < count; ++i)
        value |= uint(Bytes(address).data[i]) << (8 * i);
    return value;
}

void store_word(uint offset, uint value)
{
    uint64_t address = args.buffer_va + offset;
    if ((address & uint64_t(3)) == uint64_t(0))
        Words(address).data[0] = value;
    else
        for (uint i = 0; i < 4; ++i)
            Bytes(address).data[i] = uint8_t(value >> (8 * i));
}
