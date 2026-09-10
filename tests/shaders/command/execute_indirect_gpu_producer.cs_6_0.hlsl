cbuffer Parameters : register(b0)
{
    uint seed;
    uint count;
    uint2 packet_va;
};
RWByteAddressBuffer Packet : register(u0);

[numthreads(1, 1, 1)]
void main()
{
    Packet.Store2(256, uint2(0, 0));
    Packet.Store2(264, uint2(1, 0));
    Packet.Store(280, count);
    for (uint i = 0; i < 3; i++)
    {
        uint cbv_offset = 512 + i * 256;
        uint lo = packet_va.x + cbv_offset;
        uint hi = packet_va.y + uint(lo < packet_va.x);
        Packet.Store(cbv_offset, seed + i);
        // Root constant, unaligned 64-bit root CBV, then DRAW; stride 28.
        uint record = 284 + i * 28;
        Packet.Store(record, i);
        Packet.Store2(record + 4, uint2(lo, hi));
        Packet.Store4(record + 12, uint4(3, 1, 0, 0));
    }
}
