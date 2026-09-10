cbuffer Offset : register(b0) { uint offset; };
cbuffer Multiplier : register(b1) { uint multiplier; };
RWStructuredBuffer<uint> Output : register(u0);
struct Input
{
    float4 position : SV_Position;
    nointerpolation uint value : TEXCOORD0;
};
void main(Input input)
{
    InterlockedAdd(Output[offset], input.value + multiplier);
}
