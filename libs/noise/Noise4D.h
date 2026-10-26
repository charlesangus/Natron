// 4D noise generators: Perlin, SuperSimplex, value, white and cellular, ported to plain C++ from
// Auburn/FastNoise2 721400f9560831be2fc9790be9b72c44bee7d33f
// (include/FastNoise/Generators/{Perlin,Simplex,Value,Cellular,Utils}.inl), with the cellular
// distance-to-edge and smooth F1 outputs and the split coordinate added here. MIT, see LICENSE and
// NOTICE in this directory.
//
// Engine/Gpu/Kernels/noisecore.slang mirrors this file line for line, so the code keeps to a
// C-like subset: scalar fields, small POD structs returned by value, no references, no virtual
// calls, no library calls but sqrt. One FastSIMD lane becomes one scalar: float32v -> float,
// int32v -> uint (wrapping arithmetic; signed only where FastNoise2 relies on an arithmetic shift
// or a signed compare), mask32v -> bool, FS::Select -> a ternary. Each gradient follows FastSIMD's
// generic (SSE2/AVX2) path.
//
// There is no fused multiply-add anywhere: FS::FMulAdd(a, b, c) is written a * b + c. On the
// x86-64 baseline std::fma is a libm call, and the unfused form is what the GPU kernel computes
// when it is compiled with -fp-mode precise, so the CPU and the GPU agree to the last bit wherever
// Vulkan requires correct rounding (everything but sqrt and division). Where a build contracts
// a * b + c anyway, only the ulps of the smooth outputs move; every operation whose exactness
// matters (the split-coordinate products and the floors) has an exact product, which contraction
// cannot change.
//
// Coordinates are split into an integer cell and a fraction in [0, 1) per axis (NoiseCoord), so
// precision does not degrade far from the origin: the lattice cell of a sample is exact at any
// distance, cell indices wrap modulo 2^32, and the fraction keeps float precision. Scaling a split
// coordinate (axisScale) is exact up to the rounding of the fraction.

#pragma once

#include <bit>
#include <cmath>
#include <cstdint>

