import pixelwrap;

// The Posterize node, with Posterize.cpp written the same way, one rounding per statement and
// the same integer hash, so the CPU and the GPU agree bit for bit.

static const uint kPosterizeNone = 0u;
static const uint kPosterizeOrdered = 1u;
static const uint kPosterizeRandom = 2u;
// The words before the ordered dither's thresholds.
static const uint kPosterizeHeaderWords = 7u;

// PosterizeOp::hash.
uint posterizeHash(uint a)
{
    a = (a ^ 61u) ^ (a >> 16);
    a = a + (a << 3);
    a = a ^ (a >> 4);
    a = a * 0x27d4eb2du;
    a = a ^ (a >> 15);
    return a;
}

uint posterizeWrap(int v, uint side)
{
    const int r = v % int(side);
    return uint(r < 0 ? r + int(side) : r);
}

struct PosterizeOp : IPixelOp
{
    uint kind;
    uint side;
    uint seed;
    float rangeMin;
    float binScale;
    float levelScale;
    float outScale;

    // Parameter block, in words: the dither kind (0 none, 1 ordered, 2 random), the ordered
    // matrix's side, the random seed, rangeMin, binScale, levelScale and outScale, then the
    // ordered matrix's side² thresholds, entry [x mod side][y mod side] at (x mod side)·side +
    // (y mod side).
    [mutating] void load(StructuredBuffer<uint> params)
    {
        kind = params[0];
        side = params[1];
        seed = params[2];
        rangeMin = wrapParamFloat(params, 3);
        binScale = wrapParamFloat(params, 4);
        levelScale = wrapParamFloat(params, 5);
        outScale = wrapParamFloat(params, 6);
    }

    float quantize(float v, int2 xy, uint bit)
    {
        precise float d = v - rangeMin;
        precise float level;
        if (kind == kPosterizeNone)
        {
            precise float t = d * binScale;
            if (t <= 0.0)
                level = floor(t);
            else
            {
                precise float t1 = t - 1.0;
                level = ceil(t1);
            }
        }
        else
        {
            precise float lv = d * levelScale;
            precise float fv = floor(lv);
            precise float f = lv - fv;
            float threshold;
            if (kind == kPosterizeRandom)
            {
                const uint h = posterizeHash(posterizeHash(posterizeHash(seed ^ uint(xy.x)) ^ uint(xy.y)) ^ bit);
                precise float r = float(h >> 8) * (1.0 / 16777216.0);
                threshold = r;
            }
            else
                threshold = asfloat(wrapAux[kPosterizeHeaderWords + posterizeWrap(xy.x, side) * side + posterizeWrap(xy.y, side)]);
            if (f <= threshold)
                level = fv;
            else
                level = fv + 1.0;
        }
        precise float o = level * outScale;
        precise float w = o + rangeMin;
        return w;
    }

    float4 apply(float4 v, int2 xy)
    {
        float4 o;
        [ForceUnroll]
        for (uint c = 0; c < 4; ++c)
            o[c] = quantize(v[c], xy, c);
        return o;
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void posterize_wrapped(uint3 tid : SV_DispatchThreadID)
{
    PosterizeOp op;
    runWrapped<PosterizeOp>(op, tid);
}
