import gradeops;

StructuredBuffer<float> srcBuf;
RWStructuredBuffer<float> dstBuf;

struct GradeParams
{
    float4 a;
    float4 b;
    float4 gamma;
    float4 invGamma;
    uint width;
    uint height;
    uint nComps;
    uint channelMask;
    uint flags;
};
ConstantBuffer<GradeParams> params;

static const uint kReverse = 1;
static const uint kClampBlack = 2;
static const uint kClampWhite = 4;

[shader("compute")]
[numthreads(8, 8, 1)]
void grade_main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= params.width || tid.y >= params.height)
        return;

    uint n = params.nComps;
    uint base = (tid.y * params.width + tid.x) * n;
    bool reverse = (params.flags & kReverse) != 0;
    bool clampBlack = (params.flags & kClampBlack) != 0;
    bool clampWhite = (params.flags & kClampWhite) != 0;

    for (uint c = 0; c < n && c < 4; ++c)
    {
        float v = srcBuf[base + c];
        // A single-channel image carries alpha, so it takes the alpha knobs.
        uint bit = (n == 1) ? 3 : c;
        if ((params.channelMask & (1u << bit)) != 0)
        {
            float a = params.a[bit];
            float b = params.b[bit];
            float g = params.gamma[bit];
            v = gradeApply(v, a, b, g, params.invGamma[bit], reverse, clampBlack, clampWhite);
        }
        dstBuf[base + c] = v;
    }
}
