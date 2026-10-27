// Plain C++ port of FastNoise2's 4D Perlin, SuperSimplex, CellularDistance (Euclidean, F1) and
// FractalFBm, from Auburn/FastNoise2 721400f9560831be2fc9790be9b72c44bee7d33f
// (include/FastNoise/Generators/{Perlin,Simplex,Cellular,Fractal,Utils,BasicGenerators}.inl).
// MIT, see LICENSE in this directory. fn2port4d.slang is the line-for-line GPU mirror.
//
// One SIMD lane of FastSIMD becomes one scalar: float32v -> float, int32v -> uint32_t (wrapping
// arithmetic; signed only where FastNoise2 relies on an arithmetic shift or a signed compare),
// mask32v -> bool, FS::Select -> select, FS::Masked* -> ternaries written exactly as FastSIMD's
// generic definitions. Each gradient follows FastSIMD's generic (SSE2/AVX2) path, not the AVX-512
// permute tables, which sum in a different order.
//
// The includer defines FN2P_NS (a namespace unique to the translation unit, because each one is
// compiled with its own -march) and FN2P_FUSED: 1 maps FS::FMulAdd/FNMulAdd to fma(), 0 to a
// separate multiply and add as in FastNoise2's strict build. Build with -ffp-contract=off and
// without fast-math in both cases. Valid for |scaled coordinate| < 2^31.
//
// Vectorising the row loops at -O2 also needs -fno-trapping-math and -fno-math-errno. Neither
// changes a result: the first lets GCC if-convert selects whose arms hold floating-point compares
// or arithmetic (they could raise an FP exception if executed unconditionally), the second lets
// sqrtf become sqrtps instead of a libm call that might set errno.

#pragma once

#include <bit>
#include <cmath>
#include <cstdint>

#if !defined(FN2P_NS) || !defined(FN2P_FUSED)
#error "define FN2P_NS and FN2P_FUSED before including fn2port4d.h"
#endif

namespace FN2P_NS {
namespace {

    using u32 = uint32_t;
    using i32 = int32_t;

#define FN2P_INLINE [[gnu::always_inline]] inline

    namespace Primes {
        constexpr u32 X = 0xF797C5C7u;
        constexpr u32 Y = 0x6C060C89u;
        constexpr u32 Z = 0x465FD04Fu;
        constexpr u32 W = 0xF7A62279u;
    }
    constexpr u32 kHashMultiplierA = 0xB7E0A5F5u;
    constexpr float kInfinity = INFINITY;
    constexpr u32 kSignBit = 0x80000000u;

    // FastNoise2's output range defaults: VariableRange mRangeMin -1, mRangeScale 2.
    constexpr float kRangeMin = -1.0f;
    constexpr float kRangeScale = 2.0f;

    FN2P_INLINE float select(bool m, float a, float b) { return m ? a : b; }
    FN2P_INLINE u32 select(bool m, u32 a, u32 b) { return m ? a : b; }

    // FS::FMulAdd (a * b) + c and FS::FNMulAdd c - (a * b).
    FN2P_INLINE float muladd(float a, float b, float c)
    {
#if FN2P_FUSED
        return std::fma(a, b, c);
#else
        return a * b + c;
#endif
    }

    FN2P_INLINE float nmuladd(float a, float b, float c)
    {
#if FN2P_FUSED
        return std::fma(-a, b, c);
#else
        return c - a * b;
#endif
    }

    // SSE semantics: max_ps(a, b) is a > b ? a : b, min_ps(a, b) is a < b ? a : b.
    FN2P_INLINE float vmax(float a, float b) { return a > b ? a : b; }
    FN2P_INLINE float vmin(float a, float b) { return a < b ? a : b; }

    // float32v ^ float32v where the right side is a sign mask.
    FN2P_INLINE float xorBits(float a, u32 bits) { return std::bit_cast<float>(std::bit_cast<u32>(a) ^ bits); }

    FN2P_INLINE u32 sra(u32 a, int n) { return (u32)((i32)a >> n); }

    FN2P_INLINE float masked(bool m, float a) { return m ? a : 0.0f; }
    FN2P_INLINE float maskedIncrement(bool m, float a) { return a - masked(m, -1.0f); }
    FN2P_INLINE float maskedDecrement(bool m, float a) { return a + masked(m, -1.0f); }

    // Round half to even, exact for every float: the cvtps2dq rounding FS::Convert<int32_t> uses.
    // Adding and removing 2^23 leaves no fraction bits; magnitudes from 2^23 up are already integers
    // and get a zero constant instead, so the add and subtract need no branch.
    FN2P_INLINE float roundEven(float a)
    {
        const u32 magnitude = std::fabs(a) < 8388608.0f ? 0x4B000000u : 0u;
        const float c = std::bit_cast<float>((std::bit_cast<u32>(a) & kSignBit) | magnitude);
        return (a + c) - c;
    }

