// Resampler's filter weights, tap footprints and box integration with the source passed in, so that
// kernels with their own bindings (resample.slang, and coordmap.slang's callers under pixelwrap.slang)
// share them. Sample positions are carried in two floats, so that the filter taps and the integer
// cells the CPU picks with its double positions are the same here.

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
static const uint kFilterLanczos2 = 10;
static const uint kFilterLanczos3 = 11;
static const uint kFilterLanczos4 = 12;
static const uint kFilterSinc = 13;
static const uint kFilterBlackmanHarris = 14;
static const uint kFilterGaussian = 15;
static const uint kFilterBicubic = 16;

// Resampler::FilterSpec. lobes is only read by sinc, B and C only by bicubic.
struct FilterSpec
{
    uint filter;
    int lobes;
    float B;
    float C;
};

// Resampler::FilterSpec's defaults for `filter`.
FilterSpec defaultFilterSpec(uint filter)
{
    FilterSpec s;
    s.filter = filter;
    s.lobes = 4;
    s.B = 1.0 / 3.0;
    s.C = 1.0 / 3.0;
    return s;
}

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
// Impulse and box: the source pixel `cell`, the floor of the position; false (and zero) when it is
// outside the source.
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


FF corner(FF f, FF a, FF b, float sa, float sb)
{
    const FF ha = ffHalf(a);
    const FF hb = ffHalf(b);
    return ffAdd(ffAdd(f, sa < 0.0 ? ffNeg(ha) : ha), sb < 0.0 ? ffNeg(hb) : hb);
}

///////////////////////////////////////////////////////////////////////////////
// Resampler::FilterSpec::radius and weight, evaluated in floats on a two-float distance.

static const float kFilterPi = 3.14159265358979;

int usableLobes(int lobes)
{
    return max(2, min(lobes, 8));
}

float filterRadius(FilterSpec spec)
{
    switch (spec.filter)
    {
    case kFilterImpulse:
    case kFilterBox:
        return 0.5;
    case kFilterBilinear:
    case kFilterCubic:
        return 1.0;
    case kFilterGaussian:
        return 1.5;
    case kFilterLanczos3:
        return 3.0;
    case kFilterLanczos4:
        return 4.0;
    case kFilterSinc:
        return float(usableLobes(spec.lobes));
    default:
        return 2.0;
    }
}

// Vulkan only bounds sin() and cos() to an absolute 2^-11, which is the size of the outer lobes and
// would move every weight near a zero crossing. This series keeps the relative accuracy there.
float sinPi(float d)
{
    const float x = kFilterPi * d;
    const float x2 = x * x;
    return x * (1.0 + x2 * (-1.0 / 6.0 + x2 * (1.0 / 120.0 + x2 * (-1.0 / 5040.0 + x2 * (1.0 / 362880.0 + x2 * (-1.0 / 39916800.0 + x2 / 6227020800.0))))));
}

float cosPi(float t)
{
    const float n = floor(t + 0.5);
    const float s = sinPi(0.5 - abs(t - n));
    return (int(n) & 1) != 0 ? -s : s;
}

// sin(pi x) / (pi x) for x >= 0 that is not a nonzero integer. The integer nearest x is taken off in
// two floats first, as pi x in floats would lose the distance to that zero.
float sincPi(FF x)
{
    if (x.hi == 0.0 && x.lo == 0.0)
        return 1.0;
    const float n = floor(x.hi + 0.5);
    const FF rem = ffAddF(x, -n);
    const float s = sinPi(rem.hi + rem.lo);
    return ((int(n) & 1) != 0 ? -s : s) / (kFilterPi * (x.hi + x.lo));
}

