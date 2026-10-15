import pixelwrap;

struct AddOp : IPixelOp
{
    float4 value;

    [mutating] void load(StructuredBuffer<uint> params)
    {
        value = wrapParamFloat4(params, 0);
    }

    float4 apply(float4 v, int2 xy)
    {
        return v + value;
    }
}

struct MultiplyOp : IPixelOp
{
    float4 value;

    [mutating] void load(StructuredBuffer<uint> params)
    {
        value = wrapParamFloat4(params, 0);
    }

    float4 apply(float4 v, int2 xy)
    {
        return v * value;
    }
}

// The sign bit and the bit pattern, not a float compare: devices may flush a denormal to zero in
// a comparison, where the CPU orders it above zero and takes its pow.
bool colorMathNotPositive(float f)
{
    const uint u = asuint(f);
    const bool isNaN = (u & 0x7fffffffu) > 0x7f800000u;
    return !isNaN && ((u & 0x80000000u) != 0 || u == 0);
}

struct GammaOp : IPixelOp
{
    float4 exponent;

    [mutating] void load(StructuredBuffer<uint> params)
    {
        exponent = wrapParamFloat4(params, 0);
    }

    float4 apply(float4 v, int2 xy)
    {
        [ForceUnroll]
        for (uint c = 0; c < 4; ++c)
        {
            const float x = v[c];
            const float e = exponent[c];
            if (e == 1.0 || colorMathNotPositive(x))
                continue;
            // SPIR-V Pow is exp2(y * log2(x)), so pow(inf, 0) would be inf * 0 = NaN where
            // std::pow gives 1; a NaN base gives 1 for a zero exponent too.
            if (e == 0.0)
                v[c] = 1.0;
            else if (!isnan(x))
                v[c] = pow(x, e);
        }
        return v;
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void add_wrapped(uint3 tid : SV_DispatchThreadID)
{
    AddOp op;
    runWrapped<AddOp>(op, tid);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void multiply_wrapped(uint3 tid : SV_DispatchThreadID)
{
    MultiplyOp op;
    runWrapped<MultiplyOp>(op, tid);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void gamma_wrapped(uint3 tid : SV_DispatchThreadID)
{
    GammaOp op;
    runWrapped<GammaOp>(op, tid);
}
