// Resampler::resampleRow for one affine inverse matrix without motion blur: the ten filters of
// Resampler::filter1D, Resampler::interpolate and Resampler::interpolateSuper (supersampling and the
// box integration), reading the source from a storage buffer. Sample positions are carried in two
// floats, so that the filter taps and the integer cells the CPU picks with its double positions are
// the same here. GpuResampleParams in Engine/Gpu/GpuResample.h mirrors ResampleParams.

StructuredBuffer<float> srcBuf;
RWStructuredBuffer<float> dstBuf;

// The destination's offset addresses the output origin. The source's addresses (srcX1, srcY1).
// The matrix rows (h00 h01 h02) and (h10 h11 h12) are each a double split into hi + lo. `states`
// holds one byte per Jacobian state (bit 0: x inside, bit 1: y inside): levelX in bits 0-2,
// levelY in bits 3-5, bit 6 set when that Jacobian is zero.
struct ResampleParams
{
    int outX;
    int outY;
    uint width;
    uint height;
    uint dstOffset;
    uint dstStride;
    uint srcOffset;
    uint srcStride;
    int srcX1;
    int srcY1;
    int srcX2;
    int srcY2;
    uint nComps;
    uint filter;
    uint flags;
    uint states;
    float h00Hi;
    float h01Hi;
    float h02Hi;
    float h10Hi;
    float h11Hi;
    float h12Hi;
    float h00Lo;
    float h01Lo;
    float h02Lo;
    float h10Lo;
    float h11Lo;
    float h12Lo;
};
[[vk::push_constant]] ConstantBuffer<ResampleParams> rp;

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