float legacyValue(uint filter, float Ip, float Ic, float In, float Ia, float d)
{
    switch (filter)
    {
    case kFilterBilinear:
        return Ic + d * (In - Ic);
    case kFilterCubic:
        return Ic + d * d * ((-3.0 * Ic + 3.0 * In) + d * (2.0 * Ic - 2.0 * In));
    case kFilterKeys:
        return Ic + d * ((-Ip + In) + d * ((2.0 * Ip - 5.0 * Ic + 4.0 * In - Ia) + d * (-Ip + 3.0 * Ic - 3.0 * In + Ia))) / 2.0;
    case kFilterSimon:
        return Ic + d * ((-3.0 * Ip + 3.0 * In) + d * ((6.0 * Ip - 9.0 * Ic + 6.0 * In - 3.0 * Ia) + d * (-3.0 * Ip + 5.0 * Ic - 5.0 * In + 3.0 * Ia))) / 4.0;
    case kFilterRifman:
        return Ic + d * ((-Ip + In) + d * ((2.0 * Ip - 2.0 * Ic + In - Ia) + d * (-Ip + Ic - In + Ia)));
    case kFilterMitchell:
        return (Ip + 16.0 * Ic + In + d * ((-9.0 * Ip + 9.0 * In) + d * ((15.0 * Ip - 36.0 * Ic + 27.0 * In - 6.0 * Ia) + d * (-7.0 * Ip + 21.0 * Ic - 21.0 * In + 7.0 * Ia)))) / 18.0;
    case kFilterParzen:
        return (Ip + 4.0 * Ic + In + d * ((-3.0 * Ip + 3.0 * In) + d * ((3.0 * Ip - 6.0 * Ic + 3.0 * In) + d * (-Ip + 3.0 * Ic - 3.0 * In + Ia)))) / 6.0;
    case kFilterNotch:
        return (Ip + 2.0 * Ic + In + d * ((-2.0 * Ip + 2.0 * In) + d * ((Ip - Ic - In + Ia)))) / 4.0;
    default:
        return 0.0;
    }
}

float blackmanHarris(float t)
{
    return 0.35875 + 0.48829 * cosPi(t) + 0.14128 * cosPi(2.0 * t) + 0.01168 * cosPi(3.0 * t);
}

float taperedToEdge(float v, float edge)
{
    return (v - edge) / (1.0 - edge);
}

// The weight at distance a < radius from the centre. onZero says a is a nonzero integer, so that
// the sinc factor is exactly zero as the CPU's sincPi makes it.
float filterValue(FilterSpec spec, FF a, bool onZero)
{
    const float af = a.hi + a.lo;
    switch (spec.filter)
    {
    case kFilterLanczos2:
    case kFilterLanczos3:
    case kFilterLanczos4:
        return onZero ? 0.0 : sincPi(a) * sincPi(ffDivF(a, filterRadius(spec)));
    case kFilterSinc:
    {
        // 0.5 + 0.5 cos(2 theta) as cos^2(theta), which keeps its accuracy approaching the edge.
        const float c = cosPi(af / (2.0 * filterRadius(spec)));
        return onZero ? 0.0 : sincPi(a) * c * c;
    }
    case kFilterBlackmanHarris:
        return taperedToEdge(blackmanHarris(af * 0.5), blackmanHarris(1.0));
    case kFilterGaussian:
        return taperedToEdge(exp(-2.0 * af * af), exp(-4.5));
    case kFilterBicubic:
    {
        const float B = spec.B;
        const float C = spec.C;
        if (af < 1.0)
            return (6.0 - 2.0 * B + af * af * ((-18.0 + 12.0 * B + 6.0 * C) + af * (12.0 - 9.0 * B - 6.0 * C))) / 6.0;
        return (8.0 * B + 24.0 * C + af * ((-12.0 * B - 48.0 * C) + af * ((6.0 * B + 30.0 * C) + af * (-B - 6.0 * C)))) / 6.0;
    }
    default:
    {
        if (af < 1.0)
            return legacyValue(spec.filter, 0.0, 1.0, 0.0, 0.0, af);
        const FF d = ffAddF(a, -1.0);
        return legacyValue(spec.filter, 1.0, 0.0, 0.0, 0.0, d.hi + d.lo);
    }
    }
}