    // FS::Floor as FastSIMD's SSE2 path computes it: Round, then decrement where it rounded up.
    FN2P_INLINE float floorV(float a)
    {
        const float r = roundEven(a);
        return maskedDecrement(r > a, r);
    }

    // FS::Convert<int32_t> and FS::Convert<float>.
    FN2P_INLINE u32 toInt(float a) { return (u32)(i32)roundEven(a); }
    FN2P_INLINE float toFloat(u32 a) { return (float)(i32)a; }

    FN2P_INLINE float invSqrt(float a) { return 1.0f / std::sqrt(a); }

    FN2P_INLINE u32 hashPrimes(u32 seed, u32 x, u32 y, u32 z, u32 w)
    {
        u32 hash = seed;
        hash ^= x ^ (y ^ (z ^ w));

        hash *= kHashMultiplierA;

        return sra(hash, 15) ^ hash;
    }

    FN2P_INLINE u32 hashPrimesHB(u32 seed, u32 x, u32 y, u32 z, u32 w)
    {
        u32 hash = seed;
        hash ^= x ^ (y ^ (z ^ w));

        hash *= kHashMultiplierA;
        return hash;
    }

    FN2P_INLINE float lerp(float a, float b, float t)
    {
        return muladd(t, b - a, a);
    }

    FN2P_INLINE float interpQuintic(float t)
    {
        return t * t * t * muladd(t, muladd(t, 6.0f, -15.0f), 10.0f);
    }

    FN2P_INLINE float getGradientDotPerlin(u32 hash, float fX, float fY, float fZ, float fW)
    {
        u32 p = hash & (3u << 3);

        float a = select((i32)p > 0, fX, fY);
        float b = select((i32)(hash << 27) < 0, fY, fZ);
        float c = select((i32)p > (2 << 3), fZ, fW);

        u32 aSign = hash << 31;
        u32 bSign = (hash >> 1) << 31;
        u32 cSign = (hash >> 2) << 31;

        return xorBits(a, aSign) + xorBits(b, bSign) + xorBits(c, cSign);
    }

    // VariableRange::ScaleOutput with the default range.
    FN2P_INLINE float scaleOutput(float value, float nativeMin, float nativeMax)
    {
        return muladd(float(1.0f / (nativeMax - nativeMin)) * kRangeScale, value - nativeMin, kRangeMin);
    }

    FN2P_INLINE float perlin4(u32 seed, float x, float y, float z, float w, float frequency)
    {
        x *= frequency;
        y *= frequency;
        z *= frequency;
        w *= frequency;

        float xs = floorV(x);
        float ys = floorV(y);
        float zs = floorV(z);
        float ws = floorV(w);

        u32 x0 = toInt(xs) * Primes::X;
        u32 y0 = toInt(ys) * Primes::Y;
        u32 z0 = toInt(zs) * Primes::Z;
        u32 w0 = toInt(ws) * Primes::W;
        u32 x1 = x0 + Primes::X;
        u32 y1 = y0 + Primes::Y;
        u32 z1 = z0 + Primes::Z;
        u32 w1 = w0 + Primes::W;

        float xf0 = xs = x - xs;
        float yf0 = ys = y - ys;
        float zf0 = zs = z - zs;
        float wf0 = ws = w - ws;
        float xf1 = xf0 - 1.0f;
        float yf1 = yf0 - 1.0f;
        float zf1 = zf0 - 1.0f;
        float wf1 = wf0 - 1.0f;

        xs = interpQuintic(xs);
        ys = interpQuintic(ys);
        zs = interpQuintic(zs);
        ws = interpQuintic(ws);

        float value = lerp(lerp(lerp(
                                    lerp(getGradientDotPerlin(hashPrimes(seed, x0, y0, z0, w0), xf0, yf0, zf0, wf0), getGradientDotPerlin(hashPrimes(seed, x1, y0, z0, w0), xf1, yf0, zf0, wf0), xs),
                                    lerp(getGradientDotPerlin(hashPrimes(seed, x0, y1, z0, w0), xf0, yf1, zf0, wf0), getGradientDotPerlin(hashPrimes(seed, x1, y1, z0, w0), xf1, yf1, zf0, wf0), xs), ys),
                                lerp(
                                    lerp(getGradientDotPerlin(hashPrimes(seed, x0, y0, z1, w0), xf0, yf0, zf1, wf0), getGradientDotPerlin(hashPrimes(seed, x1, y0, z1, w0), xf1, yf0, zf1, wf0), xs),
                                    lerp(getGradientDotPerlin(hashPrimes(seed, x0, y1, z1, w0), xf0, yf1, zf1, wf0), getGradientDotPerlin(hashPrimes(seed, x1, y1, z1, w0), xf1, yf1, zf1, wf0), xs), ys),
                                zs),
                           lerp(lerp(
                                    lerp(getGradientDotPerlin(hashPrimes(seed, x0, y0, z0, w1), xf0, yf0, zf0, wf1), getGradientDotPerlin(hashPrimes(seed, x1, y0, z0, w1), xf1, yf0, zf0, wf1), xs),
                                    lerp(getGradientDotPerlin(hashPrimes(seed, x0, y1, z0, w1), xf0, yf1, zf0, wf1), getGradientDotPerlin(hashPrimes(seed, x1, y1, z0, w1), xf1, yf1, zf0, wf1), xs), ys),
                                lerp(
                                    lerp(getGradientDotPerlin(hashPrimes(seed, x0, y0, z1, w1), xf0, yf0, zf1, wf1), getGradientDotPerlin(hashPrimes(seed, x1, y0, z1, w1), xf1, yf0, zf1, wf1), xs),
                                    lerp(getGradientDotPerlin(hashPrimes(seed, x0, y1, z1, w1), xf0, yf1, zf1, wf1), getGradientDotPerlin(hashPrimes(seed, x1, y1, z1, w1), xf1, yf1, zf1, wf1), xs), ys),
                                zs),
                           ws);

        constexpr float kBounding = 1.33858621120452880859375;

        return scaleOutput(value, -kBounding, kBounding);
    }

