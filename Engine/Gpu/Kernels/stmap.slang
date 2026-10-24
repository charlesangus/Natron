import pixelwrap;
import coordmap;
import resamplecore;

// STMap::renderOp and IDistort::renderOp per pixel. The wrap's push block carries the Source,
// which is both the image sampled and the image blended under the mask and mix, the destination
// and the mask; the parameter block in wrapAux carries the settings and the UV view, whose pixels
// are in uvBuf. WarpGpuParams in Engine/Nodes/Transform/WarpGpu.h mirrors the block's words.

[[vk::binding(4, 0)]] StructuredBuffer<float> uvBuf;

static const uint kWarpClamp = 1;
static const uint kWarpBlackOutside = 2;
static const uint kWarpJacobian = 4;
static const uint kWarpSample = 8;

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

// The source coordinate of pixel (x, y), rounded to float once from the double-float value of the
// CPU's double expression.
float2 stmapCoord(Warp w, int x, int y)
{
    const FF u = ffMul(ffAdd(ffMake(uvAt(w.uv, w.uv.u, x, y), 0.0), ffNeg(w.uOffset)), w.uScale);
    const FF v = ffMul(ffAdd(ffMake(uvAt(w.uv, w.uv.v, x, y), 0.0), ffNeg(w.vOffset)), w.vScale);
    return float2(ffAdd(w.originX, ffMul(u, w.width)).hi, ffAdd(w.originY, ffMul(v, w.height)).hi);
}

float2 idistortCoord(Warp w, int x, int y)
{
    const FF u = ffMul(ffAdd(ffMake(uvAt(w.uv, w.uv.u, x, y), 0.0), ffNeg(w.uOffset)), w.uScale);
    const FF v = ffMul(ffAdd(ffMake(uvAt(w.uv, w.uv.v, x, y), 0.0), ffNeg(w.vOffset)), w.vScale);
    return float2(ffAdd(ffMake(float(x) + 0.5, 0.0), u).hi, ffAdd(ffMake(float(y) + 0.5, 0.0), v).hi);
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