namespace noise4d {

using uint = uint32_t;

constexpr float
asFloatBits(uint bits)
{
    return std::bit_cast<float>(bits);
}
constexpr uint
asUintBits(float v)
{
    return std::bit_cast<uint>(v);
}

constexpr uint kPrimeX = 0xF797C5C7u;
constexpr uint kPrimeY = 0x6C060C89u;
constexpr uint kPrimeZ = 0x465FD04Fu;
constexpr uint kPrimeW = 0xF7A62279u;
constexpr uint kHashMultiplierA = 0xB7E0A5F5u;
constexpr uint kSignBit = 0x80000000u;
constexpr float kInfinity = asFloatBits(0x7F800000u);

constexpr float kRangeMin = -1.0f;
constexpr float kTwoPow31 = 2147483648.0f;
constexpr float kTwoPowMinus31 = asFloatBits(0x30000000u); // 2^-31
constexpr float kTwoPow32 = 4294967296.0f;
constexpr float kTwoPowMinus32 = asFloatBits(0x2F800000u); // 2^-32
static_assert(kTwoPowMinus31 == 1.0f / 2147483648.0f);
static_assert(kTwoPowMinus32 == 1.0f / 4294967296.0f);

// The cellular metrics.
constexpr int kNoiseMetricEuclidean = 0;
constexpr int kNoiseMetricManhattan = 1;
constexpr int kNoiseMetricChebyshev = 2;

// The cellular outputs.
constexpr int kNoiseCellularF1 = 0;
constexpr int kNoiseCellularF2 = 1;
constexpr int kNoiseCellularF2MinusF1 = 2;
constexpr int kNoiseCellularCellValue = 3;
constexpr int kNoiseCellularDistanceToEdge = 4;
constexpr int kNoiseCellularSmoothF1 = 5;

// A point in noise space: per axis an integer cell (two's complement, wrapping) and a fraction in
// [0, 1).
struct NoiseCoord {
    uint cx;
    uint cy;
    uint cz;
    uint cw;
    float fx;
    float fy;
    float fz;
    float fw;
};

// One axis of a NoiseCoord.
struct NoiseAxis {
    uint c;
    float f;
};

struct NoiseVec4 {
    float x;
    float y;
    float z;
    float w;
};

inline float
selectF(bool m, float a, float b)
{
    return m ? a : b;
}
inline uint
selectU(bool m, uint a, uint b)
{
    return m ? a : b;
}

// SSE semantics: max_ps(a, b) is a > b ? a : b, min_ps(a, b) is a < b ? a : b.
inline float
vmax(float a, float b)
{
    return a > b ? a : b;
}
inline float
vmin(float a, float b)
{
    return a < b ? a : b;
}
inline float
vabs(float a)
{
    return asFloatBits(asUintBits(a) & 0x7FFFFFFFu);
}
inline float
vsqrt(float a)
{
    return std::sqrt(a);
}

inline float
muladd(float a, float b, float c)
{
    return a * b + c;
}
inline float
nmuladd(float a, float b, float c)
{
    return c - a * b;
}

// float32v ^ float32v where the right side is a sign mask.
inline float
xorBits(float a, uint bits)
{
    return asFloatBits(asUintBits(a) ^ bits);
}

inline uint
sra(uint a, int n)
{
    return (uint)((int)a >> n);
}

inline float
masked(bool m, float a)
{
    return m ? a : 0.0f;
}
inline float
maskedDecrement(bool m, float a)
{
    return a + masked(m, -1.0f);
}

// Round half to even, exact for every float. Adding and removing 2^23 leaves no fraction bits;
// magnitudes from 2^23 up are already integers and get a zero constant instead.
inline float
roundEven(float a)
{
    uint magnitude = vabs(a) < 8388608.0f ? 0x4B000000u : 0u;
    float c = asFloatBits((asUintBits(a) & kSignBit) | magnitude);
    return (a + c) - c;
}

// Exact floor without a library call: round, then decrement where it rounded up.
inline float
floorV(float a)
{
    float r = roundEven(a);
    return maskedDecrement(r > a, r);
}

// An integral float in the int range, as its two's complement bits.
inline uint
toUint(float a)
{
    return (uint)(int)a;
}
inline float
toFloat(uint a)
{
    return (float)(int)a;
}

inline float
invSqrt(float a)
{
    return 1.0f / vsqrt(a);
}

// An integral float of any magnitude, modulo 2^32. Every step is exact: the magnitude less a
// multiple of 2^32 has no more significant bits than the magnitude.
inline uint
wrapIntegral(float v)
{
    bool negative = v < 0.0f;
    float a = vabs(v);
    float turns = floorV(a * kTwoPowMinus32);
    float r = a - turns * kTwoPow32;
    bool high = r >= kTwoPow31;
    r = selectF(high, r - kTwoPow31, r);
    uint u = toUint(r) + selectU(high, kSignBit, 0u);
    return selectU(negative, 0u - u, u);
}

// Brings the fraction back into [0, 1), moving whole cells into the cell. |f| < 2^31.
inline NoiseAxis
axisNormalize(uint c, float f)
{
    float k = floorV(f);
    NoiseAxis r;
    r.c = c + toUint(k);
    r.f = f - k;
    // f - k rounds up to 1 when f is a tiny negative number.
    bool wrap = r.f >= 1.0f;
    r.c = r.c + selectU(wrap, 1u, 0u);
    r.f = selectF(wrap, r.f - 1.0f, r.f);
    return r;
}

// A float of any magnitude as a cell and a fraction.
inline NoiseAxis
axisFromFloat(float t)
{
    float k = floorV(t);
    NoiseAxis r;
    r.c = wrapIntegral(k);
    r.f = t - k;
    return r;
}

// (c + f) * m for the signed cell c, as a cell and a fraction. m splits into two halves of 12
// significant bits and c into two 12-bit pieces and a signed top byte, so each of the six
// products has at most 24 significant bits and is exact; only f * m and the sum of the fractions
// round. A cell in [-4096, 4096) needs only the two products of c itself.
inline NoiseAxis
axisScale(uint c, float f, float m)
{
    float mHi = asFloatBits(asUintBits(m) & 0xFFFFF000u);
    float mLo = m - mHi;
    NoiseAxis tf = axisFromFloat(f * m);
    if (c + 4096u < 8192u) {
        float cs = toFloat(c);
        NoiseAxis s0 = axisFromFloat(cs * mHi);
        NoiseAxis s1 = axisFromFloat(cs * mLo);
        return axisNormalize(s0.c + s1.c + tf.c, (s0.f + s1.f) + tf.f);
    }
    float c0 = toFloat(c & 0xFFFu);
    float c1 = toFloat((c >> 12) & 0xFFFu);
    float c2 = toFloat(sra(c, 24));
    NoiseAxis t0 = axisFromFloat(c0 * mHi);
    NoiseAxis t1 = axisFromFloat(c0 * mLo);
    NoiseAxis t2 = axisFromFloat((c1 * mHi) * 4096.0f);
    NoiseAxis t3 = axisFromFloat((c1 * mLo) * 4096.0f);
    NoiseAxis t4 = axisFromFloat((c2 * mHi) * 16777216.0f);
    NoiseAxis t5 = axisFromFloat((c2 * mLo) * 16777216.0f);
    uint cell = t0.c + t1.c + t2.c + t3.c + t4.c + t5.c + tf.c;
    float fraction = (((((t0.f + t1.f) + t2.f) + t3.f) + t4.f) + t5.f) + tf.f;
    return axisNormalize(cell, fraction);
}

// a + b.
inline NoiseAxis
axisAdd(NoiseAxis a, NoiseAxis b)
{
    return axisNormalize(a.c + b.c, a.f + b.f);
}

inline NoiseCoord
coordFromAxes(NoiseAxis x, NoiseAxis y, NoiseAxis z, NoiseAxis w)
{
    NoiseCoord p;
    p.cx = x.c;
    p.cy = y.c;
    p.cz = z.c;
    p.cw = w.c;
    p.fx = x.f;
    p.fy = y.f;
    p.fz = z.f;
    p.fw = w.f;
    return p;
}

// A point given as plain floats. Their precision is the float's; use the split forms for points
// far from the origin.
inline NoiseCoord
coordFromFloats(float x, float y, float z, float w)
{
    NoiseAxis ax = axisFromFloat(x);
    NoiseAxis ay = axisFromFloat(y);
    NoiseAxis az = axisFromFloat(z);
    NoiseAxis aw = axisFromFloat(w);
    return coordFromAxes(axisNormalize(ax.c, ax.f), axisNormalize(ay.c, ay.f), axisNormalize(az.c, az.f), axisNormalize(aw.c, aw.f));
}

// Every axis of p times m.
inline NoiseCoord
coordScale(NoiseCoord p, float m)
{
    return coordFromAxes(axisScale(p.cx, p.fx, m), axisScale(p.cy, p.fy, m), axisScale(p.cz, p.fz, m), axisScale(p.cw, p.fw, m));
}

// p moved by (x, y, z, w), each of magnitude below 2^31.
inline NoiseCoord
coordOffset(NoiseCoord p, float x, float y, float z, float w)
{
    return coordFromAxes(axisNormalize(p.cx, p.fx + x), axisNormalize(p.cy, p.fy + y), axisNormalize(p.cz, p.fz + z), axisNormalize(p.cw, p.fw + w));
}

inline uint
hashPrimes(uint seed, uint x, uint y, uint z, uint w)
{
    uint hash = seed;
    hash ^= x ^ (y ^ (z ^ w));

    hash *= kHashMultiplierA;

    return sra(hash, 15) ^ hash;
}

inline uint
hashPrimesHB(uint seed, uint x, uint y, uint z, uint w)
{
    uint hash = seed;
    hash ^= x ^ (y ^ (z ^ w));

    hash *= kHashMultiplierA;
    return hash;
}

inline float
getValueCoord(uint seed, uint x, uint y, uint z, uint w)
{
    uint hash = seed;
    hash ^= x ^ (y ^ (z ^ w));

    hash *= hash * kHashMultiplierA;
    return toFloat(hash);
}

inline float
lerpV(float a, float b, float t)
{
    return muladd(t, b - a, a);
}

inline float
interpHermite(float t)
{
    return t * t * nmuladd(t, 2.0f, 3.0f);
}

inline float
interpQuintic(float t)
{
    return t * t * t * muladd(t, muladd(t, 6.0f, -15.0f), 10.0f);
}

inline float
getGradientDotPerlin(uint hash, float fX, float fY, float fZ, float fW)
{
    uint p = hash & (3u << 3);

    float a = selectF((int)p > 0, fX, fY);
    float b = selectF((int)(hash << 27) < 0, fY, fZ);
    float c = selectF((int)p > (2 << 3), fZ, fW);

    uint aSign = hash << 31;
    uint bSign = (hash >> 1) << 31;
    uint cSign = (hash >> 2) << 31;

    return xorBits(a, aSign) + xorBits(b, bSign) + xorBits(c, cSign);
}

// VariableRange::ScaleOutput with the default range [-1, 1]; invRangeTimesScale is
// float(1.0f / (nativeMax - nativeMin)) * 2.
inline float
scaleOutput(float value, float nativeMin, float invRangeTimesScale)
{
    return muladd(invRangeTimesScale, value - nativeMin, kRangeMin);
}

constexpr float kPerlinBounding = asFloatBits(0x3fab56cbu); // 1.33858621
constexpr float kPerlinInvRange = asFloatBits(0x3f3f3f1eu); // 0.747056842
static_assert(kPerlinBounding == 1.33858621120452880859375f);
static_assert(kPerlinInvRange == float(1.0f / (kPerlinBounding + kPerlinBounding)) * 2.0f);

// Perlin noise in [-1, 1].
inline float
perlin4(uint seed, NoiseCoord p)
{
    uint x0 = p.cx * kPrimeX;
    uint y0 = p.cy * kPrimeY;
    uint z0 = p.cz * kPrimeZ;
    uint w0 = p.cw * kPrimeW;
    uint x1 = x0 + kPrimeX;
    uint y1 = y0 + kPrimeY;
    uint z1 = z0 + kPrimeZ;
    uint w1 = w0 + kPrimeW;

    float xf0 = p.fx;
    float yf0 = p.fy;
    float zf0 = p.fz;
    float wf0 = p.fw;
    float xf1 = xf0 - 1.0f;
    float yf1 = yf0 - 1.0f;
    float zf1 = zf0 - 1.0f;
    float wf1 = wf0 - 1.0f;

    float xs = interpQuintic(xf0);
    float ys = interpQuintic(yf0);
    float zs = interpQuintic(zf0);
    float ws = interpQuintic(wf0);

    float value = lerpV(lerpV(lerpV(lerpV(getGradientDotPerlin(hashPrimes(seed, x0, y0, z0, w0), xf0, yf0, zf0, wf0), getGradientDotPerlin(hashPrimes(seed, x1, y0, z0, w0), xf1, yf0, zf0, wf0), xs),
                                    lerpV(getGradientDotPerlin(hashPrimes(seed, x0, y1, z0, w0), xf0, yf1, zf0, wf0), getGradientDotPerlin(hashPrimes(seed, x1, y1, z0, w0), xf1, yf1, zf0, wf0), xs), ys),
                              lerpV(lerpV(getGradientDotPerlin(hashPrimes(seed, x0, y0, z1, w0), xf0, yf0, zf1, wf0), getGradientDotPerlin(hashPrimes(seed, x1, y0, z1, w0), xf1, yf0, zf1, wf0), xs),
                                    lerpV(getGradientDotPerlin(hashPrimes(seed, x0, y1, z1, w0), xf0, yf1, zf1, wf0), getGradientDotPerlin(hashPrimes(seed, x1, y1, z1, w0), xf1, yf1, zf1, wf0), xs), ys),
                              zs),
                        lerpV(lerpV(lerpV(getGradientDotPerlin(hashPrimes(seed, x0, y0, z0, w1), xf0, yf0, zf0, wf1), getGradientDotPerlin(hashPrimes(seed, x1, y0, z0, w1), xf1, yf0, zf0, wf1), xs),
                                    lerpV(getGradientDotPerlin(hashPrimes(seed, x0, y1, z0, w1), xf0, yf1, zf0, wf1), getGradientDotPerlin(hashPrimes(seed, x1, y1, z0, w1), xf1, yf1, zf0, wf1), xs), ys),
                              lerpV(lerpV(getGradientDotPerlin(hashPrimes(seed, x0, y0, z1, w1), xf0, yf0, zf1, wf1), getGradientDotPerlin(hashPrimes(seed, x1, y0, z1, w1), xf1, yf0, zf1, wf1), xs),
                                    lerpV(getGradientDotPerlin(hashPrimes(seed, x0, y1, z1, w1), xf0, yf1, zf1, wf1), getGradientDotPerlin(hashPrimes(seed, x1, y1, z1, w1), xf1, yf1, zf1, wf1), xs), ys),
                              zs),
                        ws);

    return scaleOutput(value, -kPerlinBounding, kPerlinInvRange);
}

constexpr float kValueBounds = 2147483648.0f;
constexpr float kValueInvRange = kTwoPowMinus31;

// Value noise in [-1, 1].
inline float
value4(uint seed, NoiseCoord p)
{
    uint x0 = p.cx * kPrimeX;
    uint y0 = p.cy * kPrimeY;
    uint z0 = p.cz * kPrimeZ;
    uint w0 = p.cw * kPrimeW;
    uint x1 = x0 + kPrimeX;
    uint y1 = y0 + kPrimeY;
    uint z1 = z0 + kPrimeZ;
    uint w1 = w0 + kPrimeW;

    float xs = interpHermite(p.fx);
    float ys = interpHermite(p.fy);
    float zs = interpHermite(p.fz);
    float ws = interpHermite(p.fw);

    float value = lerpV(lerpV(lerpV(lerpV(getValueCoord(seed, x0, y0, z0, w0), getValueCoord(seed, x1, y0, z0, w0), xs),
                                    lerpV(getValueCoord(seed, x0, y1, z0, w0), getValueCoord(seed, x1, y1, z0, w0), xs), ys),
                              lerpV(lerpV(getValueCoord(seed, x0, y0, z1, w0), getValueCoord(seed, x1, y0, z1, w0), xs),
                                    lerpV(getValueCoord(seed, x0, y1, z1, w0), getValueCoord(seed, x1, y1, z1, w0), xs), ys),
                              zs),
                        lerpV(lerpV(lerpV(getValueCoord(seed, x0, y0, z0, w1), getValueCoord(seed, x1, y0, z0, w1), xs),
                                    lerpV(getValueCoord(seed, x0, y1, z0, w1), getValueCoord(seed, x1, y1, z0, w1), xs), ys),
                              lerpV(lerpV(getValueCoord(seed, x0, y0, z1, w1), getValueCoord(seed, x1, y0, z1, w1), xs),
                                    lerpV(getValueCoord(seed, x0, y1, z1, w1), getValueCoord(seed, x1, y1, z1, w1), xs), ys),
                              zs),
                        ws);

    return scaleOutput(value, -kValueBounds, kValueInvRange);
}

// White noise in [-1, 1): one hash of the cell, so every point of a cell has the same value. The
// integer hash and the power-of-two scale are exact, so the GPU matches bit for bit.
inline float
white4(uint seed, NoiseCoord p)
{
    return getValueCoord(seed, p.cx * kPrimeX, p.cy * kPrimeY, p.cz * kPrimeZ, p.cw * kPrimeW) * kTwoPowMinus31;
}

constexpr float kSkew4 = asFloatBits(0x3e9e377au); // 1 / (sqrt(5) + 1) = 0.309017003
constexpr float kTwiceUnskew4 = asFloatBits(0xbe4ccccdu); // -1 / 5 = -0.200000003
constexpr float kUnskew4 = asFloatBits(0xbe0d8369u); // -1 / (sqrt(5) + 5) = -0.138196602
constexpr float kFalloffRadiusSquared = asFloatBits(0x3f4ccccdu); // 4 / 5 = 0.800000012
constexpr float kDistanceSquaredAHalf = asFloatBits(0x3ecccccdu); // 4 / 5 / 2 = 0.400000006
constexpr float kDistanceSquaredBHalf = asFloatBits(0x3f19999au); // 6 / 5 / 2 = 0.600000024
constexpr float kUnskew4x4p1 = asFloatBits(0x3ee4f92eu); // 4u + 1 = 0.44721359
constexpr float kUnskew4x3p1 = asFloatBits(0x3f15dd71u); // 3u + 1 = 0.585410178
constexpr float kUnskew4x3 = asFloatBits(0xbed4451du); // 3u = -0.414589792
constexpr float kUnskew4x2p1 = asFloatBits(0x3f393e4cu); // 2u + 1 = 0.723606825
constexpr float kUnskew4x2 = asFloatBits(0xbe8d8369u); // 2u = -0.276393205
constexpr float kUnskew4p1 = asFloatBits(0x3f5c9f26u); // u + 1 = 0.861803412
constexpr float kSimplexBounding = asFloatBits(0x3c394da3u); // 0.0113100139
constexpr float kSimplexInvRange = asFloatBits(0x42b0d59eu); // 88.4172211

namespace detail {
    constexpr double kRoot5 = 2.2360679774997896964091736687313;
    constexpr double kSkew4d = 1.0 / (kRoot5 + 1.0);
    constexpr double kUnskew4d = -1.0 / (kRoot5 + 5.0);
    static_assert(float(kSkew4d) == kSkew4);
    static_assert(float(-1.0 / 5.0) == kTwiceUnskew4);
    static_assert(float(kUnskew4d) == kUnskew4);
    static_assert(float(4.0 / 5.0) == kFalloffRadiusSquared);
    static_assert(float(4.0 / 5.0 * 0.5) == kDistanceSquaredAHalf);
    static_assert(float(6.0 / 5.0 * 0.5) == kDistanceSquaredBHalf);
    static_assert(float(4 * kUnskew4d + 1) == kUnskew4x4p1);
    static_assert(float(3 * kUnskew4d + 1) == kUnskew4x3p1);
    static_assert(float(3 * kUnskew4d) == kUnskew4x3);
    static_assert(float(2 * kUnskew4d + 1) == kUnskew4x2p1);
    static_assert(float(2 * kUnskew4d) == kUnskew4x2);
    static_assert(float(kUnskew4d + 1) == kUnskew4p1);
    static_assert(kSimplexBounding == 0.011310013942420482635498046875f);
    static_assert(kSimplexInvRange == float(1.0f / (kSimplexBounding + kSimplexBounding)) * 2.0f);
} // namespace detail

struct SimplexScore {
    float maxScore;
    uint moveMaskBits;
};

inline SimplexScore
considerVertex(SimplexScore s, float score, uint bits)
{
    s.moveMaskBits = selectU(score > s.maxScore, bits, s.moveMaskBits);
    s.maxScore = vmax(s.maxScore, score);
    return s;
}

// One SuperSimplex vertex's contribution added to value.
inline float
simplexVertex(float value, uint seed, uint xPrimed, uint yPrimed, uint zPrimed, uint wPrimed, float dx, float dy, float dz, float dw, float falloffBase)
{
    float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimed, yPrimed, zPrimed, wPrimed), dx, dy, dz, dw);
    falloffBase = vmax(falloffBase, 0.0f);
    return muladd((falloffBase * falloffBase) * (falloffBase * falloffBase), gradientRampValue, value);
}

