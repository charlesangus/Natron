// Resampler's filters, interpolation and supersampling with the source passed in, so that kernels
// with their own bindings (resample.slang, and coordmap.slang's callers under pixelwrap.slang) share
// them. Sample positions are carried in two floats, so that the filter taps and the integer cells the
// CPU picks with its double positions are the same here.

static const uint kFilterImpulse = 0;
static const uint kFilterBox = 1;
static const uint kFilterBilinear = 2;
static const uint kFilterCubic = 3;
static const uint kFilterKeys = 4;
static const uint kFilterSimon = 5;
static const uint kFilterRifman = 6;
static const uint kFilterMitchell = 7;
static const uint kFilterParzen = 8;
static const uint kFilterNotch = 9;

// kFlagClamp is only set for the filters Resampler::filterHonoursClamp names.
static const uint kFlagClamp = 1;
static const uint kFlagBlackOutside = 2;
static const uint kFlagSrcValid = 4;
// The exact entries' matrix rows whose float evaluation Engine/Gpu/GpuResample.cpp found exact
// over the whole window; the blurred entry has them per matrix.
static const uint kFlagExactRowX = 8;
static const uint kFlagExactRowY = 16;
static const uint kFlagMatrixInBuffer = 32;

///////////////////////////////////////////////////////////////////////////////
// Two-float arithmetic: a value is hi + lo with |lo| at most half an ulp of hi.

struct FF
{
    float hi;
    float lo;
};

FF ffMake(float hi, float lo)
{
    FF r;
    r.hi = hi;
    r.lo = lo;
    return r;
}

FF ffQuick(float a, float b)
{
    precise float s = a + b;
    precise float e = b - (s - a);
    return ffMake(s, e);
}

FF ffTwoSum(float a, float b)
{
    precise float s = a + b;
    precise float v = s - a;
    precise float e = (a - (s - v)) + (b - v);
    return ffMake(s, e);
}

FF ffAdd(FF a, FF b)
{
    const FF s = ffTwoSum(a.hi, b.hi);
    precise float lo = s.lo + (a.lo + b.lo);
    return ffQuick(s.hi, lo);
}

FF ffAddF(FF a, float b)
{
    const FF s = ffTwoSum(a.hi, b);
    precise float lo = s.lo + a.lo;
    return ffQuick(s.hi, lo);
}

// Dekker's split: a = hi + lo with both halves 12 bits wide, so their products are exact.
FF ffSplit(float a)
{
    precise float c = 4097.0 * a;
    precise float hi = c - (c - a);
    precise float lo = a - hi;
    return ffMake(hi, lo);
}

// Dekker's exact product error, as Vulkan does not require fma() to round once (lavapipe does not).
float ffProductError(float a, float b, float p)
{
    const FF sa = ffSplit(a);
    const FF sb = ffSplit(b);
    precise float e = ((sa.hi * sb.hi - p) + sa.hi * sb.lo + sa.lo * sb.hi) + sa.lo * sb.lo;
    return e;
}

FF ffMulF(FF a, float b)
{
    precise float p = a.hi * b;
    const float e = ffProductError(a.hi, b, p);
    precise float lo = e + a.lo * b;
    return ffQuick(p, lo);
}

FF ffHalf(FF a)
{
    return ffMake(a.hi * 0.5, a.lo * 0.5);
}

FF ffNeg(FF a)
{
    return ffMake(-a.hi, -a.lo);
}

FF ffMul(FF a, FF b)
{
    precise float p = a.hi * b.hi;
    const float e = ffProductError(a.hi, b.hi, p);
    precise float lo = e + (a.hi * b.lo + a.lo * b.hi);
    return ffQuick(p, lo);
}

// One correction step after the float quotient, whose rounding the device may not make exact.
FF ffDivF(FF a, float b)
{
    precise float q1 = a.hi / b;
    const FF r = ffAdd(a, ffNeg(ffMulF(ffMake(q1, 0.0), b)));
    precise float q2 = (r.hi + r.lo) / b;
    return ffQuick(q1, q2);
}

FF ffDiv(FF a, FF b)
{
    precise float q1 = a.hi / b.hi;
    const FF r = ffAdd(a, ffNeg(ffMul(ffMake(q1, 0.0), b)));
    precise float q2 = (r.hi + r.lo) / b.hi;
    return ffQuick(q1, q2);
}

