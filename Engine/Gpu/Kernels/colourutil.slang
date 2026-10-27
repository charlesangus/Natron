import pixelwrap;

// Premult and Unpremult run under a Divisor stage: v is the undivided source and d the
// "(Un)premult by" channel, 1 where there is none. Neither reads its parameter block.

// The colour bit of the divisor channel itself, which both ops copy rather than scale by itself,
// as the CPU row does; -1 when there is none.
int divisorChannelBit()
{
    return wrap.skipChannel < 0 ? -1 : int(wrapChannelBit(wrap.nComps, uint(wrap.skipChannel)));
}

struct PremultOp : IDivisorPixelOp
{
    [mutating] void load(StructuredBuffer<uint> params)
    {
    }

    // A plain product, as the CPU row computes it: a negative or NaN divisor is not clamped.
    float4 apply(float4 v, float d, int2 xy)
    {
        const int skip = divisorChannelBit();
        float4 o;
        [ForceUnroll]
        for (uint c = 0; c < 4; ++c)
        {
            precise float x = v[c] * d;
            o[c] = (int(c) == skip) ? v[c] : x;
        }
        return o;
    }
}

struct UnpremultOp : IDivisorPixelOp
{
    [mutating] void load(StructuredBuffer<uint> params)
    {
    }

    // Divides where d is above FLT_EPSILON; a NaN divisor fails the test and so divides, as in
    // Image::unPremultDivisorIsUsable.
    float4 apply(float4 v, float d, int2 xy)
    {
        if (d <= asfloat(0x34000000u))
            return v;
        const int skip = divisorChannelBit();
        float4 o;
        [ForceUnroll]
        for (uint c = 0; c < 4; ++c)
        {
            precise float x = v[c] / d;
            o[c] = (int(c) == skip) ? v[c] : x;
        }
        return o;
    }
}

struct ColorMatrixOp : IPixelOp
{
    float4 m[4];
    uint flags;

    // Parameter block, in words: the rows R, G, B, A of the matrix over r, g, b, a, then flags
    // (1 clamp black, 2 clamp white, 4 three by three).
    [mutating] void load(StructuredBuffer<uint> params)
    {
        [ForceUnroll]
        for (uint r = 0; r < 4; ++r)
            m[r] = wrapParamFloat4(params, r * 4);
        flags = params[16];
    }

    float4 apply(float4 v, int2 xy)
    {
        // A pixel without alpha reads it as 1, as the CPU row does.
        float4 p = v;
        if (wrap.nComps == 2 || wrap.nComps == 3)
            p.a = 1.0;
        // At 3x3 the alpha row and column are not read and alpha passes through untouched.
        const bool threeByThree = (flags & 4) != 0;
        float4 o;
        o.a = v.a;
        [ForceUnroll]
        for (uint r = 0; r < 4; ++r)
        {
            if (threeByThree && r == 3)
                continue;
            // Summed r, g, b, a with no fused multiply-add, in the CPU row's order.
            precise float s = m[r].x * p.r;
            precise float g = m[r].y * p.g;
            s = s + g;
            precise float b = m[r].z * p.b;
            s = s + b;
            if (!threeByThree)
            {
                precise float a = m[r].w * p.a;
                s = s + a;
            }
            if ((flags & 1) != 0 && s < 0.0)
                s = 0.0;
            else if ((flags & 2) != 0 && s > 1.0)
                s = 1.0;
            o[r] = s;
        }
        return o;
    }
}

struct ThresholdOp : IPixelOp
{
    float4 low;
    float4 high;

    // Parameter block, in words: level - softness per channel, then level + softness per channel.
    [mutating] void load(StructuredBuffer<uint> params)
    {
        low = wrapParamFloat4(params, 0);
        high = wrapParamFloat4(params, 4);
    }

    float4 apply(float4 v, int2 xy)
    {
        float4 o;
        [ForceUnroll]
        for (uint c = 0; c < 4; ++c)
        {
            const float x = v[c];
            // The top end first, so that at softness 0 a value equal to the level gives 1.
            if (x >= high[c])
                o[c] = 1.0;
            else if (x <= low[c])
                o[c] = 0.0;
            else
            {
                precise float t = x - low[c];
                precise float w = high[c] - low[c];
                precise float q = t / w;
                o[c] = q;
            }
        }
        return o;
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void premult_wrapped(uint3 tid : SV_DispatchThreadID)
{
    PremultOp op;
    runWrappedDivisor<PremultOp>(op, tid);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void unpremult_wrapped(uint3 tid : SV_DispatchThreadID)
{
    UnpremultOp op;
    runWrappedDivisor<UnpremultOp>(op, tid);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void colormatrix_wrapped(uint3 tid : SV_DispatchThreadID)
{
    ColorMatrixOp op;
    runWrapped<ColorMatrixOp>(op, tid);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void threshold_wrapped(uint3 tid : SV_DispatchThreadID)
{
    ThresholdOp op;
    runWrapped<ThresholdOp>(op, tid);
}