// A tap inside the support that floats round to zero weight, but the CPU's doubles do not, still
// counts towards the clamp range; so small a weight moves no result.
static const float kCountedWeight = 1e-30;

// Resampler::FilterSpec::weight(offset / scale). The support is an open interval.
float tapWeight(FilterSpec spec, FF offset, FF scale)
{
    const FF off = ffLess(offset, ffMake(0.0, 0.0)) ? ffNeg(offset) : offset;
    const FF a = (scale.hi == 1.0 && scale.lo == 0.0) ? off : ffDiv(off, scale);
    if (!ffLess(a, ffMake(filterRadius(spec), 0.0)))
        return 0.0;
    // The quotient may be noisy on a device whose division is not correctly rounded, so whether it
    // lies on an integer is read from the product, which is exact.
    const float n = floor(a.hi + 0.5);
    const FF rem = ffAddF(a, -n);
    bool onInteger = false;
    if (n >= 1.0 && abs(rem.hi) < 9.31322574615479e-10)
    {
        const FF diff = ffAdd(off, ffNeg(ffMulF(scale, n)));
        onInteger = diff.hi == 0.0 && diff.lo == 0.0;
    }
    const float w = filterValue(spec, a, onInteger);
    return (w == 0.0 && !onInteger) ? kCountedWeight : w;
}

///////////////////////////////////////////////////////////////////////////////
// Resampler's buildAxisTaps, resolveReads and kernelSampleT.

static const int kMaxTapsPerAxis = 81;

// One axis's footprint: the taps are decided by the caller from the floors of centre - reach and
// centre + reach, as the kernels that take the CPU's doubles need those floors from the doubles.
struct TapAxis
{
    FF centre;
    FF reach;
    FF scale;
    int first;
    int count;
    bool spread;
};

TapAxis makeTapAxis(FF centre, FF reach, FF scale, int floorLow, int ceilHigh)
{
    TapAxis a;
    a.centre = centre;
    a.reach = reach;
    a.scale = scale;
    a.first = floorLow + 1;
    const int exactCount = ceilHigh - a.first;
    a.spread = exactCount > kMaxTapsPerAxis;
    a.count = a.spread ? kMaxTapsPerAxis : max(exactCount, 0);
    return a;
}

int posCeil(Pos p)
{
    return p.f > 0.0 ? p.i + 1 : p.i;
}

// The footprint from two-float arithmetic alone.
TapAxis tapAxis(FilterSpec spec, FF coord, FF scale)
{
    const FF centre = ffAddF(ffMin(ffMax(coord, ffMake(-kPosLimit, 0.0)), ffMake(kPosLimit, 0.0)), -0.5);
    const FF reach = ffMulF(scale, filterRadius(spec));
    return makeTapAxis(centre, reach, scale, toPos(ffAdd(centre, ffNeg(reach))).i, posCeil(toPos(ffAdd(centre, reach))));
}

struct AxisTaps
{
    int count;
    bool any;
    int nearRead[kMaxTapsPerAxis];
    int farRead[kMaxTapsPerAxis];
    float frac[kMaxTapsPerAxis];
    float weight[kMaxTapsPerAxis];
};

// Relative to `lo`, -1 for a read that is black.
int readOffset(int v, int lo, int hiExclusive, bool blackOutside)
{
    if (v < lo || v >= hiExclusive)
    {
        if (blackOutside)
            return -1;
        v = clampCoord(v, lo, hiExclusive);
    }
    return v - lo;
}