// SuperSimplex (OpenSimplex2S) noise in [-1, 1]. The skew is computed on the split coordinate, so
// the skewed cell is exact far from the origin.
inline float
superSimplex4(uint seed, NoiseCoord p)
{
    NoiseAxis skew = axisScale(p.cx + p.cy + p.cz + p.cw, ((p.fx + p.fy) + p.fz) + p.fw, kSkew4);
    NoiseAxis xk = axisNormalize(p.cx + skew.c, p.fx + skew.f);
    NoiseAxis yk = axisNormalize(p.cy + skew.c, p.fy + skew.f);
    NoiseAxis zk = axisNormalize(p.cz + skew.c, p.fz + skew.f);
    NoiseAxis wk = axisNormalize(p.cw + skew.c, p.fw + skew.f);
    uint xSkewedBase = xk.c;
    uint ySkewedBase = yk.c;
    uint zSkewedBase = zk.c;
    uint wSkewedBase = wk.c;
    float dxSkewed = xk.f;
    float dySkewed = yk.f;
    float dzSkewed = zk.f;
    float dwSkewed = wk.f;

    // From the unit cell base, find the closest vertex.
    {
        // A double unskew gives the vector whose dot product with skewed vectors is the unskewed result.
        float twiceUnskewDelta = kTwiceUnskew4 * (((dxSkewed + dySkewed) + dzSkewed) + dwSkewed);
        float xNormal = dxSkewed + twiceUnskewDelta;
        float yNormal = dySkewed + twiceUnskewDelta;
        float zNormal = dzSkewed + twiceUnskewDelta;
        float wNormal = dwSkewed + twiceUnskewDelta;
        float xyzwNormal = -twiceUnskewDelta; // xNormal + yNormal + zNormal + wNormal

        // Compare scores to find the closest vertex.
        SimplexScore s;
        s.maxScore = 0.6f - xyzwNormal;
        s.moveMaskBits = selectU(0.2f > s.maxScore, 0xFFFFFFFFu, 0u);
        s.maxScore = vmax(s.maxScore, 0.2f);
        s = considerVertex(s, -wNormal, 7u);
        s = considerVertex(s, -zNormal, 11u);
        s = considerVertex(s, -yNormal, 13u);
        s = considerVertex(s, -xNormal, 14u);
        s.maxScore += xyzwNormal - 0.2f;
        s = considerVertex(s, xNormal, 1u);
        s = considerVertex(s, yNormal, 2u);
        s = considerVertex(s, zNormal, 4u);
        s = considerVertex(s, wNormal, 8u);
        s.maxScore += 0.2f - xNormal;
        s = considerVertex(s, yNormal, 3u);
        s = considerVertex(s, zNormal, 5u);
        s = considerVertex(s, wNormal, 9u);
        s.maxScore += xNormal;
        s = considerVertex(s, yNormal + zNormal, 6u);
        s.maxScore -= wNormal;
        s = considerVertex(s, yNormal, 10u);
        s = considerVertex(s, zNormal, 12u);

        bool moveX = (s.moveMaskBits & 1u) != 0u;
        bool moveY = (s.moveMaskBits & 2u) != 0u;
        bool moveZ = (s.moveMaskBits & 4u) != 0u;
        bool moveW = (s.moveMaskBits & 8u) != 0u;

        xSkewedBase += selectU(moveX, 1u, 0u);
        ySkewedBase += selectU(moveY, 1u, 0u);
        zSkewedBase += selectU(moveZ, 1u, 0u);
        wSkewedBase += selectU(moveW, 1u, 0u);

        dxSkewed = maskedDecrement(moveX, dxSkewed);
        dySkewed = maskedDecrement(moveY, dySkewed);
        dzSkewed = maskedDecrement(moveZ, dzSkewed);
        dwSkewed = maskedDecrement(moveW, dwSkewed);
    }

    uint xPrimedBase = xSkewedBase * kPrimeX;
    uint yPrimedBase = ySkewedBase * kPrimeY;
    uint zPrimedBase = zSkewedBase * kPrimeZ;
    uint wPrimedBase = wSkewedBase * kPrimeW;

    float skewedCoordinateSum = ((dxSkewed + dySkewed) + dzSkewed) + dwSkewed;
    float twiceUnskewDelta = kTwiceUnskew4 * skewedCoordinateSum;
    float xNormal = dxSkewed + twiceUnskewDelta;
    float yNormal = dySkewed + twiceUnskewDelta;
    float zNormal = dzSkewed + twiceUnskewDelta;
    float wNormal = dwSkewed + twiceUnskewDelta;
    float xyzwNormal = -twiceUnskewDelta; // xNormal + yNormal + zNormal + wNormal

    float unskewDelta = kUnskew4 * skewedCoordinateSum;
    float dxBase = dxSkewed + unskewDelta;
    float dyBase = dySkewed + unskewDelta;
    float dzBase = dzSkewed + unskewDelta;
    float dwBase = dwSkewed + unskewDelta;

    float coordinateSum = kUnskew4x4p1 * skewedCoordinateSum; // dxBase + dyBase + dzBase + dwBase

    // Vertex <0, 0, 0, 0>
    float value;
    float falloffBaseStemA;
    float falloffBaseStemB;
    {
        float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimedBase, yPrimedBase, zPrimedBase, wPrimedBase), dxBase, dyBase, dzBase, dwBase);
        float falloffBase = nmuladd(dwBase, dwBase, nmuladd(dzBase, dzBase, nmuladd(dyBase, dyBase, nmuladd(dxBase, dxBase, kFalloffRadiusSquared)))) * 0.5f;
        falloffBaseStemA = falloffBase - kDistanceSquaredAHalf;
        falloffBaseStemB = falloffBase - kDistanceSquaredBHalf;
        value = (falloffBase * falloffBase) * (falloffBase * falloffBase) * gradientRampValue;
    }

    // Vertex <1, 1, 1, 1> or <-1, -1, -1, -1>
    {
        bool signMask = xyzwNormal < 0.0f;
        uint sign = selectU(signMask, kSignBit, 0u);
        float offset = xorBits(kUnskew4x4p1, sign);
        value = simplexVertex(value, seed, xPrimedBase + selectU(signMask, 0u - kPrimeX, kPrimeX), yPrimedBase + selectU(signMask, 0u - kPrimeY, kPrimeY), zPrimedBase + selectU(signMask, 0u - kPrimeZ, kPrimeZ), wPrimedBase + selectU(signMask, 0u - kPrimeW, kPrimeW),
                              dxBase - offset, dyBase - offset, dzBase - offset, dwBase - offset, muladd(offset, coordinateSum, falloffBaseStemA));
    }

    // Vertex <1, 1, 1, 0> or <-1, -1, -1, 0>
    {
        bool signMask = xyzwNormal < wNormal;
        uint sign = selectU(signMask, kSignBit, 0u);
        float offset1 = xorBits(kUnskew4x3p1, sign);
        float offset0 = xorBits(kUnskew4x3, sign);
        value = simplexVertex(value, seed, xPrimedBase + selectU(signMask, 0u - kPrimeX, kPrimeX), yPrimedBase + selectU(signMask, 0u - kPrimeY, kPrimeY), zPrimedBase + selectU(signMask, 0u - kPrimeZ, kPrimeZ), wPrimedBase,
                              dxBase - offset1, dyBase - offset1, dzBase - offset1, dwBase - offset0, muladd(offset1, coordinateSum, falloffBaseStemB) - xorBits(dwBase, sign));
    }

    // Vertex <1, 1, 0, 1> or <-1, -1, 0, -1>
    {
        bool signMask = xyzwNormal < zNormal;
        uint sign = selectU(signMask, kSignBit, 0u);
        float offset1 = xorBits(kUnskew4x3p1, sign);
        float offset0 = xorBits(kUnskew4x3, sign);
        value = simplexVertex(value, seed, xPrimedBase + selectU(signMask, 0u - kPrimeX, kPrimeX), yPrimedBase + selectU(signMask, 0u - kPrimeY, kPrimeY), zPrimedBase, wPrimedBase + selectU(signMask, 0u - kPrimeW, kPrimeW),
                              dxBase - offset1, dyBase - offset1, dzBase - offset0, dwBase - offset1, muladd(offset1, coordinateSum, falloffBaseStemB) - xorBits(dzBase, sign));
    }

    // Vertex <1, 0, 1, 1> or <-1, 0, -1, -1>
    {
        bool signMask = xyzwNormal < yNormal;
        uint sign = selectU(signMask, kSignBit, 0u);
        float offset1 = xorBits(kUnskew4x3p1, sign);
        float offset0 = xorBits(kUnskew4x3, sign);
        value = simplexVertex(value, seed, xPrimedBase + selectU(signMask, 0u - kPrimeX, kPrimeX), yPrimedBase, zPrimedBase + selectU(signMask, 0u - kPrimeZ, kPrimeZ), wPrimedBase + selectU(signMask, 0u - kPrimeW, kPrimeW),
                              dxBase - offset1, dyBase - offset0, dzBase - offset1, dwBase - offset1, muladd(offset1, coordinateSum, falloffBaseStemB) - xorBits(dyBase, sign));
    }

    // Vertex <0, 1, 1, 1> or <0, -1, -1, -1>
    {
        bool signMask = xyzwNormal < xNormal;
        uint sign = selectU(signMask, kSignBit, 0u);
        float offset1 = xorBits(kUnskew4x3p1, sign);
        float offset0 = xorBits(kUnskew4x3, sign);
        value = simplexVertex(value, seed, xPrimedBase, yPrimedBase + selectU(signMask, 0u - kPrimeY, kPrimeY), zPrimedBase + selectU(signMask, 0u - kPrimeZ, kPrimeZ), wPrimedBase + selectU(signMask, 0u - kPrimeW, kPrimeW),
                              dxBase - offset0, dyBase - offset1, dzBase - offset1, dwBase - offset1, muladd(offset1, coordinateSum, falloffBaseStemB) - xorBits(dxBase, sign));
    }

    // Vertex <1, 0, 0, 0> or <-1, 0, 0, 0>
    {
        bool signMask = xNormal < 0.0f;
        uint sign = selectU(signMask, kSignBit, 0u);
        float offset1 = xorBits(kUnskew4p1, sign);
        float offset0 = xorBits(kUnskew4, sign);
        value = simplexVertex(value, seed, xPrimedBase + selectU(signMask, 0u - kPrimeX, kPrimeX), yPrimedBase, zPrimedBase, wPrimedBase,
                              dxBase - offset1, dyBase - offset0, dzBase - offset0, dwBase - offset0, muladd(offset0, coordinateSum, falloffBaseStemA) + xorBits(dxBase, sign));
    }

    // Vertex <1, 1, 0, 0> or <-1, -1, 0, 0>
    {
        bool signMask = xNormal < -yNormal;
        uint sign = selectU(signMask, kSignBit, 0u);
        float offset1 = xorBits(kUnskew4x2p1, sign);
        float offset0 = xorBits(kUnskew4x2, sign);
        value = simplexVertex(value, seed, xPrimedBase + selectU(signMask, 0u - kPrimeX, kPrimeX), yPrimedBase + selectU(signMask, 0u - kPrimeY, kPrimeY), zPrimedBase, wPrimedBase,
                              dxBase - offset1, dyBase - offset1, dzBase - offset0, dwBase - offset0, muladd(offset0, coordinateSum, falloffBaseStemB) + xorBits(dxBase + dyBase, sign));
    }

    // Vertex <1, 0, 1, 0> or <-1, 0, -1, 0>
    {
        bool signMask = xNormal < -zNormal;
        uint sign = selectU(signMask, kSignBit, 0u);
        float offset1 = xorBits(kUnskew4x2p1, sign);
        float offset0 = xorBits(kUnskew4x2, sign);
        value = simplexVertex(value, seed, xPrimedBase + selectU(signMask, 0u - kPrimeX, kPrimeX), yPrimedBase, zPrimedBase + selectU(signMask, 0u - kPrimeZ, kPrimeZ), wPrimedBase,
                              dxBase - offset1, dyBase - offset0, dzBase - offset1, dwBase - offset0, muladd(offset0, coordinateSum, falloffBaseStemB) + xorBits(dxBase + dzBase, sign));
    }

    // Vertex <1, 0, 0, 1> or <-1, 0, 0, -1>
    {
        bool signMask = xNormal < -wNormal;
        uint sign = selectU(signMask, kSignBit, 0u);
        float offset1 = xorBits(kUnskew4x2p1, sign);
        float offset0 = xorBits(kUnskew4x2, sign);
        value = simplexVertex(value, seed, xPrimedBase + selectU(signMask, 0u - kPrimeX, kPrimeX), yPrimedBase, zPrimedBase, wPrimedBase + selectU(signMask, 0u - kPrimeW, kPrimeW),
                              dxBase - offset1, dyBase - offset0, dzBase - offset0, dwBase - offset1, muladd(offset0, coordinateSum, falloffBaseStemB) + xorBits(dxBase + dwBase, sign));
    }

    // Vertex <0, 1, 0, 0> or <0, -1, 0, 0>
    {
        bool signMask = yNormal < 0.0f;
        uint sign = selectU(signMask, kSignBit, 0u);
        float offset1 = xorBits(kUnskew4p1, sign);
        float offset0 = xorBits(kUnskew4, sign);
        value = simplexVertex(value, seed, xPrimedBase, yPrimedBase + selectU(signMask, 0u - kPrimeY, kPrimeY), zPrimedBase, wPrimedBase,
                              dxBase - offset0, dyBase - offset1, dzBase - offset0, dwBase - offset0, muladd(offset0, coordinateSum, falloffBaseStemA) + xorBits(dyBase, sign));
    }

    // Vertex <0, 1, 1, 0> or <0, -1, -1, 0>
    {
        bool signMask = yNormal < -zNormal;
        uint sign = selectU(signMask, kSignBit, 0u);
        float offset1 = xorBits(kUnskew4x2p1, sign);
        float offset0 = xorBits(kUnskew4x2, sign);
        value = simplexVertex(value, seed, xPrimedBase, yPrimedBase + selectU(signMask, 0u - kPrimeY, kPrimeY), zPrimedBase + selectU(signMask, 0u - kPrimeZ, kPrimeZ), wPrimedBase,
                              dxBase - offset0, dyBase - offset1, dzBase - offset1, dwBase - offset0, muladd(offset0, coordinateSum, falloffBaseStemB) + xorBits(dyBase + dzBase, sign));
    }

    // Vertex <0, 1, 0, 1> or <0, -1, 0, -1>
    {
        bool signMask = yNormal < -wNormal;
        uint sign = selectU(signMask, kSignBit, 0u);
        float offset1 = xorBits(kUnskew4x2p1, sign);
        float offset0 = xorBits(kUnskew4x2, sign);
        value = simplexVertex(value, seed, xPrimedBase, yPrimedBase + selectU(signMask, 0u - kPrimeY, kPrimeY), zPrimedBase, wPrimedBase + selectU(signMask, 0u - kPrimeW, kPrimeW),
                              dxBase - offset0, dyBase - offset1, dzBase - offset0, dwBase - offset1, muladd(offset0, coordinateSum, falloffBaseStemB) + xorBits(dyBase + dwBase, sign));
    }

    // Vertex <0, 0, 1, 0> or <0, 0, -1, 0>
    {
        bool signMask = zNormal < 0.0f;
        uint sign = selectU(signMask, kSignBit, 0u);
        float offset1 = xorBits(kUnskew4p1, sign);
        float offset0 = xorBits(kUnskew4, sign);
        value = simplexVertex(value, seed, xPrimedBase, yPrimedBase, zPrimedBase + selectU(signMask, 0u - kPrimeZ, kPrimeZ), wPrimedBase,
                              dxBase - offset0, dyBase - offset0, dzBase - offset1, dwBase - offset0, muladd(offset0, coordinateSum, falloffBaseStemA) + xorBits(dzBase, sign));
    }

    // Vertex <0, 0, 1, 1> or <0, 0, -1, -1>
    {
        bool signMask = zNormal < -wNormal;
        uint sign = selectU(signMask, kSignBit, 0u);
        float offset1 = xorBits(kUnskew4x2p1, sign);
        float offset0 = xorBits(kUnskew4x2, sign);
        value = simplexVertex(value, seed, xPrimedBase, yPrimedBase, zPrimedBase + selectU(signMask, 0u - kPrimeZ, kPrimeZ), wPrimedBase + selectU(signMask, 0u - kPrimeW, kPrimeW),
                              dxBase - offset0, dyBase - offset0, dzBase - offset1, dwBase - offset1, muladd(offset0, coordinateSum, falloffBaseStemB) + xorBits(dzBase + dwBase, sign));
    }

    // Vertex <0, 0, 0, 1> or <0, 0, 0, -1>
    {
        bool signMask = wNormal < 0.0f;
        uint sign = selectU(signMask, kSignBit, 0u);
        float offset1 = xorBits(kUnskew4p1, sign);
        float offset0 = xorBits(kUnskew4, sign);
        value = simplexVertex(value, seed, xPrimedBase, yPrimedBase, zPrimedBase, wPrimedBase + selectU(signMask, 0u - kPrimeW, kPrimeW),
                              dxBase - offset0, dyBase - offset0, dzBase - offset0, dwBase - offset1, muladd(offset0, coordinateSum, falloffBaseStemA) + xorBits(dwBase, sign));
    }

    return scaleOutput(value, -kSimplexBounding, kSimplexInvRange);
}

