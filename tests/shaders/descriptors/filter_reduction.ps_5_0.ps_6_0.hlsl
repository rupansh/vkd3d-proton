Texture2D<float> texture0 : register(t0);
SamplerState sampler0 : register(s0);

float main(float4 position : SV_Position) : SV_Target
{
    uint pixel = uint(position.x);
    // Whole bilinear footprint, two mip levels, then exact texel centers.
    float2 uv = pixel < 3 ? float2(0.5, 0.5) : float2(pixel == 3 ? 0.25 : 0.75, 0.25);
    float lod = pixel < 3 ? 0.5 * pixel : 0.0;
    return texture0.SampleLevel(sampler0, uv, lod);
}