    namespace ss {
        constexpr double kRoot5 = 2.2360679774997896964091736687313;
        constexpr double kSkew4 = 1.0 / (kRoot5 + 1.0);
        constexpr double kUnskew4 = -1.0 / (kRoot5 + 5.0);
        constexpr double kTwiceUnskew4 = -1.0 / 5.0;

        constexpr double kDistanceSquaredA = 4.0 / 5.0;
        constexpr double kDistanceSquaredB = 6.0 / 5.0;
        constexpr double kFalloffRadiusSquared = kDistanceSquaredA;

        // The float32 values the .slang file spells out as literals.
        static_assert(float(kSkew4) == 0.309017003f);
        static_assert(float(kTwiceUnskew4) == -0.200000003f);
        static_assert(float(kUnskew4) == -0.138196602f);
        static_assert(float(kFalloffRadiusSquared) == 0.800000012f);
        static_assert(float(kDistanceSquaredA * 0.5) == 0.400000006f);
        static_assert(float(kDistanceSquaredB * 0.5) == 0.600000024f);
        static_assert(float(1 + 4 * kUnskew4) == 0.44721359f);
        static_assert(float(4 * kUnskew4 + 1) == 0.44721359f);
        static_assert(float(3 * kUnskew4 + 1) == 0.585410178f);
        static_assert(float(3 * kUnskew4) == -0.414589792f);
        static_assert(float(2 * kUnskew4 + 1) == 0.723606825f);
        static_assert(float(2 * kUnskew4) == -0.276393205f);
        static_assert(float(kUnskew4 + 1) == 0.861803412f);
    }

    FN2P_INLINE void considerVertex(float& maxScore, u32& moveMaskBits, float score, u32 bits)
    {
        moveMaskBits = select(score > maxScore, bits, moveMaskBits);
        maxScore = vmax(maxScore, score);
    }