constexpr float kJitter4D = asFloatBits(0x3ebb67a1u); // 0.366025
constexpr float kHalfByte = 127.5f;
static_assert(kJitter4D == 0.366025f);

// Bounds on F2 under each metric, so the outputs map [0, bound] to [-1, 1]. Two of the searched
// points are the jittered lattice point L nearest the sample and its neighbour one step along the
// sample's largest offset from L; without jitter both lie within distance 1 (Euclidean and
// Chebyshev) or 2 (Manhattan), and the jitter moves a point by at most kJitter4D in Euclidean and
// Chebyshev distance and twice that in Manhattan distance.
constexpr float kCellularEuclideanRange = 1.0f + kJitter4D;
constexpr float kCellularManhattanRange = 2.0f + 2.0f * kJitter4D;
constexpr float kCellularChebyshevRange = 1.0f + kJitter4D;
constexpr float kCellularEuclideanScale = asFloatBits(0x3fbb67b2u); // 2 / kCellularEuclideanRange
constexpr float kCellularManhattanScale = asFloatBits(0x3f3b67b2u); // 2 / kCellularManhattanRange
constexpr float kCellularChebyshevScale = asFloatBits(0x3fbb67b2u); // 2 / kCellularChebyshevRange
static_assert(kCellularEuclideanScale == 2.0f / kCellularEuclideanRange);
static_assert(kCellularManhattanScale == 2.0f / kCellularManhattanRange);
static_assert(kCellularChebyshevScale == 2.0f / kCellularChebyshevRange);

