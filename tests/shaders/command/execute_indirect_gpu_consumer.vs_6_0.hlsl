cbuffer Multiplier : register(b1) { uint multiplier; };
struct Output
{
    float4 position : SV_Position;
    nointerpolation uint value : TEXCOORD0;
};
Output main(uint vertex : SV_VertexID)
{
    Output output;
    output.position = float4(float(vertex & 1) * 4.0 - 1.0,
            float(vertex & 2) * 2.0 - 1.0, 0.0, 1.0);
    output.value = multiplier;
    return output;
}
