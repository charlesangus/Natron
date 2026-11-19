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
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Tests/Native/NativeGpuParity.h"

#include "Engine/Optics/Aperture.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuAperture.h"
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuRuntime.h"
#endif

NATRON_NAMESPACE_USING
using namespace Optics;

// aperture.slang against rasteriseApertureRows() from the same tables. The kernel repeats the CPU's
// arithmetic statement by statement, so only sqrt and the roundness's one division round
// differently; soft edges turn those last-bit differences into far less than the bound.

namespace {

const double kTolerance = 1e-5;
const int kWidth = 96;
const int kHeight = 80;
const int kImageSize = 24;

struct ParityCase {
    std::string name;
    ApertureParams params;
    bool image;
};

std::vector<ParityCase>
parityCases()
{
    std::vector<ParityCase> cases;
    {
        ParityCase c { "default polygon", ApertureParams(), false };
        cases.push_back(c);
    }
    {
        ParityCase c { "soft rounded polygon", ApertureParams(), false };
        c.params.blades = 7;
        c.params.rotation = 23.;
        c.params.roundness = 0.3;
        c.params.softness = 0.4;
        c.params.sizeX = 0.8;
        c.params.sizeY = 0.6;
        cases.push_back(c);
    }
    {
        ParityCase c { "grating", ApertureParams(), false };
        c.params.softness = 0.3;
        c.params.gratingStrength = 0.8;
        c.params.gratingLength = 1.;
        c.params.gratingSoftness = 0.3;
        cases.push_back(c);
    }
    {
        ParityCase c { "scratches", ApertureParams(), false };
        c.params.softness = 0.3;
        c.params.scratchesStrength = 0.7;
        c.params.scratchesRotation = 35.;
        c.params.scratchesRotationVariation = 0.5;
        c.params.scratchesSoftness = 0.3;
        c.params.scratchesParallaxX = 0.5;
        cases.push_back(c);
    }
    {
        ParityCase c { "dust", ApertureParams(), false };
        c.params.softness = 0.3;
        c.params.dustStrength = 0.9;
        c.params.dustSoftness = 0.3;
        c.params.dustParallaxY = 2.;
        cases.push_back(c);
    }
    {
        ParityCase c { "hard dust and scratches", ApertureParams(), false };
        c.params.dustStrength = 1.;
        c.params.scratchesStrength = 1.;
        c.params.gratingStrength = 1.;
        c.params.gratingLength = 1.;
        cases.push_back(c);
    }
    {
        ParityCase c { "image", ApertureParams(), true };
        c.params.softness = 0.3;
        c.params.imageStrength = 0.9;
        c.params.imageSizeX = 0.9;
        c.params.imageSizeY = 0.7;
        c.params.imageBlack = 0.1;
        c.params.imageWhite = 0.8;
        cases.push_back(c);
    }

    return cases;
}

std::vector<float>
testImage()
{
    std::vector<float> pixels((std::size_t)kImageSize * kImageSize);
    for (int y = 0; y < kImageSize; ++y) {
        for (int x = 0; x < kImageSize; ++x) {
            pixels[(std::size_t)y * kImageSize + x] = (float)((x * 7 + y * 3) % 11) / 10.f;
        }
    }

    return pixels;
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

bool
rasteriseGpu(GpuParityRuntime& runtime,
             const std::vector<uint32_t>& tables,
             std::vector<float>* image,
             std::vector<float>* out)
{
    gpu::GpuRuntime& vk = runtime.runtime();
    const std::size_t outFloats = (std::size_t)kWidth * kHeight;
    std::vector<float> result(outFloats, -1.f);
    gpu::GpuBuffer* dst = runtime.buffer(0, outFloats);
    gpu::GpuBuffer* imageBuffer = image ? runtime.buffer(1, image->size()) : nullptr;
    if (!dst || (image && !imageBuffer)) {
        return false;
    }
    if (!transfer(vk, *dst, result.data(), outFloats * sizeof(float), true)) {
        return false;
    }
    if (image && !transfer(vk, *imageBuffer, image->data(), image->size() * sizeof(float), true)) {
        return false;
    }

    std::unique_ptr<gpu::GpuRecordContext> context;
    const gpu::GpuStatus created = vk.newRecordContext(context);
    EXPECT_TRUE(bool(created)) << created.message;
    if (!created) {
        return false;
    }
    gpu::GpuApertureView view;
    view.buffer = dst;
    view.strideFloats = kWidth;
    const gpu::GpuStatus recorded = gpu::recordAperture(*context, tables, imageBuffer, view);
    EXPECT_TRUE(bool(recorded)) << recorded.message;
    if (!recorded) {
        return false;
    }
    const gpu::GpuStatus submitted = context->submitAndWait();
    EXPECT_TRUE(bool(submitted)) << submitted.message;
    context.reset();
    if (!submitted || !transfer(vk, *dst, result.data(), outFloats * sizeof(float), false)) {
        return false;
    }
    *out = result;

    return true;
}

#endif // NATRON_HAVE_GPU

} // namespace

TEST(OpticsApertureGpu, MatchesTheCpu)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    std::vector<float> pixels = testImage();
    ApertureFrame frame;
    frame.width = kWidth;
    frame.height = kHeight;
    frame.lightX = 0.37;
    frame.lightY = -0.21;
    for (const ParityCase& c : parityCases()) {
        ApertureImage image;
        if (c.image) {
            image.pixels = pixels.data();
            image.width = kImageSize;
            image.height = kImageSize;
        }
        const std::vector<uint32_t> tables = buildApertureTables(c.params, frame, image.width, image.height);
        std::vector<float> cpu((std::size_t)kWidth * kHeight, -1.f);
        rasteriseApertureRows(tables, image, 0, kHeight, cpu.data(), kWidth);

        std::vector<float> gpu;
        ASSERT_TRUE(rasteriseGpu(*runtime, tables, c.image ? &pixels : nullptr, &gpu)) << c.name;
        ASSERT_EQ(cpu.size(), gpu.size());
        double worst = 0.;
        std::size_t worstIndex = 0;
        double sum = 0.;
        for (std::size_t i = 0; i < cpu.size(); ++i) {
            const double d = std::fabs((double)cpu[i] - (double)gpu[i]);
            if (d > worst) {
                worst = d;
                worstIndex = i;
            }
            sum += cpu[i];
        }
        EXPECT_LE(worst, kTolerance) << c.name << ": pixel (" << worstIndex % kWidth << ", " << worstIndex / kWidth << ") cpu "
                                     << cpu[worstIndex] << " gpu " << gpu[worstIndex];
        // A mask of all zeros or ones would match without testing the layers.
        EXPECT_GT(sum, cpu.size() * 0.05) << c.name;
        EXPECT_LT(sum, cpu.size() * 0.95) << c.name;
    }
#endif
}
