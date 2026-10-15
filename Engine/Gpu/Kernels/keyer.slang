// Keyer::render()'s per-pixel key, despill and output. KeyerGpuParams and KeyerGpuPush in
// Engine/Nodes/Keyer/Keyer.cpp mirror KeyerParams and KeyerPush, which have no padding between
// members.

// Pixels of a view live at offset + (y - y1) * stride + (x - x1) * comps floats, for (x, y) in
// [x1, x2) x [y1, y2); a view of zero components is an absent input and reads zero.
struct KeyerView
{
    uint offset;
    uint stride;
    uint comps;
    // The component a mask reads.
    uint channel;
    int x1;
    int y1;
    int x2;
    int y2;
};

// The host derives every threshold, flag and key-colour term in double, from the same values
// KeyKernel holds, so that the kernel compares and scales in float what the CPU does in double.
struct KeyerParams
{
    KeyerView src;
    KeyerView inMask;
    KeyerView outMask;
    KeyerView bg;
    uint mode;
    uint show;
    uint sourceAlpha;
    uint flags;
    uint channelMask;
    uint lumMax;
    uint lumAverage;
    float lumR;
    float lumG;
    float lumB;
    // center + toleranceLower + softnessLower, center + toleranceLower, -softnessLower.
    float lowerA;
    float lowerB;
    float negSoftnessLower;
    // center + toleranceUpper, center + toleranceUpper + softnessUpper, softnessUpper.
    float upperC;
    float upperD;
    float softnessUpper;
    // The key colour over its double-precision norm.
    float unitR;
    float unitG;
    float unitB;
    // That norm over the single-precision component sum, so that the projection on the unit key
    // times it is the scalar product over the sum.
    float sumRatio;
    // 1 - norm^2 / float norm^2: the CPU subtracts scalarProd^2 over the float norm^2, which
    // leaves this much of the squared projection in the orthogonal distance.
    float projectionResidue;
    // The norm over the square root of the float norm^2, the CPU's despill divisor.
    float despillGain;
    float keyNorm;
    // The key colour over keyNorm.
    float shiftR;
    float shiftG;
    float shiftB;
    float despillLower;
    float despillUpper;
    float despillClosing;
};

struct KeyerPush
{
    int outX;
    int outY;
    uint width;
    uint height;
    uint dstOffset;
    uint dstStride;
};

[[vk::binding(0, 0)]] StructuredBuffer<float> keyerSrc;
[[vk::binding(1, 0)]] StructuredBuffer<float> keyerInMask;
[[vk::binding(2, 0)]] StructuredBuffer<float> keyerOutMask;
[[vk::binding(3, 0)]] StructuredBuffer<float> keyerBg;
[[vk::binding(4, 0)]] RWStructuredBuffer<float> keyerDst;
[[vk::binding(5, 0)]] StructuredBuffer<KeyerParams> keyerParams;
[[vk::push_constant]] ConstantBuffer<KeyerPush> keyerPush;

static const uint kModeLuminance = 0;
static const uint kModeColor = 1;
static const uint kModeScreen = 2;
static const uint kModeNone = 3;

static const uint kShowIntermediate = 0;
static const uint kShowUnpremultiplied = 2;
static const uint kShowComposite = 3;

static const uint kSourceAlphaInsideMask = 1;
static const uint kSourceAlphaNormal = 2;

static const uint kFlagSrcHasAlpha = 1;
static const uint kFlagLowerBNonPositive = 2;
static const uint kFlagSoftnessLowerNegative = 4;
static const uint kFlagUpperCAtLeastOne = 8;
static const uint kFlagSoftnessUpperPositive = 16;
static const uint kFlagKeyFromLuminance = 32;
static const uint kFlagProjectOut = 64;
static const uint kFlagDespill = 128;

// std::min and std::max, whose ordering decides which operand a NaN yields.
float keyerMin(float a, float b)
{
    return (b < a) ? b : a;
}

float keyerMax(float a, float b)
{
    return (a < b) ? b : a;
}

float keyerBound(float v)
{
    // std::numeric_limits<float>::max() / 4.
    const float bound = asfloat(0x7e7fffffu);
    if (v > bound)
        return bound;
    if (v < -bound)
        return -bound;
    return v;
}

bool keyerInside(KeyerView v, int2 p)
{
    return v.comps != 0 && p.x >= v.x1 && p.x < v.x2 && p.y >= v.y1 && p.y < v.y2;
}

uint keyerIndex(KeyerView v, int2 p)
{
    return v.offset + uint(p.y - v.y1) * v.stride + uint(p.x - v.x1) * v.comps;
}

float keyerLuminance(KeyerParams k, float3 c)
{
    if (k.lumMax != 0)
        return keyerMax(keyerMax(c.r, c.g), c.b);
    if (k.lumAverage != 0)
        return (c.r + c.g + c.b) / 3.0;
    return k.lumR * c.r + k.lumG * c.g + k.lumB * c.b;
}

