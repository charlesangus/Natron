RWStructuredBuffer<float> outBuf;

struct Params
{
    float scale;
    uint count;
};
ConstantBuffer<Params> params;

[shader("compute")]
[numthreads(64, 1, 1)]
void smoke(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x < params.count)
        outBuf[tid.x] = float(tid.x) * params.scale;
}
