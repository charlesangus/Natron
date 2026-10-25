import pixelwrap;
import coordmap;
import resamplecore;
import mapremap;

// STMap::renderOp and IDistort::renderOp per pixel. The wrap's push block carries the Source,
// which is both the image sampled and the image blended under the mask and mix, the destination
// and the mask; the parameter block in wrapAux carries the settings, the UV view, whose pixels
// are in uvBuf, and the Map view, whose pixels are in mapBuf. WarpGpuParams in
// Engine/Nodes/Transform/WarpGpu.h mirrors the block's words.

[[vk::binding(4, 0)]] StructuredBuffer<float> uvBuf;
[[vk::binding(5, 0)]] StructuredBuffer<float> mapBuf;

static const uint kWarpClamp = 1;
static const uint kWarpBlackOutside = 2;
static const uint kWarpJacobian = 4;
static const uint kWarpSample = 8;
static const uint kWarpMapConnected = 16;
static const uint kWarpMapRead = 32;
static const uint kWarpMapInvert = 64;
static const uint kWarpMapSmooth = 128;

struct WarpUV
{
    uint offset;
    uint stride;
    int x1;
    int y1;
    int x2;
    int y2;
    uint comps;
    int u;
    int v;
    int alpha;
}

struct WarpMap
{
    uint offset;
    uint stride;
    int x1;
    int y1;
    int x2;
    int y2;
    uint comps;
    int component;
    float exponent;
}

struct Warp
{
    uint filter;
    uint flags;
    uint wrapU;
    uint wrapV;
    int4 domain;
    WarpUV uv;
    FF uOffset;
    FF vOffset;
    FF uScale;
    FF vScale;
    FF originX;
    FF originY;
    FF width;
    FF height;
    WarpMap map;
}

FF warpFF(StructuredBuffer<uint> params, uint index)
{
    return ffMake(asfloat(params[index]), asfloat(params[index + 1]));
}

Warp warpLoad(StructuredBuffer<uint> params)
{
    Warp w;
    w.filter = params[0];
    w.flags = params[1];
    w.wrapU = params[2];
    w.wrapV = params[3];
    w.domain = int4(int(params[4]), int(params[5]), int(params[6]), int(params[7]));
    w.uv.offset = params[8];
    w.uv.stride = params[9];
    w.uv.x1 = int(params[10]);
    w.uv.y1 = int(params[11]);
    w.uv.x2 = int(params[12]);
    w.uv.y2 = int(params[13]);
    w.uv.comps = params[14];
    w.uv.u = int(params[15]);
    w.uv.v = int(params[16]);
    w.uOffset = warpFF(params, 17);
    w.vOffset = warpFF(params, 19);
    w.uScale = warpFF(params, 21);
    w.vScale = warpFF(params, 23);
    w.originX = warpFF(params, 25);
    w.originY = warpFF(params, 27);
    w.width = warpFF(params, 29);
    w.height = warpFF(params, 31);
    w.uv.alpha = int(params[33]);
    w.map.offset = params[34];
    w.map.stride = params[35];
    w.map.x1 = int(params[36]);
    w.map.y1 = int(params[37]);
    w.map.x2 = int(params[38]);
    w.map.y2 = int(params[39]);
    w.map.comps = params[40];
    w.map.component = int(params[41]);
    w.map.exponent = asfloat(params[42]);
    return w;
}

// A component that is not read has no bounds: the Jacobian's edge rule then never applies to it.
bool uvInside(WarpUV uv, int component, int x, int y)
{
    return component >= 0 && wrapInside(int2(x, y), uv.x1, uv.y1, uv.x2, uv.y2);
}

float uvAt(WarpUV uv, int component, int x, int y)
{
    if (!uvInside(uv, component, x, y))
        return 0.0;
    return uvBuf[uv.offset + uint(y - uv.y1) * uv.stride + uint(x - uv.x1) * uv.comps + uint(component)];
}

bool warpMapped(Warp w)
{
    return (w.flags & kWarpMapConnected) != 0;
}

// The remapped Map value at (x, y), as MapInput::read() and MapInputParams::remap() give it: 1
// with no Map, the remap of 0 off the Map's pixels.
float warpMap(Warp w, int x, int y)
{
    if (!warpMapped(w))
        return 1.0;
    float raw = 0.0;
    if ((w.flags & kWarpMapRead) != 0 && w.map.component >= 0 && wrapInside(int2(x, y), w.map.x1, w.map.y1, w.map.x2, w.map.y2))
        raw = mapBuf[w.map.offset + uint(y - w.map.y1) * w.map.stride + uint(x - w.map.x1) * w.map.comps + uint(w.map.component)];
    return mapRemap(raw, (w.flags & kWarpMapInvert) != 0, (w.flags & kWarpMapSmooth) != 0, w.map.exponent);
}

