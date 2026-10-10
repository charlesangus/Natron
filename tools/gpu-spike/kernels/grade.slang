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

// Selects spelled out so NaN lands on the same side as std::max/std::min in
// the engine kernel, which SPIR-V's FMax/FMin leave undefined.
float grade(float v, float a, float b, float gamma, float invGamma)
{
    // One rounding of a*v+b, so cancellation near zero costs no extra error.
    float x = fma(a, v, b);

    if (gamma <= 0.0)
    {
        if (x < 1.0)
            return 0.0;
        else if (x == 1.0)
            return 1.0;
        return float(1.0 / 0.0);
    }
    if (gamma == 1.0)
        return x;
    if (x <= 0.0)
        return x;
    return pow(x, invGamma);
}

float invGrade(float v, float a, float b, float gamma)
{
    if (gamma != 1.0 && v > 0.0)
        v = pow(v, gamma);
    v = v - b;
    if (a != 0.0)
        v /= a;
    return v;
}

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
            v = reverse ? invGrade(v, a, b, g) : grade(v, a, b, g, params.invGamma[bit]);
            if (clampBlack)
                v = (0.0 < v) ? v : 0.0;
            if (clampWhite)
                v = (v < 1.0) ? v : 1.0;
        }
        dstBuf[base + c] = v;
    }
}