// A feature point relative to the sample: the cell's hash gives a direction, the jitter its length
// (FastNoise2's CellularDistance inner step). offset is the cell centre less the sample.
inline NoiseVec4
cellPoint(uint hash, float jitter, float xOffset, float yOffset, float zOffset, float wOffset)
{
    float xd = toFloat(hash & 0xffu) - kHalfByte;
    float yd = toFloat((hash >> 8) & 0xffu) - kHalfByte;
    float zd = toFloat((hash >> 16) & 0xffu) - kHalfByte;
    float wd = toFloat(hash >> 24) - kHalfByte;

    float invMag = jitter * invSqrt(muladd(xd, xd, muladd(yd, yd, muladd(zd, zd, wd * wd))));
    NoiseVec4 r;
    r.x = muladd(xd, invMag, xOffset);
    r.y = muladd(yd, invMag, yOffset);
    r.z = muladd(zd, invMag, zOffset);
    r.w = muladd(wd, invMag, wOffset);
    return r;
}

// The distance of a feature point under the metric; Euclidean is left squared.
inline float
cellDistance(int metric, NoiseVec4 d)
{
    if (metric == kNoiseMetricManhattan) {
        return ((vabs(d.x) + vabs(d.y)) + vabs(d.z)) + vabs(d.w);
    }
    if (metric == kNoiseMetricChebyshev) {
        return vmax(vmax(vmax(vabs(d.x), vabs(d.y)), vabs(d.z)), vabs(d.w));
    }
    float distSqr = d.x * d.x;
    distSqr = muladd(d.y, d.y, distSqr);
    distSqr = muladd(d.z, d.z, distSqr);
    distSqr = muladd(d.w, d.w, distSqr);
    return distSqr;
}