bool ffLess(FF a, FF b)
{
    return a.hi < b.hi || (a.hi == b.hi && a.lo < b.lo);
}

FF ffMin(FF a, FF b)
{
    return ffLess(b, a) ? b : a;
}

FF ffMax(FF a, FF b)
{
    return ffLess(a, b) ? b : a;
}

// A position as its floor and its fraction: i + f with f in [0, 1]. The fraction reaches 1 only
// when the value lies a rounding below the next integer, where i is still its floor.
struct Pos
{
    int i;
    float f;
};

// Positions saturate at +-2^30, far outside any source, as Resampler's floorToInt does, NaN on the
// low side: converting a float outside int's range (or a NaN) is undefined in SPIR-V. Finiteness is
// read from the bits, which no relaxed rewrite of a float comparison can change.
static const float kPosLimit = 1073741824.0;

bool isFiniteBits(float v)
{
    return (asuint(v) & 0x7f800000u) != 0x7f800000u;
}

Pos toPos(FF a)
{
    Pos p;
    if (!isFiniteBits(a.hi) || abs(a.hi) > kPosLimit)
    {
        // Bits at most +Inf's: positive and not NaN.
        p.i = (asuint(a.hi) <= 0x7f800000u) ? int(kPosLimit) : -int(kPosLimit);
        p.f = 0.0;
        return p;
    }
    float fl = floor(a.hi);
    precise float f = (a.hi - fl) + a.lo;
    if (f < 0.0)
    {
        fl -= 1.0;
        f += 1.0;
    }
    else if (f >= 1.0)
    {
        fl += 1.0;
        f -= 1.0;
    }
    p.i = int(fl);
    p.f = f;
    return p;
}

float posValue(Pos p)
{
    precise float v = float(p.i) + p.f;
    return v;
}

// p > n for an integer n.
bool posAbove(Pos p, int n)
{
    return p.i > n || (p.i == n && p.f > 0.0);
}

// The source: `offset` addresses (x1, y1) in `buf`, rows `stride` floats apart. `valid` false reads
// zero everywhere.
struct ResampleSource
{
    StructuredBuffer<float> buf;
    uint offset;
    uint stride;
    int x1;
    int y1;
    int x2;
    int y2;
    uint nComps;
    bool valid;
    bool blackOutside;
};

///////////////////////////////////////////////////////////////////////////////
// Source access

bool insideSrc(ResampleSource src, int x, int y)
{
    return x >= src.x1 && x < src.x2 && y >= src.y1 && y < src.y2;
}

// Inside the source.
float4 srcAt(ResampleSource src, int x, int y)
{
    float4 v = float4(0.0, 0.0, 0.0, 0.0);
    const uint base = src.offset + uint(y - src.y1) * src.stride + uint(x - src.x1) * src.nComps;
    for (uint c = 0; c < src.nComps && c < 4; ++c)
        v[c] = src.buf[base + c];
    return v;
}

// The pixel at (x, y), zero outside the source; `any` records whether it was inside.
float4 pixel(ResampleSource src, int x, int y, inout bool any)
{
    if (!insideSrc(src, x, y))
        return float4(0.0, 0.0, 0.0, 0.0);
    any = true;
    return srcAt(src, x, y);
}

int clampCoord(int v, int lo, int hiExclusive)
{
    return max(lo, min(v, hiExclusive - 1));
}

///////////////////////////////////////////////////////////////////////////////
// 1-D filters, as in Resampler.h

float4 clampValue(float4 I, float4 Ic, float4 In)
{
    const float4 lo = min(Ic, In);
    const float4 hi = max(Ic, In);
    float4 r = I;
    [ForceUnroll]
    for (int c = 0; c < 4; ++c)
    {
        if (I[c] < lo[c])
            r[c] = lo[c];
        else if (I[c] > hi[c])
            r[c] = hi[c];
    }
    return r;
}

float4 filterLinear(float4 Ic, float4 In, float d)
{
    return Ic + d * (In - Ic);
}

float4 filterCubic(float4 Ic, float4 In, float d)
{
    return Ic + d * d * ((-3.0 * Ic + 3.0 * In) + d * (2.0 * Ic - 2.0 * In));
}

