// Resampler::resampleRow for affine and projective inverse matrices: the ten filters of
// Resampler::filter1D, Resampler::interpolate and Resampler::interpolateSuper (supersampling and the
// box integration), reading the source from a storage buffer. resample_main samples one matrix;
// resample_blur_main averages several per pixel as Resampler::motionBlurPixel does. Both carry sample
// positions in two floats, which the bilinear to notch filters are continuous in, except clamped
// Mitchell. Impulse and box pick a source pixel, and box a supersampling state, by floors of the CPU's
// double positions that two floats round to the other side of on exact scales, and clamped Mitchell's
// clamp range jumps where the floor of its taps changes; their entries, resample_exact_main and
// resample_blur_exact_main, decide every floor as those doubles do: with two floats where those lie provably clear of the
// decision, and with the doubles computed bit for bit otherwise. The integer double emulation is
// kept out of the two-float entries because it is many times slower to run and to compile. It has
// no division, so projective matrices, whose positions are quotients, always take the two-float
// entries. The filters, interpolation and supersampling are resamplecore.slang's. GpuResampleParams in
// Engine/Gpu/GpuResample.h mirrors ResampleParams.

import resamplecore;

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
// and their states from blurBuf instead, and so does resample_main under kFlagMatrixInBuffer, which
// a projective matrix needs: its third row does not fit the push constants.
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