    FN2P_INLINE float superSimplex4(u32 seed, float x, float y, float z, float w, float frequency)
    {
        using namespace ss;

        x *= frequency;
        y *= frequency;
        z *= frequency;
        w *= frequency;

        float skewDelta = float(kSkew4) * (x + y + z + w);

        float xSkewed = x + skewDelta;
        float ySkewed = y + skewDelta;
        float zSkewed = z + skewDelta;
        float wSkewed = w + skewDelta;
        float xSkewedBase = floorV(xSkewed);
        float ySkewedBase = floorV(ySkewed);
        float zSkewedBase = floorV(zSkewed);
        float wSkewedBase = floorV(wSkewed);
        float dxSkewed = xSkewed - xSkewedBase;
        float dySkewed = ySkewed - ySkewedBase;
        float dzSkewed = zSkewed - zSkewedBase;
        float dwSkewed = wSkewed - wSkewedBase;

        // From unit cell base, find closest vertex
        {
            // Perform a double unskew to get the vector whose dot product with skewed vectors produces the unskewed result.
            float twiceUnskewDelta = float(kTwiceUnskew4) * (dxSkewed + dySkewed + dzSkewed + dwSkewed);
            float xNormal = dxSkewed + twiceUnskewDelta;
            float yNormal = dySkewed + twiceUnskewDelta;
            float zNormal = dzSkewed + twiceUnskewDelta;
            float wNormal = dwSkewed + twiceUnskewDelta;
            float xyzwNormal = -twiceUnskewDelta; // xNormal + yNormal + zNormal + wNormal

            // Using those, compare scores to determine which vertex is closest.
            float maxScore = 0.6f - xyzwNormal;
            u32 moveMaskBits = 0.2f > maxScore ? ~0u : 0u;
            maxScore = vmax(maxScore, 0.2f);
            considerVertex(maxScore, moveMaskBits, -wNormal, 0b0111);
            considerVertex(maxScore, moveMaskBits, -zNormal, 0b1011);
            considerVertex(maxScore, moveMaskBits, -yNormal, 0b1101);
            considerVertex(maxScore, moveMaskBits, -xNormal, 0b1110);
            maxScore += xyzwNormal - 0.2f;
            considerVertex(maxScore, moveMaskBits, xNormal, 0b0001);
            considerVertex(maxScore, moveMaskBits, yNormal, 0b0010);
            considerVertex(maxScore, moveMaskBits, zNormal, 0b0100);
            considerVertex(maxScore, moveMaskBits, wNormal, 0b1000);
            maxScore += 0.2f - xNormal;
            considerVertex(maxScore, moveMaskBits, yNormal, 0b0011);
            considerVertex(maxScore, moveMaskBits, zNormal, 0b0101);
            considerVertex(maxScore, moveMaskBits, wNormal, 0b1001);
            maxScore += xNormal;
            considerVertex(maxScore, moveMaskBits, yNormal + zNormal, 0b0110);
            maxScore -= wNormal;
            considerVertex(maxScore, moveMaskBits, yNormal, 0b1010);
            considerVertex(maxScore, moveMaskBits, zNormal, 0b1100);

            // FastNoise2: (moveMaskBits & 0b0001) != 0 and so on. Tested as a sign bit here: GCC folds the
            // AND form into a one-bit bool conversion that its vectoriser does not support.
            bool moveX = (i32)(moveMaskBits << 31) < 0;
            bool moveY = (i32)(moveMaskBits << 30) < 0;
            bool moveZ = (i32)(moveMaskBits << 29) < 0;
            bool moveW = (i32)(moveMaskBits << 28) < 0;

            xSkewedBase = maskedIncrement(moveX, xSkewedBase);
            ySkewedBase = maskedIncrement(moveY, ySkewedBase);
            zSkewedBase = maskedIncrement(moveZ, zSkewedBase);
            wSkewedBase = maskedIncrement(moveW, wSkewedBase);

            dxSkewed = maskedDecrement(moveX, dxSkewed);
            dySkewed = maskedDecrement(moveY, dySkewed);
            dzSkewed = maskedDecrement(moveZ, dzSkewed);
            dwSkewed = maskedDecrement(moveW, dwSkewed);
        }

        u32 xPrimedBase = toInt(xSkewedBase) * Primes::X;
        u32 yPrimedBase = toInt(ySkewedBase) * Primes::Y;
        u32 zPrimedBase = toInt(zSkewedBase) * Primes::Z;
        u32 wPrimedBase = toInt(wSkewedBase) * Primes::W;

        float skewedCoordinateSum = dxSkewed + dySkewed + dzSkewed + dwSkewed;
        float twiceUnskewDelta = float(kTwiceUnskew4) * skewedCoordinateSum;
        float xNormal = dxSkewed + twiceUnskewDelta;
        float yNormal = dySkewed + twiceUnskewDelta;
        float zNormal = dzSkewed + twiceUnskewDelta;
        float wNormal = dwSkewed + twiceUnskewDelta;
        float xyzwNormal = -twiceUnskewDelta; // xNormal + yNormal + zNormal + wNormal

        float unskewDelta = float(kUnskew4) * skewedCoordinateSum;
        float dxBase = dxSkewed + unskewDelta;
        float dyBase = dySkewed + unskewDelta;
        float dzBase = dzSkewed + unskewDelta;
        float dwBase = dwSkewed + unskewDelta;

        float coordinateSum = float(1 + 4 * kUnskew4) * skewedCoordinateSum; // dxBase + dyBase + dzBase + dwBase

        // Vertex <0, 0, 0, 0>
        float value, falloffBaseStemA, falloffBaseStemB;
        {
            float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimedBase, yPrimedBase, zPrimedBase, wPrimedBase), dxBase, dyBase, dzBase, dwBase);
            float falloffBase = nmuladd(dwBase, dwBase, nmuladd(dzBase, dzBase, nmuladd(dyBase, dyBase, nmuladd(dxBase, dxBase, float(kFalloffRadiusSquared))))) * 0.5f;
            falloffBaseStemA = falloffBase - float(kDistanceSquaredA * 0.5);
            falloffBaseStemB = falloffBase - float(kDistanceSquaredB * 0.5);
            value = (falloffBase * falloffBase) * (falloffBase * falloffBase) * gradientRampValue;
        }

