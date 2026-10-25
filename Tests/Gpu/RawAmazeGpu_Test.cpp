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
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Tests/Native/RawGpuTestSupport.h"

#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Raw/RawKernels.h"
#include "Engine/Nodes/Raw/RawNodeSupport.h"
#include "Engine/Nodes/Raw/RawPrepare.h"
#include "Engine/RectI.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuBuffer.h"
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/Nodes/Raw/RawAmazeGpu.h"
#endif

NATRON_NAMESPACE_USING

namespace {
#ifdef NATRON_HAVE_GPU
// Provisional, until the node's GPU path is measured on more raws. AMaZE takes a branch on many
// comparisons of quotients: a colour ratio against arthresh, two variances, the Nyquist test, and
// whether a neighbourhood's hvwt or pmwt is further from 0.5. Vulkan rounds a division within 2.5
// ulp where the CPU rounds it exactly, so a photosite whose quotient sits on a threshold can take
// the other branch, and the in-place hvwt and pmwt refinements carry that flip to the photosites
// after it. Everywhere else the two agree to a few ulp.
const double kAmazeMeanBound = 1e-4;
const double kAmazeMaxBound = 1e-4;

// One pixel a thousand, rounded up.
int
outlierBudget(int width,
              int height)
{
    return (int)(((std::int64_t)width * height + 999) / 1000);
}

// A width x height mosaic, top-down, of a smooth gradient, stripes at the Nyquist frequency of each
// colour, a patch over the clip point and a little noise, seen through filters.
std::vector<float>
syntheticMosaic(int width,
                int height,
                std::uint32_t filters)
{
    std::vector<float> mosaic((std::size_t)width * height);

    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            const int colour = RawKernels::fcBayer((std::size_t)row, (std::size_t)col, filters);
            float rgb[3] = {
                0.2f + 0.5f * (float)col / (float)width,
                0.3f + 0.4f * std::sin((float)row * 0.11f) * std::cos((float)col * 0.07f),
                0.6f - 0.4f * (float)row / (float)height,
            };
            if ((col > width / 3) && (col < (2 * width) / 3) && (row > height / 4) && (row < height / 2)) {
                const float stripe = ((col / 2 + row / 2) & 1) ? 0.85f : 0.1f;
                rgb[0] = stripe;
                rgb[1] = 0.95f - stripe;
                rgb[2] = stripe;
            }
            if ((col > (3 * width) / 4) && (row > (2 * height) / 3)) {
                rgb[0] = 1.2f;
                rgb[1] = 1.15f;
                rgb[2] = 0.9f;
            }
            const std::uint32_t hash = (std::uint32_t)(row * 73856093) ^ (std::uint32_t)(col * 19349663);
            mosaic[(std::size_t)row * width + col] = std::max(0.f, rgb[colour] + 0.01f * (float)(hash % 101) / 100.f);
        }
    }

    return mosaic;
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

/**
 * @brief recordAmazeGpu() of the width x height top-down mosaic, as RGBA pixels top-down, or
 * empty after a failed expectation. The source is uploaded bottom-up, comps floats a photosite,
 * as a node's input arrives; batch tiles share the scratch.
 **/