// The source described by `rp` and srcBuf.
ResampleSource paramsSource()
{
    ResampleSource s;
    s.buf = srcBuf;
    s.offset = rp.srcOffset;
    s.stride = rp.srcStride;
    s.x1 = rp.srcX1;
    s.y1 = rp.srcY1;
    s.x2 = rp.srcX2;
    s.y2 = rp.srcY2;
    s.nComps = rp.nComps;
    s.valid = (rp.flags & kFlagSrcValid) != 0;
    s.blackOutside = (rp.flags & kFlagBlackOutside) != 0;
    return s;
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

// The matrix rows (h00 h01 h02) and (h10 h11 h12), with `states` as in ResampleParams. A projective
// matrix also has the row (h20 h21 h22), and its Jacobian and states are worked out per pixel.
struct Matrix
{
    FF h00;
    FF h01;
    FF h02;
    FF h10;
    FF h11;
    FF h12;
    uint states;
    bool projective;
    FF h20;
    FF h21;
    FF h22;
};

FF projectiveJacobian(FF hrc, FF P, FF h2c, FF Z, FF Z2)
{
    return ffDiv(ffAdd(ffMul(hrc, Z), ffNeg(ffMul(P, h2c))), Z2);
}

float4 samplePixel(Matrix m, int x, int y)
{
    const ResampleSource src = paramsSource();
    if (!src.valid)
        return float4(0.0, 0.0, 0.0, 0.0);
    const uint filter = rp.filter;
    const bool clampOn = (rp.flags & kFlagClamp) != 0;
    const float px = float(x) + 0.5;
    const float py = float(y) + 0.5;
    const FF X = ffAdd(ffAdd(ffMulF(m.h00, px), ffMulF(m.h01, py)), m.h02);
    const FF Y = ffAdd(ffAdd(ffMulF(m.h10, px), ffMulF(m.h11, py)), m.h12);
    const FF zero = ffMake(0.0, 0.0);
    FF Z = ffMake(1.0, 0.0);
    FF fx = X;
    FF fy = Y;
    if (m.projective)
    {
        Z = ffAdd(ffAdd(ffMulF(m.h20, px), ffMulF(m.h21, py)), m.h22);
        // Behind the camera, or at infinity.
        if (!ffLess(zero, Z))
            return float4(0.0, 0.0, 0.0, 0.0);
        fx = ffDiv(X, Z);
        fy = ffDiv(Y, Z);
    }

    float4 r;
    if (filter == kFilterImpulse)
    {
        interpolate(src, filter, clampOn, fx, fy, r);
        return r;
    }
    // x1 <= fx + 0.5 && fx - 0.5 < x2, and likewise for y.
    bool xinside = toPos(ffAddF(fx, 0.5)).i >= src.x1 && toPos(ffAddF(fx, -0.5)).i < src.x2;
    bool yinside = toPos(ffAddF(fy, 0.5)).i >= src.y1 && toPos(ffAddF(fy, -0.5)).i < src.y2;
    if (src.blackOutside && !(xinside && yinside))
    {
        xinside = false;
        yinside = false;
    }
    FF Jxx = xinside ? m.h00 : zero;
    FF Jxy = xinside ? m.h01 : zero;
    FF Jyx = yinside ? m.h10 : zero;
    FF Jyy = yinside ? m.h11 : zero;
    uint states = m.states;
    uint state = (xinside ? 1u : 0u) | (yinside ? 2u : 0u);
    if (m.projective)
    {
        const FF Z2 = ffMul(Z, Z);
        Jxx = xinside ? projectiveJacobian(m.h00, X, m.h20, Z, Z2) : zero;
        Jxy = xinside ? projectiveJacobian(m.h01, X, m.h21, Z, Z2) : zero;
        Jyx = yinside ? projectiveJacobian(m.h10, Y, m.h20, Z, Z2) : zero;
        Jyy = yinside ? projectiveJacobian(m.h11, Y, m.h21, Z, Z2) : zero;
        states = jacobianStates(Jxx, Jxy, Jyx, Jyy);
        state = 0;
    }
    return interpolateSuper(src, filter, clampOn, fx, fy, Jxx, Jxy, Jyx, Jyy, states, state);
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

// What the exact entries' filters other than impulse and box take from the CPU's double position:
// that position, the floors of its taps as Pos values of fx - 0.5 and fy - 0.5, and whether it
// overlaps the source in x and in y.
struct SmoothTaps
{
    FF fx;
    FF fy;
    Pos px;
    Pos py;
    bool xinside;
    bool yinside;
};

SmoothTaps noTaps()
{
    SmoothTaps t;
    t.fx = ffMake(0.0, 0.0);
    t.fy = t.fx;
    t.px.i = 0;
    t.px.f = 0.0;
    t.py = t.px;
    t.xinside = false;
    t.yinside = false;
    return t;
}

// Impulse and box at the CPU's double position: the pixel it falls in, or for box with a nonzero
// Jacobian, Resampler's integration over the back-transformed pixel. For the other filters, `taps`
// at that position.
float4 samplePixelExact(MatrixBits m, int x, int y, out SmoothTaps taps)
{
    taps = noTaps();
    float4 r = float4(0.0, 0.0, 0.0, 0.0);
    if (!paramsSource().valid)
        return r;
    // The CPU's h00 * (x + 0.5) + h01 * (y + 0.5) + h02, in that order and unfused.
    const int kx = 2 * x + 1;
    const int ky = 2 * y + 1;
    const Dbl dx = dblAdd(dblAdd(dblMulHalfInt(m.h00, kx), dblMulHalfInt(m.h01, ky)), m.h02);
    const Dbl dy = dblAdd(dblAdd(dblMulHalfInt(m.h10, kx), dblMulHalfInt(m.h11, ky)), m.h12);
    const int2 cell = int2(dblFloor(dx), dblFloor(dy));
    if (rp.filter == kFilterImpulse)
    {
        cellPixel(paramsSource(), cell, r);
        return r;
    }

    const Dbl half = dblMake(0, 0x3fe00000u);
    const Dbl bx = dblSub(dx, half);
    const Dbl by = dblSub(dy, half);
    // x1 <= x + 0.5 && x - 0.5 < x2, and likewise for y.
    bool xinside = dblFloor(dblAdd(dx, half)) >= rp.srcX1 && dblFloor(bx) < rp.srcX2;
    bool yinside = dblFloor(dblAdd(dy, half)) >= rp.srcY1 && dblFloor(by) < rp.srcY2;
    if (paramsSource().blackOutside && !(xinside && yinside))
    {
        xinside = false;
        yinside = false;
    }
    if (rp.filter != kFilterBox)
    {
        taps.fx = dblToFF(dx);
        taps.fy = dblToFF(dy);
        taps.px = dblToPos(bx);
        taps.py = dblToPos(by);
        taps.xinside = xinside;
        taps.yinside = yinside;
        return r;
    }
    const uint state = (xinside ? 1u : 0u) | (yinside ? 2u : 0u);
    if ((((m.states >> (8 * state)) & 0xffu) & 0x40u) != 0)
    {
        cellPixel(paramsSource(), cell, r);
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
        cellPixel(paramsSource(), cell, r);
        return r;
    }
    const Dbl ox = dblFromInt(rp.srcX1);
    const Dbl oy = dblFromInt(rp.srcY1);
    const Dbl rx1 = dblSub(x1, ox);
    const Dbl rx2 = dblSub(x2, ox);
    const Dbl ry1 = dblSub(y1, oy);
    const Dbl ry2 = dblSub(y2, oy);
    float4 v = integrate2d(paramsSource(), dblToPos(rx1), dblToPos(ry1), dblToPos(rx2), dblToPos(ry2), paramsSource().blackOutside);
    precise float s = dblToFloat(dblSub(rx2, rx1)) * dblToFloat(dblSub(ry2, ry1));
    if (s != 0.0)
        v /= s;
    return v;
}

// Positions, sources and bounds stay below this, so that the float pixel centres and source
// origins are exact and every floor fits an int.
static const float kFastLimit = 4194304.0;

// The bound on how far a row's two-float position, and the box corners and source-relative
// offsets made from it, can lie from the CPU's doubles. With u = 2^-24 and M the sum of the
// magnitudes involved: dblToFF loses u^2 |h|, each ffMulF, ffAdd and ffAddF adds at most
// 3u^2 of its operands, and each double rounding is below u^2 / 32 of M: under 21u^2 M in all.
// Twice that covers toPos rounding a fraction across 1 or 0; 2^-42 M = 64u^2 M leaves room for
// the float rounding of M itself.
float decisionBound(FF h0, FF h1, FF h2, float px, float py, int origin)
{
    const float m = abs(h0.hi) * abs(px) + abs(h1.hi) * abs(py) + abs(h2.hi) + abs(h0.hi) + abs(h1.hi) + abs(float(origin)) + 1.0;
    return m * pow2f(-42);
}

// Whether a value within e of p, which toPos gave, may have a floor other than p.i.
bool nearInteger(Pos p, float e)
{
    return e > 0.0 && (p.f <= e || p.f >= 1.0 - e);
}

// Whether floor(v) >= n may differ from p.i >= n for a value v within e of p.
bool floorGeAmbiguous(Pos p, float e, int n)
{
    return e > 0.0 && ((p.f <= e && p.i == n) || (p.f >= 1.0 - e && p.i + 1 == n));
}

// samplePixelExact from two-float positions, false where one of its floors may differ from the
// doubles'. Away from the source edges a box corner's floor only moves weight between neighbouring
// pixels by its distance to the integer, so only the floors that switch a pixel in or out of the
// integration count. `exactRows` holds kFlagExactRowX and kFlagExactRowY shifted to bits 0 and 1.
bool samplePixelFast(MatrixBits mb, uint exactRows, int x, int y, out float4 r, out SmoothTaps taps)
{
    taps = noTaps();
    r = float4(0.0, 0.0, 0.0, 0.0);
    if (abs(x) >= int(kFastLimit) || abs(y) >= int(kFastLimit))
        return false;
    const FF h00 = dblToFF(mb.h00);
    const FF h01 = dblToFF(mb.h01);
    const FF h02 = dblToFF(mb.h02);
    const FF h10 = dblToFF(mb.h10);
    const FF h11 = dblToFF(mb.h11);
    const FF h12 = dblToFF(mb.h12);
    const float px = float(x) + 0.5;
    const float py = float(y) + 0.5;
    const float ex = decisionBound(h00, h01, h02, px, py, rp.srcX1);
    const float ey = decisionBound(h10, h11, h12, px, py, rp.srcY1);
    if (!(ex < kFastLimit * pow2f(-42)) || !(ey < kFastLimit * pow2f(-42)))
        return false;
    const float exPos = (exactRows & 1u) != 0 ? 0.0 : ex;
    const float eyPos = (exactRows & 2u) != 0 ? 0.0 : ey;
    const FF fx = ffAdd(ffAdd(ffMulF(h00, px), ffMulF(h01, py)), h02);
    const FF fy = ffAdd(ffAdd(ffMulF(h10, px), ffMulF(h11, py)), h12);
    const Pos cx = toPos(fx);
    const Pos cy = toPos(fy);
    const int2 cell = int2(cx.i, cy.i);
    const bool cellExact = !nearInteger(cx, exPos) && !nearInteger(cy, eyPos);
    if (rp.filter == kFilterImpulse)
    {
        if (cellExact)
            cellPixel(paramsSource(), cell, r);
        return cellExact;
    }

    const Pos ax = toPos(ffAddF(fx, 0.5));
    const Pos bx = toPos(ffAddF(fx, -0.5));
    const Pos ay = toPos(ffAddF(fy, 0.5));
    const Pos by = toPos(ffAddF(fy, -0.5));
    if (floorGeAmbiguous(ax, exPos, rp.srcX1) || floorGeAmbiguous(bx, exPos, rp.srcX2) || floorGeAmbiguous(ay, eyPos, rp.srcY1)
        || floorGeAmbiguous(by, eyPos, rp.srcY2))
        return false;
    bool xinside = ax.i >= rp.srcX1 && bx.i < rp.srcX2;
    bool yinside = ay.i >= rp.srcY1 && by.i < rp.srcY2;
    if (paramsSource().blackOutside && !(xinside && yinside))
    {
        xinside = false;
        yinside = false;
    }
    if (rp.filter != kFilterBox)
    {
        if (nearInteger(bx, exPos) || nearInteger(by, eyPos))
            return false;
        taps.fx = fx;
        taps.fy = fy;
        taps.px = bx;
        taps.py = by;
        taps.xinside = xinside;
        taps.yinside = yinside;
        return true;
    }
    const uint state = (xinside ? 1u : 0u) | (yinside ? 2u : 0u);
    if ((((mb.states >> (8 * state)) & 0xffu) & 0x40u) != 0)
    {
        if (cellExact)
            cellPixel(paramsSource(), cell, r);
        return cellExact;
    }

    // Corners less than twice the bound apart may coincide in the doubles and not here, or the
    // reverse; a Jacobian row that is exactly zero collapses them in both.
    const FF zero = ffMake(0.0, 0.0);
    const FF jxx = xinside ? h00 : zero;
    const FF jxy = xinside ? h01 : zero;
    const FF jyx = yinside ? h10 : zero;
    const FF jyy = yinside ? h11 : zero;
    const bool zeroX = !xinside || (dblIsZero(mb.h00) && dblIsZero(mb.h01));
    const bool zeroY = !yinside || (dblIsZero(mb.h10) && dblIsZero(mb.h11));
    if ((!zeroX && max(abs(jxx.hi), abs(jxy.hi)) * 0.5 <= 2.0 * ex) || (!zeroY && max(abs(jyx.hi), abs(jyy.hi)) * 0.5 <= 2.0 * ey))
        return false;
    FF x1 = fx;
    FF x2 = fx;
    FF y1 = fy;
    FF y2 = fy;
    [loop]
    for (int k = 0; k < 4; ++k)
    {
        const float sa = (k & 1) != 0 ? 1.0 : -1.0;
        const float sb = (k & 2) != 0 ? 1.0 : -1.0;
        const FF qx = corner(fx, jxx, jxy, sa, sb);
        const FF qy = corner(fy, jyx, jyy, sa, sb);
        if (k == 0 || ffLess(qx, x1))
            x1 = qx;
        if (k == 0 || ffLess(x2, qx))
            x2 = qx;
        if (k == 0 || ffLess(qy, y1))
            y1 = qy;
        if (k == 0 || ffLess(y2, qy))
            y2 = qy;
    }
    if (!ffLess(x1, x2) || !ffLess(y1, y2))
    {
        if (cellExact)
            cellPixel(paramsSource(), cell, r);
        return cellExact;
    }
    const FF rx1 = ffAddF(x1, -float(rp.srcX1));
    const FF rx2 = ffAddF(x2, -float(rp.srcX1));
    const FF ry1 = ffAddF(y1, -float(rp.srcY1));
    const FF ry2 = ffAddF(y2, -float(rp.srcY1));
    const Pos px1 = toPos(rx1);
    const Pos px2 = toPos(rx2);
    const Pos py1 = toPos(ry1);
    const Pos py2 = toPos(ry2);
    // integrate1d keeps the edge pixel's full weight once a footprint starts at or past the far
    // edge or ends before the near one, so those two floors switch it; elsewhere they are continuous.
    const int awidth = rp.srcX2 - rp.srcX1;
    const int aheight = rp.srcY2 - rp.srcY1;
    if (floorGeAmbiguous(px1, ex, awidth) || floorGeAmbiguous(px2, ex, 0) || floorGeAmbiguous(py1, ey, aheight)
        || floorGeAmbiguous(py2, ey, 0))
        return false;
    float4 v = integrate2d(paramsSource(), px1, py1, px2, py2, paramsSource().blackOutside);
    const FF wx = ffAdd(rx2, ffNeg(rx1));
    const FF wy = ffAdd(ry2, ffNeg(ry1));
    precise float s = (wx.hi + wx.lo) * (wy.hi + wy.lo);
    if (s != 0.0)
        v /= s;
    r = v;
    return true;
}

// samplePixelFast where it decides, else samplePixelExact: one call site of each, and of the
// filter on the taps they decide, keeps a single copy of the emulated doubles and of the filter in
// the compiled kernel.
float4 samplePixelDecided(MatrixBits m, uint exactRows, int x, int y)
{
    if (!paramsSource().valid)
        return float4(0.0, 0.0, 0.0, 0.0);
    float4 r;
    SmoothTaps t;
    if (!samplePixelFast(m, exactRows, x, y, r, t))
        r = samplePixelExact(m, x, y, t);
    if (rp.filter == kFilterImpulse || rp.filter == kFilterBox)
        return r;
    const FF zero = ffMake(0.0, 0.0);
    const uint state = (t.xinside ? 1u : 0u) | (t.yinside ? 2u : 0u);
    return interpolateSuperAt(paramsSource(), rp.filter, (rp.flags & kFlagClamp) != 0, t.fx, t.fy, t.px, t.py, t.xinside ? dblToFF(m.h00) : zero,
                              t.xinside ? dblToFF(m.h01) : zero, t.yinside ? dblToFF(m.h10) : zero, t.yinside ? dblToFF(m.h11) : zero,
                              m.states, state);
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
    if ((rp.flags & kFlagMatrixInBuffer) != 0)
    {
        m = blurMatrix(0);
    }
    else
    {
        m.h00 = ffMake(asfloat(rp.m0), asfloat(rp.m6));
        m.h01 = ffMake(asfloat(rp.m1), asfloat(rp.m7));
        m.h02 = ffMake(asfloat(rp.m2), asfloat(rp.m8));
        m.h10 = ffMake(asfloat(rp.m3), asfloat(rp.m9));
        m.h11 = ffMake(asfloat(rp.m4), asfloat(rp.m10));
        m.h12 = ffMake(asfloat(rp.m5), asfloat(rp.m11));
        m.states = rp.states;
        m.projective = false;
        m.h20 = ffMake(0.0, 0.0);
        m.h21 = ffMake(0.0, 0.0);
        m.h22 = ffMake(1.0, 0.0);
    }
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
    const uint exactRows = (rp.flags / kFlagExactRowX) & 3u;
    storePixel(tid, samplePixelDecided(m, exactRows, rp.outX + int(tid.x), rp.outY + int(tid.y)));
}

///////////////////////////////////////////////////////////////////////////////
// Resampler's motionBlurPixelT. The CPU accumulates in doubles and stops on the variance, so the
// sums here are kept in two floats: a sample more or less moves the mean by far more than the
// tolerance.

// The header: matrix count, minimum samples, maximum samples, the seed's offset
// (unsigned(0x10000 * motionBlur)), the squared maximum error as hi and lo float bits, and 1 when the
// matrices are weighted. A matrix: twelve words holding h00 h01 h02 h10 h11 h12 as the push
// constants' m0-m11 do for the entry that reads them, its states, its weight as hi and lo float
// bits, for the exact entry its exact rows as samplePixelFast takes them, and for the two-float
// entries 1 when it is projective, then hi of h20 h21 h22 and their lo.
static const uint kBlurHeaderWords = 8;
static const uint kBlurMatrixWords = 23;

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
    m.projective = blurBuf[base + 16] != 0;
    m.h20 = ffMake(asfloat(blurBuf[base + 17]), asfloat(blurBuf[base + 20]));
    m.h21 = ffMake(asfloat(blurBuf[base + 18]), asfloat(blurBuf[base + 21]));
    m.h22 = ffMake(asfloat(blurBuf[base + 19]), asfloat(blurBuf[base + 22]));
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

// The samples the variance asks for, as Resampler's samplesForVariance: a quotient that is negative,
// not finite, or at or past 2^31 asks for none more.
int samplesForVariance(FF var, FF maxErr2)
{
    const FF q = ffDiv(var, maxErr2);
    if (!isFiniteBits(q.hi) || !(q.hi >= 0.0) || !(q.hi < 2147483648.0))
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
        return samplePixelDecided(blurMatrixBits(t), blurBuf[kBlurHeaderWords + t * kBlurMatrixWords + 15], x, y);
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
    if (paramsSource().valid)
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
    if (paramsSource().valid)
    {
        ExactSampler sampler;
        v = motionBlurPixel(sampler, rp.outX + int(tid.x), rp.outY + int(tid.y));
    }
    storePixel(tid, v);
}
