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
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Native/NativeGpuParity.h"
#include "NativeParity.h"

#include "Engine/Nodes/Filter/EdgeAware/BoxFilter.h"
#include "Engine/Nodes/Filter/EdgeAware/Eigf.h"
#include "Engine/Nodes/Filter/EdgeAware/GuidedFilter.h"
#include "Engine/Nodes/Filter/EdgeAware/PoissonHeal.h"
#include "Engine/Nodes/Filter/EdgeAware/Wavelets.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/Nodes/Filter/EdgeAware/EdgeAwareGpu.h"
#endif

NATRON_NAMESPACE_USING

#ifdef NATRON_HAVE_GPU

namespace {
using namespace EdgeAware;

const ParallelFor kSerial = serialFor;

// ParityTolerance scales the relative bound by max(1, |reference|), so it is also an absolute bound
// of the same size near 0. That covers a device flushing denormals to zero (RADV, llvmpipe), which
// the denormal images exercise, without a special case.
const ParityTolerance kTolerance = ParityTolerance::make(0., 1e-4);
const ParityTolerance kHealTolerance = ParityTolerance::make(0., 1e-3);

// What an output buffer holds before a run, so a sample the device never writes shows up.
const float kUnwritten = 1e30f;

struct Size {
    int width;
    int height;
};

// A single pixel, single rows and columns, odd sizes, and sizes over a 16 x 16 workgroup.
const Size kSizes[] = {
    { 1, 1 },
    { 1, 23 },
    { 29, 1 },
    { 7, 5 },
    { 64, 48 },
    { 97, 61 },
};

enum ImageKind {
    eImageRandom,
    eImageSmooth,
    eImageNonFinite,
    eImageDenormal,
};

const char*
kindName(ImageKind kind)
{
    switch (kind) {
    case eImageRandom:
        return "random";
    case eImageSmooth:
        return "smooth";
    case eImageNonFinite:
        return "non-finite";
    case eImageDenormal:
        return "denormal";
    }

    return "";
}

// width x height x channels samples in [lo, hi]. Smooth images vary slowly, as the surface blurs
// expect; non-finite ones have NaN, +inf and -inf in every seventh sample; denormal ones have a
// subnormal in every third.
std::vector<float>
makeImage(int width,
          int height,
          int channels,
          unsigned seed,
          ImageKind kind,
          float lo = 0.f,
          float hi = 1.f)
{
    const std::size_t samples = (std::size_t)width * (std::size_t)height * (std::size_t)channels;
    std::vector<float> image(samples);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> dist(lo, hi);

    if (kind == eImageSmooth) {
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                for (int c = 0; c < channels; ++c) {
                    const float t = 0.5f + 0.3f * std::sin(0.21f * (float)x + 0.9f * (float)c) + 0.15f * std::cos(0.13f * (float)y + 0.4f * (float)c);
                    const float noise = 0.05f * (dist(rng) - lo) / (hi - lo);
                    image[((std::size_t)y * width + x) * channels + c] = lo + (hi - lo) * std::clamp(t + noise, 0.f, 1.f);
                }
            }
        }
    } else {
        for (float& v : image) {
            v = dist(rng);
        }
    }
    if (kind == eImageNonFinite) {
        const float specials[] = { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity() };
        for (std::size_t i = 0; i < samples; i += 7) {
            image[i] = specials[(i / 7) % 3];
        }
    } else if (kind == eImageDenormal) {
        for (std::size_t i = 0; i < samples; i += 3) {
            image[i] = std::numeric_limits<float>::denorm_min() * (float)(1 + (i % 1000));
        }
    }

    return image;
}

bool
transfer(gpu::GpuRuntime& runtime,
         gpu::GpuBuffer& buffer,
         std::vector<float>& data,
         bool toDevice)
{
    gpu::TimelineValue done;
    const gpu::HostRange range { data.data(), data.size() * sizeof(float) };
    gpu::GpuStatus s = toDevice ? runtime.upload(range, buffer, 0, done) : runtime.download(buffer, 0, range, done);
    if (s) {
        s = runtime.wait(done);
    }
    EXPECT_TRUE(bool(s)) << s.message;

    return bool(s);
}

// The buffers of one run, each in a slot of its own, and its recording.
class DeviceRun {
public:
    explicit DeviceRun(GpuParityRuntime& runtime)
        : _runtime(runtime)
    {
    }