// The double-float error terms of an infinite or overflowing operand are inf - inf, so the result
// would be NaN where the CPU's double expression gives an infinity, which clamp wrapping moves to
// the edge rather than reading black. Plain float arithmetic keeps the infinity and its sign.
float finiteOrFloat(FF v, float fallback)
{
    return isFiniteF(v.hi) ? v.hi : fallback;
}

// The source coordinate of pixel (x, y), rounded to float once from the double-float value of the
// CPU's double expression. A Map value m other than 1 moves it from the pixel's centre c to
// c + (s - c) * m, c itself at 0.
float2 stmapCoord(Warp w, int x, int y)
{
    const float mapU = uvAt(w.uv, w.uv.u, x, y);
    const float mapV = uvAt(w.uv, w.uv.v, x, y);
    const FF u = ffMul(ffAdd(ffMake(mapU, 0.0), ffNeg(w.uOffset)), w.uScale);
    const FF v = ffMul(ffAdd(ffMake(mapV, 0.0), ffNeg(w.vOffset)), w.vScale);
    const float m = warpMap(w, x, y);
    if (m == 0.0)
        return float2(float(x) + 0.5, float(y) + 0.5);
    if (m != 1.0)
    {
        const FF cx = ffMake(float(x) + 0.5, 0.0);
        const FF cy = ffMake(float(y) + 0.5, 0.0);
        const FF mm = ffMake(m, 0.0);
        const FF sx = ffAdd(cx, ffMul(ffAdd(ffAdd(w.originX, ffMul(u, w.width)), ffNeg(cx)), mm));
        const FF sy = ffAdd(cy, ffMul(ffAdd(ffAdd(w.originY, ffMul(v, w.height)), ffNeg(cy)), mm));
        const float fx = w.originX.hi + (mapU - w.uOffset.hi) * w.uScale.hi * w.width.hi;
        const float fy = w.originY.hi + (mapV - w.vOffset.hi) * w.vScale.hi * w.height.hi;
        return float2(finiteOrFloat(sx, cx.hi + (fx - cx.hi) * m), finiteOrFloat(sy, cy.hi + (fy - cy.hi) * m));
    }
    return float2(finiteOrFloat(ffAdd(w.originX, ffMul(u, w.width)), w.originX.hi + (mapU - w.uOffset.hi) * w.uScale.hi * w.width.hi),
                  finiteOrFloat(ffAdd(w.originY, ffMul(v, w.height)), w.originY.hi + (mapV - w.vOffset.hi) * w.vScale.hi * w.height.hi));
}

// A Map value m scales the displacement, the pixel's centre itself at 0.
float2 idistortCoord(Warp w, int x, int y)
{
    const float mapU = uvAt(w.uv, w.uv.u, x, y);
    const float mapV = uvAt(w.uv, w.uv.v, x, y);
    const FF u = ffMul(ffAdd(ffMake(mapU, 0.0), ffNeg(w.uOffset)), w.uScale);
    const FF v = ffMul(ffAdd(ffMake(mapV, 0.0), ffNeg(w.vOffset)), w.vScale);
    if (warpMapped(w))
    {
        const float m = warpMap(w, x, y);
        if (m == 0.0)
            return float2(float(x) + 0.5, float(y) + 0.5);
        const FF mm = ffMake(m, 0.0);
        return float2(finiteOrFloat(ffAdd(ffMake(float(x) + 0.5, 0.0), ffMul(u, mm)), float(x) + 0.5 + (mapU - w.uOffset.hi) * w.uScale.hi * m),
                      finiteOrFloat(ffAdd(ffMake(float(y) + 0.5, 0.0), ffMul(v, mm)), float(y) + 0.5 + (mapV - w.vOffset.hi) * w.vScale.hi * m));
    }
    return float2(finiteOrFloat(ffAdd(ffMake(float(x) + 0.5, 0.0), u), float(x) + 0.5 + (mapU - w.uOffset.hi) * w.uScale.hi),
                  finiteOrFloat(ffAdd(ffMake(float(y) + 0.5, 0.0), v), float(y) + 0.5 + (mapV - w.vOffset.hi) * w.vScale.hi));
}

float centralDifference(float before, float after)
{
    return finiteOrZero(ffHalf(ffAdd(ffMake(after, 0.0), ffNeg(ffMake(before, 0.0)))).hi);
}

float centralDifferenceFF(FF before, FF after)
{
    return finiteOrZero(ffHalf(ffAdd(after, ffNeg(before))).hi);
}

