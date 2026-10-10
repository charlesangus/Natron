import gradeops;

StructuredBuffer<float4> srcBuf;
RWStructuredBuffer<float4> dstBuf;
StructuredBuffer<float> maskBuf;

struct PointParams
{
    float4 a;
    float4 b;
    float4 gamma;
    float4 invGamma;
    uint width;
    uint height;
    uint channelMask;
    uint flags;
    float mixAmount;
    uint premult;
};
ConstantBuffer<PointParams> params;

interface IPixelOp
{
    float4 apply(float4 v, uint2 xy);
}

struct GradeOp : IPixelOp
{
    float4 a;
    float4 b;
    float4 gamma;
    float4 invGamma;
    uint channelMask;
    uint flags;

    float4 apply(float4 v, uint2 xy)
    {
        const bool reverse = (flags & 1) != 0;
        const bool clampBlack = (flags & 2) != 0;
        const bool clampWhite = (flags & 4) != 0;
        [ForceUnroll]
        for (uint c = 0; c < 4; ++c)
        {
            if ((channelMask & (1u << c)) != 0)
                v[c] = gradeApply(v[c], a[c], b[c], gamma[c], invGamma[c],
                                  reverse, clampBlack, clampWhite);
        }
        return v;
    }
}

struct InvertOp : IPixelOp
{
    float4 apply(float4 v, uint2 xy)
    {
        return float4(1.0 - v.rgb, v.a);
    }
}

struct Chain<A : IPixelOp, B : IPixelOp> : IPixelOp
{
    A first;
    B second;

    float4 apply(float4 v, uint2 xy)
    {
        return second.apply(first.apply(v, xy), xy);
    }
}

struct Masked<Op : IPixelOp> : IPixelOp
{
    Op op;
    StructuredBuffer<float> mask;
    uint width;
    float mixAmount;
    bool premult;

    float4 apply(float4 v, uint2 xy)
    {
        float4 in = v;
        if (premult && v.a != 0.0)
            in = float4(v.rgb / v.a, v.a);
        float4 out = op.apply(in, xy);
        if (premult)
            out = float4(out.rgb * v.a, out.a);
        return lerp(v, out, mask[xy.y * width + xy.x] * mixAmount);
    }
}

void runPointOp<Op : IPixelOp>(Op op, uint3 tid)
{
    if (tid.x >= params.width || tid.y >= params.height)
        return;
    const uint i = tid.y * params.width + tid.x;
    dstBuf[i] = op.apply(srcBuf[i], tid.xy);
}

GradeOp makeGrade()
{
    GradeOp g;
    g.a = params.a;
    g.b = params.b;
    g.gamma = params.gamma;
    g.invGamma = params.invGamma;
    g.channelMask = params.channelMask;
    g.flags = params.flags;
    return g;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void pointop_grade(uint3 tid : SV_DispatchThreadID)
{
    runPointOp<GradeOp>(makeGrade(), tid);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void pointop_invert(uint3 tid : SV_DispatchThreadID)
{
    InvertOp op;
    runPointOp<InvertOp>(op, tid);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void pointop_chain(uint3 tid : SV_DispatchThreadID)
{
    Chain<GradeOp, InvertOp> op;
    op.first = makeGrade();
    runPointOp<Chain<GradeOp, InvertOp>>(op, tid);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void pointop_masked(uint3 tid : SV_DispatchThreadID)
{
    Masked<GradeOp> op;
    op.op = makeGrade();
    op.mask = maskBuf;
    op.width = params.width;
    op.mixAmount = params.mixAmount;
    op.premult = params.premult != 0;
    runPointOp<Masked<GradeOp>>(op, tid);
}
