// FXC SM5 and DXC SM6 use the same source; no shader-model override.
cbuffer Args : register(b0)
{
    float4 rectangle;
    uint mode;
    uint samples;
    uint width;
    uint height;
};
RWByteAddressBuffer data : register(u1);
Texture2DMS<float4> image : register(t0);
Texture2D<float4> single_image : register(t0);

struct Vertex
{
    float4 position : SV_Position;
    noperspective float2 pixel : TEXCOORD0;
};

Vertex vs_main(uint id : SV_VertexID)
{
    uint corner = id == 0 || id == 3 ? 0 : id == 1 ? 1 : id == 5 ? 2 : 3;
    float2 p = float2(corner & 1 ? rectangle.z : rectangle.x, corner & 2 ? rectangle.w : rectangle.y);
    Vertex result;
    result.position = float4(2 * p.x / width - 1, 1 - 2 * p.y / height, 0, 1);
    result.pixel = p;
    return result;
}

float4 shade(Vertex v, uint coverage)
{
    uint x = uint(v.position.x), y = uint(v.position.y);
    uint offset = 16 * (y * width + x);
    uint previous;
    data.InterlockedAdd(offset, 1, previous);
    data.InterlockedOr(offset + 4, coverage, previous);
    uint raster_samples = GetRenderTargetSampleCount();
    data.Store(offset + 8, raster_samples);
    data.Store(offset + 12, asuint(EvaluateAttributeAtSample(v.pixel.x, 0)));
    if (mode == 3 && (x & 1))
        discard;
    return float4(0.25, 0.5, 0.75, mode == 4 ? 0 : mode == 5 ? 1 :
            mode == 6 ? 0.25 : mode == 7 ? 0.75 : mode == 8 ? asfloat(0x7fc00000) : 0.5);
}

float4 ps_main(Vertex v, uint coverage : SV_Coverage) : SV_Target
{
    return shade(v, coverage);
}

struct MaskOutput
{
    float4 color : SV_Target;
    uint mask : SV_Coverage;
};
MaskOutput mask_main(Vertex v, uint coverage : SV_Coverage)
{
    MaskOutput result;
    result.color = shade(v, coverage);
    result.mask = mode == 2 ? 0 : ((uint(v.position.x) + uint(v.position.y)) & 1) ? 1 : 10;
    return result;
}

[numthreads(8, 8, 1)]
void cs_main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height)
        return;
    uint offset = 16 * width * height + 16 * samples * (id.y * width + id.x);
    for (uint s = 0; s < samples; s++)
        data.Store4(offset + 16 * s, asuint(image.Load(id.xy, s)));
}

[numthreads(8, 8, 1)]
void single_cs_main(uint3 id : SV_DispatchThreadID)
{
    if (id.x < width && id.y < height)
        data.Store4(16 * width * height + 16 * (id.y * width + id.x), asuint(single_image.Load(int3(id.xy, 0))));
}

struct MRTOutput { float4 a : SV_Target0; float4 b : SV_Target1; };
MRTOutput mrt_main()
{
    MRTOutput result;
    result.a = float4(1, 2, 3, 4);
    result.b = float4(8, 16, 32, 64);
    return result;
}

struct UIntOutput { uint4 a : SV_Target0; uint4 b : SV_Target1; };
UIntOutput uint_main()
{
    UIntOutput result;
    result.a = uint4(1, 2, 3, 4);
    result.b = uint4(8, 16, 32, 64);
    return result;
}

float4 sample_main(uint sample_index : SV_SampleIndex) : SV_Target { return sample_index; }
float depth_main() : SV_Depth { return 0.5; }
