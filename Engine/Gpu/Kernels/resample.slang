// Resampler::resampleRow for affine inverse matrices: the ten filters of Resampler::filter1D,
// Resampler::interpolate and Resampler::interpolateSuper (supersampling and the box integration),
// reading the source from a storage buffer. resample_main samples one matrix; resample_blur_main
// averages several per pixel as Resampler::motionBlurPixel does. Both carry sample positions in two
// floats, which the bilinear to notch filters are continuous in. Impulse and box pick a source pixel,
// and box a supersampling state, by floors of the CPU's double positions that two floats round to the
// other side of on exact scales; their entries, resample_exact_main and resample_blur_exact_main,
// compute those positions bit for bit. The integer double emulation is kept out of the two-float
// entries because it is many times slower to run and to compile.
// GpuResampleParams in Engine/Gpu/GpuResample.h mirrors ResampleParams.

StructuredBuffer<float> srcBuf;
RWStructuredBuffer<float> dstBuf;
// resample_blur_main's matrices, laid out as Engine/Gpu/GpuResample.cpp writes them: a header of
// kBlurHeaderWords words, then kBlurMatrixWords words per matrix.
StructuredBuffer<uint> blurBuf;

// The destination's offset addresses the output origin. The source's addresses (srcX1, srcY1).
// The matrix rows (h00 h01 h02) and (h10 h11 h12) are, for the two-float entries, the six high
// floats' bits in m0-m5 and the six low floats' in m6-m11, and for the exact entries the doubles'
// bits, low word first, in m0-m11. `states`
// holds one byte per Jacobian state (bit 0: x inside, bit 1: y inside): levelX in bits 0-2,
// levelY in bits 3-5, bit 6 set when that Jacobian is zero. resample_blur_main reads the matrices
// and their states from blurBuf instead.
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
    uint m0;
    uint m1;
    uint m2;
    uint m3;
    uint m4;
    uint m5;
    uint m6;
    uint m7;
    uint m8;
    uint m9;
    uint m10;
    uint m11;
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

