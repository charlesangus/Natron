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

#include <cstddef>
#include <cstdint>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Native/NativeGpuParity.h"
#include "NativeParity.h"

#include "Engine/Nodes/Filter/BlurKernels.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuBlur.h"
#include "Engine/Gpu/GpuMorphology.h"
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuRuntime.h"
#endif

NATRON_NAMESPACE_USING

#ifdef NATRON_HAVE_GPU

namespace {

struct NamedFilter {
    std::string name;
    BlurKernels::LineFilter filter;
};

std::vector<NamedFilter>
lineFilters()
{
    std::vector<NamedFilter> out;
    for (unsigned int order : { 0u, 1u }) {
        for (bool neumann : { false, true }) {
            std::ostringstream suffix;
            suffix << " order " << order << (neumann ? " neumann" : " black");
            for (float sigma : { 0.6f, 4.f, 25.f }) {
                std::ostringstream name;
                name << "deriche sigma " << sigma << suffix.str();
                out.push_back({ name.str(), BlurKernels::LineFilter::deriche(sigma, order, neumann) });
            }
            for (float sigma : { 1.5f, 4.f, 30.f }) {
                std::ostringstream name;
                name << "vanVliet sigma " << sigma << suffix.str();
                out.push_back({ name.str(), BlurKernels::LineFilter::vanVliet(sigma, order, neumann) });
            }
            for (float size : { 0.5f, 1.f, 2.5f, 5.f, 11.f }) {
                for (unsigned int iterations : { 1u, 2u, 3u }) {
                    const BlurKernels::LineFilter f = BlurKernels::LineFilter::box(size, (int)order, neumann, iterations);
                    // A box of width 1 or less does not smooth, whatever the iterations.
                    if (f.isIdentity() || ((size <= 1.f) && (iterations > 1))) {
                        continue;
                    }
                    std::ostringstream name;
                    name << "box size " << size << " x" << iterations << suffix.str();
                    out.push_back({ name.str(), f });
                }
            }
        }
    }

    return out;
}

struct Shape {
    int width;
    int height;
    int nComps;
};

const Shape kShapes[] = {
    { 1, 1, 4 },
    { 2, 3, 4 },
    { 3, 2, 4 },
    { 1, 40, 4 },
    { 57, 1, 4 },
    { 3, 61, 3 },
    { 64, 48, 4 },
    { 45, 37, 1 },
};

// Deterministic values in [0, 1), varying along both axes so no line is constant.
std::vector<float>
testPixels(const Shape& s)
{
    std::vector<float> pixels((std::size_t)s.width * s.height * s.nComps);
    uint32_t state = 12345u + (uint32_t)(s.width * 131 + s.height * 7 + s.nComps);
    for (float& v : pixels) {
        state = state * 1664525u + 1013904223u;
        v = (float)(state >> 8) / 16777216.f;
    }

    return pixels;
}

// The CPU's passes: every row with filterX, then every column with filterY, each over the whole rect.
void
cpuReference(const Shape& s,
             const BlurKernels::LineFilter* filterX,
             const BlurKernels::LineFilter* filterY,
             std::vector<float>& pixels)
{
    const std::ptrdiff_t rowFloats = (std::ptrdiff_t)s.width * s.nComps;
    BlurKernels::LineScratch scratch;
    if (filterX) {
        for (int y = 0; y < s.height; ++y) {
            for (int c = 0; c < s.nComps; ++c) {
                filterX->apply(&pixels[(std::size_t)(y * rowFloats + c)], s.width, s.nComps, scratch);
            }
        }
    }
    if (filterY) {
        for (int x = 0; x < s.width; ++x) {
            for (int c = 0; c < s.nComps; ++c) {
                filterY->apply(&pixels[(std::size_t)(x * s.nComps + c)], s.height, rowFloats, scratch);
            }
        }
    }
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

// Records recordLineBlur over the whole rect, its filtered rect, with filterX on the rows and
// filterY on the columns, and returns the downloaded output.
bool
deviceRun(GpuParityRuntime& runtime,
          const Shape& s,
          const gpu::GpuLineFilter& filterX,
          const gpu::GpuLineFilter& filterY,
          const std::vector<float>& input,
          std::vector<float>& output,
          gpu::GpuStatus& recorded)
{
    gpu::GpuRuntime& vk = runtime.runtime();
    gpu::GpuBuffer* src = runtime.buffer(0, input.size());
    gpu::GpuBuffer* dst = runtime.buffer(1, input.size());
    if (!src || !dst) {
        return false;
    }
    std::vector<float> upload = input;
    if (!transfer(vk, *src, upload, true)) {
        return false;
    }

    // Away from the origin, so image and buffer positions differ.
    const gpu::GpuBlurRect rect { 5, -3, 5 + s.width, -3 + s.height };
    std::unique_ptr<gpu::GpuRecordContext> context;
    const gpu::GpuStatus created = vk.newRecordContext(context);
    EXPECT_TRUE(bool(created)) << created.message;
    if (!created) {
        return false;
    }
    gpu::GpuBuffer* window = nullptr;
    const gpu::GpuStatus windowed = gpu::recordFilterWindow(*context, rect, rect, 0, 0, window);
    EXPECT_TRUE(bool(windowed)) << windowed.message;
    if (!windowed) {
        return false;
    }
    gpu::GpuLineBlur blur;
    blur.src.buffer = src;
    blur.src.bounds = rect;
    blur.src.strideFloats = (uint32_t)(s.width * s.nComps);
    blur.srcValid = rect;
    blur.dst.buffer = dst;
    blur.dst.bounds = rect;
    blur.dst.strideFloats = blur.src.strideFloats;
    blur.output = rect;
    blur.domain = rect;
    blur.window = window;
    blur.channels = (uint32_t)s.nComps;
    blur.filterX = filterX;
    blur.filterY = filterY;
    recorded = gpu::recordLineBlur(*context, blur);
    if (!recorded) {
        return false;
    }
    const gpu::GpuStatus submitted = context->submitAndWait();
    EXPECT_TRUE(bool(submitted)) << submitted.message;
    if (!submitted) {
        return false;
    }
    output.assign(input.size(), 0.f);

    return transfer(vk, *dst, output, false);
}

} // namespace

// recordLineBlur against LineFilter::apply for every recursive and box filter the device runs, at
// orders 0 and 1, black and Neumann, on lines of 1 to 3 pixels and longer ones, along each axis
// alone and both.
TEST(GpuLineFilter, MatchesLineFilterApply)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const gpu::GpuLineFilter identity;
    int nCompared = 0;
    for (const NamedFilter& nf : lineFilters()) {
        const gpu::GpuLineFilter device = BlurKernels::toGpuLineFilter(nf.filter);
        EXPECT_EQ(nf.filter.order(), device.order) << nf.name;
        EXPECT_EQ(nf.filter.neumann(), device.neumann) << nf.name;
        for (const Shape& s : kShapes) {
            const std::vector<float> input = testPixels(s);
            for (int axes = 0; axes < 3; ++axes) {
                const bool onX = axes != 1;
                const bool onY = axes != 0;
                std::vector<float> reference = input;
                cpuReference(s, onX ? &nf.filter : NULL, onY ? &nf.filter : NULL, reference);
                std::vector<float> got;
                gpu::GpuStatus recorded;
                ASSERT_TRUE(deviceRun(*runtime, s, onX ? device : identity, onY ? device : identity, input, got, recorded))
                    << nf.name << ": " << recorded.message;
                const ImageDiff diff = compareImages(reference, got, ParityTolerance::iir());
                EXPECT_EQ(0u, diff.mismatches)
                    << nf.name << ", " << s.width << "x" << s.height << "x" << s.nComps
                    << (axes == 0 ? " rows" : (axes == 1 ? " columns" : " rows and columns")) << ": worst "
                    << diff.worstAbsolute << " at float " << diff.worstIndex << ", first mismatch at float "
                    << diff.firstMismatchIndex;
                ++nCompared;
            }
        }
    }
    EXPECT_GT(nCompared, 0);
}

// Orders above 1 have no device path, so recording one fails instead of computing something else.
TEST(GpuLineFilter, RefusesHigherOrders)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const Shape s { 8, 8, 4 };
    const std::vector<float> input = testPixels(s);
    const BlurKernels::LineFilter filters[] = {
        BlurKernels::LineFilter::deriche(3.f, 2, false),
        BlurKernels::LineFilter::vanVliet(3.f, 2, false),
        BlurKernels::LineFilter::box(3.f, 2, false, 1),
    };
    for (const BlurKernels::LineFilter& f : filters) {
        std::vector<float> got;
        gpu::GpuStatus recorded;
        EXPECT_FALSE(deviceRun(*runtime, s, BlurKernels::toGpuLineFilter(f), gpu::GpuLineFilter(), input, got, recorded));
        EXPECT_FALSE(bool(recorded));
    }
}

#else // NATRON_HAVE_GPU

TEST(GpuLineFilter, MatchesLineFilterApply)
{
    reportNoGpu("built without GPU support");
}

#endif // NATRON_HAVE_GPU
