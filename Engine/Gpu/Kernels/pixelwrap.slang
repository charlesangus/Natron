import gradeops;

// The per-pixel wrapping of a CPU point op: divide by the "(Un)premult by" channel,
// the op, multiply back, blend with the undivided source by mask x mix, and pass unprocessed
// channels through from the undivided source. GpuWrapParams in Engine/Gpu/GpuWrap.h mirrors
// WrapParams.

// Pixels of a view live at offset + (y - y1) * stride + (x - x1) * comps floats, for (x, y) in
// [x1, x2) x [y1, y2). The destination and the filtered result have no bounds: their offsets
// address the output origin and they cover the whole output rect.
struct WrapParams
{
    int outX;
    int outY;
    uint width;
    uint height;
    uint srcOffset;
    uint srcStride;
    int srcX1;
    int srcY1;
    int srcX2;
    int srcY2;
    uint dstOffset;
    uint dstStride;
    uint maskOffset;
    uint maskStride;
    int maskX1;
    int maskY1;
    int maskX2;
    int maskY2;
    uint auxOffset;
    uint auxStride;
    uint nComps;
    // In the bit space of pixelKernelChannelBit: a one-channel pixel is alpha, bit 3.
    uint channelMask;
    uint maskChannel;
    uint maskComps;
    // Component indices into the source pixel, -1 for none.
    int divisorChannel;
    int skipChannel;
    float mix;
    uint flags;
};

[[vk::binding(0, 0)]] StructuredBuffer<float> wrapSrc;
[[vk::binding(1, 0)]] RWStructuredBuffer<float> wrapDst;
[[vk::binding(2, 0)]] StructuredBuffer<float> wrapMask;
// The op's parameter block, or the filtered result for wrap_finish.
[[vk::binding(3, 0)]] StructuredBuffer<uint> wrapAux;
[[vk::push_constant]] ConstantBuffer<WrapParams> wrap;

static const uint kWrapMask = 1;
static const uint kWrapMaskInvert = 2;

// The op sees and returns pixels in channel-bit order, absent channels reading as zero, and only
// the channels in channelMask of its result are kept.
interface IPixelOp
{
    [mutating] void load(StructuredBuffer<uint> params);
    float4 apply(float4 v, int2 xy);
}

float wrapParamFloat(StructuredBuffer<uint> params, uint index)
{
    return asfloat(params[index]);
}

float4 wrapParamFloat4(StructuredBuffer<uint> params, uint index)
{
    return float4(asfloat(params[index]), asfloat(params[index + 1]), asfloat(params[index + 2]),
                  asfloat(params[index + 3]));
}

uint wrapChannelBit(uint nComps, uint c)
{
    return nComps == 1 ? 3 : c;
}

bool wrapInside(int2 p, int x1, int y1, int x2, int y2)
{
    return p.x >= x1 && p.x < x2 && p.y >= y1 && p.y < y2;
}

float4 wrapReadSource(int2 p)
{
    float4 v = float4(0.0, 0.0, 0.0, 0.0);
    if (!wrapInside(p, wrap.srcX1, wrap.srcY1, wrap.srcX2, wrap.srcY2))
        return v;
    const uint n = wrap.nComps;
    const uint base = wrap.srcOffset + uint(p.y - wrap.srcY1) * wrap.srcStride + uint(p.x - wrap.srcX1) * n;
    for (uint c = 0; c < n && c < 4; ++c)
        v[wrapChannelBit(n, c)] = wrapSrc[base + c];
    return v;
}

bool wrapHasDivisor()
{
    return wrap.divisorChannel >= 0;
}

float wrapDivisor(int2 p, float4 src)
{
    if (!wrapInside(p, wrap.srcX1, wrap.srcY1, wrap.srcX2, wrap.srcY2))
        return 1.0;
    return src[wrapChannelBit(wrap.nComps, uint(wrap.divisorChannel))];
}

float4 wrapDivide(float4 v, float d)
{
    // FLT_EPSILON. A NaN divisor fails this test and so divides, as in Image::unPremultDivisorIsUsable.
    if (d <= asfloat(0x34000000u))
        return v;
    const uint n = wrap.nComps;
    for (uint c = 0; c < n && c < 4; ++c)
    {
        if (int(c) != wrap.skipChannel)
        {
            const uint bit = wrapChannelBit(n, c);
            precise float q = v[bit] / d;
            v[bit] = q;
        }
    }
    return v;
}