float4 filter4(uint filter, bool clampOn, float4 Ip, float4 Ic, float4 In, float4 Ia, float d)
{
    float4 I;
    switch (filter)
    {
    case kFilterKeys:
        I = Ic + d * ((-Ip + In) + d * ((2.0 * Ip - 5.0 * Ic + 4.0 * In - Ia) + d * (-Ip + 3.0 * Ic - 3.0 * In + Ia))) / 2.0;
        break;
    case kFilterSimon:
        I = Ic + d * ((-3.0 * Ip + 3.0 * In) + d * ((6.0 * Ip - 9.0 * Ic + 6.0 * In - 3.0 * Ia) + d * (-3.0 * Ip + 5.0 * Ic - 5.0 * In + 3.0 * Ia))) / 4.0;
        break;
    case kFilterRifman:
        I = Ic + d * ((-Ip + In) + d * ((2.0 * Ip - 2.0 * Ic + In - Ia) + d * (-Ip + Ic - In + Ia)));
        break;
    case kFilterMitchell:
        I = (Ip + 16.0 * Ic + In + d * ((-9.0 * Ip + 9.0 * In) + d * ((15.0 * Ip - 36.0 * Ic + 27.0 * In - 6.0 * Ia) + d * (-7.0 * Ip + 21.0 * Ic - 21.0 * In + 7.0 * Ia)))) / 18.0;
        break;
    case kFilterParzen:
        return (Ip + 4.0 * Ic + In + d * ((-3.0 * Ip + 3.0 * In) + d * ((3.0 * Ip - 6.0 * Ic + 3.0 * In) + d * (-Ip + 3.0 * Ic - 3.0 * In + Ia)))) / 6.0;
    case kFilterNotch:
        return (Ip + 2.0 * Ic + In + d * ((-2.0 * Ip + 2.0 * In) + d * ((Ip - Ic - In + Ia)))) / 4.0;
    default:
        return float4(0.0, 0.0, 0.0, 0.0);
    }
    return clampOn ? clampValue(I, Ic, In) : I;
}

///////////////////////////////////////////////////////////////////////////////
// Resampler's interpolateT: false (and zero) when every tap is outside the source.

// Impulse and box: the source pixel `cell`, the floor of the position.
bool cellPixel(ResampleSource src, int2 cell, out float4 result)
{
    result = float4(0.0, 0.0, 0.0, 0.0);
    if (!src.valid)
        return false;
    int mx = cell.x;
    int my = cell.y;
    if (!src.blackOutside)
    {
        mx = clampCoord(mx, src.x1, src.x2);
        my = clampCoord(my, src.y1, src.y2);
    }
    if (!insideSrc(src, mx, my))
        return false;
    result = srcAt(src, mx, my);
    return true;
}

// The bilinear to notch filters, at the position (px + 0.5, py + 0.5).
bool interpolateAt(ResampleSource src, uint filter, bool clampOn, Pos px, Pos py, out float4 result)
{
    result = float4(0.0, 0.0, 0.0, 0.0);
    if (!src.valid)
        return false;
    const bool black = src.blackOutside;

    if (filter == kFilterBilinear || filter == kFilterCubic)
    {
        int cx = px.i;
        int cy = py.i;
        int nx = cx + 1;
        int ny = cy + 1;
        if (!black)
        {
            cx = clampCoord(cx, src.x1, src.x2);
            cy = clampCoord(cy, src.y1, src.y2);
            nx = clampCoord(nx, src.x1, src.x2);
            ny = clampCoord(ny, src.y1, src.y2);
        }
        const float dx = clamp(float(px.i - cx) + px.f, 0.0, 1.0);
        const float dy = clamp(float(py.i - cy) + py.f, 0.0, 1.0);
        bool any = false;
        const float4 Icc = pixel(src, cx, cy, any);
        const float4 Inc = pixel(src, nx, cy, any);
        const float4 Icn = pixel(src, cx, ny, any);
        const float4 Inn = pixel(src, nx, ny, any);
        if (!any)
            return false;
        if (filter == kFilterBilinear)
            result = filterLinear(filterLinear(Icc, Inc, dx), filterLinear(Icn, Inn, dx), dy);
        else
            result = filterCubic(filterCubic(Icc, Inc, dx), filterCubic(Icn, Inn, dx), dy);
        return true;
    }

    int xs[4];
    int ys[4];
    [ForceUnroll]
    for (int k = 0; k < 4; ++k)
    {
        xs[k] = px.i - 1 + k;
        ys[k] = py.i - 1 + k;
        if (!black)
        {
            xs[k] = clampCoord(xs[k], src.x1, src.x2);
            ys[k] = clampCoord(ys[k], src.y1, src.y2);
        }
    }
    const float dx = clamp(float(px.i - xs[1]) + px.f, 0.0, 1.0);
    const float dy = clamp(float(py.i - ys[1]) + py.f, 0.0, 1.0);
    bool any = false;
    float4 rows[4];
    [ForceUnroll]
    for (int r = 0; r < 4; ++r)
    {
        const float4 Ip = pixel(src, xs[0], ys[r], any);
        const float4 Ic = pixel(src, xs[1], ys[r], any);
        const float4 In = pixel(src, xs[2], ys[r], any);
        const float4 Ia = pixel(src, xs[3], ys[r], any);
        rows[r] = filter4(filter, clampOn, Ip, Ic, In, Ia, dx);
    }
    if (!any)
        return false;
    result = filter4(filter, clampOn, rows[0], rows[1], rows[2], rows[3], dy);
    return true;
}