void buildAxisTaps(FilterSpec spec, TapAxis a, int lo, int hiExclusive, bool blackOutside, out AxisTaps t)
{
    t.count = a.count;
    t.any = false;
    FF sum = ffMake(0.0, 0.0);
    // Spread taps need no double positions of their own: bilinear reads are continuous across the
    // pixel a tap falls in, and the clamp range counts that interpolated read.
    const FF step = a.spread ? ffDivF(ffMulF(a.reach, 2.0), float(kMaxTapsPerAxis)) : ffMake(0.0, 0.0);
    [loop]
    for (int i = 0; i < a.count; ++i)
    {
        FF offset;
        int low;
        float frac;
        if (a.spread)
        {
            offset = ffAdd(ffMulF(step, float(i) + 0.5), ffNeg(a.reach));
            const Pos p = toPos(ffAdd(a.centre, offset));
            low = p.i;
            frac = p.f;
        }
        else
        {
            low = a.first + i;
            offset = ffAddF(ffNeg(a.centre), float(low));
            frac = 0.0;
        }
        const float w = tapWeight(spec, offset, a.scale);
        t.frac[i] = frac;
        t.weight[i] = w;
        sum = ffAddF(sum, w);
        t.nearRead[i] = readOffset(low, lo, hiExclusive, blackOutside);
        t.farRead[i] = frac != 0.0 ? readOffset(low + 1, lo, hiExclusive, blackOutside) : -1;
        t.any = t.any || t.nearRead[i] >= 0 || t.farRead[i] >= 0;
    }
    const float total = sum.hi + sum.lo;
    [loop]
    for (int i = 0; i < a.count; ++i)
        t.weight[i] = total != 0.0 ? t.weight[i] / total : 0.0;
}

float4 readAlongRow(ResampleSource src, int row, int nearRead, int farRead, float frac)
{
    const float4 black = float4(0.0, 0.0, 0.0, 0.0);
    if (row < 0)
        return black;
    const float4 a = nearRead >= 0 ? srcRel(src, nearRead, row) : black;
    if (frac == 0.0)
        return a;
    const float4 b = farRead >= 0 ? srcRel(src, farRead, row) : black;
    return a * (1.0 - frac) + b * frac;
}

// Black reads count towards the clamp range, zero-weight taps do not. NaN source values are ignored
// by the range as std::min and std::max ignore them, which min() and max() do not promise.
bool kernelSample(ResampleSource src, FilterSpec spec, bool clampOn, TapAxis ax, TapAxis ay, out float4 result)
{
    result = float4(0.0, 0.0, 0.0, 0.0);
    if (!src.valid)
        return false;
    AxisTaps tx;
    AxisTaps ty;
    buildAxisTaps(spec, ax, src.x1, src.x2, src.blackOutside, tx);
    buildAxisTaps(spec, ay, src.y1, src.y2, src.blackOutside, ty);
    if (!tx.any || !ty.any)
        return false;

    const float inf = asfloat(0x7f800000u);
    float4 acc = float4(0.0, 0.0, 0.0, 0.0);
    float4 lowest = float4(inf, inf, inf, inf);
    float4 highest = -lowest;
    [loop]
    for (int j = 0; j < ty.count; ++j)
    {
        const float wy = ty.weight[j];
        if (wy == 0.0)
            continue;
        const float fracY = ty.frac[j];
        float4 rowAcc = float4(0.0, 0.0, 0.0, 0.0);
        [loop]
        for (int i = 0; i < tx.count; ++i)
        {
            const float wx = tx.weight[i];
            if (wx == 0.0)
                continue;
            float4 v = readAlongRow(src, ty.nearRead[j], tx.nearRead[i], tx.farRead[i], tx.frac[i]);
            if (fracY != 0.0)
                v = v * (1.0 - fracY) + readAlongRow(src, ty.farRead[j], tx.nearRead[i], tx.farRead[i], tx.frac[i]) * fracY;
            rowAcc += wx * v;
            if (clampOn)
            {
                [ForceUnroll]
                for (int c = 0; c < 4; ++c)
                {
                    if (v[c] < lowest[c])
                        lowest[c] = v[c];
                    if (highest[c] < v[c])
                        highest[c] = v[c];
                }
            }
        }
        acc += wy * rowAcc;
    }
    result = acc;
    if (clampOn)
    {
        [ForceUnroll]
        for (int c = 0; c < 4; ++c)
        {
            if (lowest[c] <= highest[c])
            {
                const float capped = highest[c] < acc[c] ? highest[c] : acc[c];
                result[c] = lowest[c] < capped ? capped : lowest[c];
            }
        }
    }
    return true;
}