FF ffMulF(FF a, float b)
{
    precise float p = a.hi * b;
    precise float e = fma(a.hi, b, -p);
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

Pos toPos(FF a)
{
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
    Pos p;
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

///////////////////////////////////////////////////////////////////////////////
// Source access

bool srcValid()
{
    return (rp.flags & kFlagSrcValid) != 0;
}

bool blackOutside()
{
    return (rp.flags & kFlagBlackOutside) != 0;
}

bool insideSrc(int x, int y)
{
    return x >= rp.srcX1 && x < rp.srcX2 && y >= rp.srcY1 && y < rp.srcY2;
}

// Inside the source.
float4 srcAt(int x, int y)
{
    float4 v = float4(0.0, 0.0, 0.0, 0.0);
    const uint base = rp.srcOffset + uint(y - rp.srcY1) * rp.srcStride + uint(x - rp.srcX1) * rp.nComps;
    for (uint c = 0; c < rp.nComps && c < 4; ++c)
        v[c] = srcBuf[base + c];
    return v;
}

// The pixel at (x, y), zero outside the source; `any` records whether it was inside.
float4 pixel(int x, int y, inout bool any)
{
    if (!insideSrc(x, y))
        return float4(0.0, 0.0, 0.0, 0.0);
    any = true;
    return srcAt(x, y);
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

bool interpolate(uint filter, bool clampOn, FF fx, FF fy, out float4 result)
{
    result = float4(0.0, 0.0, 0.0, 0.0);
    if (!srcValid())
        return false;
    const bool black = blackOutside();

    if (filter == kFilterImpulse || filter == kFilterBox)
    {
        int mx = toPos(fx).i;
        int my = toPos(fy).i;
        if (!black)
        {
            mx = clampCoord(mx, rp.srcX1, rp.srcX2);
            my = clampCoord(my, rp.srcY1, rp.srcY2);
        }
        if (!insideSrc(mx, my))
            return false;
        result = srcAt(mx, my);
        return true;
    }

    const Pos px = toPos(ffAddF(fx, -0.5));
    const Pos py = toPos(ffAddF(fy, -0.5));
    if (filter == kFilterBilinear || filter == kFilterCubic)
    {
        int cx = px.i;
        int cy = py.i;
        int nx = cx + 1;
        int ny = cy + 1;
        if (!black)
        {
            cx = clampCoord(cx, rp.srcX1, rp.srcX2);
            cy = clampCoord(cy, rp.srcY1, rp.srcY2);
            nx = clampCoord(nx, rp.srcX1, rp.srcX2);
            ny = clampCoord(ny, rp.srcY1, rp.srcY2);
        }
        const float dx = clamp(float(px.i - cx) + px.f, 0.0, 1.0);
        const float dy = clamp(float(py.i - cy) + py.f, 0.0, 1.0);
        bool any = false;
        const float4 Icc = pixel(cx, cy, any);
        const float4 Inc = pixel(nx, cy, any);
        const float4 Icn = pixel(cx, ny, any);
        const float4 Inn = pixel(nx, ny, any);
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
            xs[k] = clampCoord(xs[k], rp.srcX1, rp.srcX2);
            ys[k] = clampCoord(ys[k], rp.srcY1, rp.srcY2);
        }
    }
    const float dx = clamp(float(px.i - xs[1]) + px.f, 0.0, 1.0);
    const float dy = clamp(float(py.i - ys[1]) + py.f, 0.0, 1.0);
    bool any = false;
    float4 rows[4];
    [ForceUnroll]
    for (int r = 0; r < 4; ++r)
    {
        const float4 Ip = pixel(xs[0], ys[r], any);
        const float4 Ic = pixel(xs[1], ys[r], any);
        const float4 In = pixel(xs[2], ys[r], any);
        const float4 Ia = pixel(xs[3], ys[r], any);
        rows[r] = filter4(filter, clampOn, Ip, Ic, In, Ia, dx);
    }
    if (!any)
        return false;
    result = filter4(filter, clampOn, rows[0], rows[1], rows[2], rows[3], dy);
    return true;
}

///////////////////////////////////////////////////////////////////////////////
// The box filter's integration, Resampler's integrate1d and integrate2d, with positions relative
// to the source origin.

float4 srcRel(int ix, int iy)
{
    return srcAt(rp.srcX1 + ix, rp.srcY1 + iy);
}

void integrate1d(int row, int n, Pos x1, Pos x2, bool zeroOutside, inout float4 v)
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
        v += srcRel(ifirst, row) * (-posValue(x1));
    if (fracfirst > 0.0)
        v -= srcRel(ifirst, row) * fracfirst;
    for (int i = ifirst; i <= ilast; ++i)
        v += srcRel(i, row);
    if (fraclast > 0.0)
        v -= srcRel(ilast, row) * fraclast;
    if (posAbove(x2, n) && !zeroOutside)
    {
        precise float w = float(x2.i - n) + x2.f;
        v += srcRel(ilast, row) * w;
    }
}

float4 integrate2d(Pos x1, Pos y1, Pos x2, Pos y2, bool zeroOutside)
{
    const int awidth = rp.srcX2 - rp.srcX1;
    const int aheight = rp.srcY2 - rp.srcY1;
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
    integrate1d(ifirst, awidth, x1, x2, zeroOutside, p);
    if (y1.i < 0 && !zeroOutside)
        v += p * (-posValue(y1));
    if (fracfirst > 0.0)
        v -= p * fracfirst;
    v += p;
    for (int i = ifirst + 1; i < ilast; ++i)
        integrate1d(i, awidth, x1, x2, zeroOutside, v);
    if (ilast > ifirst)
    {
        p = float4(0.0, 0.0, 0.0, 0.0);
        integrate1d(ilast, awidth, x1, x2, zeroOutside, p);
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

bool outsideBounds(FF x, FF y)
{
    const int ix = toPos(x).i;
    const int iy = toPos(y).i;
    return ix < rp.srcX1 || rp.srcX2 <= ix || iy < rp.srcY1 || rp.srcY2 <= iy;
}

FF corner(FF f, FF a, FF b, float sa, float sb)
{
    const FF ha = ffHalf(a);
    const FF hb = ffHalf(b);
    return ffAdd(ffAdd(f, sa < 0.0 ? ffNeg(ha) : ha), sb < 0.0 ? ffNeg(hb) : hb);
}

int pow3(int e)
{
    int r = 1;
    for (int k = 0; k < e; ++k)
        r *= 3;
    return r;
}

///////////////////////////////////////////////////////////////////////////////
// Resampler's interpolateSuperT for the Jacobian (Jxx Jxy; Jyx Jyy) of state `state`.

float4 interpolateSuper(uint filter, bool clampOn, FF fx, FF fy, FF Jxx, FF Jxy, FF Jyx, FF Jyy, uint state)
{
    float4 tmp;
    const uint bits = (rp.states >> (8 * state)) & 0xffu;
    if ((bits & 0x40u) != 0)
    {
        interpolate(filter, clampOn, fx, fy, tmp);
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
            interpolate(filter, clampOn, fx, fy, tmp);
            return tmp;
        }
        const Pos px1 = toPos(ffAddF(x1, -float(rp.srcX1)));
        const Pos px2 = toPos(ffAddF(x2, -float(rp.srcX1)));
        const Pos py1 = toPos(ffAddF(y1, -float(rp.srcY1)));
        const Pos py2 = toPos(ffAddF(y2, -float(rp.srcY1)));
        float4 v = integrate2d(px1, py1, px2, py2, blackOutside());
        const FF wx = ffAdd(x2, ffNeg(x1));
        const FF wy = ffAdd(y2, ffNeg(y1));
        precise float s = (wx.hi + wx.lo) * (wy.hi + wy.lo);
        if (s != 0.0)
            v /= s;
        return v;
    }

    const bool inside = interpolate(filter, clampOn, fx, fy, tmp);
    if (!inside)
    {
        // A black centre is only kept when the whole back-transformed pixel is outside.
        if (outsideBounds(corner(fx, Jxx, Jxy, -1.0, -1.0), corner(fy, Jyx, Jyy, -1.0, -1.0))
            && outsideBounds(corner(fx, Jxx, Jxy, 1.0, -1.0), corner(fy, Jyx, Jyy, 1.0, -1.0))
            && outsideBounds(corner(fx, Jxx, Jxy, -1.0, 1.0), corner(fy, Jyx, Jyy, -1.0, 1.0))
            && outsideBounds(corner(fx, Jxx, Jxy, 1.0, 1.0), corner(fy, Jyx, Jyy, 1.0, 1.0)))
            return tmp;
    }
    const int isx = int(bits & 7u);
    const int isy = int((bits >> 3) & 7u);
    if (isx == 0 && isy == 0)
        return tmp;

    const int nisx = pow3(isx);
    const int nisy = pow3(isy);
    const float jxx = Jxx.hi + Jxx.lo;
    const float jxy = Jxy.hi + Jxy.lo;
    const float jyx = Jyx.hi + Jyx.lo;
    const float jyy = Jyy.hi + Jyy.lo;
    for (int y = -nisy / 2; y <= nisy / 2; ++y)
    {
        for (int x = -nisx / 2; x <= nisx / 2; ++x)
        {
            if (x != 0 || y != 0)
            {
                const float ox = (jxx * float(x)) / float(nisx) + (jxy * float(y)) / float(nisy);
                const float oy = (jyx * float(x)) / float(nisx) + (jyy * float(y)) / float(nisy);
                float4 s;
                interpolate(kFilterBilinear, false, ffAddF(fx, ox), ffAddF(fy, oy), s);
                tmp += s;
            }
        }
    }
    return tmp / float(nisx * nisy);
}

///////////////////////////////////////////////////////////////////////////////

float4 samplePixel(int x, int y)
{
    if (!srcValid())
        return float4(0.0, 0.0, 0.0, 0.0);
    const uint filter = rp.filter;
    const bool clampOn = (rp.flags & kFlagClamp) != 0;
    const FF h00 = ffMake(rp.h00Hi, rp.h00Lo);
    const FF h01 = ffMake(rp.h01Hi, rp.h01Lo);
    const FF h02 = ffMake(rp.h02Hi, rp.h02Lo);
    const FF h10 = ffMake(rp.h10Hi, rp.h10Lo);
    const FF h11 = ffMake(rp.h11Hi, rp.h11Lo);
    const FF h12 = ffMake(rp.h12Hi, rp.h12Lo);
    const float px = float(x) + 0.5;
    const float py = float(y) + 0.5;
    const FF fx = ffAdd(ffAdd(ffMulF(h00, px), ffMulF(h01, py)), h02);
    const FF fy = ffAdd(ffAdd(ffMulF(h10, px), ffMulF(h11, py)), h12);

    float4 r;
    if (filter == kFilterImpulse)
    {
        interpolate(filter, clampOn, fx, fy, r);
        return r;
    }
    // x1 <= fx + 0.5 && fx - 0.5 < x2, and likewise for y.
    bool xinside = toPos(ffAddF(fx, 0.5)).i >= rp.srcX1 && toPos(ffAddF(fx, -0.5)).i < rp.srcX2;
    bool yinside = toPos(ffAddF(fy, 0.5)).i >= rp.srcY1 && toPos(ffAddF(fy, -0.5)).i < rp.srcY2;
    if (blackOutside() && !(xinside && yinside))
    {
        xinside = false;
        yinside = false;
    }
    const FF zero = ffMake(0.0, 0.0);
    const uint state = (xinside ? 1u : 0u) | (yinside ? 2u : 0u);
    return interpolateSuper(filter, clampOn, fx, fy, xinside ? h00 : zero, xinside ? h01 : zero, yinside ? h10 : zero,
                            yinside ? h11 : zero, state);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void resample_main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= rp.width || tid.y >= rp.height)
        return;
    const float4 v = samplePixel(rp.outX + int(tid.x), rp.outY + int(tid.y));
    const uint base = rp.dstOffset + tid.y * rp.dstStride + tid.x * rp.nComps;
    for (uint c = 0; c < rp.nComps && c < 4; ++c)
        dstBuf[base + c] = v[c];
}