bool interpolate(ResampleSource src, uint filter, bool clampOn, FF fx, FF fy, out float4 result)
{
    if (filter == kFilterImpulse || filter == kFilterBox)
        return cellPixel(src, int2(toPos(fx).i, toPos(fy).i), result);
    return interpolateAt(src, filter, clampOn, toPos(ffAddF(fx, -0.5)), toPos(ffAddF(fy, -0.5)), result);
}

///////////////////////////////////////////////////////////////////////////////
// The box filter's integration, Resampler's integrate1d and integrate2d, with positions relative
// to the source origin.

float4 srcRel(ResampleSource src, int ix, int iy)
{
    return srcAt(src, src.x1 + ix, src.y1 + iy);
}

void integrate1d(ResampleSource src, int row, int n, Pos x1, Pos x2, bool zeroOutside, inout float4 v)
{
    int ifirst;
    float fracfirst;
    if (x1.i < 0)
    {
        ifirst = 0;
        fracfirst = 0.0;
    }
    else if (x1.i >= n)
    {
        ifirst = n - 1;
        fracfirst = 0.0;
    }
    else
    {
        ifirst = x1.i;
        fracfirst = x1.f;
    }
    int ilast;
    float fraclast;
    if (x2.i < 0)
    {
        ilast = 0;
        fraclast = 0.0;
    }
    else if (x2.i >= n)
    {
        ilast = n - 1;
        fraclast = 0.0;
    }
    else
    {
        ilast = x2.i;
        fraclast = 1.0 - x2.f;
    }
    if (x1.i < 0 && !zeroOutside)
        v += srcRel(src, ifirst, row) * (-posValue(x1));
    if (fracfirst > 0.0)
        v -= srcRel(src, ifirst, row) * fracfirst;
    for (int i = ifirst; i <= ilast; ++i)
        v += srcRel(src, i, row);
    if (fraclast > 0.0)
        v -= srcRel(src, ilast, row) * fraclast;
    if (posAbove(x2, n) && !zeroOutside)
    {
        precise float w = float(x2.i - n) + x2.f;
        v += srcRel(src, ilast, row) * w;
    }
}

float4 integrate2d(ResampleSource src, Pos x1, Pos y1, Pos x2, Pos y2, bool zeroOutside)
{
    const int awidth = src.x2 - src.x1;
    const int aheight = src.y2 - src.y1;
    int ifirst;
    float fracfirst;
    if (y1.i < 0)
    {
        ifirst = 0;
        fracfirst = 0.0;
    }
    else if (y1.i >= aheight)
    {
        ifirst = aheight - 1;
        fracfirst = 0.0;
    }
    else
    {
        ifirst = y1.i;
        fracfirst = y1.f;
    }
    int ilast;
    float fraclast;
    if (y2.i < 0)
    {
        ilast = 0;
        fraclast = 0.0;
    }
    else if (y2.i >= aheight)
    {
        ilast = aheight - 1;
        fraclast = 0.0;
    }
    else
    {
        ilast = y2.i;
        fraclast = 1.0 - y2.f;
    }

    float4 v = float4(0.0, 0.0, 0.0, 0.0);
    float4 p = float4(0.0, 0.0, 0.0, 0.0);
    integrate1d(src, ifirst, awidth, x1, x2, zeroOutside, p);
    if (y1.i < 0 && !zeroOutside)
        v += p * (-posValue(y1));
    if (fracfirst > 0.0)
        v -= p * fracfirst;
    v += p;
    for (int i = ifirst + 1; i < ilast; ++i)
        integrate1d(src, i, awidth, x1, x2, zeroOutside, v);
    if (ilast > ifirst)
    {
        p = float4(0.0, 0.0, 0.0, 0.0);
        integrate1d(src, ilast, awidth, x1, x2, zeroOutside, p);
        v += p;
    }
    if (fraclast > 0.0)
        v -= p * fraclast;
    if (posAbove(y2, aheight) && !zeroOutside)
    {
        precise float w = float(y2.i - aheight) + y2.f;
        v += p * w;
    }
    return v;
}

