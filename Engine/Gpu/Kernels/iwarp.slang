import pixelwrap;
import coordmap;
import resamplecore;
import mapremap;

// IWarp::renderOp per pixel without motion blur, for ITransform and ICornerPin. The wrap's push
// block carries the Source, which is both the image sampled and the image blended under the mask
// and mix, the destination and the mask; the parameter block in wrapAux carries the settings, the
// Map view, whose pixels are in mapBuf, and the matrix curve (TransformInterp::MatrixCurve::pack()).
// IWarp::renderOpGpu writes the block's words.

[[vk::binding(4, 0)]] StructuredBuffer<float> mapBuf;

static const uint kIWarpClamp = 1;
static const uint kIWarpBlackOutside = 2;
static const uint kIWarpJacobian = 4;
static const uint kIWarpSample = 8;
static const uint kIWarpMapConnected = 16;
static const uint kIWarpMapRead = 32;
static const uint kIWarpMapInvert = 64;
static const uint kIWarpMapSmooth = 128;

// The words before the curve, and the floats of one knot: its matrix's high floats, its matrix's
// low floats, then its derivative in m.
static const uint kIWarpHeaderWords = 12;
static const uint kIWarpKnotFloats = 27;

struct IWarp
{
    uint filter;
    uint flags;
    uint mapOffset;
    uint mapStride;
    int mapX1;
    int mapY1;
    int mapX2;
    int mapY2;
    uint mapComps;
    int mapComponent;
    float exponent;
    uint segments;
}

IWarp iwarpLoad(StructuredBuffer<uint> params)
{
    IWarp w;
    w.filter = params[0];
    w.flags = params[1];
    w.mapOffset = params[2];
    w.mapStride = params[3];
    w.mapX1 = int(params[4]);
    w.mapY1 = int(params[5]);
    w.mapX2 = int(params[6]);
    w.mapY2 = int(params[7]);
    w.mapComps = params[8];
    w.mapComponent = int(params[9]);
    w.exponent = asfloat(params[10]);
    w.segments = params[11];
    return w;
}

// The remapped Map value at (x, y): 1 with no Map, and 0, neither remapped nor inverted, off the
// Map's pixels or when its channel is not read, as MapInput::fill() gives it.
float iwarpMap(IWarp w, int x, int y)
{
    if ((w.flags & kIWarpMapConnected) == 0)
        return 1.0;
    if ((w.flags & kIWarpMapRead) == 0 || w.mapComponent < 0 || !wrapInside(int2(x, y), w.mapX1, w.mapY1, w.mapX2, w.mapY2))
        return 0.0;
    const float raw = mapBuf[w.mapOffset + uint(y - w.mapY1) * w.mapStride + uint(x - w.mapX1) * w.mapComps + uint(w.mapComponent)];
    return mapRemap(raw, (w.flags & kIWarpMapInvert) != 0, (w.flags & kIWarpMapSmooth) != 0, w.exponent);
}

// Knot k's matrix entry i as two floats, the high float of the CPU's double then the rest.
FF iwarpKnotH(uint knot, uint i)
{
    const uint base = kIWarpHeaderWords + knot * kIWarpKnotFloats;
    return ffMake(asfloat(wrapAux[base + i]), asfloat(wrapAux[base + 9 + i]));
}

// Knot k's derivative entry i, which only weighs in at a segment's width or a Map slope.
float iwarpKnotD(uint knot, uint i)
{
    return asfloat(wrapAux[kIWarpHeaderWords + knot * kIWarpKnotFloats + 18 + i]);
}