    // A device buffer holding data; null after a failed expectation.
    gpu::GpuBuffer* upload(const std::vector<float>& data)
    {
        gpu::GpuBuffer* buffer = _runtime.buffer(_nextSlot++, std::max<std::size_t>(data.size(), 1));
        if (!buffer) {
            return nullptr;
        }
        std::vector<float> copy = data;
        if (copy.empty()) {
            copy.push_back(kUnwritten);
        }

        return transfer(_runtime.runtime(), *buffer, copy, true) ? buffer : nullptr;
    }

    // A device buffer of floats samples set to kUnwritten.
    gpu::GpuBuffer* output(std::size_t floats)
    {
        return upload(std::vector<float>(floats, kUnwritten));
    }

    // Records body into a new context, submits it and waits.
    bool record(const std::function<gpu::GpuStatus(gpu::GpuRecordContext&)>& body)
    {
        std::unique_ptr<gpu::GpuRecordContext> context;
        const gpu::GpuStatus created = _runtime.runtime().newRecordContext(context);
        EXPECT_TRUE(bool(created)) << created.message;
        if (!created) {
            return false;
        }
        const gpu::GpuStatus recorded = body(*context);
        EXPECT_TRUE(bool(recorded)) << recorded.message;
        if (!recorded) {
            return false;
        }
        const gpu::GpuStatus submitted = context->submitAndWait();
        EXPECT_TRUE(bool(submitted)) << submitted.message;

        return bool(submitted);
    }

    bool download(gpu::GpuBuffer* buffer,
                  std::size_t floats,
                  std::vector<float>& out)
    {
        out.assign(floats, 0.f);

        return buffer && transfer(_runtime.runtime(), *buffer, out, false);
    }

private:
    GpuParityRuntime& _runtime;
    int _nextSlot = 0;
};

void
expectMatch(const std::vector<float>& want,
            const std::vector<float>& got,
            const ParityTolerance& tolerance,
            const std::string& what)
{
    ASSERT_EQ(want.size(), got.size()) << what;
    const ImageDiff diff = compareImages(want, got, tolerance);
    EXPECT_EQ(0u, diff.mismatches)
        << what << ": " << diff.mismatches << " of " << diff.compared << " floats differ, worst " << diff.worstAbsolute
        << " at float " << diff.worstIndex << " (want " << want[diff.worstIndex] << ", got " << got[diff.worstIndex]
        << "), first mismatch at float " << diff.firstMismatchIndex;
}

std::string
sizeName(const Size& s)
{
    std::ostringstream name;
    name << s.width << "x" << s.height;

    return name.str();
}

// A single-channel mask: a disc of 1 in the middle, with a 0 and a NaN inside it.
std::vector<float>
discMask(int width,
         int height)
{
    std::vector<float> mask((std::size_t)width * (std::size_t)height, 0.f);
    const float cx = 0.5f * (float)width;
    const float cy = 0.5f * (float)height;
    const float r = 0.35f * (float)std::max(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float dx = (float)x + 0.5f - cx;
            const float dy = (float)y + 0.5f - cy;
            if (dx * dx + dy * dy <= r * r) {
                mask[(std::size_t)y * width + x] = 1.f;
            }
        }
    }
    if (mask.size() > 4) {
        mask[mask.size() / 2] = 0.f;
        mask[mask.size() / 2 + 1] = std::numeric_limits<float>::quiet_NaN();
    }

    return mask;
}

} // namespace

TEST(EdgeAwareGpu, BoxMean)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    unsigned seed = 1;
    for (const Size& s : kSizes) {
        for (int channels : { 1, 4, 9 }) {
            for (ImageKind kind : { eImageRandom, eImageNonFinite, eImageDenormal }) {
                for (int radius : { 0, 2, 200 }) {
                    for (int iterations : { 1, 3 }) {
                        for (BoxSummationEnum summation : { eBoxSummationPlain, eBoxSummationKahan }) {
                            const std::vector<float> input = makeImage(s.width, s.height, channels, seed++, kind);
                            std::vector<float> want = input;
                            boxMean(want.data(), s.width, s.height, channels, radius, iterations, summation, kSerial);
                            DeviceRun run(*runtime);
                            gpu::GpuBuffer* buf = run.upload(input);
                            ASSERT_TRUE(buf);
                            ASSERT_TRUE(run.record([&](gpu::GpuRecordContext& ctx) {
                                return recordBoxMean(ctx, *buf, s.width, s.height, channels, radius, iterations, summation);
                            }));
                            std::vector<float> got;
                            ASSERT_TRUE(run.download(buf, input.size(), got));
                            std::ostringstream what;
                            what << sizeName(s) << "x" << channels << " " << kindName(kind) << " radius " << radius << " x" << iterations
                                 << (summation == eBoxSummationKahan ? " kahan" : " plain");
                            expectMatch(want, got, kTolerance, what.str());
                        }
                    }
                }
            }
        }
    }
}

