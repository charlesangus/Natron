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
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Tests/Native/NativeGpuParity.h"

#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/EdgeSample.h"
#include "Engine/RectI.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuRuntime.h"
#endif

NATRON_NAMESPACE_USING

// edgesample_test.slang's entry against EdgeSample: reads at and around a source's edges, in both
// edge modes, through edgesample.slang's edgePixel and resamplecore.slang's cellPixel and
// readOffset, against EdgeSample::pixel and EdgeSample::coord.

namespace {

const int kWords = 10;
const int kMargin = 3;

// Distinct and non-zero everywhere, so that a zero can only come from a read outside.
float
valueAt(const RectI& bounds,
        int x,
        int y,
        int c)
{
    return (float)(100 * (c + 1) + 10 * (y - bounds.y1) + (x - bounds.x1) + 1);
}

std::vector<float>
makeSource(const RectI& bounds,
           int nComps)
{
    std::vector<float> pixels(bounds.isNull() ? 0 : (std::size_t)bounds.area() * nComps);
    for (int y = bounds.y1; y < bounds.y2; ++y) {
        for (int x = bounds.x1; x < bounds.x2; ++x) {
            for (int c = 0; c < nComps; ++c) {
                pixels[((std::size_t)(y - bounds.y1) * bounds.width() + (x - bounds.x1)) * nComps + c] = valueAt(bounds, x, y, c);
            }
        }
    }

    return pixels;
}

// Every position from kMargin before the source to kMargin past it on both axes, corners included;
// around `around` when the source is empty.
std::vector<int32_t>
makePositions(const RectI& around)
{
    std::vector<int32_t> positions;
    for (int y = around.y1 - kMargin; y < around.y2 + kMargin; ++y) {
        for (int x = around.x1 - kMargin; x < around.x2 + kMargin; ++x) {
            positions.push_back(x);
            positions.push_back(y);
        }
    }

    return positions;
}

// The words edgesample_test_main writes for each position, from EdgeSample.
std::vector<float>
expected(const RectI& bounds,
         int nComps,
         EdgeMode mode,
         const std::vector<int32_t>& positions)
{
    const std::size_t count = positions.size() / 2;
    std::vector<float> words(count * kWords, 0.f);
    for (std::size_t i = 0; i < count; ++i) {
        const int x = positions[i * 2];
        const int y = positions[i * 2 + 1];
        float* w = &words[i * kWords];
        int sx = 0;
        int sy = 0;
        if (EdgeSample::pixel(x, y, bounds, mode, &sx, &sy)) {
            for (int c = 0; c < nComps; ++c) {
                w[c] = valueAt(bounds, sx, sy, c);
                w[4 + c] = w[c];
            }
        }
        int s = 0;
        w[8] = EdgeSample::coord(x, bounds.x1, bounds.x2, mode, &s) ? (float)(s - bounds.x1) : -1.f;
        w[9] = EdgeSample::coord(y, bounds.y1, bounds.y2, mode, &s) ? (float)(s - bounds.y1) : -1.f;
    }

    return words;
}

#ifdef NATRON_HAVE_GPU

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

// Mirrors EdgeTestParams in Kernels/edgesample_test.slang.
struct EdgeTestParams {
    int32_t x1;
    int32_t y1;
    int32_t x2;
    int32_t y2;
    uint32_t nComps;
    uint32_t count;
    uint32_t edgeMode;
    uint32_t padding;
};
static_assert(sizeof(EdgeTestParams) == 32);

// kEdgeStretch and kEdgeBlack in Kernels/edgesample.slang.
uint32_t
kernelMode(EdgeMode mode)
{
    return mode == EdgeMode::Black ? 1u : 0u;
}

bool
renderGpu(GpuParityRuntime& runtime,
          const RectI& bounds,
          int nComps,
          EdgeMode mode,
          const std::vector<int32_t>& positionsIn,
          std::vector<float>* out)
{
    gpu::GpuRuntime& vk = runtime.runtime();
    std::vector<float> src = makeSource(bounds, nComps);
    if (src.empty()) {
        src.push_back(std::numeric_limits<float>::quiet_NaN());
    }
    std::vector<int32_t> positions = positionsIn;
    const std::size_t count = positions.size() / 2;
    std::vector<float> dst(count * kWords, std::numeric_limits<float>::quiet_NaN());

    gpu::GpuBuffer* srcBuffer = runtime.buffer(0, src.size());
    gpu::GpuBuffer* dstBuffer = runtime.buffer(1, dst.size());
    gpu::GpuBuffer* posBuffer = runtime.buffer(2, positions.size());
    if (!srcBuffer || !dstBuffer || !posBuffer) {
        return false;
    }
    if (!transfer(vk, *srcBuffer, src.data(), src.size() * sizeof(float), true) || !transfer(vk, *dstBuffer, dst.data(), dst.size() * sizeof(float), true)
        || !transfer(vk, *posBuffer, positions.data(), positions.size() * sizeof(int32_t), true)) {
        return false;
    }

    EdgeTestParams p {};
    p.x1 = bounds.x1;
    p.y1 = bounds.y1;
    p.x2 = bounds.x2;
    p.y2 = bounds.y2;
    p.nComps = (uint32_t)nComps;
    p.count = (uint32_t)count;
    p.edgeMode = kernelMode(mode);

    std::unique_ptr<gpu::GpuRecordContext> context;
    const gpu::GpuStatus created = vk.newRecordContext(context);
    EXPECT_TRUE(bool(created)) << created.message;
    if (!created) {
        return false;
    }
    const gpu::GpuStatus dispatched = context->dispatch(gpu::GpuKernelId::edgesample_test, { srcBuffer, dstBuffer, posBuffer }, p, { p.count, 1, 1 });
    EXPECT_TRUE(bool(dispatched)) << dispatched.message;
    if (!dispatched) {
        return false;
    }
    const gpu::GpuStatus submitted = context->submitAndWait();
    EXPECT_TRUE(bool(submitted)) << submitted.message;
    context.reset();
    if (!submitted || !transfer(vk, *dstBuffer, dst.data(), dst.size() * sizeof(float), false)) {
        return false;
    }
    *out = dst;

    return true;
}

std::string
describe(const RectI& bounds,
         int nComps,
         EdgeMode mode,
         const std::vector<int32_t>& positions,
         std::size_t word)
{
    const std::size_t i = word / kWords;
    std::ostringstream s;
    s << "bounds (" << bounds.x1 << ", " << bounds.y1 << ", " << bounds.x2 << ", " << bounds.y2 << ") nComps " << nComps << " mode "
      << (mode == EdgeMode::Black ? "Black" : "Stretch") << " read (" << positions[i * 2] << ", " << positions[i * 2 + 1] << ") word " << word % kWords;

    return s.str();
}

void
expectParity(GpuParityRuntime& runtime,
             const RectI& bounds,
             const RectI& around,
             int nComps,
             EdgeMode mode)
{
    const std::vector<int32_t> positions = makePositions(around);
    const std::vector<float> cpu = expected(bounds, nComps, mode, positions);
    std::vector<float> gpu;
    ASSERT_TRUE(renderGpu(runtime, bounds, nComps, mode, positions, &gpu));
    ASSERT_EQ(cpu.size(), gpu.size());
    for (std::size_t i = 0; i < cpu.size(); ++i) {
        ASSERT_EQ(cpu[i], gpu[i]) << describe(bounds, nComps, mode, positions, i);
    }
}

#endif // NATRON_HAVE_GPU

} // namespace