float wrapMaskValue(int2 p)
{
    if (!wrapInside(p, wrap.maskX1, wrap.maskY1, wrap.maskX2, wrap.maskY2))
        return 0.0;
    return wrapMask[wrap.maskOffset + uint(p.y - wrap.maskY1) * wrap.maskStride
                    + uint(p.x - wrap.maskX1) * wrap.maskComps + wrap.maskChannel];
}

// Image::premultiplyValue's std::max(0, d) yields 0 for a NaN divisor, which GLSL max leaves undefined.
float wrapPremultiply(float v, float d)
{
    precise float r = v * ((0.0 < d) ? d : 0.0);
    return r;
}

void wrapWrite(uint2 tid, int2 p, float4 src, float4 result, float d)
{
    precise float alpha = wrap.mix;
    if ((wrap.flags & kWrapMask) != 0)
    {
        const float m = wrapMaskValue(p);
        precise float scale = (wrap.flags & kWrapMaskInvert) != 0 ? 1.0 - m : m;
        alpha = scale * wrap.mix;
    }
    const uint n = wrap.nComps;
    const uint base = wrap.dstOffset + tid.y * wrap.dstStride + tid.x * n;
    for (uint c = 0; c < n && c < 4; ++c)
    {
        const uint bit = wrapChannelBit(n, c);
        const float s = src[bit];
        if ((wrap.channelMask & (1u << bit)) == 0)
        {
            wrapDst[base + c] = s;
            continue;
        }
        precise float v = result[bit];
        if (wrapHasDivisor() && int(c) != wrap.skipChannel)
            v = wrapPremultiply(v, d);
        if (alpha == 0.0)
            v = s;
        else if (alpha != 1.0)
        {
            precise float blended = v * alpha + (1.0 - alpha) * s;
            v = blended;
        }
        wrapDst[base + c] = v;
    }
}

bool wrapOutside(uint3 tid)
{
    return tid.x >= wrap.width || tid.y >= wrap.height;
}

int2 wrapPixel(uint3 tid)
{
    return int2(wrap.outX + int(tid.x), wrap.outY + int(tid.y));
}

void runWrapped<Op : IPixelOp>(Op op, uint3 tid)
{
    if (wrapOutside(tid))
        return;
    const int2 p = wrapPixel(tid);
    const float4 src = wrapReadSource(p);
    float d = 1.0;
    float4 divided = src;
    if (wrapHasDivisor())
    {
        d = wrapDivisor(p, src);
        divided = wrapDivide(src, d);
    }
    Op o = op;
    o.load(wrapAux);
    wrapWrite(tid.xy, p, src, o.apply(divided, p), d);
}

// An op under a Divisor stage: it sees the undivided source and the "(Un)premult by" channel (1
// where there is none, and outside the source), and nothing is multiplied back.
interface IDivisorPixelOp
{
    [mutating] void load(StructuredBuffer<uint> params);
    float4 apply(float4 v, float d, int2 xy);
}

void runWrappedDivisor<Op : IDivisorPixelOp>(Op op, uint3 tid)
{
    if (wrapOutside(tid))
        return;
    const int2 p = wrapPixel(tid);
    const float4 src = wrapReadSource(p);
    const float d = wrapHasDivisor() ? wrapDivisor(p, src) : 1.0;
    Op o = op;
    o.load(wrapAux);
    // Multiplying back by 1 leaves the result exactly as the op wrote it.
    wrapWrite(tid.xy, p, src, o.apply(src, d, p), 1.0);
}

// The source over the output rect, divided where the divisor is usable, for a spatial filter to
// read; every channel is written, processed or not.
[shader("compute")]
[numthreads(8, 8, 1)]
void wrap_divide(uint3 tid : SV_DispatchThreadID)
{
    if (wrapOutside(tid))
        return;
    const int2 p = wrapPixel(tid);
    float4 v = wrapReadSource(p);
    if (wrapHasDivisor())
        v = wrapDivide(v, wrapDivisor(p, v));
    const uint n = wrap.nComps;
    const uint base = wrap.dstOffset + tid.y * wrap.dstStride + tid.x * n;
    for (uint c = 0; c < n && c < 4; ++c)
        wrapDst[base + c] = v[wrapChannelBit(n, c)];
}