// Resampler::coordJacobiansWithinBounds' differencesWithinBounds for one coordinate: inside the
// UV bounds with a neighbour outside, the pixel's own coordinate moved by `step` stands in for the
// neighbour.
float2 differences(WarpUV uv, int component, int2 p, float centre, float left, float right, float below, float above,
                   float2 step)
{
    float dx = centralDifference(left, right);
    float dy = centralDifference(below, above);
    if (!uvInside(uv, component, p.x, p.y))
        return float2(dx, dy);
    const FF c = ffMake(centre, 0.0);
    const bool hasLeft = uvInside(uv, component, p.x - 1, p.y);
    const bool hasRight = uvInside(uv, component, p.x + 1, p.y);
    if (!hasLeft || !hasRight)
        dx = centralDifferenceFF(hasLeft ? ffMake(left, 0.0) : ffAddF(c, -step.x), hasRight ? ffMake(right, 0.0) : ffAddF(c, step.x));
    const bool hasBelow = uvInside(uv, component, p.x, p.y - 1);
    const bool hasAbove = uvInside(uv, component, p.x, p.y + 1);
    if (!hasBelow || !hasAbove)
        dy = centralDifferenceFF(hasBelow ? ffMake(below, 0.0) : ffAddF(c, -step.y), hasAbove ? ffMake(above, 0.0) : ffAddF(c, step.y));
    return float2(dx, dy);
}

ResampleSource warpSource(Warp w)
{
    ResampleSource src;
    src.buf = wrapSrc;
    src.offset = wrap.srcOffset;
    src.stride = wrap.srcStride;
    src.x1 = wrap.srcX1;
    src.y1 = wrap.srcY1;
    src.x2 = wrap.srcX2;
    src.y2 = wrap.srcY2;
    src.nComps = wrap.nComps;
    src.valid = (w.flags & kWarpSample) != 0 && wrap.srcX2 > wrap.srcX1 && wrap.srcY2 > wrap.srcY1;
    src.blackOutside = (w.flags & kWarpBlackOutside) != 0;
    return src;
}

// The sample in channel-bit order, a one-channel pixel being alpha.
float4 warpSample(Warp w, int2 p, bool idistort)
{
    const float4 zero = float4(0.0, 0.0, 0.0, 0.0);
    if ((w.flags & kWarpSample) == 0)
        return zero;
    // Off the UV view, as with no alpha channel at all, every position is valid; where the Map is
    // 0 the pixel keeps its own, so the gate does not apply.
    if (!idistort && uvInside(w.uv, w.uv.alpha, p.x, p.y) && !(uvAt(w.uv, w.uv.alpha, p.x, p.y) > 0.0) && warpMap(w, p.x, p.y) > 0.0)
        return zero;
    const float2 s = idistort ? idistortCoord(w, p.x, p.y) : stmapCoord(w, p.x, p.y);
    float4 J = zero;
    const bool jacobian = (w.flags & kWarpJacobian) != 0;
    if (jacobian)
    {
        const float2 l = idistort ? idistortCoord(w, p.x - 1, p.y) : stmapCoord(w, p.x - 1, p.y);
        const float2 r = idistort ? idistortCoord(w, p.x + 1, p.y) : stmapCoord(w, p.x + 1, p.y);
        const float2 b = idistort ? idistortCoord(w, p.x, p.y - 1) : stmapCoord(w, p.x, p.y - 1);
        const float2 above = idistort ? idistortCoord(w, p.x, p.y + 1) : stmapCoord(w, p.x, p.y + 1);
        const float step = idistort ? 1.0 : 0.0;
        const float2 dsx = differences(w.uv, w.uv.u, p, s.x, l.x, r.x, b.x, above.x, float2(step, 0.0));
        const float2 dsy = differences(w.uv, w.uv.v, p, s.y, l.y, r.y, b.y, above.y, float2(0.0, step));
        J = float4(dsx.x, dsx.y, dsy.x, dsy.y);
    }
    const uint wrapU = idistort ? kWrapNone : w.wrapU;
    const uint wrapV = idistort ? kWrapNone : w.wrapV;
    const int4 domain = idistort ? int4(0, 0, 0, 0) : w.domain;
    const float4 v = sampleAt(warpSource(w), s.x, s.y, J, jacobian, w.filter, (w.flags & kWarpClamp) != 0, wrapU, wrapV, domain);
    if (wrap.nComps == 1)
        return float4(0.0, 0.0, 0.0, v.x);
    return v;
}

struct STMapOp : IPixelOp
{
    Warp w;

    [mutating] void load(StructuredBuffer<uint> params)
    {
        w = warpLoad(params);
    }

    float4 apply(float4 v, int2 xy)
    {
        return warpSample(w, xy, false);
    }
}

struct IDistortOp : IPixelOp
{
    Warp w;

    [mutating] void load(StructuredBuffer<uint> params)
    {
        w = warpLoad(params);
    }

    float4 apply(float4 v, int2 xy)
    {
        return warpSample(w, xy, true);
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void stmap_main(uint3 tid : SV_DispatchThreadID)
{
    STMapOp op;
    runWrapped(op, tid);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void idistort_main(uint3 tid : SV_DispatchThreadID)
{
    IDistortOp op;
    runWrapped(op, tid);
}