std::vector<float>
amazeOnGpu(GpuParityRuntime& runtime,
           const std::vector<float>& mosaic,
           int width,
           int height,
           std::uint32_t filters,
           float clipPt,
           std::uint32_t batch)
{
    const std::size_t nPhotosites = (std::size_t)width * height;
    std::vector<float> source(nPhotosites);
    for (int row = 0; row < height; ++row) {
        std::copy_n(mosaic.data() + (std::size_t)row * width, width, source.data() + (std::size_t)(height - 1 - row) * width);
    }
    batch = std::min<std::uint32_t>(batch, amazeGpuBatch(batch));
    gpu::GpuBuffer* src = runtime.buffer(0, nPhotosites);
    gpu::GpuBuffer* plane = runtime.buffer(1, 4 * nPhotosites);
    gpu::GpuBuffer* tiles = runtime.buffer(2, batch * kAmazeGpuTileFloats);
    gpu::GpuBuffer* dst = runtime.buffer(3, 4 * nPhotosites);
    if (!src || !plane || !tiles || !dst || !transfer(runtime.runtime(), *src, source, true)) {
        return std::vector<float>();
    }
    RawAmazeGpuPush push = {};
    push.filters = filters;
    push.clipPt = clipPt;
    push.width = (std::uint32_t)width;
    push.height = (std::uint32_t)height;
    push.srcStride = (std::uint32_t)width;
    push.srcComps = 1;
    push.srcCol2 = width;
    push.srcRow2 = height;
    push.dstStride = (std::uint32_t)width * 4;
    push.outRow = height - 1;
    push.outWidth = (std::uint32_t)width;
    push.outHeight = (std::uint32_t)height;
    std::unique_ptr<gpu::GpuRecordContext> ctx;
    gpu::GpuStatus s = runtime.runtime().newRecordContext(ctx);
    EXPECT_TRUE(bool(s)) << s.message;
    if (!s) {
        return std::vector<float>();
    }
    EXPECT_TRUE(recordAmazeGpu(*ctx, push, src, plane, tiles, batch, dst));
    s = ctx->submitAndWait();
    EXPECT_TRUE(bool(s)) << s.message;
    ctx.reset();
    if (!s) {
        return std::vector<float>();
    }
    std::vector<float> bottomUp(4 * nPhotosites);
    if (!transfer(runtime.runtime(), *dst, bottomUp, false)) {
        return std::vector<float>();
    }
    std::vector<float> pixels(4 * nPhotosites);
    for (int row = 0; row < height; ++row) {
        std::copy_n(bottomUp.data() + (std::size_t)(height - 1 - row) * width * 4, (std::size_t)width * 4, pixels.data() + (std::size_t)row * width * 4);
    }

    return pixels;
}

/**
 * @brief Expects recordAmazeGpu() to match RawKernels::amaze() of the mosaic: the mean error of
 * the colour channels within kAmazeMeanBound of the CPU's mean level, and at most outlierBudget()
 * pixels with a channel off by more than kAmazeMaxBound of it.
 **/
void
expectAmazeMatchesCpu(GpuParityRuntime& runtime,
                      const std::vector<float>& mosaic,
                      int width,
                      int height,
                      std::uint32_t filters,
                      float clipPt,
                      std::uint32_t batch,
                      const std::string& what)
{
    const std::size_t nPhotosites = (std::size_t)width * height;
    std::vector<float> cpu(4 * nPhotosites, 0.f);
    RawKernels::amaze(mosaic.data(), cpu.data(), width, height, filters, clipPt, RawKernels::serialFor);
    const std::vector<float> gpu = amazeOnGpu(runtime, mosaic, width, height, filters, clipPt, batch);
    ASSERT_EQ(cpu.size(), gpu.size()) << what;

    double sum = 0.;
    double level = 0.;
    double worst = 0.;
    std::size_t worstPixel = 0;
    std::size_t nonFinite = 0;
    std::vector<double> pixelErrors(nPhotosites, 0.);
    for (std::size_t p = 0; p < nPhotosites; ++p) {
        for (std::size_t c = 0; c < 3; ++c) {
            const float a = cpu[p * 4 + c];
            const float b = gpu[p * 4 + c];
            const double error = (a == b) ? 0. : std::fabs((double)a - (double)b);
            if (!std::isfinite(error)) {
                ++nonFinite;
                continue;
            }
            sum += error;
            level += std::fabs((double)a);
            pixelErrors[p] = std::max(pixelErrors[p], error);
        }
        if (pixelErrors[p] > worst) {
            worst = pixelErrors[p];
            worstPixel = p;
        }
    }
    const double count = 3. * (double)nPhotosites;
    const double meanError = sum / count;
    level /= count;
    ASSERT_GT(level, 0.) << what << ": the CPU render is black";
    const int outliers = (int)std::count_if(pixelErrors.begin(), pixelErrors.end(), [&](double e) { return e > kAmazeMaxBound * level; });
    std::printf("[          ] %s: mean error %g (%.2e of the mean level %g), worst %g (%.2e) at column %d row %d, %d of %zu pixels over %.0e\n", what.c_str(), meanError,
                meanError / level, level, worst, worst / level, (int)(worstPixel % width), (int)(worstPixel / width), outliers, nPhotosites, kAmazeMaxBound);
    EXPECT_EQ(0u, nonFinite) << what << ": colour floats finite on one side only";
    EXPECT_LE(meanError / level, kAmazeMeanBound) << what;
    EXPECT_LE(outliers, outlierBudget(width, height)) << what << ": worst error " << worst / level << " of the mean level";
}
#endif // NATRON_HAVE_GPU
} // namespace