// MatrixCurve::evaluate(): the matrix H and its derivative dH in m, cubic Hermite between knots,
// in two floats as the CPU's are doubles.
void iwarpCurve(IWarp w, float m, out FF H[9], out FF dH[9])
{
    const int segments = int(w.segments);
    if (!(m > 0.0))
        m = 0.0;
    else if (m > 1.0)
        m = 1.0;
    const float n = float(segments);
    const FF u = ffMulF(ffMake(m, 0.0), n);
    int k = int(floor(u.hi));
    if (float(k) == u.hi && u.lo < 0.0)
        k -= 1;
    if (k >= segments)
        k = segments - 1;
    if (k < 0)
        k = 0;
    const FF t = ffAddF(u, -float(k));
    const uint k0 = uint(k);
    const uint k1 = uint(k + 1);
    const bool atStart = t.hi == 0.0 && t.lo == 0.0;
    const bool atEnd = t.hi == 1.0 && t.lo == 0.0;
    if (atStart || atEnd)
    {
        const uint knot = atStart ? k0 : k1;
        [unroll]
        for (uint i = 0; i < 9; ++i)
        {
            H[i] = iwarpKnotH(knot, i);
            dH[i] = ffMake(iwarpKnotD(knot, i), 0.0);
        }
        return;
    }
    const FF t2 = ffMul(t, t);
    const FF t3 = ffMul(t2, t);
    const FF h00 = ffAddF(ffAdd(ffMulF(t3, 2.0), ffMulF(t2, -3.0)), 1.0);
    const FF h10 = ffAdd(ffAdd(t3, ffMulF(t2, -2.0)), t);
    const FF h01 = ffAdd(ffMulF(t3, -2.0), ffMulF(t2, 3.0));
    const FF h11 = ffAdd(t3, ffNeg(t2));
    // The basis derivatives in t; d/dm = d/dt * segments.
    const FF g00 = ffAdd(ffMulF(t2, 6.0), ffMulF(t, -6.0));
    const FF g10 = ffAddF(ffAdd(ffMulF(t2, 3.0), ffMulF(t, -4.0)), 1.0);
    const FF g01 = ffAdd(ffMulF(t2, -6.0), ffMulF(t, 6.0));
    const FF g11 = ffAdd(ffMulF(t2, 3.0), ffMulF(t, -2.0));
    // h10 and h11 times the segment's width.
    const FF w10 = ffDivF(h10, n);
    const FF w11 = ffDivF(h11, n);
    [unroll]
    for (uint i = 0; i < 9; ++i)
    {
        const FF a = iwarpKnotH(k0, i);
        const FF b = iwarpKnotH(k1, i);
        const float da = iwarpKnotD(k0, i);
        const float db = iwarpKnotD(k1, i);
        H[i] = ffAdd(ffAdd(ffMul(h00, a), ffMulF(w10, da)), ffAdd(ffMul(h01, b), ffMulF(w11, db)));
        dH[i] = ffAdd(ffMulF(ffAdd(ffMul(g00, a), ffMul(g01, b)), n), ffAdd(ffMulF(g10, da), ffMulF(g11, db)));
    }
}

// (u v - p q) / d, a term of the projective Jacobian.
FF iwarpCross(FF u, FF v, FF p, FF q, FF d)
{
    return ffDiv(ffAdd(ffMul(u, v), ffNeg(ffMul(p, q))), d);
}