TEST(EdgeAwareGpu, BoxMeanVariance)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    unsigned seed = 100;
    for (const Size& s : kSizes) {
        for (ImageKind kind : { eImageRandom, eImageNonFinite, eImageDenormal }) {
            for (int radius : { 0, 3, 200 }) {
                const std::vector<float> input = makeImage(s.width, s.height, 1, seed++, kind);
                std::vector<float> wantMean(input.size());
                std::vector<float> wantVariance(input.size());
                boxMeanVariance(input.data(), s.width, s.height, radius, wantMean.data(), wantVariance.data(), kSerial);
                DeviceRun run(*runtime);
                gpu::GpuBuffer* in = run.upload(input);
                gpu::GpuBuffer* mean = run.output(input.size());
                gpu::GpuBuffer* variance = run.output(input.size());
                ASSERT_TRUE(in && mean && variance);
                ASSERT_TRUE(run.record([&](gpu::GpuRecordContext& ctx) {
                    return recordBoxMeanVariance(ctx, *in, s.width, s.height, radius, *mean, *variance);
                }));
                std::vector<float> gotMean;
                std::vector<float> gotVariance;
                ASSERT_TRUE(run.download(mean, input.size(), gotMean));
                ASSERT_TRUE(run.download(variance, input.size(), gotVariance));
                std::ostringstream what;
                what << sizeName(s) << " " << kindName(kind) << " radius " << radius;
                expectMatch(wantMean, gotMean, kTolerance, what.str() + " mean");
                expectMatch(wantVariance, gotVariance, kTolerance, what.str() + " variance");
            }
        }
    }
}

TEST(EdgeAwareGpu, GuidedFilter)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    unsigned seed = 200;
    for (const Size& s : kSizes) {
        for (int guideChannels : { 3, 4 }) {
            for (ImageKind kind : { eImageRandom, eImageNonFinite }) {
                for (int radius : { 1, 4, 60 }) {
                    for (float guideWeight : { 1.f, 0.5f }) {
                        const std::vector<float> guide = makeImage(s.width, s.height, guideChannels, seed++, kind);
                        const std::vector<float> input = makeImage(s.width, s.height, 1, seed++, kind);
                        const float sqrtEps = 0.1f;
                        std::vector<float> want(input.size());
                        guidedFilter(guide.data(), guideChannels, input.data(), want.data(), s.width, s.height, radius, sqrtEps, guideWeight, 0.f, 1.f, kSerial);
                        DeviceRun run(*runtime);
                        gpu::GpuBuffer* g = run.upload(guide);
                        gpu::GpuBuffer* in = run.upload(input);
                        gpu::GpuBuffer* out = run.output(input.size());
                        ASSERT_TRUE(g && in && out);
                        ASSERT_TRUE(run.record([&](gpu::GpuRecordContext& ctx) {
                            return recordGuidedFilter(ctx, *g, guideChannels, *in, *out, s.width, s.height, radius, sqrtEps, guideWeight, 0.f, 1.f);
                        }));
                        std::vector<float> got;
                        ASSERT_TRUE(run.download(out, input.size(), got));
                        std::ostringstream what;
                        what << sizeName(s) << " guide x" << guideChannels << " " << kindName(kind) << " radius " << radius << " weight " << guideWeight;
                        expectMatch(want, got, kTolerance, what.str());
                    }
                }
            }
        }
    }
}

TEST(EdgeAwareGpu, GuidedFilterGrey)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    unsigned seed = 300;
    for (const Size& s : kSizes) {
        for (ImageKind kind : { eImageSmooth, eImageNonFinite }) {
            for (int radius : { 1, 4, 30 }) {
                for (float eps : { 1e-4f, 1e-1f }) {
                    // 0 is guidedFilterGrey(); the others fastGuidedFilterGrey() at that subsampling.
                    for (int subsampling : { 0, 1, 2, 4 }) {
                        const std::vector<float> guide = makeImage(s.width, s.height, 1, seed++, kind);
                        const std::vector<float> target = makeImage(s.width, s.height, 1, seed++, kind);
                        std::vector<float> want(guide.size());
                        if (subsampling == 0) {
                            guidedFilterGrey(guide.data(), target.data(), want.data(), s.width, s.height, radius, eps, kSerial);
                        } else {
                            fastGuidedFilterGrey(guide.data(), target.data(), want.data(), s.width, s.height, radius, eps, subsampling, kSerial);
                        }
                        DeviceRun run(*runtime);
                        gpu::GpuBuffer* g = run.upload(guide);
                        gpu::GpuBuffer* t = run.upload(target);
                        gpu::GpuBuffer* out = run.output(guide.size());
                        ASSERT_TRUE(g && t && out);
                        ASSERT_TRUE(run.record([&](gpu::GpuRecordContext& ctx) {
                            if (subsampling == 0) {
                                return recordGuidedFilterGrey(ctx, *g, *t, *out, s.width, s.height, radius, eps);
                            }

                            return recordFastGuidedFilterGrey(ctx, *g, *t, *out, s.width, s.height, radius, eps, subsampling);
                        }));
                        std::vector<float> got;
                        ASSERT_TRUE(run.download(out, guide.size(), got));
                        std::ostringstream what;
                        what << sizeName(s) << " " << kindName(kind) << " radius " << radius << " eps " << eps << " subsampling " << subsampling;
                        expectMatch(want, got, kTolerance, what.str());
                    }
                }
            }
        }
    }
}