        // Vertex <1, 1, 1, 1> or <-1, -1, -1, -1>
        {
            bool signMask = xyzwNormal < 0.0f;
            u32 sign = signMask ? kSignBit : 0u;

            u32 xPrimed = xPrimedBase + select(signMask, 0u - Primes::X, Primes::X);
            u32 yPrimed = yPrimedBase + select(signMask, 0u - Primes::Y, Primes::Y);
            u32 zPrimed = zPrimedBase + select(signMask, 0u - Primes::Z, Primes::Z);
            u32 wPrimed = wPrimedBase + select(signMask, 0u - Primes::W, Primes::W);

            float offset = xorBits(float(4 * kUnskew4 + 1), sign);

            float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimed, yPrimed, zPrimed, wPrimed), dxBase - offset, dyBase - offset, dzBase - offset, dwBase - offset);
            float falloffBase = vmax(muladd(offset, coordinateSum, falloffBaseStemA), 0.0f);
            value = muladd((falloffBase * falloffBase) * (falloffBase * falloffBase), gradientRampValue, value);
        }

        // Vertex <1, 1, 1, 0> or <-1, -1, -1, 0>
        {
            bool signMask = xyzwNormal < wNormal;
            u32 sign = signMask ? kSignBit : 0u;

            u32 xPrimed = xPrimedBase + select(signMask, 0u - Primes::X, Primes::X);
            u32 yPrimed = yPrimedBase + select(signMask, 0u - Primes::Y, Primes::Y);
            u32 zPrimed = zPrimedBase + select(signMask, 0u - Primes::Z, Primes::Z);

            float offset1 = xorBits(float(3 * kUnskew4 + 1), sign);
            float offset0 = xorBits(float(3 * kUnskew4), sign);

            float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimed, yPrimed, zPrimed, wPrimedBase), dxBase - offset1, dyBase - offset1, dzBase - offset1, dwBase - offset0);
            float falloffBase = vmax(muladd(offset1, coordinateSum, falloffBaseStemB) - xorBits(dwBase, sign), 0.0f);
            value = muladd((falloffBase * falloffBase) * (falloffBase * falloffBase), gradientRampValue, value);
        }

        // Vertex <1, 1, 0, 1> or <-1, -1, 0, -1>
        {
            bool signMask = xyzwNormal < zNormal;
            u32 sign = signMask ? kSignBit : 0u;

            u32 xPrimed = xPrimedBase + select(signMask, 0u - Primes::X, Primes::X);
            u32 yPrimed = yPrimedBase + select(signMask, 0u - Primes::Y, Primes::Y);
            u32 wPrimed = wPrimedBase + select(signMask, 0u - Primes::W, Primes::W);

            float offset1 = xorBits(float(3 * kUnskew4 + 1), sign);
            float offset0 = xorBits(float(3 * kUnskew4), sign);

            float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimed, yPrimed, zPrimedBase, wPrimed), dxBase - offset1, dyBase - offset1, dzBase - offset0, dwBase - offset1);
            float falloffBase = vmax(muladd(offset1, coordinateSum, falloffBaseStemB) - xorBits(dzBase, sign), 0.0f);
            value = muladd((falloffBase * falloffBase) * (falloffBase * falloffBase), gradientRampValue, value);
        }

        // Vertex <1, 0, 1, 1> or <-1, 0, -1, -1>
        {
            bool signMask = xyzwNormal < yNormal;
            u32 sign = signMask ? kSignBit : 0u;

            u32 xPrimed = xPrimedBase + select(signMask, 0u - Primes::X, Primes::X);
            u32 zPrimed = zPrimedBase + select(signMask, 0u - Primes::Z, Primes::Z);
            u32 wPrimed = wPrimedBase + select(signMask, 0u - Primes::W, Primes::W);

            float offset1 = xorBits(float(3 * kUnskew4 + 1), sign);
            float offset0 = xorBits(float(3 * kUnskew4), sign);

            float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimed, yPrimedBase, zPrimed, wPrimed), dxBase - offset1, dyBase - offset0, dzBase - offset1, dwBase - offset1);
            float falloffBase = vmax(muladd(offset1, coordinateSum, falloffBaseStemB) - xorBits(dyBase, sign), 0.0f);
            value = muladd((falloffBase * falloffBase) * (falloffBase * falloffBase), gradientRampValue, value);
        }

        // Vertex <0, 1, 1, 1> or <0, -1, -1, -1>
        {
            bool signMask = xyzwNormal < xNormal;
            u32 sign = signMask ? kSignBit : 0u;

            u32 yPrimed = yPrimedBase + select(signMask, 0u - Primes::Y, Primes::Y);
            u32 zPrimed = zPrimedBase + select(signMask, 0u - Primes::Z, Primes::Z);
            u32 wPrimed = wPrimedBase + select(signMask, 0u - Primes::W, Primes::W);

            float offset1 = xorBits(float(3 * kUnskew4 + 1), sign);
            float offset0 = xorBits(float(3 * kUnskew4), sign);

            float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimedBase, yPrimed, zPrimed, wPrimed), dxBase - offset0, dyBase - offset1, dzBase - offset1, dwBase - offset1);
            float falloffBase = vmax(muladd(offset1, coordinateSum, falloffBaseStemB) - xorBits(dxBase, sign), 0.0f);
            value = muladd((falloffBase * falloffBase) * (falloffBase * falloffBase), gradientRampValue, value);
        }

        // Vertex <1, 0, 0, 0> or <-1, 0, 0, 0>
        {
            bool signMask = xNormal < 0.0f;
            u32 sign = signMask ? kSignBit : 0u;

            u32 xPrimed = xPrimedBase + select(signMask, 0u - Primes::X, Primes::X);

            float offset1 = xorBits(float(kUnskew4 + 1), sign);
            float offset0 = xorBits(float(kUnskew4), sign);

            float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimed, yPrimedBase, zPrimedBase, wPrimedBase), dxBase - offset1, dyBase - offset0, dzBase - offset0, dwBase - offset0);
            float falloffBase = vmax(muladd(offset0, coordinateSum, falloffBaseStemA) + xorBits(dxBase, sign), 0.0f);
            value = muladd((falloffBase * falloffBase) * (falloffBase * falloffBase), gradientRampValue, value);
        }

        // Vertex <1, 1, 0, 0> or <-1, -1, 0, 0>
        {
            bool signMask = xNormal < -yNormal;
            u32 sign = signMask ? kSignBit : 0u;

            u32 xPrimed = xPrimedBase + select(signMask, 0u - Primes::X, Primes::X);
            u32 yPrimed = yPrimedBase + select(signMask, 0u - Primes::Y, Primes::Y);

            float offset1 = xorBits(float(2 * kUnskew4 + 1), sign);
            float offset0 = xorBits(float(2 * kUnskew4), sign);

            float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimed, yPrimed, zPrimedBase, wPrimedBase), dxBase - offset1, dyBase - offset1, dzBase - offset0, dwBase - offset0);
            float falloffBase = vmax(muladd(offset0, coordinateSum, falloffBaseStemB) + xorBits(dxBase + dyBase, sign), 0.0f);
            value = muladd((falloffBase * falloffBase) * (falloffBase * falloffBase), gradientRampValue, value);
        }

        // Vertex <1, 0, 1, 0> or <-1, 0, -1, 0>
        {
            bool signMask = xNormal < -zNormal;
            u32 sign = signMask ? kSignBit : 0u;

            u32 xPrimed = xPrimedBase + select(signMask, 0u - Primes::X, Primes::X);
            u32 zPrimed = zPrimedBase + select(signMask, 0u - Primes::Z, Primes::Z);

            float offset1 = xorBits(float(2 * kUnskew4 + 1), sign);
            float offset0 = xorBits(float(2 * kUnskew4), sign);

            float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimed, yPrimedBase, zPrimed, wPrimedBase), dxBase - offset1, dyBase - offset0, dzBase - offset1, dwBase - offset0);
            float falloffBase = vmax(muladd(offset0, coordinateSum, falloffBaseStemB) + xorBits(dxBase + dzBase, sign), 0.0f);
            value = muladd((falloffBase * falloffBase) * (falloffBase * falloffBase), gradientRampValue, value);
        }

        // Vertex <1, 0, 0, 1> or <-1, 0, 0, -1>
        {
            bool signMask = xNormal < -wNormal;
            u32 sign = signMask ? kSignBit : 0u;

            u32 xPrimed = xPrimedBase + select(signMask, 0u - Primes::X, Primes::X);
            u32 wPrimed = wPrimedBase + select(signMask, 0u - Primes::W, Primes::W);

            float offset1 = xorBits(float(2 * kUnskew4 + 1), sign);
            float offset0 = xorBits(float(2 * kUnskew4), sign);

            float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimed, yPrimedBase, zPrimedBase, wPrimed), dxBase - offset1, dyBase - offset0, dzBase - offset0, dwBase - offset1);
            float falloffBase = vmax(muladd(offset0, coordinateSum, falloffBaseStemB) + xorBits(dxBase + dwBase, sign), 0.0f);
            value = muladd((falloffBase * falloffBase) * (falloffBase * falloffBase), gradientRampValue, value);
        }

        // Vertex <0, 1, 0, 0> or <0, -1, 0, 0>
        {
            bool signMask = yNormal < 0.0f;
            u32 sign = signMask ? kSignBit : 0u;

            u32 yPrimed = yPrimedBase + select(signMask, 0u - Primes::Y, Primes::Y);

            float offset1 = xorBits(float(kUnskew4 + 1), sign);
            float offset0 = xorBits(float(kUnskew4), sign);

            float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimedBase, yPrimed, zPrimedBase, wPrimedBase), dxBase - offset0, dyBase - offset1, dzBase - offset0, dwBase - offset0);
            float falloffBase = vmax(muladd(offset0, coordinateSum, falloffBaseStemA) + xorBits(dyBase, sign), 0.0f);
            value = muladd((falloffBase * falloffBase) * (falloffBase * falloffBase), gradientRampValue, value);
        }

        // Vertex <0, 1, 1, 0> or <0, -1, -1, 0>
        {
            bool signMask = yNormal < -zNormal;
            u32 sign = signMask ? kSignBit : 0u;

            u32 yPrimed = yPrimedBase + select(signMask, 0u - Primes::Y, Primes::Y);
            u32 zPrimed = zPrimedBase + select(signMask, 0u - Primes::Z, Primes::Z);

            float offset1 = xorBits(float(2 * kUnskew4 + 1), sign);
            float offset0 = xorBits(float(2 * kUnskew4), sign);

            float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimedBase, yPrimed, zPrimed, wPrimedBase), dxBase - offset0, dyBase - offset1, dzBase - offset1, dwBase - offset0);
            float falloffBase = vmax(muladd(offset0, coordinateSum, falloffBaseStemB) + xorBits(dyBase + dzBase, sign), 0.0f);
            value = muladd((falloffBase * falloffBase) * (falloffBase * falloffBase), gradientRampValue, value);
        }

        // Vertex <0, 1, 0, 1> or <0, -1, 0, -1>
        {
            bool signMask = yNormal < -wNormal;
            u32 sign = signMask ? kSignBit : 0u;

            u32 yPrimed = yPrimedBase + select(signMask, 0u - Primes::Y, Primes::Y);
            u32 wPrimed = wPrimedBase + select(signMask, 0u - Primes::W, Primes::W);

            float offset1 = xorBits(float(2 * kUnskew4 + 1), sign);
            float offset0 = xorBits(float(2 * kUnskew4), sign);

            float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimedBase, yPrimed, zPrimedBase, wPrimed), dxBase - offset0, dyBase - offset1, dzBase - offset0, dwBase - offset1);
            float falloffBase = vmax(muladd(offset0, coordinateSum, falloffBaseStemB) + xorBits(dyBase + dwBase, sign), 0.0f);
            value = muladd((falloffBase * falloffBase) * (falloffBase * falloffBase), gradientRampValue, value);
        }

        // Vertex <0, 0, 1, 0> or <0, 0, -1, 0>
        {
            bool signMask = zNormal < 0.0f;
            u32 sign = signMask ? kSignBit : 0u;

            u32 zPrimed = zPrimedBase + select(signMask, 0u - Primes::Z, Primes::Z);

            float offset1 = xorBits(float(kUnskew4 + 1), sign);
            float offset0 = xorBits(float(kUnskew4), sign);

            float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimedBase, yPrimedBase, zPrimed, wPrimedBase), dxBase - offset0, dyBase - offset0, dzBase - offset1, dwBase - offset0);
            float falloffBase = vmax(muladd(offset0, coordinateSum, falloffBaseStemA) + xorBits(dzBase, sign), 0.0f);
            value = muladd((falloffBase * falloffBase) * (falloffBase * falloffBase), gradientRampValue, value);
        }

        // Vertex <0, 0, 1, 1> or <0, 0, -1, -1>
        {
            bool signMask = zNormal < -wNormal;
            u32 sign = signMask ? kSignBit : 0u;

            u32 zPrimed = zPrimedBase + select(signMask, 0u - Primes::Z, Primes::Z);
            u32 wPrimed = wPrimedBase + select(signMask, 0u - Primes::W, Primes::W);

            float offset1 = xorBits(float(2 * kUnskew4 + 1), sign);
            float offset0 = xorBits(float(2 * kUnskew4), sign);

            float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimedBase, yPrimedBase, zPrimed, wPrimed), dxBase - offset0, dyBase - offset0, dzBase - offset1, dwBase - offset1);
            float falloffBase = vmax(muladd(offset0, coordinateSum, falloffBaseStemB) + xorBits(dzBase + dwBase, sign), 0.0f);
            value = muladd((falloffBase * falloffBase) * (falloffBase * falloffBase), gradientRampValue, value);
        }

        // Vertex <0, 0, 0, 1> or <0, 0, 0, -1>
        {
            bool signMask = wNormal < 0.0f;
            u32 sign = signMask ? kSignBit : 0u;

            u32 wPrimed = wPrimedBase + select(signMask, 0u - Primes::W, Primes::W);

            float offset1 = xorBits(float(kUnskew4 + 1), sign);
            float offset0 = xorBits(float(kUnskew4), sign);

            float gradientRampValue = getGradientDotPerlin(hashPrimes(seed, xPrimedBase, yPrimedBase, zPrimedBase, wPrimed), dxBase - offset0, dyBase - offset0, dzBase - offset0, dwBase - offset1);
            float falloffBase = vmax(muladd(offset0, coordinateSum, falloffBaseStemA) + xorBits(dwBase, sign), 0.0f);
            value = muladd((falloffBase * falloffBase) * (falloffBase * falloffBase), gradientRampValue, value);
        }

        constexpr float kBounding = 0.011310013942420482635498046875;

        return scaleOutput(value, -kBounding, kBounding);
    }

    constexpr float kJitter4D = 0.366025f;

    // The four-lane hash-to-offset step of the CellularDistance inner loop; shared by the scalar
    // function below and the row loop in fn2port_rows.cpp so both stay one implementation.
    FN2P_INLINE float cellularCellDistance(u32 hash, float jitter, float xcfOffset, float ycfOffset, float zcfOffset, float wcfOffset)
    {
        float xd = toFloat(hash & 0xffu) - float(0xff / 2.0f);
        float yd = toFloat((hash >> 8) & 0xffu) - float(0xff / 2.0f);
        float zd = toFloat((hash >> 16) & 0xffu) - float(0xff / 2.0f);
        float wd = toFloat(hash >> 24) - float(0xff / 2.0f);

        float invMag = jitter * invSqrt(muladd(xd, xd, muladd(yd, yd, muladd(zd, zd, wd * wd))));
        xd = muladd(xd, invMag, xcfOffset);
        yd = muladd(yd, invMag, ycfOffset);
        zd = muladd(zd, invMag, zcfOffset);
        wd = muladd(wd, invMag, wcfOffset);

        // CalcDistance<false>( Euclidean ) is the squared distance.
        float distSqr = xd * xd;
        distSqr = muladd(yd, yd, distSqr);
        distSqr = muladd(zd, zd, distSqr);
        distSqr = muladd(wd, wd, distSqr);
        return distSqr;
    }

    // GetReturn for Euclidean, ReturnType::Index0, mDistanceIndex0 0.
    FN2P_INLINE float cellularReturnF1(float distance0)
    {
        distance0 = std::sqrt(distance0);

        float maxDist = 1 + kJitter4D;
        maxDist *= maxDist;

        return scaleOutput(distance0, 0, maxDist);
    }

    // CellularDistance with DistanceFunction::Euclidean, ReturnType::Index0, mDistanceIndex0 0 and no
    // size jitter. Only distance[0] (F1) is kept: the others never feed it.
    FN2P_INLINE float cellularF1(u32 seed, float x, float y, float z, float w, float frequency, float gridJitter)
    {
        float jitter = kJitter4D * gridJitter;

        float distance0 = kInfinity;

        x *= frequency;
        y *= frequency;
        z *= frequency;
        w *= frequency;

        u32 xc = toInt(x) + u32(-1);
        u32 ycBase = toInt(y) + u32(-1);
        u32 zcBase = toInt(z) + u32(-1);
        u32 wcBase = toInt(w) + u32(-1);

        float xcf = toFloat(xc);
        float ycfBase = toFloat(ycBase);
        float zcfBase = toFloat(zcBase);
        float wcfBase = toFloat(wcBase);

        xc *= Primes::X;
        ycBase *= Primes::Y;
        zcBase *= Primes::Z;
        wcBase *= Primes::W;

        for (int xi = 0; xi < 3; xi++) {
            float xcfOffset = xcf - x;
            float ycf = ycfBase;
            u32 yc = ycBase;
            for (int yi = 0; yi < 3; yi++) {
                float ycfOffset = ycf - y;
                float zcf = zcfBase;
                u32 zc = zcBase;
                for (int zi = 0; zi < 3; zi++) {
                    float zcfOffset = zcf - z;
                    float wcf = wcfBase;
                    u32 wc = wcBase;
                    for (int wi = 0; wi < 3; wi++) {
                        u32 hash = hashPrimesHB(seed, xc, yc, zc, wc);
                        float newDistance = cellularCellDistance(hash, jitter, xcfOffset, ycfOffset, zcfOffset, wcf - w);

                        distance0 = vmin(distance0, newDistance);

                        wcf += 1.0f;
                        wc += Primes::W;
                    }
                    zcf += 1.0f;
                    zc += Primes::Z;
                }
                ycf += 1.0f;
                yc += Primes::Y;
            }
            xcf += 1.0f;
            xc += Primes::X;
        }

        return cellularReturnF1(distance0);
    }

    enum Type : int { kPerlin = 0,
                      kSuperSimplex = 1,
                      kCellularF1 = 2 };

    struct FbmParams {
        int type;
        float frequency;
        int octaves;
        float lacunarity;
        float gain;
        float weightedStrength;
        float gridJitter;
    };

    FN2P_INLINE float source(const FbmParams& p, u32 seed, float x, float y, float z, float w)
    {
        switch (p.type) {
        case kPerlin:
            return perlin4(seed, x, y, z, w, p.frequency);
        case kSuperSimplex:
            return superSimplex4(seed, x, y, z, w, p.frequency);
        default:
            return cellularF1(seed, x, y, z, w, p.frequency, p.gridJitter);
        }
    }

    // FractalFBm::GenT over `source`. One octave renders the source alone, which is what FBm with one
    // octave computes (noise * 1).
    inline float fbm(const FbmParams& p, u32 seed, float x, float y, float z, float w)
    {
        if (p.octaves <= 1) {
            return source(p, seed, x, y, z, w);
        }
        float gain = p.gain;
        float weightedStrength = p.weightedStrength;
        float lacunarity = p.lacunarity;
        float amp = 1.0f;
        float noise = source(p, seed, x, y, z, w);

        float sum = noise * amp;

        for (int i = 1; i < p.octaves; i++) {
            seed -= u32(-1);
            amp *= lerp(1.0f, (noise + 1.0f) * 0.5f, weightedStrength);
            amp *= gain;

            noise = source(p, seed, x *= lacunarity, y *= lacunarity, z *= lacunarity, w *= lacunarity);
            sum += noise * amp;
        }

        return sum;
    }

} // namespace
} // namespace FN2P_NS