float keyerBackground(KeyerParams k, float kfg)
{
    if ((k.flags & kFlagLowerBNonPositive) != 0 && kfg <= 0.0)
        return 1.0;
    if (kfg < k.lowerA)
        return 0.0;
    if (kfg < k.lowerB && (k.flags & kFlagSoftnessLowerNegative) != 0)
        return (kfg - k.lowerA) / k.negSoftnessLower;
    if (kfg <= k.upperC)
        return 1.0;
    if ((k.flags & kFlagUpperCAtLeastOne) != 0 && 1.0 <= kfg)
        return 1.0;
    if (kfg < k.upperD && (k.flags & kFlagSoftnessUpperPositive) != 0)
        return (k.upperD - kfg) / k.softnessUpper;
    return 0.0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void keyer_main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= keyerPush.width || tid.y >= keyerPush.height)
        return;
    const KeyerParams k = keyerParams[0];
    const int2 p = int2(keyerPush.outX + int(tid.x), keyerPush.outY + int(tid.y));
    const bool srcHasAlpha = (k.flags & kFlagSrcHasAlpha) != 0;

    const bool hasSrc = keyerInside(k.src, p);
    float4 src = float4(0.0, 0.0, 0.0, 0.0);
    if (hasSrc)
    {
        const uint i = keyerIndex(k.src, p);
        src.r = keyerSrc[i];
        src.g = keyerSrc[i + 1];
        src.b = keyerSrc[i + 2];
        if (srcHasAlpha)
            src.a = keyerSrc[i + 3];
    }
    float3 bg = float3(0.0, 0.0, 0.0);
    if (keyerInside(k.bg, p))
    {
        const uint i = keyerIndex(k.bg, p);
        bg = float3(keyerBg[i], keyerBg[i + 1], keyerBg[i + 2]);
    }
    float inMask = keyerInside(k.inMask, p) ? keyerInMask[keyerIndex(k.inMask, p) + k.inMask.channel] : 0.0;
    float outMask = keyerInside(k.outMask, p) ? keyerOutMask[keyerIndex(k.outMask, p) + k.outMask.channel] : 0.0;

    if (k.sourceAlpha == kSourceAlphaInsideMask && hasSrc && srcHasAlpha)
        inMask = keyerMax(inMask, src.a);
    inMask = keyerMax(0.0, keyerMin(inMask, 1.0));
    outMask = keyerMax(0.0, keyerMin(outMask, 1.0));

    float kbg = 0.0;
    float3 fg = float3(0.0, 0.0, 0.0);
    if (!hasSrc || outMask >= 1.0)
    {
        kbg = 1.0;
    }
    else
    {
        fg = float3(keyerBound(src.r), keyerBound(src.g), keyerBound(src.b));

        // The projection on the key and the distance to its axis are taken on a power-of-two
        // scaled colour, so that the squares stay in float range.
        float scale = 1.0;
        const float m = keyerMax(keyerMax(abs(fg.r), abs(fg.g)), abs(fg.b));
        if (m > asfloat(0x5d800000u))
            scale = asfloat(0x1f800000u);
        const float3 fs = fg * scale;
        const float3 unit = float3(k.unitR, k.unitG, k.unitB);
        const float projection = dot(fs, unit);
        float d = 0.0;
        if (k.mode == kModeScreen || k.mode == kModeNone)
        {
            float residue;
            if ((k.flags & kFlagProjectOut) != 0)
            {
                // The orthogonal part itself rather than |fg|^2 - projection^2, which float
                // cannot hold for colours near the key axis.
                const float3 r = fs - projection * unit;
                residue = dot(r, r) + projection * projection * k.projectionResidue;
            }
            else
            {
                residue = dot(fs, fs);
            }
            d = sqrt(keyerMax(0.0, residue)) / scale;
        }
        const float proj = projection / scale;

        float kfg = 0.0;
        if (k.mode == kModeLuminance)
            kfg = keyerLuminance(k, fg);
        else if (k.mode == kModeColor || k.mode == kModeScreen)
        {
            kfg = (k.flags & kFlagKeyFromLuminance) != 0 ? keyerLuminance(k, fg) : proj * k.sumRatio;
            if (k.mode == kModeScreen)
                kfg -= d;
        }

        kbg = (k.mode == kModeNone) ? 1.0 : keyerBackground(k, kfg);
        if (inMask > 0.0 && kbg > 1.0 - inMask)
            kbg = 1.0 - inMask;
        if (outMask > 0.0 && kbg < outMask)
            kbg = outMask;

        if ((k.flags & kFlagDespill) != 0)
        {
            const float along = proj * k.despillGain - d * k.despillClosing;
            if (along > 0.0)
            {
                const float maxDespill = kbg * k.despillLower + (1.0 - kbg) * k.despillUpper;
                const float shift = keyerMin(maxDespill * keyerMax(k.keyNorm, along), along);
                fg -= shift * float3(k.shiftR, k.shiftG, k.shiftB);
            }
        }

        if (k.show != kShowUnpremultiplied)
            fg *= (1.0 - kbg);
        fg = float3(keyerMax(0.0, keyerMin(fg.r, 1.0)), keyerMax(0.0, keyerMin(fg.g, 1.0)), keyerMax(0.0, keyerMin(fg.b, 1.0)));
    }

    float4 result;
    result.a = 1.0 - kbg;
    if (k.show == kShowIntermediate)
        result.rgb = src.rgb;
    else if (k.show == kShowComposite)
    {
        const float compAlpha = (k.sourceAlpha == kSourceAlphaNormal && hasSrc && srcHasAlpha) ? src.a : 1.0;
        result.rgb = compAlpha * (fg + bg * kbg) + (1.0 - compAlpha) * bg;
    }
    else
        result.rgb = fg;

    const uint base = keyerPush.dstOffset + tid.y * keyerPush.dstStride + tid.x * 4;
    [ForceUnroll]
    for (uint c = 0; c < 4; ++c)
    {
        if ((k.channelMask & (1u << c)) != 0)
            keyerDst[base + c] = result[c];
        else
            keyerDst[base + c] = (hasSrc && (c < 3 || srcHasAlpha)) ? src[c] : 0.0;
    }
}
