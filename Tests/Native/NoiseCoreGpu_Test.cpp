/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 * (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-2.0.html>
 * ***** END LICENSE BLOCK ***** */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <type_traits>
#include <vector>

#include <gtest/gtest.h>

#include "Native/NativeGpuParity.h"
#include "NativeParity.h"

#include "Engine/Nodes/Generator/NoiseCore.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuRuntime.h"
#endif

NATRON_NAMESPACE_USING

#ifdef NATRON_HAVE_GPU

using noise4d::NoiseCoord;
using NoiseCore::NoiseParams;

namespace {

// noisecore_test.slang's NoiseSampleParams.
struct NoiseSampleParams {
    NoiseParams noise;
    uint32_t count;
};
static_assert(std::is_trivially_copyable<NoiseSampleParams>::value, "pushed to the GPU");
static_assert(sizeof(NoiseSampleParams) == 60, "must match noisecore_test.slang");

// How a configuration's GPU result must match the CPU's.
enum class Match {
    Exact, // bit for bit
    Near, // within kNear
    NearUpToTies, // within kNear except where the two nearest feature points tie
};

constexpr float kNear = 1e-5f;
// F2 - F1 below this (in output units) counts as a tie: the GPU's rounding of the distances may
// pick the other point as the nearest.
constexpr float kTie = 1e-4f;

struct Case {
    std::string name;
    NoiseParams params;
    Match match;
};

NoiseParams
baseParams(int type)
{
    NoiseParams p = NoiseCore::defaultNoiseParams();
    p.type = type;
    p.seed = 0xC0FFEEu + (uint32_t)type;
    p.jitter = 0.85f;
    p.smoothness = 0.25f;

    return p;
}

std::vector<Case>
cases()
{
    std::vector<Case> out;
    out.push_back({ "perlin", baseParams(NoiseCore::kNoiseTypePerlin), Match::Near });
    out.push_back({ "simplex", baseParams(NoiseCore::kNoiseTypeSimplex), Match::Near });
    out.push_back({ "value", baseParams(NoiseCore::kNoiseTypeValue), Match::Near });
    out.push_back({ "white", baseParams(NoiseCore::kNoiseTypeWhite), Match::Exact });
    for (int metric = 0; metric < 3; ++metric) {
        for (int output = 0; output < 6; ++output) {
            NoiseParams p = baseParams(NoiseCore::kNoiseTypeCellular);
            p.cellularMetric = metric;
            p.cellularOutput = output;
            out.push_back({ "cellular metric " + std::to_string(metric) + " output " + std::to_string(output), p,
                            Match::NearUpToTies });
        }
    }
    for (int fractal = NoiseCore::kNoiseFractalFBm; fractal <= NoiseCore::kNoiseFractalHybrid; ++fractal) {
        for (int type : { NoiseCore::kNoiseTypePerlin, NoiseCore::kNoiseTypeSimplex, NoiseCore::kNoiseTypeValue }) {
            NoiseParams p = baseParams(type);
            p.fractal = fractal;
            p.octaves = 7;
            p.lacunarity = 2.07f;
            p.gain = 0.55f;
            p.weightedStrength = 0.3f;
            out.push_back({ "fractal " + std::to_string(fractal) + " over type " + std::to_string(type), p, Match::Near });
        }
    }
    for (int warp : { NoiseCore::kNoiseWarpPerlin, NoiseCore::kNoiseWarpSimplex }) {
        NoiseParams p = baseParams(NoiseCore::kNoiseTypeSimplex);
        p.fractal = NoiseCore::kNoiseFractalFBm;
        p.octaves = 4;
        p.warpType = warp;
        p.warpAmount = 0.75f;
        p.warpFrequency = 0.43f;
        out.push_back({ "warp " + std::to_string(warp), p, Match::Near });
    }

    return out;
}

// Random 4D points, a third of them far from the origin on three axes.
std::vector<NoiseCoord>
randomPoints(std::size_t count)
{
    std::mt19937 rng(20261025u);
    std::uniform_real_distribution<float> fraction(0.f, 1.f);
    std::vector<NoiseCoord> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const bool far = (i % 3) == 0;
        NoiseCoord p;
        p.cx = far ? (uint32_t)rng() : (uint32_t)((int)(rng() % 512) - 256);
        p.cy = far ? (uint32_t)rng() : (uint32_t)((int)(rng() % 512) - 256);
        p.cz = (uint32_t)((int)(rng() % 32) - 16);
        p.cw = far ? (uint32_t)rng() : (uint32_t)(rng() % 4);
        p.fx = fraction(rng);
        p.fy = fraction(rng);
        p.fz = fraction(rng);
        p.fw = fraction(rng);
        // uniform_real_distribution may return its upper bound in float.
        p.fx = p.fx < 1.f ? p.fx : 0.f;
        p.fy = p.fy < 1.f ? p.fy : 0.f;
        p.fz = p.fz < 1.f ? p.fz : 0.f;
        p.fw = p.fw < 1.f ? p.fw : 0.f;
        out.push_back(p);
    }

    return out;
}

bool
transfer(gpu::GpuRuntime& runtime,
         gpu::GpuBuffer& buffer,
         void* data,
         std::size_t bytes,
         bool toDevice)
{
    gpu::TimelineValue done;
    const gpu::HostRange range { data, bytes };
    gpu::GpuStatus s = toDevice ? runtime.upload(range, buffer, 0, done) : runtime.download(buffer, 0, range, done);
    if (s) {
        s = runtime.wait(done);
    }
    EXPECT_TRUE(bool(s)) << s.message;

    return bool(s);
}

// Dispatches noisecore_sample over the points already in `points` and downloads the values.
bool
deviceRun(GpuParityRuntime& runtime,
          gpu::GpuBuffer& points,
          const NoiseParams& params,
          uint32_t count,
          std::vector<float>& output)
{
    gpu::GpuRuntime& vk = runtime.runtime();
    gpu::GpuBuffer* dst = runtime.buffer(1, count);
    if (!dst) {
        return false;
    }
    std::unique_ptr<gpu::GpuRecordContext> context;
    const gpu::GpuStatus created = vk.newRecordContext(context);
    EXPECT_TRUE(bool(created)) << created.message;
    if (!created) {
        return false;
    }
    NoiseSampleParams push;
    std::memset(&push, 0, sizeof(push));
    push.noise = params;
    push.count = count;
    const gpu::GpuStatus dispatched = context->dispatch(gpu::GpuKernelId::noisecore_sample, { &points, dst }, push, { count, 1u, 1u });
    EXPECT_TRUE(bool(dispatched)) << dispatched.message;
    if (!dispatched) {
        return false;
    }
    const gpu::GpuStatus submitted = context->submitAndWait();
    EXPECT_TRUE(bool(submitted)) << submitted.message;
    if (!submitted) {
        return false;
    }
    output.assign(count, 0.f);

    return transfer(vk, *dst, output.data(), output.size() * sizeof(float), false);
}

} // namespace