// Polynomial smooth minimum: min(a, b) less a quadratic blend where they are within k.
inline float
smoothMin(float a, float b, float k)
{
    float e = vmax(k - vabs(a - b), 0.0f);
    return vmin(a, b) - e * e * (0.25f / k);
}

// The distance from the sample to the bisector of the nearest point n and another point q, both
// relative to the sample.
inline float
bisectorDistance(NoiseVec4 n, NoiseVec4 q)
{
    float dx = q.x - n.x;
    float dy = q.y - n.y;
    float dz = q.z - n.z;
    float dw = q.w - n.w;
    float lengthSqr = muladd(dw, dw, muladd(dz, dz, muladd(dy, dy, dx * dx)));
    float along = muladd((n.w + q.w) * 0.5f, dw, muladd((n.z + q.z) * 0.5f, dz, muladd((n.y + q.y) * 0.5f, dy, ((n.x + q.x) * 0.5f) * dx)));
    return selectF(lengthSqr > 0.0f, along * invSqrt(lengthSqr), kInfinity);
}

// Cellular (Worley) noise over the 3^4 cells around the sample's nearest lattice point
// (FastNoise2's CellularDistance and CellularValue search), in [-1, 1].
//
// - F1, F2, F2 - F1: the nearest and second-nearest feature distances under the metric.
// - Cell value: the nearest cell's hash.
// - Distance to edge: Euclidean only, the distance to the nearest bisector between the nearest
//   point and any other, found in a second pass. Manhattan and Chebyshev cells are not bounded by
//   planes, so those metrics give (F2 - F1) / 2 instead.
// - Smooth F1: every distance folded through smoothMin with width smoothness.
//
// gridJitter is clamped to [0, 1], so the feature points stay inside their cells.
inline float
cellular4(uint seed, NoiseCoord p, int metric, int output, float gridJitter, float smoothness)
{
    float jitter = kJitter4D * vmin(vmax(gridJitter, 0.0f), 1.0f);
    float smoothWidth = vmax(smoothness, 0.000001f);
    bool euclidean = metric != kNoiseMetricManhattan && metric != kNoiseMetricChebyshev;

    // The 3^4 block starts one cell below the lattice point nearest the sample.
    uint xcBase = p.cx + selectU(p.fx >= 0.5f, 1u, 0u) - 1u;
    uint ycBase = p.cy + selectU(p.fy >= 0.5f, 1u, 0u) - 1u;
    uint zcBase = p.cz + selectU(p.fz >= 0.5f, 1u, 0u) - 1u;
    uint wcBase = p.cw + selectU(p.fw >= 0.5f, 1u, 0u) - 1u;
    float xcfBase = toFloat(xcBase - p.cx);
    float ycfBase = toFloat(ycBase - p.cy);
    float zcfBase = toFloat(zcBase - p.cz);
    float wcfBase = toFloat(wcBase - p.cw);
    uint xpBase = xcBase * kPrimeX;
    uint ypBase = ycBase * kPrimeY;
    uint zpBase = zcBase * kPrimeZ;
    uint wpBase = wcBase * kPrimeW;

    float distance0 = kInfinity;
    float distance1 = kInfinity;
    float smooth = kInfinity;
    uint valueHash = 0u;
    int nearestIndex = 0;
    NoiseVec4 nearest;
    nearest.x = 0.0f;
    nearest.y = 0.0f;
    nearest.z = 0.0f;
    nearest.w = 0.0f;

    int index = 0;
    float xcf = xcfBase;
    uint xc = xpBase;
    for (int xi = 0; xi < 3; xi++) {
        float ycf = ycfBase;
        uint yc = ypBase;
        for (int yi = 0; yi < 3; yi++) {
            float zcf = zcfBase;
            uint zc = zpBase;
            for (int zi = 0; zi < 3; zi++) {
                float wcf = wcfBase;
                uint wc = wpBase;
                for (int wi = 0; wi < 3; wi++) {
                    uint hash = hashPrimesHB(seed, xc, yc, zc, wc);
                    NoiseVec4 d = cellPoint(hash, jitter, xcf - p.fx, ycf - p.fy, zcf - p.fz, wcf - p.fw);
                    float newDistance = cellDistance(metric, d);

                    bool closer0 = newDistance < distance0;
                    bool closer1 = newDistance < distance1;
                    distance1 = selectF(closer0, distance0, selectF(closer1, newDistance, distance1));
                    distance0 = selectF(closer0, newDistance, distance0);
                    valueHash = selectU(closer0, hash, valueHash);
                    nearestIndex = closer0 ? index : nearestIndex;
                    nearest.x = selectF(closer0, d.x, nearest.x);
                    nearest.y = selectF(closer0, d.y, nearest.y);
                    nearest.z = selectF(closer0, d.z, nearest.z);
                    nearest.w = selectF(closer0, d.w, nearest.w);

                    if (output == kNoiseCellularSmoothF1) {
                        smooth = smoothMin(smooth, euclidean ? vsqrt(newDistance) : newDistance, smoothWidth);
                    }

                    index++;
                    wcf += 1.0f;
                    wc += kPrimeW;
                }
                zcf += 1.0f;
                zc += kPrimeZ;
            }
            ycf += 1.0f;
            yc += kPrimeY;
        }
        xcf += 1.0f;
        xc += kPrimeX;
    }

    if (output == kNoiseCellularCellValue) {
        return toFloat(valueHash) * kTwoPowMinus31;
    }

    if (euclidean) {
        distance0 = vsqrt(distance0);
        distance1 = vsqrt(distance1);
    }
    float scale = euclidean ? kCellularEuclideanScale : (metric == kNoiseMetricManhattan ? kCellularManhattanScale : kCellularChebyshevScale);

    float v = distance0;
    if (output == kNoiseCellularF2) {
        v = distance1;
    } else if (output == kNoiseCellularF2MinusF1) {
        v = distance1 - distance0;
    } else if (output == kNoiseCellularSmoothF1) {
        v = vmax(smooth, 0.0f);
    } else if (output == kNoiseCellularDistanceToEdge) {
        v = (distance1 - distance0) * 0.5f;
        if (euclidean) {
            float edge = kInfinity;
            index = 0;
            xcf = xcfBase;
            xc = xpBase;
            for (int xi = 0; xi < 3; xi++) {
                float ycf = ycfBase;
                uint yc = ypBase;
                for (int yi = 0; yi < 3; yi++) {
                    float zcf = zcfBase;
                    uint zc = zpBase;
                    for (int zi = 0; zi < 3; zi++) {
                        float wcf = wcfBase;
                        uint wc = wpBase;
                        for (int wi = 0; wi < 3; wi++) {
                            uint hash = hashPrimesHB(seed, xc, yc, zc, wc);
                            NoiseVec4 d = cellPoint(hash, jitter, xcf - p.fx, ycf - p.fy, zcf - p.fz, wcf - p.fw);
                            float b = bisectorDistance(nearest, d);
                            edge = selectF(index != nearestIndex, vmin(edge, b), edge);

                            index++;
                            wcf += 1.0f;
                            wc += kPrimeW;
                        }
                        zcf += 1.0f;
                        zc += kPrimeZ;
                    }
                    ycf += 1.0f;
                    yc += kPrimeY;
                }
                xcf += 1.0f;
                xc += kPrimeX;
            }
            v = edge;
        }
    }

    // The square roots may round past the bounds by an ulp.
    return vmin(muladd(v, scale, kRangeMin), 1.0f);
}

} // namespace noise4d