FF ffMul(FF a, FF b)
{
    precise float p = a.hi * b.hi;
    precise float e = fma(a.hi, b.hi, -p);
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
// IEEE binary64 addition and multiplication, rounded to nearest even in 32-bit integer arithmetic.
// Impulse and box take floor() of the CPU's double positions, and Reformat's exact scales put
// those positions on integers, where two floats (48 bits, and only as exact as the device's fma)
// round to the other side. Results are assumed normal: subnormals flush to zero and the exponent
// is not checked for overflow, which no sample position reaches.

// The raw bits of a double.
struct Dbl
{
    uint lo;
    uint hi;
};

Dbl dblMake(uint lo, uint hi)
{
    Dbl d;
    d.lo = lo;
    d.hi = hi;
    return d;
}

bool dblIsZero(Dbl d)
{
    return (d.hi & 0x7fffffffu) == 0 && d.lo == 0;
}

bool dblNegative(Dbl d)
{
    return (d.hi >> 31) != 0;
}

Dbl dblNeg(Dbl d)
{
    return dblMake(d.lo, d.hi ^ 0x80000000u);
}

// |a| < |b|: the magnitude bits order as the magnitudes do.
bool dblMagLess(Dbl a, Dbl b)
{
    const uint ah = a.hi & 0x7fffffffu;
    const uint bh = b.hi & 0x7fffffffu;
    return ah < bh || (ah == bh && a.lo < b.lo);
}

bool dblLess(Dbl a, Dbl b)
{
    if (dblIsZero(a) && dblIsZero(b))
        return false;
    const bool an = dblNegative(a);
    if (an != dblNegative(b))
        return an;
    return an ? dblMagLess(b, a) : dblMagLess(a, b);
}

Dbl dblMin(Dbl a, Dbl b)
{
    return dblLess(b, a) ? b : a;
}

Dbl dblMax(Dbl a, Dbl b)
{
    return dblLess(a, b) ? b : a;
}

// 96-bit unsigned integers, x the low word.

uint3 u96Shl(uint3 v, uint s)
{
    if (s >= 96)
        return uint3(0, 0, 0);
    if (s >= 64)
    {
        v = uint3(0, 0, v.x);
        s -= 64;
    }
    else if (s >= 32)
    {
        v = uint3(0, v.x, v.y);
        s -= 32;
    }
    if (s == 0)
        return v;
    return uint3(v.x << s, (v.y << s) | (v.x >> (32 - s)), (v.z << s) | (v.y >> (32 - s)));
}

uint3 u96Shr(uint3 v, uint s)
{
    if (s >= 96)
        return uint3(0, 0, 0);
    if (s >= 64)
    {
        v = uint3(v.z, 0, 0);
        s -= 64;
    }
    else if (s >= 32)
    {
        v = uint3(v.y, v.z, 0);
        s -= 32;
    }
    if (s == 0)
        return v;
    return uint3((v.x >> s) | (v.y << (32 - s)), (v.y >> s) | (v.z << (32 - s)), v.z >> s);
}

// The bits [0, n) of a word, n at most 32.
uint lowMask(uint n)
{
    return n >= 32 ? 0xffffffffu : (1u << n) - 1u;
}

// Whether any of bits [0, n) is set.
bool u96AnyBelow(uint3 v, uint n)
{
    if (n <= 32)
        return (v.x & lowMask(n)) != 0;
    if (n <= 64)
        return v.x != 0 || (v.y & lowMask(n - 32)) != 0;
    return v.x != 0 || v.y != 0 || (v.z & lowMask(min(n - 64, 32u))) != 0;
}

bool u96Bit(uint3 v, uint n)
{
    const uint w = n < 32 ? v.x : (n < 64 ? v.y : v.z);
    return ((w >> (n & 31u)) & 1u) != 0;
}

// The index of the highest set bit, -1 for zero.
int u96Msb(uint3 v)
{
    if (v.z != 0)
        return 64 + int(firstbithigh(v.z));
    if (v.y != 0)
        return 32 + int(firstbithigh(v.y));
    if (v.x != 0)
        return int(firstbithigh(v.x));
    return -1;
}

uint3 u96Add(uint3 a, uint3 b)
{
    const uint x = a.x + b.x;
    const uint cx = x < a.x ? 1u : 0u;
    const uint y0 = a.y + b.y;
    const uint y = y0 + cx;
    const uint cy = (y0 < a.y || y < y0) ? 1u : 0u;
    return uint3(x, y, a.z + b.z + cy);
}

// a - b for a >= b.
uint3 u96Sub(uint3 a, uint3 b)
{
    const uint x = a.x - b.x;
    const uint bx = a.x < b.x ? 1u : 0u;
    const uint y0 = a.y - b.y;
    const uint y = y0 - bx;
    const uint by = (a.y < b.y || y0 < bx) ? 1u : 0u;
    return uint3(x, y, a.z - b.z - by);
}

// The 64-bit product of two words, as (low, high).
uint2 mulWide(uint a, uint b)
{
    const uint a0 = a & 0xffffu;
    const uint a1 = a >> 16;
    const uint b0 = b & 0xffffu;
    const uint b1 = b >> 16;
    const uint p00 = a0 * b0;
    const uint p01 = a0 * b1;
    const uint p10 = a1 * b0;
    const uint p11 = a1 * b1;
    const uint mid = (p00 >> 16) + (p01 & 0xffffu) + (p10 & 0xffffu);
    return uint2((p00 & 0xffffu) | (mid << 16), p11 + (p01 >> 16) + (p10 >> 16) + (mid >> 16));
}

// A double as (-1)^neg * m * 2^e with m below 2^53.
struct DblParts
{
    bool neg;
    int e;
    uint3 m;
};

DblParts dblUnpack(Dbl d)
{
    DblParts p;
    p.neg = dblNegative(d);
    const int be = int((d.hi >> 20) & 0x7ffu);
    const uint frac = d.hi & 0xfffffu;
    if (be == 0)
    {
        p.e = -1074;
        p.m = uint3(d.lo, frac, 0);
    }
    else
    {
        p.e = be - 1075;
        p.m = uint3(d.lo, frac | 0x100000u, 0);
    }
    return p;
}

// (-1)^neg * n * 2^e rounded to the nearest double, ties to even.
Dbl dblRound(bool neg, uint3 n, int e)
{
    const uint sign = neg ? 0x80000000u : 0u;
    const int top = u96Msb(n);
    if (top < 0)
        return dblMake(0, sign);
    uint3 m;
    if (top <= 52)
    {
        m = u96Shl(n, uint(52 - top));
        e -= 52 - top;
    }
    else
    {
        const uint s = uint(top - 52);
        m = u96Shr(n, s);
        if (u96Bit(n, s - 1) && (u96AnyBelow(n, s - 1) || (m.x & 1u) != 0))
        {
            m = u96Add(m, uint3(1, 0, 0));
            if ((m.y >> 21) != 0)
            {
                m = u96Shr(m, 1);
                e += 1;
            }
        }
        e += int(s);
    }
    const int be = e + 1075;
    if (be <= 0)
        return dblMake(0, sign);
    return dblMake(m.x, sign | (uint(be) << 20) | (m.y & 0xfffffu));
}

Dbl dblFromInt(int v)
{
    const uint mag = v < 0 ? uint(0 - v) : uint(v);
    return dblRound(v < 0, uint3(mag, 0, 0), 0);
}

Dbl dblHalf(Dbl d)
{
    if (dblIsZero(d))
        return d;
    const DblParts p = dblUnpack(d);
    return dblRound(p.neg, p.m, p.e - 1);
}

// a * (k / 2), which is how the CPU's pixel-centre ordinate x + 0.5 enters, with k = 2x + 1.
Dbl dblMulHalfInt(Dbl a, int k)
{
    const bool neg = dblNegative(a) != (k < 0);
    if (dblIsZero(a) || k == 0)
        return dblMake(0, neg ? 0x80000000u : 0u);
    const DblParts p = dblUnpack(a);
    const uint mag = k < 0 ? uint(0 - k) : uint(k);
    const uint2 low = mulWide(p.m.x, mag);
    const uint2 high = mulWide(p.m.y, mag);
    const uint mid = low.y + high.x;
    const uint carry = mid < low.y ? 1u : 0u;
    return dblRound(neg, uint3(low.x, mid, high.y + carry), p.e - 1);
}

Dbl dblAdd(Dbl a, Dbl b)
{
    if (dblIsZero(b))
        return dblIsZero(a) ? dblMake(0, a.hi & b.hi) : a;
    if (dblIsZero(a))
        return b;
    if (dblMagLess(a, b))
    {
        const Dbl t = a;
        a = b;
        b = t;
    }
    const DblParts pa = dblUnpack(a);
    const DblParts pb = dblUnpack(b);
    // With the larger operand 40 bits up, an aligned smaller one either fits exactly or loses bits
    // far enough below the rounding position that one sticky bit in their place rounds the same.
    const uint d = uint(pa.e - pb.e);
    const uint3 na = u96Shl(pa.m, 40);
    uint3 nb;
    if (d <= 40)
    {
        nb = u96Shl(pb.m, 40 - d);
    }
    else
    {
        nb = u96Shr(pb.m, d - 40);
        if (u96AnyBelow(pb.m, d - 40))
            nb.x |= 1u;
    }
    const bool same = pa.neg == pb.neg;
    const uint3 n = same ? u96Add(na, nb) : u96Sub(na, nb);
    if (!same && all(n == uint3(0, 0, 0)))
        return dblMake(0, 0);
    return dblRound(pa.neg, n, pa.e - 40);
}

Dbl dblSub(Dbl a, Dbl b)
{
    return dblAdd(a, dblNeg(b));
}

// Positions are clamped this far out, beyond any source or output; the CPU's int conversion of
// such a floor is undefined anyway.
static const int kDblFloorLimit = 0x40000000;

int dblFloor(Dbl d)
{
    if (dblIsZero(d))
        return 0;
    const DblParts p = dblUnpack(d);
    if (p.e >= 0)
        return p.neg ? -kDblFloorLimit : kDblFloorLimit;
    const uint s = uint(-p.e);
    const uint3 whole = u96Shr(p.m, s);
    if (whole.y != 0 || whole.z != 0 || whole.x >= uint(kDblFloorLimit))
        return p.neg ? -kDblFloorLimit : kDblFloorLimit;
    const int v = int(whole.x);
    if (!p.neg)
        return v;
    return u96AnyBelow(p.m, s) ? -v - 1 : -v;
}

// 2^n, zero below the normal range.
float pow2f(int n)
{
    if (n < -126)
        return 0.0;
    return asfloat(uint(min(n, 127) + 127) << 23);
}

// The double to two floats, its 53 bits split 24 + 29 so that the high part is exact.
FF dblToFF(Dbl d)
{
    if (dblIsZero(d))
        return ffMake(0.0, 0.0);
    const DblParts p = dblUnpack(d);
    const uint top = (p.m.y << 3) | (p.m.x >> 29);
    const uint bottom = p.m.x & 0x1fffffffu;
    const float s = p.neg ? -1.0 : 1.0;
    return ffQuick(s * float(top) * pow2f(p.e + 29), s * float(bottom) * pow2f(p.e));
}

float dblToFloat(Dbl d)
{
    const FF f = dblToFF(d);
    return f.hi + f.lo;
}

Pos dblToPos(Dbl d)
{
    Pos p;
    p.i = dblFloor(d);
    p.f = dblToFloat(dblSub(d, dblFromInt(p.i)));
    return p;
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

// Impulse and box: the source pixel `cell`, the floor of the position.
bool cellPixel(int2 cell, out float4 result)
{
    result = float4(0.0, 0.0, 0.0, 0.0);
    if (!srcValid())
        return false;
    int mx = cell.x;
    int my = cell.y;
    if (!blackOutside())
    {
        mx = clampCoord(mx, rp.srcX1, rp.srcX2);
        my = clampCoord(my, rp.srcY1, rp.srcY2);
    }
    if (!insideSrc(mx, my))
        return false;
    result = srcAt(mx, my);
    return true;
}

// The bilinear to notch filters.
bool interpolate(uint filter, bool clampOn, FF fx, FF fy, out float4 result)
{
    result = float4(0.0, 0.0, 0.0, 0.0);
    if (!srcValid())
        return false;
    const bool black = blackOutside();

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
// Resampler's interpolateSuperT for the bilinear to notch filters and the Jacobian (Jxx Jxy; Jyx Jyy)
// of state `state`.

float4 interpolateSuper(uint filter, bool clampOn, FF fx, FF fy, FF Jxx, FF Jxy, FF Jyx, FF Jyy, uint states, uint state)
{
    float4 tmp;
    const uint bits = (states >> (8 * state)) & 0xffu;
    if ((bits & 0x40u) != 0)
    {
        interpolate(filter, clampOn, fx, fy, tmp);
        return tmp;
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
    for (int sy = -nisy / 2; sy <= nisy / 2; ++sy)
    {
        for (int sx = -nisx / 2; sx <= nisx / 2; ++sx)
        {
            if (sx != 0 || sy != 0)
            {
                const float ox = (jxx * float(sx)) / float(nisx) + (jxy * float(sy)) / float(nisy);
                const float oy = (jyx * float(sx)) / float(nisx) + (jyy * float(sy)) / float(nisy);
                float4 s;
                interpolate(kFilterBilinear, false, ffAddF(fx, ox), ffAddF(fy, oy), s);
                tmp += s;
            }
        }
    }
    return tmp / float(nisx * nisy);
}

///////////////////////////////////////////////////////////////////////////////

// The matrix rows (h00 h01 h02) and (h10 h11 h12), with `states` as in ResampleParams.
struct Matrix
{
    FF h00;
    FF h01;
    FF h02;
    FF h10;
    FF h11;
    FF h12;
    uint states;
};

float4 samplePixel(Matrix m, int x, int y)
{
    if (!srcValid())
        return float4(0.0, 0.0, 0.0, 0.0);
    const uint filter = rp.filter;
    const bool clampOn = (rp.flags & kFlagClamp) != 0;
    const float px = float(x) + 0.5;
    const float py = float(y) + 0.5;
    const FF fx = ffAdd(ffAdd(ffMulF(m.h00, px), ffMulF(m.h01, py)), m.h02);
    const FF fy = ffAdd(ffAdd(ffMulF(m.h10, px), ffMulF(m.h11, py)), m.h12);

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
    return interpolateSuper(filter, clampOn, fx, fy, xinside ? m.h00 : zero, xinside ? m.h01 : zero, yinside ? m.h10 : zero,
                            yinside ? m.h11 : zero, m.states, state);
}

// The same matrix as the doubles' bits.
struct MatrixBits
{
    Dbl h00;
    Dbl h01;
    Dbl h02;
    Dbl h10;
    Dbl h11;
    Dbl h12;
    uint states;
};

// Impulse and box at the CPU's double position: the pixel it falls in, or for box with a nonzero
// Jacobian, Resampler's integration over the back-transformed pixel.
float4 samplePixelExact(MatrixBits m, int x, int y)
{
    float4 r = float4(0.0, 0.0, 0.0, 0.0);
    if (!srcValid())
        return r;
    // The CPU's h00 * (x + 0.5) + h01 * (y + 0.5) + h02, in that order and unfused.
    const int kx = 2 * x + 1;
    const int ky = 2 * y + 1;
    const Dbl dx = dblAdd(dblAdd(dblMulHalfInt(m.h00, kx), dblMulHalfInt(m.h01, ky)), m.h02);
    const Dbl dy = dblAdd(dblAdd(dblMulHalfInt(m.h10, kx), dblMulHalfInt(m.h11, ky)), m.h12);
    const int2 cell = int2(dblFloor(dx), dblFloor(dy));
    if (rp.filter != kFilterBox)
    {
        cellPixel(cell, r);
        return r;
    }

    const Dbl half = dblMake(0, 0x3fe00000u);
    // x1 <= x + 0.5 && x - 0.5 < x2, and likewise for y.
    bool xinside = dblFloor(dblAdd(dx, half)) >= rp.srcX1 && dblFloor(dblSub(dx, half)) < rp.srcX2;
    bool yinside = dblFloor(dblAdd(dy, half)) >= rp.srcY1 && dblFloor(dblSub(dy, half)) < rp.srcY2;
    if (blackOutside() && !(xinside && yinside))
    {
        xinside = false;
        yinside = false;
    }
    const uint state = (xinside ? 1u : 0u) | (yinside ? 2u : 0u);
    if ((((m.states >> (8 * state)) & 0xffu) & 0x40u) != 0)
    {
        cellPixel(cell, r);
        return r;
    }

    // The corners dx -+ Jxx / 2 -+ Jxy / 2, added in the CPU's order. One loop body keeps a single
    // copy of the emulated additions in the compiled kernel.
    const Dbl zero = dblMake(0, 0);
    const Dbl hxx = dblHalf(xinside ? m.h00 : zero);
    const Dbl hxy = dblHalf(xinside ? m.h01 : zero);
    const Dbl hyx = dblHalf(yinside ? m.h10 : zero);
    const Dbl hyy = dblHalf(yinside ? m.h11 : zero);
    Dbl x1 = dx;
    Dbl x2 = dx;
    Dbl y1 = dy;
    Dbl y2 = dy;
    [loop]
    for (int k = 0; k < 4; ++k)
    {
        const bool sa = (k & 1) != 0;
        const bool sb = (k & 2) != 0;
        const Dbl cx = dblAdd(dblAdd(dx, sa ? hxx : dblNeg(hxx)), sb ? hxy : dblNeg(hxy));
        const Dbl cy = dblAdd(dblAdd(dy, sa ? hyx : dblNeg(hyx)), sb ? hyy : dblNeg(hyy));
        if (k == 0)
        {
            x1 = cx;
            x2 = cx;
            y1 = cy;
            y2 = cy;
        }
        else
        {
            x1 = dblMin(x1, cx);
            x2 = dblMax(x2, cx);
            y1 = dblMin(y1, cy);
            y2 = dblMax(y2, cy);
        }
    }
    if (!dblLess(x1, x2) || !dblLess(y1, y2))
    {
        cellPixel(cell, r);
        return r;
    }
    const Dbl ox = dblFromInt(rp.srcX1);
    const Dbl oy = dblFromInt(rp.srcY1);
    const Dbl rx1 = dblSub(x1, ox);
    const Dbl rx2 = dblSub(x2, ox);
    const Dbl ry1 = dblSub(y1, oy);
    const Dbl ry2 = dblSub(y2, oy);
    float4 v = integrate2d(dblToPos(rx1), dblToPos(ry1), dblToPos(rx2), dblToPos(ry2), blackOutside());
    precise float s = dblToFloat(dblSub(rx2, rx1)) * dblToFloat(dblSub(ry2, ry1));
    if (s != 0.0)
        v /= s;
    return v;
}

void storePixel(uint3 tid, float4 v)
{
    const uint base = rp.dstOffset + tid.y * rp.dstStride + tid.x * rp.nComps;
    for (uint c = 0; c < rp.nComps && c < 4; ++c)
        dstBuf[base + c] = v[c];
}

[shader("compute")]
[numthreads(8, 8, 1)]
void resample_main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= rp.width || tid.y >= rp.height)
        return;
    Matrix m;
    m.h00 = ffMake(asfloat(rp.m0), asfloat(rp.m6));
    m.h01 = ffMake(asfloat(rp.m1), asfloat(rp.m7));
    m.h02 = ffMake(asfloat(rp.m2), asfloat(rp.m8));
    m.h10 = ffMake(asfloat(rp.m3), asfloat(rp.m9));
    m.h11 = ffMake(asfloat(rp.m4), asfloat(rp.m10));
    m.h12 = ffMake(asfloat(rp.m5), asfloat(rp.m11));
    m.states = rp.states;
    storePixel(tid, samplePixel(m, rp.outX + int(tid.x), rp.outY + int(tid.y)));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void resample_exact_main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= rp.width || tid.y >= rp.height)
        return;
    MatrixBits m;
    m.h00 = dblMake(rp.m0, rp.m1);
    m.h01 = dblMake(rp.m2, rp.m3);
    m.h02 = dblMake(rp.m4, rp.m5);
    m.h10 = dblMake(rp.m6, rp.m7);
    m.h11 = dblMake(rp.m8, rp.m9);
    m.h12 = dblMake(rp.m10, rp.m11);
    m.states = rp.states;
    storePixel(tid, samplePixelExact(m, rp.outX + int(tid.x), rp.outY + int(tid.y)));
}

///////////////////////////////////////////////////////////////////////////////
// Resampler's motionBlurPixelT. The CPU accumulates in doubles and stops on the variance, so the
// sums here are kept in two floats: a sample more or less moves the mean by far more than the
// tolerance.

// The header: matrix count, minimum samples, maximum samples, the seed's offset
// (unsigned(0x10000 * motionBlur)), the squared maximum error as hi and lo float bits, and 1 when the
// matrices are weighted. A matrix: twelve words holding h00 h01 h02 h10 h11 h12 as the push
// constants' m0-m11 do for the entry that reads them, its states, and its weight as hi and lo float
// bits.
static const uint kBlurHeaderWords = 8;
static const uint kBlurMatrixWords = 16;

uint motionBlurHash(uint a)
{
    a = (a ^ 61u) ^ (a >> 16);
    a = a + (a << 3);
    a = a ^ (a >> 4);
    a = a * 0x27d4eb2du;
    a = a ^ (a >> 15);
    return a;
}

// floor(vanDerCorput2(seed) * count) exactly: the sequence's element is reversebits(seed) / 2^32,
// and count is below 2^16.
uint vanDerCorputIndex(uint rev, uint count)
{
    return ((rev >> 16) * count + (((rev & 0xffffu) * count) >> 16)) >> 16;
}

Matrix blurMatrix(uint t)
{
    const uint base = kBlurHeaderWords + t * kBlurMatrixWords;
    Matrix m;
    m.h00 = ffMake(asfloat(blurBuf[base + 0]), asfloat(blurBuf[base + 6]));
    m.h01 = ffMake(asfloat(blurBuf[base + 1]), asfloat(blurBuf[base + 7]));
    m.h02 = ffMake(asfloat(blurBuf[base + 2]), asfloat(blurBuf[base + 8]));
    m.h10 = ffMake(asfloat(blurBuf[base + 3]), asfloat(blurBuf[base + 9]));
    m.h11 = ffMake(asfloat(blurBuf[base + 4]), asfloat(blurBuf[base + 10]));
    m.h12 = ffMake(asfloat(blurBuf[base + 5]), asfloat(blurBuf[base + 11]));
    m.states = blurBuf[base + 12];
    return m;
}

MatrixBits blurMatrixBits(uint t)
{
    const uint base = kBlurHeaderWords + t * kBlurMatrixWords;
    MatrixBits m;
    m.h00 = dblMake(blurBuf[base + 0], blurBuf[base + 1]);
    m.h01 = dblMake(blurBuf[base + 2], blurBuf[base + 3]);
    m.h02 = dblMake(blurBuf[base + 4], blurBuf[base + 5]);
    m.h10 = dblMake(blurBuf[base + 6], blurBuf[base + 7]);
    m.h11 = dblMake(blurBuf[base + 8], blurBuf[base + 9]);
    m.h12 = dblMake(blurBuf[base + 10], blurBuf[base + 11]);
    m.states = blurBuf[base + 12];
    return m;
}

FF blurWeight(uint t)
{
    const uint base = kBlurHeaderWords + t * kBlurMatrixWords;
    return ffMake(asfloat(blurBuf[base + 13]), asfloat(blurBuf[base + 14]));
}

// The samples the variance asks for, as the CPU's (int)(var / maxErr2); 0 asks for none more. On
// x86 the CPU's conversion of a quotient at or past 2^31, or of a NaN, gives INT_MIN, which asks for
// none more either.
int samplesForVariance(FF var, FF maxErr2)
{
    const FF q = ffDiv(var, maxErr2);
    if (!(q.hi >= 0.0) || !(q.hi < 2147483648.0))
        return 0;
    return max(toPos(q).i, 0);
}

// How a blurred sample is taken: from the two-float or the exact matrix words.
interface IBlurSampler
{
    float4 sample(uint t, int x, int y);
};

struct TwoFloatSampler : IBlurSampler
{
    float4 sample(uint t, int x, int y)
    {
        return samplePixel(blurMatrix(t), x, y);
    }
};

struct ExactSampler : IBlurSampler
{
    float4 sample(uint t, int x, int y)
    {
        return samplePixelExact(blurMatrixBits(t), x, y);
    }
};

float4 motionBlurPixel<S : IBlurSampler>(S sampler, int x, int y)
{
    const uint count = blurBuf[0];
    const int minSamples = int(blurBuf[1]);
    const int maxIt = int(blurBuf[2]);
    const FF maxErr2 = ffMake(asfloat(blurBuf[4]), asfloat(blurBuf[5]));
    const bool weighted = blurBuf[6] != 0;
    const uint nComps = min(rp.nComps, 4u);
    const FF zero = ffMake(0.0, 0.0);

    FF acc = zero;
    FF accPix[4] = { zero, zero, zero, zero };
    FF accPix2[4] = { zero, zero, zero, zero };
    FF mean[4] = { zero, zero, zero, zero };

    uint seed = motionBlurHash(motionBlurHash(uint(x) + blurBuf[3]) + uint(y));
    int sample = 0;
    int maxSamples = minSamples;
    while (sample < maxSamples)
    {
        for (; sample < maxSamples; ++sample, ++seed)
        {
            const uint index = vanDerCorputIndex(reversebits(seed), count);
            // (sample + vanDerCorput2(seed)) * count / minSamples, floored: the fraction left over
            // from the index cannot carry the integer quotient past the next integer.
            const uint t = (sample < minSamples) ? (uint(sample) * count + index) / uint(minSamples) : index;
            const float4 v = sampler.sample(t, x, y);
            if (!weighted)
            {
                for (uint c = 0; c < nComps; ++c)
                {
                    precise float sq = v[c] * v[c];
                    accPix[c] = ffAddF(accPix[c], v[c]);
                    accPix2[c] = ffAddF(accPix2[c], sq);
                }
            }
            else
            {
                const FF w = blurWeight(t);
                acc = ffAdd(acc, w);
                for (uint c = 0; c < nComps; ++c)
                {
                    precise float sq = v[c] * v[c];
                    accPix[c] = ffAdd(accPix[c], ffMulF(w, v[c]));
                    accPix2[c] = ffAdd(accPix2[c], ffMulF(w, sq));
                }
            }
        }
        if (!weighted)
        {
            for (uint c = 0; c < nComps; ++c)
            {
                mean[c] = ffDivF(accPix[c], float(sample));
                if (sample > 1)
                {
                    const FF spread = ffAdd(accPix2[c], ffNeg(ffMulF(ffMul(mean[c], mean[c]), float(sample))));
                    const FF var = ffDivF(spread, float(sample - 1));
                    if (maxSamples < maxIt)
                        maxSamples = max(maxSamples, min(samplesForVariance(var, maxErr2), maxIt));
                }
            }
        }
        else if (ffLess(zero, acc))
        {
            for (uint c = 0; c < nComps; ++c)
            {
                mean[c] = ffDiv(accPix[c], acc);
                if (sample > 1)
                {
                    const FF var = ffAdd(ffDiv(accPix2[c], acc), ffNeg(ffMul(mean[c], mean[c])));
                    if (maxSamples < maxIt)
                        maxSamples = max(maxSamples, min(samplesForVariance(var, maxErr2), maxIt));
                }
            }
        }
    }
    float4 r = float4(0.0, 0.0, 0.0, 0.0);
    for (uint c = 0; c < nComps; ++c)
        r[c] = mean[c].hi + mean[c].lo;
    return r;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void resample_blur_main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= rp.width || tid.y >= rp.height)
        return;
    // Every sample of a source that is not there is zero, and so is their mean.
    float4 v = float4(0.0, 0.0, 0.0, 0.0);
    if (srcValid())
    {
        TwoFloatSampler sampler;
        v = motionBlurPixel(sampler, rp.outX + int(tid.x), rp.outY + int(tid.y));
    }
    storePixel(tid, v);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void resample_blur_exact_main(uint3 tid : SV_DispatchThreadID)
{
    if (tid.x >= rp.width || tid.y >= rp.height)
        return;
    float4 v = float4(0.0, 0.0, 0.0, 0.0);
    if (srcValid())
    {
        ExactSampler sampler;
        v = motionBlurPixel(sampler, rp.outX + int(tid.x), rp.outY + int(tid.y));
    }
    storePixel(tid, v);
}
