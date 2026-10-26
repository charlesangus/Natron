// Row loops over the plain C++ FastNoise2 port. Built once per variant; the build defines
// FN2P_NS, FN2P_FUSED, FN2P_VARIANT (the exported object) and FN2P_NEEDS_V3.
//
// The row loop is structure-of-arrays so the compiler can vectorise across pixels: positions are
// processed in fixed-size chunks, the FBm octave loop and the cellular 3x3x3x3 cell loop run outside
// the pixel loop, and every pixel loop body is straight-line code. Chunk loops have a constant trip
// count and restrict pointers because -O2 uses GCC's very-cheap vectoriser cost model, which
// rejects loops needing a scalar epilogue or alias checks; a short last chunk is computed in full
// and only its first pixels are copied out. The result is bit-identical to calling
// fbm() per pixel (rowScalar), which the harness checks: the cell loop uses base + offset where
// FastNoise2 accumulates, and both are exact for cell indices below 2^24.

#include "fn2port_rows.h"

#include "fn2port4d.h"

#include <algorithm>

namespace FN2P_NS {
namespace {

    constexpr int kChunk = 256;

    [[gnu::noinline]] void perlinChunk(u32 seed, const float* __restrict px, float y, float z, float w, float frequency, float* __restrict out)
    {
        for (int i = 0; i < kChunk; i++) {
            out[i] = perlin4(seed, px[i], y, z, w, frequency);
        }
    }

    [[gnu::noinline]] void superSimplexChunk(u32 seed, const float* __restrict px, float y, float z, float w, float frequency, float* __restrict out)
    {
        for (int i = 0; i < kChunk; i++) {
            out[i] = superSimplex4(seed, px[i], y, z, w, frequency);
        }
    }

    // cellularF1 with the y, z, w terms hoisted (they are uniform across a row) and the cell loop
    // outside the pixel loop.
    [[gnu::noinline]] void cellularChunk(u32 seed, const float* __restrict px, float y, float z, float w, float frequency, float gridJitter,
                                         float* __restrict out)
    {
        float jitter = kJitter4D * gridJitter;

        alignas(64) float xs[kChunk];
        alignas(64) float xcfBase[kChunk];
        alignas(64) u32 xcBase[kChunk];
        alignas(64) float distance0[kChunk];

        for (int i = 0; i < kChunk; i++) {
            float x = px[i] * frequency;
            u32 xc = toInt(x) + u32(-1);
            xs[i] = x;
            xcfBase[i] = toFloat(xc);
            xcBase[i] = xc * Primes::X;
            distance0[i] = kInfinity;
        }

        y *= frequency;
        z *= frequency;
        w *= frequency;

        u32 ycBase = toInt(y) + u32(-1);
        u32 zcBase = toInt(z) + u32(-1);
        u32 wcBase = toInt(w) + u32(-1);

        float ycfBase = toFloat(ycBase);
        float zcfBase = toFloat(zcBase);
        float wcfBase = toFloat(wcBase);

        ycBase *= Primes::Y;
        zcBase *= Primes::Z;
        wcBase *= Primes::W;

        for (int xi = 0; xi < 3; xi++) {
            const float xcfStep = float(xi);
            const u32 xcStep = u32(xi) * Primes::X;
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
                        const float wcfOffset = wcf - w;
                        for (int i = 0; i < kChunk; i++) {
                            u32 hash = hashPrimesHB(seed, xcBase[i] + xcStep, yc, zc, wc);
                            float xcfOffset = (xcfBase[i] + xcfStep) - xs[i];
                            float newDistance = cellularCellDistance(hash, jitter, xcfOffset, ycfOffset, zcfOffset, wcfOffset);
                            distance0[i] = vmin(distance0[i], newDistance);
                        }
                        wcf += 1.0f;
                        wc += Primes::W;
                    }
                    zcf += 1.0f;
                    zc += Primes::Z;
                }
                ycf += 1.0f;
                yc += Primes::Y;
            }
        }

        for (int i = 0; i < kChunk; i++) {
            out[i] = cellularReturnF1(distance0[i]);
        }
    }

    void sourceChunk(const FbmParams& p, u32 seed, const float* __restrict px, float y, float z, float w, float* __restrict out)
    {
        switch (p.type) {
        case kPerlin:
            perlinChunk(seed, px, y, z, w, p.frequency, out);
            break;
        case kSuperSimplex:
            superSimplexChunk(seed, px, y, z, w, p.frequency, out);
            break;
        default:
            cellularChunk(seed, px, y, z, w, p.frequency, p.gridJitter, out);
            break;
        }
    }

    // fbm() over a chunk; px is scaled in place by the lacunarity.
    void fbmChunk(const FbmParams& p, u32 seed, float* __restrict px, float y, float z, float w, float* __restrict out)
    {
        if (p.octaves <= 1) {
            sourceChunk(p, seed, px, y, z, w, out);
            return;
        }

        alignas(64) float noise[kChunk];
        alignas(64) float amp[kChunk];

        sourceChunk(p, seed, px, y, z, w, noise);
        for (int i = 0; i < kChunk; i++) {
            amp[i] = 1.0f;
            out[i] = noise[i] * amp[i];
        }

        const float gain = p.gain;
        const float weightedStrength = p.weightedStrength;
        const float lacunarity = p.lacunarity;
        for (int octave = 1; octave < p.octaves; octave++) {
            seed -= u32(-1);
            for (int i = 0; i < kChunk; i++) {
                float a = amp[i] * lerp(1.0f, (noise[i] + 1.0f) * 0.5f, weightedStrength);
                amp[i] = a * gain;
                px[i] *= lacunarity;
            }
            y *= lacunarity;
            z *= lacunarity;
            w *= lacunarity;

            sourceChunk(p, seed, px, y, z, w, noise);
            for (int i = 0; i < kChunk; i++) {
                out[i] += noise[i] * amp[i];
            }
        }
    }

    FbmParams toFbm(const Fn2PortParams& p)
    {
        return FbmParams { p.type, p.frequency, p.octaves, p.lacunarity, p.gain, p.weightedStrength, p.gridJitter };
    }

    void row(const Fn2PortParams& p, int y, float* out)
    {
        const FbmParams fp = toFbm(p);
        const float py = p.originY + (float(y) + 0.5f);
        alignas(64) float px[kChunk];
        alignas(64) float result[kChunk];
        for (int x0 = 0; x0 < p.width; x0 += kChunk) {
            const int n = std::min(kChunk, p.width - x0);
            for (int i = 0; i < kChunk; i++) {
                px[i] = p.originX + (float(x0 + i) + 0.5f);
            }
            fbmChunk(fp, p.seed, px, py, p.z, p.w, result);
            std::copy(result, result + n, out + x0);
        }
    }

    void rowScalar(const Fn2PortParams& p, int y, float* out)
    {
        const FbmParams fp = toFbm(p);
        const float py = p.originY + (float(y) + 0.5f);
        for (int x = 0; x < p.width; x++) {
            out[x] = fbm(fp, p.seed, p.originX + (float(x) + 0.5f), py, p.z, p.w);
        }
    }

} // namespace
} // namespace FN2P_NS

#define FN2P_STR2(x) #x
#define FN2P_STR(x) FN2P_STR2(x)

extern const Fn2PortVariant FN2P_VARIANT = { FN2P_STR(FN2P_VARIANT), FN2P_NEEDS_V3 != 0, &FN2P_NS::row,
                                             &FN2P_NS::rowScalar };