class RawAmazeGpuTest
    : public RawGpuTest {
protected:
    // The prepared image's layout and size.
    static std::optional<RawPrepare::Settings> preparedSettings(const NodePtr& prepare)
    {
        RawPrepare* effect = dynamic_cast<RawPrepare*>(prepare->getEffectInstance().get());

        return effect ? effect->resolveSettings(kTime, ViewIdx(0)) : std::nullopt;
    }
};

#ifdef NATRON_HAVE_GPU
// Each pattern's phase, with tiles that end fewer than 16 rows short of a full tile, whose lower
// border the CPU writes on past its cfa buffer into the Nyquist flags, and an odd-sized buffer.
TEST_F(RawAmazeGpuTest, MatchesTheCpuOnASyntheticMosaic)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const struct {
        const char* pattern;
        int width;
        int height;
        std::uint32_t batch;
    } cases[] = {
        { "RGGB", 300, 264, 4 },
        { "GBRG", 150, 136, 1 },
        { "GRBG", 157, 141, 64 },
        { "BGGR", 290, 300, 64 },
    };
    for (const auto& c : cases) {
        const std::optional<RawLayout> layout = RawLayout::fromPattern(c.pattern);
        ASSERT_TRUE(bool(layout)) << c.pattern;
        const std::uint32_t filters = RawNodeSupport::bayerFiltersAt(*layout, RectI(0, 0, c.width, c.height));
        const std::vector<float> mosaic = syntheticMosaic(c.width, c.height, filters);
        expectAmazeMatchesCpu(*runtime, mosaic, c.width, c.height, filters, 1.f, c.batch,
                              std::string("AMaZE ") + c.pattern + " " + std::to_string(c.width) + "x" + std::to_string(c.height));
    }
}

TEST_F(RawAmazeGpuTest, MatchesTheCpuOnACr2Crop)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    NodePtr read = createRead(bayerFixture());
    NodePtr prepare = createAfter(PLUGINID_NATRON_RAWPREPARE, read);
    ASSERT_TRUE(read && prepare);
    const std::optional<RawPrepare::Settings> settings = preparedSettings(prepare);
    ASSERT_TRUE(bool(settings));
    ASSERT_EQ(2, settings->layout.period());
    ASSERT_GE(settings->kept.width(), 701);
    ASSERT_GE(settings->kept.height(), 564);
    const RawLayout layout = settings->layout.croppedTo(settings->kept.x1, settings->kept.y1);
    const RectI windows[] = {
        RectI(401, 300, 701, 564),
        RectI(0, 0, 157, 141),
    };
    for (const RectI& window : windows) {
        RenderedPlane plane;
        ASSERT_TRUE(renderCpu(prepare, window, &plane));
        const int width = window.width();
        const int height = window.height();
        const std::size_t nComps = plane.channels.size();
        ASSERT_GT(nComps, 0u);
        ASSERT_EQ((std::size_t)width * height * nComps, plane.pixels.size());
        std::vector<float> mosaic((std::size_t)width * height);
        for (int row = 0; row < height; ++row) {
            for (int col = 0; col < width; ++col) {
                mosaic[(std::size_t)row * width + col] = plane.pixels[((std::size_t)(height - 1 - row) * width + col) * nComps];
            }
        }
        const std::uint32_t filters = RawNodeSupport::bayerFiltersAt(layout, window);
        expectAmazeMatchesCpu(*runtime, mosaic, width, height, filters, 1.f, amazeGpuBatch(64),
                              "AMaZE CR2 over " + std::to_string(window.x1) + "," + std::to_string(window.y1) + " " + std::to_string(width) + "x" + std::to_string(height));
    }
}
#endif // NATRON_HAVE_GPU
