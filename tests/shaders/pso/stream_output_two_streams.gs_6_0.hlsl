struct Vertex
{
    float4 position : SV_POSITION;
    float3 arg0 : ARG0;
    float2 arg1 : ARG1;
    uint4 arg2 : ARG2;
};

struct First
{
    float arg0 : ARG0;
};

struct Second
{
    float2 arg1 : ARG1;
};

// Four emitted vertices in total, including the second stream.
[maxvertexcount(4)]
void main(triangle Vertex vertices[3], inout PointStream<First> first,
        inout PointStream<Second> second)
{
    for (uint i = 0; i < 3; ++i)
    {
        First v;
        v.arg0 = vertices[i].arg0.x;
        first.Append(v);
    }
    Second v;
    v.arg1 = vertices[0].arg1;
    second.Append(v);
}