// The GPU kernel against the C++ core on random 4D points, for every basis, cellular metric and
// output, fractal and warp: white bit for bit, the rest within 1e-5, and cellular outputs that
// depend on which point is nearest (cell value, distance to edge) only where two points tie.
TEST(NoiseCoreGpu, MatchesCpu)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    gpu::GpuRuntime& vk = runtime->runtime();

    const std::vector<NoiseCoord> points = randomPoints(4096);
    const uint32_t count = (uint32_t)points.size();
    static_assert(sizeof(NoiseCoord) == 8 * sizeof(uint32_t), "noisecore_sample reads eight words per point");
    std::vector<uint32_t> words(points.size() * 8);
    std::memcpy(words.data(), points.data(), words.size() * sizeof(uint32_t));
    gpu::GpuBuffer* pointBuffer = runtime->buffer(0, words.size());
    ASSERT_TRUE(pointBuffer != nullptr);
    ASSERT_TRUE(transfer(vk, *pointBuffer, words.data(), words.size() * sizeof(uint32_t), true));

    for (const Case& c : cases()) {
        std::vector<float> got;
        ASSERT_TRUE(deviceRun(*runtime, *pointBuffer, c.params, count, got)) << c.name;

        NoiseParams gapParams = c.params;
        gapParams.cellularOutput = NoiseCore::kNoiseCellularF2MinusF1;
        std::size_t mismatches = 0;
        std::size_t ties = 0;
        float worst = 0.f;
        for (std::size_t i = 0; i < points.size(); ++i) {
            const float expected = NoiseCore::noise4(c.params, points[i]);
            const float diff = std::fabs(expected - got[i]);
            worst = std::max(worst, diff);
            bool ok;
            if (c.match == Match::Exact) {
                ok = std::memcmp(&expected, &got[i], sizeof(float)) == 0;
            } else {
                ok = diff <= kNear;
            }
            if (!ok && (c.match == Match::NearUpToTies)) {
                // The gap output is gap * scale - 1.
                const float gap = NoiseCore::noise4(gapParams, points[i]) + 1.f;
                if (gap < kTie) {
                    ok = true;
                    ++ties;
                }
            }
            if (!ok) {
                if (mismatches < 5) {
                    ADD_FAILURE() << c.name << " point " << i << ": cpu " << expected << " gpu " << got[i];
                }
                ++mismatches;
            }
        }
        EXPECT_EQ(0u, mismatches) << c.name << ", worst difference " << worst;
        // Ties are rare; many would mean a real divergence hidden by the allowance.
        EXPECT_LT(ties, points.size() / 100) << c.name;
    }
}

#else // NATRON_HAVE_GPU

TEST(NoiseCoreGpu, MatchesCpu)
{
    reportNoGpu("built without GPU support");
}

#endif // NATRON_HAVE_GPU