TEST(EdgeAwareGpu, InterpolateBilinear)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    struct Resize {
        Size in;
        Size out;
    };
    const Resize resizes[] = {
        { { 64, 48 }, { 16, 12 } },
        { { 16, 12 }, { 64, 48 } },
        { { 1, 1 }, { 5, 3 } },
        { { 7, 5 }, { 7, 5 } },
        { { 97, 61 }, { 24, 15 } },
        { { 1, 23 }, { 3, 5 } },
    };
    unsigned seed = 400;
    for (const Resize& r : resizes) {
        for (int channels : { 1, 2, 4 }) {
            const std::vector<float> input = makeImage(r.in.width, r.in.height, channels, seed++, eImageRandom);
            const std::size_t outFloats = (std::size_t)r.out.width * r.out.height * channels;
            std::vector<float> want(outFloats);
            interpolateBilinear(input.data(), r.in.width, r.in.height, want.data(), r.out.width, r.out.height, channels, kSerial);
            DeviceRun run(*runtime);
            gpu::GpuBuffer* in = run.upload(input);
            gpu::GpuBuffer* out = run.output(outFloats);
            ASSERT_TRUE(in && out);
            ASSERT_TRUE(run.record([&](gpu::GpuRecordContext& ctx) {
                return recordInterpolateBilinear(ctx, *in, r.in.width, r.in.height, *out, r.out.width, r.out.height, channels);
            }));
            std::vector<float> got;
            ASSERT_TRUE(run.download(out, outFloats, got));
            std::ostringstream what;
            what << sizeName(r.in) << " to " << sizeName(r.out) << " x" << channels;
            expectMatch(want, got, kTolerance, what.str());
        }
    }
}

// Samplings 0 and 1 are exact on the device, so they are compared exactly.
TEST(EdgeAwareGpu, Quantize)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    unsigned seed = 500;
    for (const Size& s : kSizes) {
        for (ImageKind kind : { eImageRandom, eImageNonFinite, eImageDenormal }) {
            for (float sampling : { 0.f, 1.f }) {
                std::vector<float> input = makeImage(s.width, s.height, 1, seed++, kind, -1.f, 4.f);
                if (input.size() > 3) {
                    input[1] = 0.f;
                    input[2] = -0.f;
                    input[3] = 2.f;
                }
                std::vector<float> want(input.size());
                quantize(input.data(), want.data(), input.size(), sampling, 0.01f, 2.5f, kSerial);
                DeviceRun run(*runtime);
                gpu::GpuBuffer* in = run.upload(input);
                gpu::GpuBuffer* out = run.output(input.size());
                ASSERT_TRUE(in && out);
                ASSERT_TRUE(run.record([&](gpu::GpuRecordContext& ctx) {
                    return recordQuantize(ctx, *in, *out, s.width, s.height, sampling, 0.01f, 2.5f);
                }));
                std::vector<float> got;
                ASSERT_TRUE(run.download(out, input.size(), got));
                std::ostringstream what;
                what << sizeName(s) << " " << kindName(kind) << " sampling " << sampling;
                expectMatch(want, got, ParityTolerance::exact(), what.str());
            }
        }
    }
}

