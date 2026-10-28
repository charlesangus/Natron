import pixelwrap;
import coordmap;
import resamplecore;

// SplineWarp::renderOp per pixel, SplineWarpField::getRow() included. The wrap's push block
// carries the Source, which is both the image sampled and the image blended under the mask and
// mix, the destination and the mask; the parameter block in wrapAux carries the settings, the
// field's window and table layout, and the inverse of a concatenated upstream matrix; fieldBuf
// holds the tables SplineWarpField::getGpuTables() builds, after a table of the fields to average
// with motion blur. SplineWarpGpuParams in
// Engine/Nodes/Transform/SplineWarp/SplineWarpGpu.h mirrors the block's words.

[[vk::binding(4, 0)]] StructuredBuffer<uint> fieldBuf;

static const uint kSplineWarpClamp = 1;
static const uint kSplineWarpBlackOutside = 2;
static const uint kSplineWarpJacobian = 4;
static const uint kSplineWarpSample = 8;
static const uint kSplineWarpField = 16;
static const uint kSplineWarpFeathers = 32;
static const uint kSplineWarpMatrix = 64;

// Where SplineWarp's mapRowThrough sends a coordinate the matrix maps behind the camera.
static const float kOutsideCoord = -1e9;

// Words per grid point and per window column or row in fieldBuf.
static const uint kNodeWords = 8;
static const uint kLineWords = 3;

struct SplineWarpParams
{
    uint filter;
    uint flags;
    int x1;
    int y1;
    uint width;
    uint height;
    uint nx;
    uint nodes;
    uint columns;
    uint rows;
    uint feathers;
    uint featherStride;
    FF m[9];
    uint subFrames;
}

SplineWarpParams splineWarpLoad(StructuredBuffer<uint> params)
{
    SplineWarpParams s;
    s.filter = params[0];
    s.flags = params[1];
    s.x1 = int(params[2]);
    s.y1 = int(params[3]);
    s.width = params[4];
    s.height = params[5];
    s.nx = params[6];
    s.nodes = params[7];
    s.columns = params[8];
    s.rows = params[9];
    s.feathers = params[10];
    s.featherStride = params[11];
    for (uint k = 0; k < 9; ++k)
        s.m[k] = ffMake(asfloat(params[12 + 2 * k]), asfloat(params[13 + 2 * k]));
    s.subFrames = params[30];
    return s;
}

// Words per entry of the sub-frame table at the start of fieldBuf (setSplineWarpGpuSubFrame()).
static const uint kSubFrameWords = 11;

// s with sub-frame i's field in place of its own.
SplineWarpParams subFrameParams(SplineWarpParams s, uint i)
{
    const uint e = i * kSubFrameWords;
    SplineWarpParams t = s;
    t.flags = (s.flags & ~(kSplineWarpField | kSplineWarpFeathers)) | fieldBuf[e];
    t.x1 = int(fieldBuf[e + 1]);
    t.y1 = int(fieldBuf[e + 2]);
    t.width = fieldBuf[e + 3];
    t.height = fieldBuf[e + 4];
    t.nx = fieldBuf[e + 5];
    t.nodes = fieldBuf[e + 6];
    t.columns = fieldBuf[e + 7];
    t.rows = fieldBuf[e + 8];
    t.feathers = fieldBuf[e + 9];
    t.featherStride = fieldBuf[e + 10];
    return t;
}

FF fieldFF(uint index)
{
    return ffMake(asfloat(fieldBuf[index]), asfloat(fieldBuf[index + 1]));
}

// (1 - f) a + f b, as the CPU's double expression.
FF lerpFF(FF a, FF b, FF f)
{
    return ffAdd(ffMul(a, ffAddF(ffNeg(f), 1.0)), ffMul(b, f));
}

bool insideWindow(SplineWarpParams s, int2 p)
{
    return p.x >= s.x1 && p.y >= s.y1 && p.x - s.x1 < int(s.width) && p.y - s.y1 < int(s.height);
}

bool insideFeather(SplineWarpParams s, int2 p)
{
    const uint col = uint(p.x - s.x1);
    const uint word = fieldBuf[s.feathers + uint(p.y - s.y1) * s.featherStride + col / 32];
    return ((word >> (col % 32)) & 1) != 0;
}

// The source of pixel p in pixels at the render scale and, with the Jacobian flag, its Jacobian
// (Jxx, Jxy, Jyx, Jyy), as SplineWarpField::getRow() gives them. The bilinear lookup runs in
// two-float arithmetic so the coordinate rounds to the float the CPU's double expression rounds to,
// which decides whether the pixel is left in place.
void fieldAt(SplineWarpParams s, int2 p, out float2 coord, out float4 J)
{
    coord = float2(float(p.x) + 0.5, float(p.y) + 0.5);
    J = float4(1.0, 0.0, 0.0, 1.0);
    if ((s.flags & kSplineWarpField) == 0 || !insideWindow(s, p))
        return;
    if ((s.flags & kSplineWarpFeathers) != 0 && !insideFeather(s, p))
        return;
    const uint col = s.columns + uint(p.x - s.x1) * kLineWords;
    const uint row = s.rows + uint(p.y - s.y1) * kLineWords;
    const FF fx = fieldFF(col + 1);
    const FF fy = fieldFF(row + 1);
    const uint a = s.nodes + (fieldBuf[row] * s.nx + fieldBuf[col]) * kNodeWords;
    const uint b = a + kNodeWords;
    const uint c = a + s.nx * kNodeWords;
    const uint d = c + kNodeWords;
    const FF sx = lerpFF(lerpFF(fieldFF(a), fieldFF(b), fx), lerpFF(fieldFF(c), fieldFF(d), fx), fy);
    const FF sy = lerpFF(lerpFF(fieldFF(a + 2), fieldFF(b + 2), fx), lerpFF(fieldFF(c + 2), fieldFF(d + 2), fx), fy);
    coord = float2(sx.hi, sy.hi);
    if ((s.flags & kSplineWarpJacobian) == 0)
        return;
    const float u = fx.hi + fx.lo;
    const float v = fy.hi + fy.lo;
    for (uint k = 0; k < 4; ++k)
    {
        const float ja = asfloat(fieldBuf[a + 4 + k]);
        const float jb = asfloat(fieldBuf[b + 4 + k]);
        const float jc = asfloat(fieldBuf[c + 4 + k]);
        const float jd = asfloat(fieldBuf[d + 4 + k]);
        J[k] = (1.0 - v) * ((1.0 - u) * ja + u * jb) + v * ((1.0 - u) * jc + u * jd);
    }
}