// The filtered result in the op-params binding, laid out like the destination, finished against
// the undivided source. With mix 0 and no mask it copies the source.
[shader("compute")]
[numthreads(8, 8, 1)]
void wrap_finish(uint3 tid : SV_DispatchThreadID)
{
    if (wrapOutside(tid))
        return;
    const int2 p = wrapPixel(tid);
    const float4 src = wrapReadSource(p);
    const float d = wrapHasDivisor() ? wrapDivisor(p, src) : 1.0;
    const uint n = wrap.nComps;
    const uint base = wrap.auxOffset + tid.y * wrap.auxStride + tid.x * n;
    float4 filtered = float4(0.0, 0.0, 0.0, 0.0);
    for (uint c = 0; c < n && c < 4; ++c)
        filtered[wrapChannelBit(n, c)] = asfloat(wrapAux[base + c]);
    wrapWrite(tid.xy, p, src, filtered, d);
}

struct WrapIdentity : IPixelOp
{
    [mutating] void load(StructuredBuffer<uint> params) {}

    float4 apply(float4 v, int2 xy)
    {
        return v;
    }
}

struct WrapScale : IPixelOp
{
    [mutating] void load(StructuredBuffer<uint> params) {}

    float4 apply(float4 v, int2 xy)
    {
        return v * 2.0;
    }
}

struct GradeOp : IPixelOp
{
    float4 a;
    float4 b;
    float4 gamma;
    float4 invGamma;
    uint channelMask;
    uint flags;

    // Parameter block, in words: a, b, gamma, invGamma (4 each), channelMask, flags.
    [mutating] void load(StructuredBuffer<uint> params)
    {
        a = wrapParamFloat4(params, 0);
        b = wrapParamFloat4(params, 4);
        gamma = wrapParamFloat4(params, 8);
        invGamma = wrapParamFloat4(params, 12);
        channelMask = params[16];
        flags = params[17];
    }

    float4 apply(float4 v, int2 xy)
    {
        const bool reverse = (flags & 1) != 0;
        const bool clampBlack = (flags & 2) != 0;
        const bool clampWhite = (flags & 4) != 0;
        [ForceUnroll]
        for (uint c = 0; c < 4; ++c)
        {
            if ((channelMask & (1u << c)) != 0)
                v[c] = gradeApply(v[c], a[c], b[c], gamma[c], invGamma[c], reverse, clampBlack, clampWhite);
        }
        return v;
    }
}

struct WrapInvert : IPixelOp
{
    [mutating] void load(StructuredBuffer<uint> params) {}

    float4 apply(float4 v, int2 xy)
    {
        return float4(1.0 - v.rgb, v.a);
    }
}

// Both ops load from the same parameter block, so only one of them may read it.
struct Chain<A : IPixelOp, B : IPixelOp> : IPixelOp
{
    A first;
    B second;

    [mutating] void load(StructuredBuffer<uint> params)
    {
        first.load(params);
        second.load(params);
    }

    float4 apply(float4 v, int2 xy)
    {
        return second.apply(first.apply(v, xy), xy);
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void pixelwrap_grade(uint3 tid : SV_DispatchThreadID)
{
    GradeOp op;
    runWrapped<GradeOp>(op, tid);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void grade_wrapped(uint3 tid : SV_DispatchThreadID)
{
    GradeOp op;
    runWrapped<GradeOp>(op, tid);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void pixelwrap_invert(uint3 tid : SV_DispatchThreadID)
{
    WrapInvert op;
    runWrapped<WrapInvert>(op, tid);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void pixelwrap_chain(uint3 tid : SV_DispatchThreadID)
{
    Chain<GradeOp, WrapInvert> op;
    runWrapped<Chain<GradeOp, WrapInvert>>(op, tid);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void pixelwrap_identity(uint3 tid : SV_DispatchThreadID)
{
    WrapIdentity op;
    runWrapped<WrapIdentity>(op, tid);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void pixelwrap_scale(uint3 tid : SV_DispatchThreadID)
{
    WrapScale op;
    runWrapped<WrapScale>(op, tid);
}