// The sample in channel-bit order, a one-channel pixel being alpha.
float4 iwarpSample(IWarp w, int2 p)
{
    const float4 zero = float4(0.0, 0.0, 0.0, 0.0);
    if ((w.flags & kIWarpSample) == 0)
        return zero;
    const bool jacobian = (w.flags & kIWarpJacobian) != 0;
    const float m = iwarpMap(w, p.x, p.y);
    // Where the Map is 0 the output is the Source pixel itself, never filtered, which would soften
    // a held pixel with its displaced neighbours.
    if ((w.flags & kIWarpMapConnected) != 0 && m == 0.0)
        return wrapReadSource(p);
    const FF none = ffMake(0.0, 0.0);
    // The central differences of the Map, exact as the CPU's doubles are.
    FF dmdx = none;
    FF dmdy = none;
    if (jacobian)
    {
        dmdx = ffHalf(ffTwoSum(iwarpMap(w, p.x + 1, p.y), -iwarpMap(w, p.x - 1, p.y)));
        dmdy = ffHalf(ffTwoSum(iwarpMap(w, p.x, p.y + 1), -iwarpMap(w, p.x, p.y - 1)));
    }
    FF H[9];
    FF dH[9];
    iwarpCurve(w, m, H, dH);

    // TransformInterp::sourceSample() in two floats, rounded to the floats that
    // Resampler::resampleRowFromCoords() takes as the CPU rounds its doubles.
    const float px = float(p.x) + 0.5;
    const float py = float(p.y) + 0.5;
    const FF ax = ffAdd(ffAdd(ffMulF(H[0], px), ffMulF(H[1], py)), H[2]);
    const FF ay = ffAdd(ffAdd(ffMulF(H[3], px), ffMulF(H[4], py)), H[5]);
    const FF z = ffAdd(ffAdd(ffMulF(H[6], px), ffMulF(H[7], py)), H[8]);
    if (!ffLess(none, z))
        return zero;
    const float sx = ffDiv(ax, z).hi;
    const float sy = ffDiv(ay, z).hi;
    if (!isFiniteF(sx) || !isFiniteF(sy))
        return zero;
    float4 J = zero;
    if (jacobian)
    {
        const FF z2 = ffMul(z, z);
        FF Jxx = iwarpCross(H[0], z, ax, H[6], z2);
        FF Jxy = iwarpCross(H[1], z, ax, H[7], z2);
        FF Jyx = iwarpCross(H[3], z, ay, H[6], z2);
        FF Jyy = iwarpCross(H[4], z, ay, H[7], z2);
        if (dmdx.hi != 0.0 || dmdy.hi != 0.0)
        {
            const FF dax = ffAdd(ffAdd(ffMulF(dH[0], px), ffMulF(dH[1], py)), dH[2]);
            const FF day = ffAdd(ffAdd(ffMulF(dH[3], px), ffMulF(dH[4], py)), dH[5]);
            const FF dz = ffAdd(ffAdd(ffMulF(dH[6], px), ffMulF(dH[7], py)), dH[8]);
            const FF dxdm = iwarpCross(dax, z, ax, dz, z2);
            const FF dydm = iwarpCross(day, z, ay, dz, z2);
            Jxx = ffAdd(Jxx, ffMul(dxdm, dmdx));
            Jxy = ffAdd(Jxy, ffMul(dxdm, dmdy));
            Jyx = ffAdd(Jyx, ffMul(dydm, dmdx));
            Jyy = ffAdd(Jyy, ffMul(dydm, dmdy));
        }
        J = float4(Jxx.hi, Jxy.hi, Jyx.hi, Jyy.hi);
    }

    ResampleSource src;
    src.buf = wrapSrc;
    src.offset = wrap.srcOffset;
    src.stride = wrap.srcStride;
    src.x1 = wrap.srcX1;
    src.y1 = wrap.srcY1;
    src.x2 = wrap.srcX2;
    src.y2 = wrap.srcY2;
    src.nComps = wrap.nComps;
    src.valid = wrap.srcX2 > wrap.srcX1 && wrap.srcY2 > wrap.srcY1;
    src.blackOutside = (w.flags & kIWarpBlackOutside) != 0;
    const float4 v = sampleAt(src, sx, sy, J, jacobian, w.filter, (w.flags & kIWarpClamp) != 0, kWrapNone, kWrapNone, int4(0, 0, 0, 0));
    if (wrap.nComps == 1)
        return float4(0.0, 0.0, 0.0, v.x);
    return v;
}

struct IWarpOp : IPixelOp
{
    IWarp w;

    [mutating] void load(StructuredBuffer<uint> params)
    {
        w = iwarpLoad(params);
    }

    float4 apply(float4 v, int2 xy)
    {
        return iwarpSample(w, xy);
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void iwarp_main(uint3 tid : SV_DispatchThreadID)
{
    IWarpOp op;
    runWrapped(op, tid);
}