TEST(EdgeAwareGpu, RecursiveGaussianBlur)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    unsigned seed = 600;
    for (const Size& s : kSizes) {
        for (int channels : { 1, 2, 4 }) {
            for (float sigma : { 0.8f, 4.f, 25.f, 1e9f }) {
                // The data's own range, then a narrower one that clamps.
                for (bool clamped : { false, true }) {
                    const std::vector<float> input = makeImage(s.width, s.height, channels, seed++, eImageRandom);
                    float min[4];
                    float max[4];
                    for (int c = 0; c < channels; ++c) {
                        min[c] = clamped ? 0.2f : 0.f;
                        max[c] = clamped ? 0.8f : 1.f;
                    }
                    std::vector<float> want(input.size());
                    recursiveGaussianBlur(input.data(), want.data(), s.width, s.height, channels, sigma, min, max, kSerial);
                    DeviceRun run(*runtime);
                    gpu::GpuBuffer* in = run.upload(input);
                    gpu::GpuBuffer* out = run.output(input.size());
                    ASSERT_TRUE(in && out);
                    ASSERT_TRUE(run.record([&](gpu::GpuRecordContext& ctx) {
                        return recordRecursiveGaussianBlur(ctx, *in, *out, s.width, s.height, channels, sigma, min, max);
                    }));
                    std::vector<float> got;
                    ASSERT_TRUE(run.download(out, input.size(), got));
                    std::ostringstream what;
                    what << sizeName(s) << "x" << channels << " sigma " << sigma << (clamped ? " clamped" : "");
                    expectMatch(want, got, kTolerance, what.str());
                }
            }
        }
    }
}

// Quantization 0 and 1 only: other samplings may put a sample on the other side of a step on the
// device, which a surface blur turns into a large local difference.
TEST(EdgeAwareGpu, FastSurfaceBlur)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    unsigned seed = 700;
    for (const Size& s : kSizes) {
        for (ImageKind kind : { eImageSmooth, eImageNonFinite }) {
            for (int radius : { 2, 12 }) {
                for (int iterations : { 0, 1, 3 }) {
                    for (GuidedBlendingEnum blending : { eGuidedBlendingLinear, eGuidedBlendingGeomean }) {
                        for (float quantization : { 0.f, 1.f }) {
                            const std::vector<float> input = makeImage(s.width, s.height, 1, seed++, kind, 0.05f, 0.95f);
                            const float feathering = 0.01f;
                            std::vector<float> want = input;
                            fastSurfaceBlur(want.data(), s.width, s.height, radius, feathering, iterations, blending, quantization, 0.01f, 1.f, kSerial);
                            DeviceRun run(*runtime);
                            gpu::GpuBuffer* image = run.upload(input);
                            ASSERT_TRUE(image);
                            ASSERT_TRUE(run.record([&](gpu::GpuRecordContext& ctx) {
                                return recordFastSurfaceBlur(ctx, *image, s.width, s.height, radius, feathering, iterations, blending, quantization, 0.01f, 1.f);
                            }));
                            std::vector<float> got;
                            ASSERT_TRUE(run.download(image, input.size(), got));
                            std::ostringstream what;
                            what << sizeName(s) << " " << kindName(kind) << " radius " << radius << " x" << iterations
                                 << (blending == eGuidedBlendingGeomean ? " geomean" : " linear") << " quantization " << quantization;
                            expectMatch(want, got, kTolerance, what.str());
                        }
                    }
                }
            }
        }
    }
}

TEST(EdgeAwareGpu, EigfSurfaceBlur)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    unsigned seed = 800;
    for (const Size& s : kSizes) {
        for (ImageKind kind : { eImageSmooth, eImageNonFinite }) {
            for (float sigma : { 1.f, 3.f, 10.f, 1e9f }) {
                for (int iterations : { 1, 2 }) {
                    for (GuidedBlendingEnum blending : { eGuidedBlendingLinear, eGuidedBlendingGeomean }) {
                        for (float quantization : { 0.f, 1.f }) {
                            const std::vector<float> input = makeImage(s.width, s.height, 1, seed++, kind, 0.05f, 0.95f);
                            const float feathering = 0.01f;
                            std::vector<float> want = input;
                            eigfSurfaceBlur(want.data(), s.width, s.height, sigma, feathering, iterations, blending, quantization, 0.01f, 1.f, kSerial);
                            DeviceRun run(*runtime);
                            gpu::GpuBuffer* image = run.upload(input);
                            ASSERT_TRUE(image);
                            ASSERT_TRUE(run.record([&](gpu::GpuRecordContext& ctx) {
                                return recordEigfSurfaceBlur(ctx, *image, s.width, s.height, sigma, feathering, iterations, blending, quantization, 0.01f, 1.f);
                            }));
                            std::vector<float> got;
                            ASSERT_TRUE(run.download(image, input.size(), got));
                            std::ostringstream what;
                            what << sizeName(s) << " " << kindName(kind) << " sigma " << sigma << " x" << iterations
                                 << (blending == eGuidedBlendingGeomean ? " geomean" : " linear") << " quantization " << quantization;
                            expectMatch(want, got, kTolerance, what.str());
                        }
                    }
                }
            }
        }
    }
}