bool outsideBounds(ResampleSource src, FF x, FF y)
{
    const int ix = toPos(x).i;
    const int iy = toPos(y).i;
    return ix < src.x1 || src.x2 <= ix || iy < src.y1 || src.y2 <= iy;
}

FF corner(FF f, FF a, FF b, float sa, float sb)
{
    const FF ha = ffHalf(a);
    const FF hb = ffHalf(b);
    return ffAdd(ffAdd(f, sa < 0.0 ? ffNeg(ha) : ha), sb < 0.0 ? ffNeg(hb) : hb);
}

// GpuResample.h's kGpuResampleMaxLevel: at most 3^4 x 3^4 supersamples.
static const int kMaxLevel = 4;

int pow3(int e)
{
    int r = 1;
    for (int k = 0; k < e; ++k)
        r *= 3;
    return r;
}

///////////////////////////////////////////////////////////////////////////////
// Resampler's interpolateSuperT for the bilinear to notch filters and the Jacobian (Jxx Jxy; Jyx Jyy)
// of state `state`, with the centre sample's fx - 0.5 and fy - 0.5 as px and py.

float4 interpolateSuperAt(ResampleSource src, uint filter, bool clampOn, FF fx, FF fy, Pos px, Pos py, FF Jxx, FF Jxy, FF Jyx, FF Jyy, uint states,
                          uint state)
{
    float4 tmp;
    const uint bits = (states >> (8 * state)) & 0xffu;
    if ((bits & 0x40u) != 0)
    {
        interpolateAt(src, filter, clampOn, px, py, tmp);
        return tmp;
    }

    const bool inside = interpolateAt(src, filter, clampOn, px, py, tmp);
    if (!inside)
    {
        // A black centre is only kept when the whole back-transformed pixel is outside.
        if (outsideBounds(src, corner(fx, Jxx, Jxy, -1.0, -1.0), corner(fy, Jyx, Jyy, -1.0, -1.0))
            && outsideBounds(src, corner(fx, Jxx, Jxy, 1.0, -1.0), corner(fy, Jyx, Jyy, 1.0, -1.0))
            && outsideBounds(src, corner(fx, Jxx, Jxy, -1.0, 1.0), corner(fy, Jyx, Jyy, -1.0, 1.0))
            && outsideBounds(src, corner(fx, Jxx, Jxy, 1.0, 1.0), corner(fy, Jyx, Jyy, 1.0, 1.0)))
            return tmp;
    }
    const int isx = min(int(bits & 7u), kMaxLevel);
    const int isy = min(int((bits >> 3) & 7u), kMaxLevel);
    if (isx == 0 && isy == 0)
        return tmp;

    const int nisx = pow3(isx);
    const int nisy = pow3(isy);
    const float jxx = Jxx.hi + Jxx.lo;
    const float jxy = Jxy.hi + Jxy.lo;
    const float jyx = Jyx.hi + Jyx.lo;
    const float jyy = Jyy.hi + Jyy.lo;
    for (int sy = -nisy / 2; sy <= nisy / 2; ++sy)
    {
        for (int sx = -nisx / 2; sx <= nisx / 2; ++sx)
        {
            if (sx != 0 || sy != 0)
            {
                const float ox = (jxx * float(sx)) / float(nisx) + (jxy * float(sy)) / float(nisy);
                const float oy = (jyx * float(sx)) / float(nisx) + (jyy * float(sy)) / float(nisy);
                float4 s;
                interpolate(src, kFilterBilinear, false, ffAddF(fx, ox), ffAddF(fy, oy), s);
                tmp += s;
            }
        }
    }
    return tmp / float(nisx * nisy);
}

