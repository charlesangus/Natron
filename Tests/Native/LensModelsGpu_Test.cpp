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
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Tests/Native/NativeGpuParity.h"

#include "Engine/Nodes/Transform/LensModels.h"
#include "Engine/Nodes/Transform/LensModels3DE.h"
#include "Engine/Nodes/Transform/LensModelsGpu.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuRuntime.h"
#endif

NATRON_NAMESPACE_USING
using namespace LensModels;

// lensmodels_test.slang's entry against LensModels::Model::map(): every model mapped both ways over
// a grid of the format on the CPU and on the device. The device computes in float pairs, about
// 2^-44 from the CPU's doubles; the bound below is far above that and far below a float's 2^-24.

namespace {

const Frame kHD = { 0., 0., 1920., 1080., 1. };

// The parameters of LensModels_Test and LensModels3DE_Test.
const Camera3DE kCamera = { 0., 0., 1., 1., 3.5, 100., 3.6, 2.025, 0.01, -0.02, 1. };
const Classic3DEParams kClassic = { 0.05, 1.1, 0.01, -0.02, 0.01 };
const Anamorphic6Params kAnamorphic6 = {
    0.04, 0.03, 0.01, -0.005,
    0.01, 0.008, 0.002, -0.001, 0.001, 0.0005,
    0.002, 0.001, 0.0005, -0.0003, 0.0002, 0.0001, 0.0001, -0.0001
};
const Fisheye8Params kFisheye = { -0.05, 0.01, 0., 0.001 };
const Camera3DE kFisheyeCamera = { 0., 0., 1., 1., 2.5, 100., 3.6, 2.025, 0.01, -0.02, 1. };
// A 1 cm focal length takes the corners of the format beyond the fisheye's 90 degrees.
const Camera3DE kWideFisheyeCamera = { 0., 0., 1., 1., 1., 100., 3.6, 2.025, 0.01, -0.02, 1. };
const Standard3DEParams kStandard = { 0.05, 0.01, -0.01, 0.01, 0.002, -0.003, 30., 0.05 };
const Anamorphic4Params kAnamorphic4 = {
    0.04, 0.03, 0.01, -0.005,
    0.01, 0.008, 0.002, -0.001, 0.001, 0.0005,
    10., 1.05, 0.98
};
const Camera3DE kAnamorphicCamera = { 0., 0., 1., 1., 5., 100., 2.2, 1.866, 0.01, -0.02, 2. };
const NukeParams kNuke = { 0.05, 0.01, 0.02, -0.01, 1.2, 0.01, 0.005 };
const PanoToolsParams kPanoTools = { 0.01, -0.02, 0.03, 5., -3., 2., 1. };
const PFBarrelParams kPFBarrel = { 0.05, 0.01, 0.52, 0.47, 1.1 };

// Relative to the larger of 1 and the CPU's value, for positions and Jacobians alike.
const double kTolerance = 1e-9;

// The words of lensmodels_test.slang's points and results.
const int kPointWords = 4;
const int kResultWords = 13;

struct ModelCase {
    std::string name;
    Model model;
};

std::vector<ModelCase>
modelCases()
{
    return {
        { "Nuke", makeNuke(kHD, kNuke) },
        { "PFBarrel", makePFBarrel(kHD, kPFBarrel) },
        { "3DE Classic", make3DEClassic(kHD, kCamera, kClassic) },
        { "3DE Anamorphic 6", make3DEAnamorphic6(kHD, kCamera, kAnamorphic6) },
        { "3DE Fisheye 8", make3DEFisheye8(kHD, kFisheyeCamera, kFisheye) },
        { "3DE Standard", make3DEStandard(kHD, kCamera, kStandard) },
        { "3DE Anamorphic 4", make3DEAnamorphic4(kHD, kAnamorphicCamera, kAnamorphic4) },
        { "PanoTools", makePanoTools(kHD, kPanoTools) },
    };
}

// (n + 1) x (n + 1) points over the format, off the pixel grid.
void
gridPoints(int n,
           std::vector<double>* xs,
           std::vector<double>* ys)
{
    for (int j = 0; j <= n; ++j) {
        for (int i = 0; i <= n; ++i) {
            xs->push_back(kHD.x1 + (kHD.x2 - kHD.x1) * i / n + 0.37);
            ys->push_back(kHD.y1 + (kHD.y2 - kHD.y1) * j / n + 0.21);
        }
    }
}

double
relativeError(double cpu,
              double gpu)
{
    return std::fabs(gpu - cpu) / std::max(1., std::fabs(cpu));
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

// lensmodels_test.slang's push block.
struct LensTestParams {
    uint32_t count = 0;
    uint32_t direction = 0;
};

// The device's results for the points, kResultWords floats each.
bool
mapGpu(GpuParityRuntime& runtime,
       const Model& model,
       Direction direction,
       const std::vector<double>& xs,
       const std::vector<double>& ys,
       std::vector<float>* out)
{
    gpu::GpuRuntime& vk = runtime.runtime();
    LensModelGpu packed = packLensModelGpu(model);
    std::vector<float> points(xs.size() * kPointWords);
    for (std::size_t i = 0; i < xs.size(); ++i) {
        setLensModelGpuDouble(xs[i], &points[i * kPointWords]);
        setLensModelGpuDouble(ys[i], &points[i * kPointWords + 2]);
    }
    std::vector<float> results(xs.size() * kResultWords, -1.f);

    gpu::GpuBuffer* modelBuffer = runtime.buffer(0, sizeof(packed) / sizeof(float));
    gpu::GpuBuffer* pointBuffer = runtime.buffer(1, points.size());
    gpu::GpuBuffer* resultBuffer = runtime.buffer(2, results.size());
    if (!modelBuffer || !pointBuffer || !resultBuffer) {
        return false;
    }
    if (!transfer(vk, *modelBuffer, &packed, sizeof(packed), true) || !transfer(vk, *pointBuffer, points.data(), points.size() * sizeof(float), true)
        || !transfer(vk, *resultBuffer, results.data(), results.size() * sizeof(float), true)) {
        return false;
    }

    LensTestParams p;
    p.count = (uint32_t)xs.size();
    p.direction = (uint32_t)direction;
    std::unique_ptr<gpu::GpuRecordContext> context;
    const gpu::GpuStatus created = vk.newRecordContext(context);
    EXPECT_TRUE(bool(created)) << created.message;
    if (!created) {
        return false;
    }
    const gpu::GpuStatus dispatched = context->dispatch(gpu::GpuKernelId::lensmodels_test, { modelBuffer, pointBuffer, resultBuffer }, p, { p.count, 1, 1 });
    EXPECT_TRUE(bool(dispatched)) << dispatched.message;
    if (!dispatched) {
        return false;
    }
    const gpu::GpuStatus submitted = context->submitAndWait();
    EXPECT_TRUE(bool(submitted)) << submitted.message;
    context.reset();
    if (!submitted || !transfer(vk, *resultBuffer, results.data(), results.size() * sizeof(float), false)) {
        return false;
    }
    *out = results;

    return true;
}

// Maps the points both ways on both sides: the same points map, to within kTolerance. Returns how
// many points failed on the CPU in each direction.
void
expectParity(GpuParityRuntime& runtime,
             const ModelCase& c,
             const std::vector<double>& xs,
             const std::vector<double>& ys,
             int failures[2])
{
    for (Direction direction : { eDirectionDistort, eDirectionUndistort }) {
        std::ostringstream what;
        what << c.name << (direction == eDirectionDistort ? " distort" : " undistort");
        std::vector<float> gpu;
        ASSERT_TRUE(mapGpu(runtime, c.model, direction, xs, ys, &gpu)) << what.str();
        failures[direction] = 0;
        int wrong = 0;
        double worst = 0.;
        for (std::size_t i = 0; i < xs.size(); ++i) {
            const float* r = &gpu[i * kResultWords];
            Sample s;
            const bool ok = c.model.map(direction, xs[i], ys[i], &s);
            if (!ok) {
                ++failures[direction];
            }
            if ((r[0] == 1.f) != ok) {
                if (wrong++ == 0) {
                    ADD_FAILURE() << what.str() << ": (" << xs[i] << ", " << ys[i] << ") maps on the " << (ok ? "CPU" : "GPU") << " only";
                }
                continue;
            }
            if (!ok) {
                continue;
            }
            const double cpu[6] = { s.x, s.y, s.J[0], s.J[1], s.J[2], s.J[3] };
            for (int k = 0; k < 6; ++k) {
                const double e = relativeError(cpu[k], (double)r[1 + 2 * k] + (double)r[2 + 2 * k]);
                worst = std::max(worst, e);
                if (!(e <= kTolerance)) {
                    if (wrong++ == 0) {
                        ADD_FAILURE() << what.str() << ": (" << xs[i] << ", " << ys[i] << ") value " << k << " is " << (double)r[1 + 2 * k] + (double)r[2 + 2 * k]
                                      << " on the GPU, " << cpu[k] << " on the CPU";
                    }
                    break;
                }
            }
        }
        EXPECT_EQ(0, wrong) << what.str() << ": worst relative error " << worst;
    }
}

#endif // NATRON_HAVE_GPU

} // namespace

TEST(LensModelsGpu, EveryModelMatchesTheCpu)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    std::vector<double> xs;
    std::vector<double> ys;
    gridPoints(16, &xs, &ys);
    for (const ModelCase& c : modelCases()) {
        ASSERT_TRUE(c.model.valid) << c.name;
        int failures[2] = { 0, 0 };
        expectParity(*runtime, c, xs, ys, failures);
        EXPECT_EQ(0, failures[eDirectionDistort]) << c.name << " distort";
        EXPECT_EQ(0, failures[eDirectionUndistort]) << c.name << " undistort";
    }
#endif
}

