// This legal semantic has index 7 but occupies physical output register 0.
float4 main(uint id : SV_VertexID) : __HELIOS_DDI_SO_REGISTER7
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