// Reads up to three pixels past every edge and corner of a source off the origin give the nearest
// pixel under Stretch and zero under Black, on the device as in EdgeSample.
TEST(EdgeSampleGpu, ReadsOutsideTheSourceMatchTheCpu)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const RectI bounds(10, -4, 15, 0);
    for (int nComps : { 1, 4 }) {
        for (EdgeMode mode : { EdgeMode::Stretch, EdgeMode::Black }) {
            expectParity(*runtime, bounds, bounds, nComps, mode);
        }
    }
#endif
}

// A one-pixel source reads its pixel everywhere under Stretch and only at itself under Black.
TEST(EdgeSampleGpu, OnePixelSourceMatchesTheCpu)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const RectI bounds(-2, 5, -1, 6);
    for (EdgeMode mode : { EdgeMode::Stretch, EdgeMode::Black }) {
        expectParity(*runtime, bounds, bounds, 4, mode);
    }
#endif
}

// An empty source reads zero everywhere in both modes.
TEST(EdgeSampleGpu, EmptySourceReadsZero)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const RectI empty(4, 2, 4, 6);
    const RectI around(2, 2, 6, 6);
    for (EdgeMode mode : { EdgeMode::Stretch, EdgeMode::Black }) {
        expectParity(*runtime, empty, around, 4, mode);
    }
#endif
}