// m[r] x + m[r + 1] y + m[r + 2].
FF matrixRow(SplineWarpParams s, uint r, float x, float y)
{
    return ffAdd(ffAdd(ffMulF(s.m[r], x), ffMulF(s.m[r + 1], y)), s.m[r + 2]);
}

// SplineWarp's mapRowThrough: the coordinate through the inverse of the concatenated upstream
// matrix into the upstream image's pixels, the Jacobian chained with the map's derivative.
void mapThrough(SplineWarpParams s, inout float2 coord, inout float4 J)
{
    const float x = coord.x;
    const float y = coord.y;
    const FF w = matrixRow(s, 6, x, y);
    if (!(w.hi > 0.0))
    {
        coord = float2(kOutsideCoord, kOutsideCoord);
        return;
    }
    const FF qx = ffDiv(matrixRow(s, 0, x, y), w);
    const FF qy = ffDiv(matrixRow(s, 3, x, y), w);
    // The two-float error terms of an overflowing quotient are NaN where the CPU's double gives an
    // infinity; plain float arithmetic keeps it.
    const float wf = w.hi;
    const float px = isFiniteF(qx.hi) ? qx.hi : (s.m[0].hi * x + s.m[1].hi * y + s.m[2].hi) / wf;
    const float py = isFiniteF(qy.hi) ? qy.hi : (s.m[3].hi * x + s.m[4].hi * y + s.m[5].hi) / wf;
    coord = float2(px, py);
    if ((s.flags & kSplineWarpJacobian) == 0)
        return;
    const float d00 = (s.m[0].hi - px * s.m[6].hi) / wf;
    const float d01 = (s.m[1].hi - px * s.m[7].hi) / wf;
    const float d10 = (s.m[3].hi - py * s.m[6].hi) / wf;
    const float d11 = (s.m[4].hi - py * s.m[7].hi) / wf;
    J = float4(d00 * J.x + d01 * J.z, d00 * J.y + d01 * J.w, d10 * J.x + d11 * J.z, d10 * J.y + d11 * J.w);
}

ResampleSource splineWarpSource(SplineWarpParams s)
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
    src.valid = (s.flags & kSplineWarpSample) != 0 && wrap.srcX2 > wrap.srcX1 && wrap.srcY2 > wrap.srcY1;
    src.blackOutside = (s.flags & kSplineWarpBlackOutside) != 0;
    return src;
}

// The sample in channel-bit order, a one-channel pixel being alpha.
float4 splineWarpSample(SplineWarpParams s, int2 p)
{
    const float4 zero = float4(0.0, 0.0, 0.0, 0.0);
    if ((s.flags & kSplineWarpSample) == 0)
        return zero;
    float2 coord;
    float4 J;
    fieldAt(s, p, coord, J);
    if ((s.flags & kSplineWarpMatrix) != 0)
    {
        mapThrough(s, coord, J);
    }
    else if (coord.x == float(p.x) + 0.5 && coord.y == float(p.y) + 0.5)
    {
        // A pixel the warp leaves in place is the Source pixel itself, which a filter would soften
        // with its neighbours.
        return wrapReadSource(p);
    }
    const bool jacobian = (s.flags & kSplineWarpJacobian) != 0;
    const float4 v = sampleAt(splineWarpSource(s), coord.x, coord.y, J, jacobian, s.filter, (s.flags & kSplineWarpClamp) != 0, kWrapNone,
                              kWrapNone, int4(0, 0, 0, 0));
    if (wrap.nComps == 1)
        return float4(0.0, 0.0, 0.0, v.x);
    return v;
}

struct SplineWarpOp : IPixelOp
{
    SplineWarpParams s;

    [mutating] void load(StructuredBuffer<uint> params)
    {
        s = splineWarpLoad(params);
    }

    float4 apply(float4 v, int2 xy)
    {
        if (s.subFrames == 0)
            return splineWarpSample(s, xy);
        // Summed in sub-frame order then divided, as SplineWarp::renderOp does.
        float4 sum = splineWarpSample(subFrameParams(s, 0), xy);
        for (uint i = 1; i < s.subFrames; ++i)
            sum += splineWarpSample(subFrameParams(s, i), xy);
        return sum / float(s.subFrames);
    }
}

[shader("compute")]
[numthreads(8, 8, 1)]
void splinewarp_main(uint3 tid : SV_DispatchThreadID)
{
    SplineWarpOp op;
    runWrapped(op, tid);
}
