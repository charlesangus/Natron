import curvesnapshot;
import pixelwrap;

// Parameter block, in words. Every offset is mirrored in ColorCorrect.cpp.
static const uint CC_FLAGS = 0;
static const uint CC_WEIGHTS = 1;
static const uint CC_TABLE_RANGE = 4;
static const uint CC_KEY_COUNT = 10;
static const uint CC_Y_MIN = 12;
static const uint CC_Y_MAX = 14;
// Per group (master, shadows, midtones, highlights): saturation, contrast, 1 / gamma, gain and
// offset, four channels each.
static const uint CC_GROUPS = 16;
static const uint CC_GROUP_WORDS = 20;
static const uint CC_HEADER_WORDS = 96;
static const uint CC_LUT_BASE = CC_HEADER_WORDS;
static const uint CC_CURVE_BASE = CC_LUT_BASE + 2 * CS_TABLE_SIZE;

static const uint CC_CLAMP_BLACK = 1;
static const uint CC_CLAMP_WHITE = 2;
static const uint CC_LUMA_MAX = 4;
static const uint CC_ACTIVE_SHIFT = 4;

static const uint CC_MASTER = 0;
static const uint CC_SHADOWS = 1;
static const uint CC_MIDTONES = 2;
static const uint CC_HIGHLIGHTS = 3;

// The sign bit and the bit pattern, not a float compare: devices may flush a denormal to zero in
// a comparison, where the CPU's double takes it as above zero.
bool ccPositive(float f)
{
    const uint u = asuint(f);
    return (u & 0x80000000u) == 0 && u != 0 && (u & 0x7fffffffu) <= 0x7f800000u;
}

// std::pow for a positive x. SPIR-V Pow is exp2(y * log2(x)): pow(inf, 0) and pow(1, inf) would be
// inf * 0 = NaN where std::pow gives 1.
float ccPow(float x, float y)
{
    if (y == 0.0 || x == 1.0)
        return 1.0;
    return pow(x, y);
}

// The tables are read from the bound parameter buffer, wrapAux, since an op cannot hold a buffer.
struct ColorCorrectOp : IPixelOp
{
    uint flags;
    float3 weights;
    CurveTableRange range;

    [mutating] void load(StructuredBuffer<uint> params)
    {
        flags = params[CC_FLAGS];
        weights = float3(wrapParamFloat(params, CC_WEIGHTS), wrapParamFloat(params, CC_WEIGHTS + 1),
                         wrapParamFloat(params, CC_WEIGHTS + 2));
        range.load(params, CC_TABLE_RANGE);
    }

    float clampOut(float v)
    {
        if ((flags & CC_CLAMP_BLACK) != 0 && v < 0.0)
            return 0.0;
        else if ((flags & CC_CLAMP_WHITE) != 0 && v > 1.0)
            return 1.0;
        return v;
    }

    float luminance(float3 c)
    {
        return (flags & CC_LUMA_MAX) != 0 ? max(max(c.r, c.g), c.b) : dot(weights, c);
    }

    bool isActive(uint group)
    {
        return (flags & (1u << (CC_ACTIVE_SHIFT + group))) != 0;
    }

    // The weight of tone curve `curve` at luminance `value`: the table inside the range, the curve
    // itself outside it, both clamped as the output is.
    float toneWeight(uint curve, float value)
    {
        if (range.outside(value))
            return clampOut(curveSnapshotAt(CC_CURVE_BASE + curve * CS_CURVE_WORDS, wrapAux[CC_KEY_COUNT + curve],
                                            asfloat(wrapAux[CC_Y_MIN + curve]), asfloat(wrapAux[CC_Y_MAX + curve]), value));
        return range.at(CC_LUT_BASE + curve * CS_TABLE_SIZE, value);
    }

    float4 applyGroup(uint group, bool4 process, float4 p)
    {
        const uint base = CC_GROUPS + group * CC_GROUP_WORDS;
        const float4 saturation = wrapParamFloat4(wrapAux, base);
        const float4 contrast = wrapParamFloat4(wrapAux, base + 4);
        const float4 invGamma = wrapParamFloat4(wrapAux, base + 8);
        const float4 gain = wrapParamFloat4(wrapAux, base + 12);
        const float4 offset = wrapParamFloat4(wrapAux, base + 16);

        // One luminance for all three channels, taken before any of them changes.
        const float l = luminance(p.rgb);
        [ForceUnroll]
        for (uint i = 0; i < 3; ++i)
        {
            if (process[i] && saturation[i] != 1.0)
                p[i] = (1.0 - saturation[i]) * l + saturation[i] * p[i];
        }
        [ForceUnroll]
        for (uint i = 0; i < 4; ++i)
        {
            if (!process[i])
                continue;
            float v = p[i];
            if (contrast[i] != 1.0 && ccPositive(v))
                v = ccPow(v / 0.18, contrast[i]) * 0.18;
            if (invGamma[i] != 1.0 && ccPositive(v))
                v = ccPow(v, invGamma[i]);
            if (gain[i] != 1.0)
                v = v * gain[i];
            if (offset[i] != 0.0)
                v = v + offset[i];
            p[i] = v;
        }
        return p;
    }

    float4 apply(float4 v, int2 xy)
    {
        // The channels the CPU kernel processes: those selected that the pixel has. An unprocessed
        // channel keeps its value through the groups, where the master group's luminance reads it.
        const uint present = wrap.nComps == 1 ? 8u : ((1u << min(wrap.nComps, 4u)) - 1u);
        const uint selected = wrap.channelMask & present;
        const bool4 process = bool4((selected & 1u) != 0, (selected & 2u) != 0, (selected & 4u) != 0, (selected & 8u) != 0);

        const float l = luminance(v.rgb);
        const float sScale = toneWeight(0, l);
        const float hScale = toneWeight(1, l);
        const float mScale = 1.0 - sScale - hScale;

        float4 s = v;
        float4 m = v;
        float4 h = v;
        if (isActive(CC_SHADOWS))
            s = applyGroup(CC_SHADOWS, process, s);
        if (isActive(CC_MIDTONES))
            m = applyGroup(CC_MIDTONES, process, m);
        if (isActive(CC_HIGHLIGHTS))
            h = applyGroup(CC_HIGHLIGHTS, process, h);
        float4 p = v;
        [ForceUnroll]
        for (uint i = 0; i < 4; ++i)
        {
            if (process[i])
                p[i] = s[i] * sScale + m[i] * mScale + h[i] * hScale;
        }
        if (isActive(CC_MASTER))
            p = applyGroup(CC_MASTER, process, p);
        [ForceUnroll]
        for (uint i = 0; i < 4; ++i)
        {
            if (process[i])
                p[i] = clampOut(p[i]);
        }
        return p;
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void colorcorrect_wrapped(uint3 tid : SV_DispatchThreadID)
{
    ColorCorrectOp op;
    runWrapped<ColorCorrectOp>(op, tid);
}