// Both decompositions band by band, then the recomposition of the device's bands against the CPU's
// of its own.
TEST(EdgeAwareGpu, WaveletScales)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    unsigned seed = 900;
    for (const Size& s : kSizes) {
        const std::size_t samples = 4 * (std::size_t)s.width * (std::size_t)s.height;
        for (ImageKind kind : { eImageRandom, eImageNonFinite, eImageDenormal }) {
            for (bool bspline : { false, true }) {
                for (int scales : { 1, 3, 10 }) {
                    const std::vector<float> input = makeImage(s.width, s.height, 4, seed++, kind, -0.5f, 1.5f);
                    WaveletScales want;
                    int wantBands = scales;
                    if (bspline) {
                        bsplineDecomposeScales(input.data(), s.width, s.height, scales, want, kSerial);
                    } else {
                        wantBands = dwtDecomposeScales(input.data(), s.width, s.height, scales, want, kSerial);
                    }
                    std::vector<float> wantRecomposed(samples);
                    waveletRecompose(want, s.width, s.height, wantRecomposed.data(), kSerial);

                    DeviceRun run(*runtime);
                    gpu::GpuBuffer* in = run.upload(input);
                    gpu::GpuBuffer* residual = run.output(samples);
                    gpu::GpuBuffer* recomposed = run.output(samples);
                    ASSERT_TRUE(in && residual && recomposed);
                    std::vector<gpu::GpuBuffer*> bands;
                    std::vector<const gpu::GpuBuffer*> details;
                    for (int b = 0; b < wantBands; ++b) {
                        bands.push_back(run.output(samples));
                        ASSERT_TRUE(bands.back());
                        details.push_back(bands.back());
                    }
                    int gotBands = -1;
                    ASSERT_TRUE(run.record([&](gpu::GpuRecordContext& ctx) {
                        gpu::GpuStatus status = bspline ? recordBsplineDecomposeScales(ctx, *in, s.width, s.height, scales, details, *residual)
                                                        : recordDwtDecomposeScales(ctx, *in, s.width, s.height, scales, details, *residual, gotBands);
                        if (!status) {
                            return status;
                        }

                        return recordWaveletRecompose(ctx, details, *residual, s.width, s.height, *recomposed);
                    }));
                    std::ostringstream what;
                    what << sizeName(s) << " " << kindName(kind) << (bspline ? " b-spline" : " hat") << " scales " << scales;
                    if (!bspline) {
                        EXPECT_EQ(wantBands, gotBands) << what.str();
                    }
                    ASSERT_EQ((std::size_t)wantBands, want.details.size()) << what.str();
                    std::vector<float> got;
                    for (int b = 0; b < wantBands; ++b) {
                        ASSERT_TRUE(run.download(bands[b], samples, got));
                        expectMatch(want.details[b], got, kTolerance, what.str() + " band " + std::to_string(b));
                    }
                    ASSERT_TRUE(run.download(residual, samples, got));
                    expectMatch(want.residual, got, kTolerance, what.str() + " residual");
                    ASSERT_TRUE(run.download(recomposed, samples, got));
                    expectMatch(wantRecomposed, got, kTolerance, what.str() + " recomposed");
                }
            }
        }
    }
}

TEST(EdgeAwareGpu, Bspline)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    unsigned seed = 1000;
    for (const Size& s : kSizes) {
        const std::size_t samples = 4 * (std::size_t)s.width * (std::size_t)s.height;
        // A spacing past the image clamps every tap to the edge.
        for (int mult : { 1, 4, 300 }) {
            for (bool clip : { false, true }) {
                const std::vector<float> input = makeImage(s.width, s.height, 4, seed++, eImageRandom, -0.5f, 1.5f);
                std::vector<float> wantBlur(samples);
                blur2DBspline(input.data(), wantBlur.data(), s.width, s.height, mult, clip, kSerial);
                std::vector<float> wantHF(samples);
                std::vector<float> wantLF(samples);
                decompose2DBspline(input.data(), wantHF.data(), wantLF.data(), s.width, s.height, mult, kSerial);
                DeviceRun run(*runtime);
                gpu::GpuBuffer* in = run.upload(input);
                gpu::GpuBuffer* blur = run.output(samples);
                gpu::GpuBuffer* hf = run.output(samples);
                gpu::GpuBuffer* lf = run.output(samples);
                ASSERT_TRUE(in && blur && hf && lf);
                ASSERT_TRUE(run.record([&](gpu::GpuRecordContext& ctx) {
                    gpu::GpuStatus status = recordBlur2DBspline(ctx, *in, *blur, s.width, s.height, mult, clip);
                    if (!status) {
                        return status;
                    }

                    return recordDecompose2DBspline(ctx, *in, *hf, *lf, s.width, s.height, mult);
                }));
                std::ostringstream what;
                what << sizeName(s) << " mult " << mult << (clip ? " clipped" : "");
                std::vector<float> got;
                ASSERT_TRUE(run.download(blur, samples, got));
                expectMatch(wantBlur, got, kTolerance, what.str() + " blur");
                ASSERT_TRUE(run.download(hf, samples, got));
                expectMatch(wantHF, got, kTolerance, what.str() + " HF");
                ASSERT_TRUE(run.download(lf, samples, got));
                expectMatch(wantLF, got, kTolerance, what.str() + " LF");
            }
        }
    }
}

