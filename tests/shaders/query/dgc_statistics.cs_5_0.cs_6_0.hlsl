RWStructuredBuffer<uint> counts : register(u0);
cbuffer Args : register(b0) { uint slot; };

[numthreads(4, 3, 2)]
void main()
{
    InterlockedAdd(counts[slot], 1);
}
