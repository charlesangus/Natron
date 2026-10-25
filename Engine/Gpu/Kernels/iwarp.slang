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

// The words before the curve, and the floats of one knot: its matrix then its derivative in m.
static const uint kIWarpHeaderWords = 12;
static const uint kIWarpKnotFloats = 18;

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

// The remapped Map value at (x, y): 1 with no Map, the remap of 0 off the Map's pixels.
float iwarpMap(IWarp w, int x, int y)
{
    if ((w.flags & kIWarpMapConnected) == 0)
        return 1.0;
    float raw = 0.0;
    if ((w.flags & kIWarpMapRead) != 0 && w.mapComponent >= 0 && wrapInside(int2(x, y), w.mapX1, w.mapY1, w.mapX2, w.mapY2))
        raw = mapBuf[w.mapOffset + uint(y - w.mapY1) * w.mapStride + uint(x - w.mapX1) * w.mapComps + uint(w.mapComponent)];
    return mapRemap(raw, (w.flags & kIWarpMapInvert) != 0, (w.flags & kIWarpMapSmooth) != 0, w.exponent);
}

float iwarpKnot(uint knot, uint i)
{
    return asfloat(wrapAux[kIWarpHeaderWords + knot * kIWarpKnotFloats + i]);
}

// MatrixCurve::evaluate(): the matrix H and its derivative dH in m, cubic Hermite between knots.
void iwarpCurve(IWarp w, float m, out float H[9], out float dH[9])
{
    const int segments = int(w.segments);
    if (!(m > 0.0))
        m = 0.0;
    else if (m > 1.0)
        m = 1.0;
    const float u = m * float(segments);
    int k = int(floor(u));
    if (k >= segments)
        k = segments - 1;
    const float t = u - float(k);
    const float width = 1.0 / float(segments);
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float h00 = 2.0 * t3 - 3.0 * t2 + 1.0;
    const float h10 = t3 - 2.0 * t2 + t;
    const float h01 = -2.0 * t3 + 3.0 * t2;
    const float h11 = t3 - t2;
    const float g00 = 6.0 * t2 - 6.0 * t;
    const float g10 = 3.0 * t2 - 4.0 * t + 1.0;
    const float g01 = -6.0 * t2 + 6.0 * t;
    const float g11 = 3.0 * t2 - 2.0 * t;
    const uint k0 = uint(k);
    const uint k1 = uint(k + 1);
    [unroll]
    for (uint i = 0; i < 9; ++i)
    {
        const float a = iwarpKnot(k0, i);
        const float b = iwarpKnot(k1, i);
        const float da = iwarpKnot(k0, 9 + i);
        const float db = iwarpKnot(k1, 9 + i);
        if (t == 0.0)
        {
            H[i] = a;
            dH[i] = da;
        }
        else
        {
            H[i] = h00 * a + h10 * width * da + h01 * b + h11 * width * db;
            dH[i] = (g00 * a + g01 * b) * float(segments) + g10 * da + g11 * db;
        }
    }
}

// The sample in channel-bit order, a one-channel pixel being alpha.
float4 iwarpSample(IWarp w, int2 p)
{
    const float4 zero = float4(0.0, 0.0, 0.0, 0.0);
    if ((w.flags & kIWarpSample) == 0)
        return zero;
    const bool jacobian = (w.flags & kIWarpJacobian) != 0;
    const float m = iwarpMap(w, p.x, p.y);
    float dmdx = 0.0;
    float dmdy = 0.0;
    if (jacobian)
    {
        dmdx = 0.5 * (iwarpMap(w, p.x + 1, p.y) - iwarpMap(w, p.x - 1, p.y));
        dmdy = 0.5 * (iwarpMap(w, p.x, p.y + 1) - iwarpMap(w, p.x, p.y - 1));
    }
    float H[9];
    float dH[9];
    iwarpCurve(w, m, H, dH);

    // TransformInterp::sourceSample().
    const float px = float(p.x) + 0.5;
    const float py = float(p.y) + 0.5;
    const float ax = H[0] * px + H[1] * py + H[2];
    const float ay = H[3] * px + H[4] * py + H[5];
    const float z = H[6] * px + H[7] * py + H[8];
    if (!(z > 0.0))
        return zero;
    const float sx = ax / z;
    const float sy = ay / z;
    if (!isFiniteF(sx) || !isFiniteF(sy))
        return zero;
    float4 J = zero;
    if (jacobian)
    {
        const float z2 = z * z;
        J = float4((H[0] * z - ax * H[6]) / z2, (H[1] * z - ax * H[7]) / z2, (H[3] * z - ay * H[6]) / z2, (H[4] * z - ay * H[7]) / z2);
        if (dmdx != 0.0 || dmdy != 0.0)
        {
            const float dax = dH[0] * px + dH[1] * py + dH[2];
            const float day = dH[3] * px + dH[4] * py + dH[5];
            const float dz = dH[6] * px + dH[7] * py + dH[8];
            const float dxdm = (dax * z - ax * dz) / z2;
            const float dydm = (day * z - ay * dz) / z2;
            J += float4(dxdm * dmdx, dxdm * dmdy, dydm * dmdx, dydm * dmdy);
        }
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