// A radius or tap spacing of the largest int must match the CPU, which clamps it to the image.
TEST(EdgeAwareGpu, HugeRadiusMatchesTheCpu)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const int huge = std::numeric_limits<int>::max();
    unsigned seed = 1100;
    for (const Size& s : kSizes) {
        const std::size_t pixels = (std::size_t)s.width * (std::size_t)s.height;
        const std::vector<float> box = makeImage(s.width, s.height, 4, seed++, eImageRandom);
        const std::vector<float> colourGuide = makeImage(s.width, s.height, 3, seed++, eImageRandom);
        const std::vector<float> guide = makeImage(s.width, s.height, 1, seed++, eImageSmooth);
        const std::vector<float> target = makeImage(s.width, s.height, 1, seed++, eImageSmooth);
        std::vector<float> wantBox = box;
        boxMean(wantBox.data(), s.width, s.height, 4, huge, 2, eBoxSummationKahan, kSerial);
        std::vector<float> wantColour(pixels);
        guidedFilter(colourGuide.data(), 3, target.data(), wantColour.data(), s.width, s.height, huge, 0.1f, 1.f, 0.f, 1.f, kSerial);
        std::vector<float> wantFast(pixels);
        fastGuidedFilterGrey(guide.data(), target.data(), wantFast.data(), s.width, s.height, huge, 1e-2f, kFastGuidedSubsampling, kSerial);
        std::vector<float> wantBlur(4 * pixels);
        blur2DBspline(box.data(), wantBlur.data(), s.width, s.height, huge, true, kSerial);

        DeviceRun run(*runtime);
        gpu::GpuBuffer* boxBuf = run.upload(box);
        gpu::GpuBuffer* colourBuf = run.upload(colourGuide);
        gpu::GpuBuffer* guideBuf = run.upload(guide);
        gpu::GpuBuffer* targetBuf = run.upload(target);
        gpu::GpuBuffer* colourOut = run.output(pixels);
        gpu::GpuBuffer* fastOut = run.output(pixels);
        gpu::GpuBuffer* blurOut = run.output(4 * pixels);
        ASSERT_TRUE(boxBuf && colourBuf && guideBuf && targetBuf && colourOut && fastOut && blurOut);
        ASSERT_TRUE(run.record([&](gpu::GpuRecordContext& ctx) {
            gpu::GpuStatus status = recordBlur2DBspline(ctx, *boxBuf, *blurOut, s.width, s.height, huge, true);
            if (status) {
                status = recordGuidedFilter(ctx, *colourBuf, 3, *targetBuf, *colourOut, s.width, s.height, huge, 0.1f, 1.f, 0.f, 1.f);
            }
            if (status) {
                status = recordFastGuidedFilterGrey(ctx, *guideBuf, *targetBuf, *fastOut, s.width, s.height, huge, 1e-2f, kFastGuidedSubsampling);
            }
            if (status) {
                ctx.computeBarrier();
                status = recordBoxMean(ctx, *boxBuf, s.width, s.height, 4, huge, 2, eBoxSummationKahan);
            }

            return status;
        }));
        std::vector<float> got;
        ASSERT_TRUE(run.download(boxBuf, box.size(), got));
        expectMatch(wantBox, got, kTolerance, sizeName(s) + " box mean");
        ASSERT_TRUE(run.download(colourOut, pixels, got));
        expectMatch(wantColour, got, kTolerance, sizeName(s) + " colour guided");
        ASSERT_TRUE(run.download(fastOut, pixels, got));
        expectMatch(wantFast, got, kTolerance, sizeName(s) + " fast guided");
        ASSERT_TRUE(run.download(blurOut, 4 * pixels, got));
        expectMatch(wantBlur, got, kTolerance, sizeName(s) + " B-spline blur");
    }
}