// Resampler's interpolateSuperT for every filter but impulse, which the callers sample unsupersampled.
float4 interpolateSuper(ResampleSource src, uint filter, bool clampOn, FF fx, FF fy, FF Jxx, FF Jxy, FF Jyx, FF Jyy, uint states, uint state)
{
    float4 tmp;
    const uint bits = (states >> (8 * state)) & 0xffu;
    if ((bits & 0x40u) != 0)
    {
        interpolate(src, filter, clampOn, fx, fy, tmp);
        return tmp;
    }
    if (filter == kFilterBox)
    {
        const FF c0x = corner(fx, Jxx, Jxy, -1.0, -1.0);
        const FF c0y = corner(fy, Jyx, Jyy, -1.0, -1.0);
        const FF c1x = corner(fx, Jxx, Jxy, 1.0, -1.0);
        const FF c1y = corner(fy, Jyx, Jyy, 1.0, -1.0);
        const FF c2x = corner(fx, Jxx, Jxy, -1.0, 1.0);
        const FF c2y = corner(fy, Jyx, Jyy, -1.0, 1.0);
        const FF c3x = corner(fx, Jxx, Jxy, 1.0, 1.0);
        const FF c3y = corner(fy, Jyx, Jyy, 1.0, 1.0);
        const FF x1 = ffMin(ffMin(c0x, c1x), ffMin(c2x, c3x));
        const FF x2 = ffMax(ffMax(c0x, c1x), ffMax(c2x, c3x));
        const FF y1 = ffMin(ffMin(c0y, c1y), ffMin(c2y, c3y));
        const FF y2 = ffMax(ffMax(c0y, c1y), ffMax(c2y, c3y));
        if (!ffLess(x1, x2) || !ffLess(y1, y2))
        {
            interpolate(src, filter, clampOn, fx, fy, tmp);
            return tmp;
        }
        const Pos px1 = toPos(ffAddF(x1, -float(src.x1)));
        const Pos px2 = toPos(ffAddF(x2, -float(src.x1)));
        const Pos py1 = toPos(ffAddF(y1, -float(src.y1)));
        const Pos py2 = toPos(ffAddF(y2, -float(src.y1)));
        float4 v = integrate2d(src, px1, py1, px2, py2, src.blackOutside);
        const FF wx = ffAdd(x2, ffNeg(x1));
        const FF wy = ffAdd(y2, ffNeg(y1));
        precise float s = (wx.hi + wx.lo) * (wy.hi + wy.lo);
        if (s != 0.0)
            v /= s;
        return v;
    }

    return interpolateSuperAt(src, filter, clampOn, fx, fy, toPos(ffAddF(fx, -0.5)), toPos(ffAddF(fy, -0.5)), Jxx, Jxy, Jyx, Jyy, states,
                              state);
}

// The states byte of Jacobian (Jxx Jxy; Jyx Jyy) as Resampler::supersampleLevels decides it: level n
// along an axis once the squared norm of its column exceeds 3^(2n - 1), up to 4.
uint jacobianStates(FF Jxx, FF Jxy, FF Jyx, FF Jyy)
{
    if (Jxx.hi == 0.0 && Jxy.hi == 0.0 && Jyx.hi == 0.0 && Jyy.hi == 0.0)
        return 0x40u;
    const FF dx = ffAdd(ffMul(Jxx, Jxx), ffMul(Jyx, Jyx));
    const FF dy = ffAdd(ffMul(Jxy, Jxy), ffMul(Jyy, Jyy));
    uint levelX = 0;
    uint levelY = 0;
    float threshold = 3.0;
    [ForceUnroll]
    for (int n = 0; n < kMaxLevel; ++n)
    {
        if (ffLess(ffMake(threshold, 0.0), dx))
            ++levelX;
        if (ffLess(ffMake(threshold, 0.0), dy))
            ++levelY;
        threshold *= 9.0;
    }
    return levelX | (levelY << 3);
}

// One entry of the Jacobian of (X / Z, Y / Z), as Resampler's samplePixel computes it: (h Z - P g) / Z^2,
// with h the matrix entry of that row and column, P the row's X or Y, and g the third row's entry in
// that column.