// Beyond 90 degrees the fisheye has no value in its closed-form direction and Newton no solution in
// the other: the device fails where the CPU does.
TEST(LensModelsGpu, FisheyeDomainMatchesTheCpu)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    std::vector<double> xs;
    std::vector<double> ys;
    gridPoints(24, &xs, &ys);
    const ModelCase c = { "wide 3DE Fisheye 8", make3DEFisheye8(kHD, kWideFisheyeCamera, kFisheye) };
    ASSERT_TRUE(c.model.valid);
    int failures[2] = { 0, 0 };
    expectParity(*runtime, c, xs, ys, failures);
    EXPECT_GT(failures[eDirectionDistort], 0);
    EXPECT_GT(failures[eDirectionUndistort], 0);
#endif
}

// The packed words are the model's doubles split in two floats whose sum is the double to 2^-48.
TEST(LensModelsGpu, PackingCarriesTheDoubles)
{
    const Model model = make3DEStandard(kHD, kCamera, kStandard);
    ASSERT_TRUE(model.valid);
    const LensModelGpu packed = packLensModelGpu(model);
    EXPECT_EQ(1u, packed.valid);
    EXPECT_EQ((uint32_t)eModel3DEStandard, packed.type);
    EXPECT_EQ((uint32_t)model.core, packed.core);
    const Affine* const affines[4] = { &model.toCore, &model.fromCore, &model.toCoreInverse, &model.fromCoreInverse };
    for (int i = 0; i < 4; ++i) {
        const double words[6] = { affines[i]->m00, affines[i]->m01, affines[i]->m10, affines[i]->m11, affines[i]->tx, affines[i]->ty };
        for (int k = 0; k < 6; ++k) {
            const double sum = (double)packed.affines[i][k][0] + (double)packed.affines[i][k][1];
            EXPECT_LE(std::fabs(sum - words[k]), std::ldexp(std::fabs(words[k]), -48)) << "affine " << i << " word " << k;
        }
    }
    for (int i = 0; i < kMaxCoeffs; ++i) {
        const double sum = (double)packed.c[i][0] + (double)packed.c[i][1];
        EXPECT_LE(std::fabs(sum - model.c[i]), std::ldexp(std::fabs(model.c[i]), -48)) << "coefficient " << i;
    }
}