TEST(EdgeAwareGpu, FastGuidedFilterBelowTwoSubsamplingIsThePlainOne)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    unsigned seed = 1200;
    for (const Size& s : kSizes) {
        const std::vector<float> guide = makeImage(s.width, s.height, 1, seed++, eImageSmooth);
        const std::vector<float> target = makeImage(s.width, s.height, 1, seed++, eImageSmooth);
        std::vector<float> want(guide.size());
        guidedFilterGrey(guide.data(), target.data(), want.data(), s.width, s.height, 3, 1e-2f, kSerial);
        for (int subsampling : { -3, 0, 1 }) {
            DeviceRun run(*runtime);
            gpu::GpuBuffer* g = run.upload(guide);
            gpu::GpuBuffer* t = run.upload(target);
            gpu::GpuBuffer* out = run.output(guide.size());
            ASSERT_TRUE(g && t && out);
            ASSERT_TRUE(run.record([&](gpu::GpuRecordContext& ctx) {
                return recordFastGuidedFilterGrey(ctx, *g, *t, *out, s.width, s.height, 3, 1e-2f, subsampling);
            }));
            std::vector<float> got;
            ASSERT_TRUE(run.download(out, guide.size(), got));
            std::ostringstream what;
            what << sizeName(s) << " subsampling " << subsampling;
            expectMatch(want, got, kTolerance, what.str());
        }
    }
}

// The error sum runs in another order on the device, so an iteration count near convergence may
// differ by one; the tolerance is wider for it.
TEST(EdgeAwareGpu, PoissonHeal)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    unsigned seed = 1100;
    for (const Size& s : kSizes) {
        for (ImageKind kind : { eImageRandom, eImageNonFinite }) {
            for (int maxIter : { 1, 40, 300 }) {
                const std::vector<float> src = makeImage(s.width, s.height, 4, seed++, eImageRandom);
                const std::vector<float> dest = makeImage(s.width, s.height, 4, seed++, kind);
                const std::vector<float> mask = discMask(s.width, s.height);
                std::vector<float> want = dest;
                poissonHeal(src.data(), want.data(), mask.data(), s.width, s.height, maxIter, kSerial);
                DeviceRun run(*runtime);
                gpu::GpuBuffer* srcBuffer = run.upload(src);
                gpu::GpuBuffer* destBuffer = run.upload(dest);
                gpu::GpuBuffer* maskBuffer = run.upload(mask);
                ASSERT_TRUE(srcBuffer && destBuffer && maskBuffer);
                ASSERT_TRUE(run.record([&](gpu::GpuRecordContext& ctx) {
                    return recordPoissonHeal(ctx, *srcBuffer, *destBuffer, *maskBuffer, s.width, s.height, maxIter);
                }));
                std::vector<float> got;
                ASSERT_TRUE(run.download(destBuffer, dest.size(), got));
                std::ostringstream what;
                what << sizeName(s) << " " << kindName(kind) << " dest, " << maxIter << " iterations";
                expectMatch(want, got, kHealTolerance, what.str());
            }
        }
    }
}

// Without push descriptors each dispatch takes a descriptor set from the kernel's pool, which a long
// heal would exhaust, so recording refuses it.
TEST(EdgeAwareGpu, PoissonHealRefusesLongRunsWithoutPushDescriptors)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    std::unique_ptr<gpu::GpuRecordContext> context;
    const gpu::GpuStatus created = runtime->runtime().newRecordContext(context);
    ASSERT_TRUE(bool(created)) << created.message;
    if (context->device().info().pushDescriptor) {
        return;
    }
    const std::vector<float> image = makeImage(8, 8, 4, 1200, eImageRandom);
    DeviceRun run(*runtime);
    gpu::GpuBuffer* src = run.upload(image);
    gpu::GpuBuffer* dest = run.upload(image);
    gpu::GpuBuffer* mask = run.upload(discMask(8, 8));
    ASSERT_TRUE(src && dest && mask);
    const gpu::GpuStatus recorded = recordPoissonHeal(*context, *src, *dest, *mask, 8, 8, kPoissonHealGpuMaxIterationsWithoutPushDescriptors + 1);
    EXPECT_FALSE(bool(recorded));
}

#else // NATRON_HAVE_GPU

TEST(EdgeAwareGpu, BoxMean)
{
    reportNoGpu("built without GPU support");
}

#endif // NATRON_HAVE_GPU