///////////////////////////////////////////////////////////////////////////////
// Resampler::kernelScale and interpolateSuperT.

FF kernelScaleSqrt(FF a)
{
    const float x = sqrt(a.hi);
    const FF r = ffMake(x, 0.0);
    const FF diff = ffAdd(a, ffNeg(ffMulF(r, x)));
    precise float correction = (diff.hi + diff.lo) * 0.5 / x;
    return ffQuick(x, correction);
}

// hypot(a, b), 1 unless it is finite and above 1, and at most 2^30. Anything at the limit stands in
// for the sum of squares, which floats could overflow.
FF kernelScale(FF a, FF b)
{
    const FF one = ffMake(1.0, 0.0);
    if (!isFiniteBits(a.hi) || !isFiniteBits(b.hi))
        return one;
    if (abs(a.hi) >= kPosLimit || abs(b.hi) >= kPosLimit)
        return ffMake(kPosLimit, 0.0);
    const FF norm2 = ffAdd(ffMul(a, a), ffMul(b, b));
    if (!ffLess(one, norm2))
        return one;
    return ffMin(kernelScaleSqrt(norm2), ffMake(kPosLimit, 0.0));
}

float4 boxIntegrate(ResampleSource src, FF fx, FF fy, FF Jxx, FF Jxy, FF Jyx, FF Jyy)
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
    float4 v;
    if (!ffLess(x1, x2) || !ffLess(y1, y2))
    {
        cellPixel(src, int2(toPos(fx).i, toPos(fy).i), v);
        return v;
    }
    const Pos px1 = toPos(ffAddF(x1, -float(src.x1)));
    const Pos px2 = toPos(ffAddF(x2, -float(src.x1)));
    const Pos py1 = toPos(ffAddF(y1, -float(src.y1)));
    const Pos py2 = toPos(ffAddF(y2, -float(src.y1)));
    v = integrate2d(src, px1, py1, px2, py2, src.blackOutside);
    const FF wx = ffAdd(x2, ffNeg(x1));
    const FF wy = ffAdd(y2, ffNeg(y1));
    precise float s = (wx.hi + wx.lo) * (wy.hi + wy.lo);
    if (s != 0.0)
        v /= s;
    return v;
}

bool ffIsExactZero(FF a)
{
    return a.hi == 0.0 && a.lo == 0.0;
}

// The sample at (fx, fy) with the Jacobian (Jxx Jxy; Jyx Jyy) in two floats. A zero row gives its
// axis the plain kernel.
float4 interpolateSuper(ResampleSource src, FilterSpec spec, bool clampOn, FF fx, FF fy, FF Jxx, FF Jxy, FF Jyx, FF Jyy)
{
    float4 r = float4(0.0, 0.0, 0.0, 0.0);
    if (spec.filter == kFilterImpulse || spec.filter == kFilterBox)
    {
        if (spec.filter == kFilterBox && !(ffIsExactZero(Jxx) && ffIsExactZero(Jxy) && ffIsExactZero(Jyx) && ffIsExactZero(Jyy)))
            return src.valid ? boxIntegrate(src, fx, fy, Jxx, Jxy, Jyx, Jyy) : r;
        cellPixel(src, int2(toPos(fx).i, toPos(fy).i), r);
        return r;
    }
    kernelSample(src, spec, clampOn, tapAxis(spec, fx, kernelScale(Jxx, Jxy)), tapAxis(spec, fy, kernelScale(Jyx, Jyy)), r);
    return r;
}
