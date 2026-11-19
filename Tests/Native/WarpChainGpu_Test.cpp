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
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <QString>
#include <gtest/gtest.h>

#include "BaseTest.h"
#include "Native/NativeGpuParity.h"
#include "Native/SplineWarpTestCurves.h"
#include "NativeParity.h"
#include "WarpChainTestEffects.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/InverseWarpChain.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/EdgeModeKnob.h"
#include "Engine/Nodes/Transform/LensDistortion.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarp.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/Transform.h"
#include "Engine/ViewIdx.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuRuntime.h"
#endif

NATRON_NAMESPACE_USING
using namespace SplineWarpTestCurves;

// warpchain_test.slang's entry against InverseWarpChain::mapRow(): chains of matrices and of the
// SplineWarp and LensDistortion maps packed by InverseWarpChain::packGpu(), every point mapped on
// the CPU and on the device. Points without a source must agree, and so must a NaN going in.

namespace {

// In pixels for the coordinates, relative to the larger of 1 and the CPU's value for J.
const double kCoordTolerance = 1e-6;
const double kJacobianTolerance = 1e-4;

// The words of warpchain_test.slang's points and results.
const int kPointWords = 4;
const int kResultWords = 8;

// warpchain_test.slang's push block.
struct WarpChainTestParams {
    uint32_t count = 0;
    uint32_t jacobian = 0;
};

void
pushPair(double v,
         float* out)
{
    out[0] = (float)v;
    out[1] = (float)(v - (double)out[0]);
}

// (n + 1) x (n + 1) points over window, off the pixel grid, then a NaN in each coordinate.
void
gridPoints(const RectD& window,
           int n,
           std::vector<double>* xs,
           std::vector<double>* ys)
{
    for (int j = 0; j <= n; ++j) {
        for (int i = 0; i <= n; ++i) {
            xs->push_back(window.x1 + window.width() * i / n + 0.37);
            ys->push_back(window.y1 + window.height() * j / n + 0.21);
        }
    }
    const double nan = std::numeric_limits<double>::quiet_NaN();
    xs->push_back(nan);
    ys->push_back(window.y1 + 1.);
    xs->push_back(window.x1 + 1.);
    ys->push_back(nan);
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

// The device's results for the points, kResultWords floats each.
bool
mapGpu(GpuParityRuntime& runtime,
       const InverseWarpChain& chain,
       const std::vector<double>& xs,
       const std::vector<double>& ys,
       std::vector<float>* out)
{
    gpu::GpuRuntime& vk = runtime.runtime();
    std::vector<uint32_t> words;
    EXPECT_TRUE(chain.packGpu(&words));
    if (words.empty()) {
        return false;
    }
    std::vector<float> points(xs.size() * kPointWords);
    for (std::size_t i = 0; i < xs.size(); ++i) {
        pushPair(xs[i], &points[i * kPointWords]);
        pushPair(ys[i], &points[i * kPointWords + 2]);
    }
    std::vector<float> results(xs.size() * kResultWords, -1.f);

    gpu::GpuBuffer* chainBuffer = runtime.buffer(0, words.size());
    gpu::GpuBuffer* pointBuffer = runtime.buffer(1, points.size());
    gpu::GpuBuffer* resultBuffer = runtime.buffer(2, results.size());
    if (!chainBuffer || !pointBuffer || !resultBuffer) {
        return false;
    }
    if (!transfer(vk, *chainBuffer, words.data(), words.size() * sizeof(uint32_t), true)
        || !transfer(vk, *pointBuffer, points.data(), points.size() * sizeof(float), true)
        || !transfer(vk, *resultBuffer, results.data(), results.size() * sizeof(float), true)) {
        return false;
    }

    WarpChainTestParams p;
    p.count = (uint32_t)xs.size();
    p.jacobian = 1;
    std::unique_ptr<gpu::GpuRecordContext> context;
    const gpu::GpuStatus created = vk.newRecordContext(context);
    EXPECT_TRUE(bool(created)) << created.message;
    if (!created) {
        return false;
    }
    const gpu::GpuStatus dispatched = context->dispatch(gpu::GpuKernelId::warpchain_test, { chainBuffer, pointBuffer, resultBuffer }, p, { p.count, 1, 1 });
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

// Maps the points through chain on both sides and compares. Returns how many points had no source
// on the CPU.
int
expectParity(GpuParityRuntime& runtime,
             const InverseWarpChain& chain,
             const std::vector<double>& xs,
             const std::vector<double>& ys,
             const std::string& what)
{
    std::vector<float> gpu;
    EXPECT_TRUE(mapGpu(runtime, chain, xs, ys, &gpu)) << what;
    if (gpu.size() != xs.size() * kResultWords) {
        return 0;
    }
    const std::size_t n = xs.size();
    std::vector<double> sx = xs;
    std::vector<double> sy = ys;
    std::vector<double> J(n * 4);
    for (std::size_t i = 0; i < n; ++i) {
        J[i * 4] = 1.;
        J[i * 4 + 1] = 0.;
        J[i * 4 + 2] = 0.;
        J[i * 4 + 3] = 1.;
    }
    chain.mapRow(n, sx.data(), sy.data(), J.data());

    int unmapped = 0;
    int wrong = 0;
    double worstCoord = 0.;
    double worstJ = 0.;
    for (std::size_t i = 0; i < n; ++i) {
        const float* r = &gpu[i * kResultWords];
        const bool cpuMapped = std::isfinite(sx[i]) && std::isfinite(sy[i]);
        const bool gpuMapped = std::isfinite(r[0]) && std::isfinite(r[2]);
        if (!cpuMapped) {
            ++unmapped;
        }
        if (cpuMapped != gpuMapped) {
            if (wrong++ == 0) {
                ADD_FAILURE() << what << ": (" << xs[i] << ", " << ys[i] << ") has a source on the " << (cpuMapped ? "CPU" : "GPU") << " only";
            }
            continue;
        }
        if (!cpuMapped) {
            continue;
        }
        worstCoord = std::max(worstCoord, std::fabs((double)r[0] + (double)r[1] - sx[i]));
        worstCoord = std::max(worstCoord, std::fabs((double)r[2] + (double)r[3] - sy[i]));
        for (int k = 0; k < 4; ++k) {
            const double cpu = J[i * 4 + k];
            worstJ = std::max(worstJ, std::fabs((double)r[4 + k] - cpu) / std::max(1., std::fabs(cpu)));
        }
    }
    EXPECT_EQ(0, wrong) << what;
    EXPECT_LE(worstCoord, kCoordTolerance) << what;
    EXPECT_LE(worstJ, kJacobianTolerance) << what;

    return unmapped;
}

#endif // NATRON_HAVE_GPU

} // namespace

class WarpChainGpuTest
    : public BaseTest {
protected:
    void resetProject(double par = 1.)
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "warpChainGpu64x48par" + std::to_string(par), par));
    }

    NodePtr makeSource()
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
        EXPECT_TRUE(bool(source));

        return source;
    }

    // A SplineWarp on source pulling a quad across the frame, with no boundary: fields with
    // feather boundaries stay on the CPU.
    NodePtr makeSplineWarp(const NodePtr& source,
                           const std::string& edgeMode)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_SPLINEWARP), PLUGIN_MAJOR_NATRON_SPLINEWARP);
        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        connectNodes(source, node, kSplineWarpInputSource, true);
        EXPECT_TRUE(setKnobValues(node, kSplineWarpParamBoundaryBBox, { 0. }));
        EXPECT_TRUE(setKnobValue(node, kEdgeModeParam, edgeMode));
        EXPECT_TRUE(bool(linkQuad(contextOf(node), 30., 22., 14., 3.5, -2.5, 1.15)));

        return node;
    }

    NodePtr makeLensDistortion(const NodePtr& source,
                               const std::string& edgeMode)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_LENSDISTORTION), PLUGIN_MAJOR_NATRON_LENSDISTORTION);
        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        connectNodes(source, node, kLensDistortionInputSource, true);
        EXPECT_TRUE(setKnobValue(node, kLensDistortionParamModel, std::string(kLensDistortionModelNuke)));
        EXPECT_TRUE(setKnobValues(node, kLensDistortionParamK1, { 0.05 }));
        EXPECT_TRUE(setKnobValues(node, kLensDistortionParamK2, { 0.01 }));
        EXPECT_TRUE(setKnobValues(node, kLensDistortionParamCenter, { 0.02, -0.01 }));
        EXPECT_TRUE(setKnobValue(node, kEdgeModeParam, edgeMode));

        return node;
    }

    static InverseWarpMapConstPtr mapOf(const NodePtr& node,
                                        const RectD& window)
    {
        return node->getEffectInstance()->getInverseWarpMap(kTime, ViewIdx(0), RenderScale::identity, false, window);
    }
};

TEST_F(WarpChainGpuTest, MatricesMatchTheCpu)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const RectD window(-120., -90., 184., 138.);
    std::vector<double> xs, ys;
    gridPoints(window, 40, &xs, &ys);

    InverseWarpChain affine(RenderScale::identity, 1.);
    affine.appendMatrix(Transform::Matrix3x3(1.2, 0.3, 4.5, -0.2, 0.9, -3.25, 0., 0., 1.));
    EXPECT_EQ(2, expectParity(*runtime, affine, xs, ys, "affine"));

    // Steep enough that part of the window lies behind the horizon, where no point has a source.
    InverseWarpChain projective(RenderScale::identity, 1.);
    projective.appendMatrix(Transform::Matrix3x3(1., 0.1, 2., 0.05, 1.1, -1., 0.012, -0.004, 1.));
    EXPECT_GT(expectParity(*runtime, projective, xs, ys, "projective"), 2);
#endif
}

TEST_F(WarpChainGpuTest, SplineWarpAndLensMapsMatchTheCpu)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    resetProject();
    // Beyond the format on every side, so the maps' crops both clamp and drop points.
    const RectD window(-24., -20., kParitySourceWidth + 24., kParitySourceHeight + 20.);
    std::vector<double> xs, ys;
    gridPoints(window, 48, &xs, &ys);

    for (const char* edgeMode : { "black", "stretch" }) {
        NodePtr source = makeSource();
        NodePtr warp = makeSplineWarp(source, edgeMode);
        NodePtr lens = makeLensDistortion(source, edgeMode);
        ASSERT_TRUE(source && warp && lens);
        const InverseWarpMapConstPtr warpMap = mapOf(warp, window);
        const InverseWarpMapConstPtr lensMap = mapOf(lens, window);
        ASSERT_TRUE(warpMap && lensMap) << edgeMode;

        InverseWarpChain chain(RenderScale::identity, 1.);
        chain.appendMatrix(Transform::Matrix3x3(0.95, 0.1, 1.5, -0.08, 1.05, -2., 0., 0., 1.));
        chain.appendMap(warpMap, InverseWarpChain::eGpuStepSplineWarp);
        chain.appendMatrix(Transform::Matrix3x3(1., 0.02, 0.5, 0.01, 1., 0.25, 0.0005, -0.0003, 1.));
        chain.appendMap(lensMap, InverseWarpChain::eGpuStepLens);
        EXPECT_EQ(InverseWarpChain::eGpuStepMatrix | InverseWarpChain::eGpuStepSplineWarp | InverseWarpChain::eGpuStepLens, chain.getGpuSteps());
        const int unmapped = expectParity(*runtime, chain, xs, ys, edgeMode);
        if (std::string(edgeMode) == "black") {
            EXPECT_GT(unmapped, 2) << "a black edge drops the points beyond the crop";
        } else {
            EXPECT_EQ(2, unmapped) << "only the NaN points lack a source under stretch";
        }

        InverseWarpChain warpOnly(RenderScale::identity, 1.);
        warpOnly.appendMap(warpMap, InverseWarpChain::eGpuStepSplineWarp);
        expectParity(*runtime, warpOnly, xs, ys, std::string(edgeMode) + " SplineWarp");
        InverseWarpChain lensOnly(RenderScale::identity, 1.);
        lensOnly.appendMap(lensMap, InverseWarpChain::eGpuStepLens);
        expectParity(*runtime, lensOnly, xs, ys, std::string(edgeMode) + " LensDistortion");
    }
#endif
}

// A boundary's feather decides which points move by the pixel containing them, on both sides.
TEST_F(WarpChainGpuTest, FeatheredSplineWarpMatchesTheCpu)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr warp = makeSplineWarp(source, "black");
    ASSERT_TRUE(bool(warp));
    SplineWarpContextPtr ctx = contextOf(warp);
    BezierPtr boundary = makeRectangle(ctx, 8., 6., 56., 42.);
    ASSERT_TRUE(bool(boundary));
    ctx->setBoundary(boundary, true);
    const RectD window(0., 0., kParitySourceWidth, kParitySourceHeight);
    const InverseWarpMapConstPtr feathered = mapOf(warp, window);
    ASSERT_TRUE(bool(feathered));
    std::vector<double> xs, ys;
    gridPoints(RectD(0., 0., kParitySourceWidth - 1., kParitySourceHeight - 1.), 60, &xs, &ys);

    InverseWarpChain chain(RenderScale::identity, 1.);
    chain.appendMap(feathered, InverseWarpChain::eGpuStepSplineWarp);
    expectParity(*runtime, chain, xs, ys, "feathered SplineWarp");

    // The boundary leaves points outside its feather in place.
    std::vector<double> sx = xs;
    std::vector<double> sy = ys;
    chain.mapRow(sx.size(), sx.data(), sy.data(), NULL);
    int still = 0;
    for (std::size_t i = 0; i < sx.size(); ++i) {
        still += (std::isfinite(sx[i]) && (sx[i] == xs[i]) && (sy[i] == ys[i])) ? 1 : 0;
    }
    EXPECT_GT(still, 0) << "no point lay outside the boundary's feather";
#endif
}

// With non-square pixels the field's derivative is rescaled between its canonical lattice and the
// chain's pixels; a sheared warp gives that derivative the off-diagonal terms the rescale touches.
TEST_F(WarpChainGpuTest, NonSquarePixelsMatchTheCpu)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const double par = 2.;
    resetProject(par);
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr warp = createNode(QString::fromUtf8(PLUGINID_NATRON_SPLINEWARP), PLUGIN_MAJOR_NATRON_SPLINEWARP);
    ASSERT_TRUE(bool(warp));
    connectNodes(source, warp, kSplineWarpInputSource, true);
    EXPECT_TRUE(setKnobValues(warp, kSplineWarpParamBoundaryBBox, { 0. }));
    EXPECT_TRUE(setKnobValue(warp, kEdgeModeParam, std::string("stretch")));
    SplineWarpContextPtr ctx = contextOf(warp);
    BezierPtr from = makeQuad(ctx, 52., 14., 20.);
    BezierPtr to = makeCurve(ctx, { { 49., 13. }, { 70., 17. }, { 80., 36. }, { 55., 31. } });
    ASSERT_TRUE(from && to);
    ASSERT_TRUE(bool(ctx->linkWithAlignment(from, to, 0., false)));
    NodePtr lens = makeLensDistortion(source, "stretch");
    ASSERT_TRUE(bool(lens));
    ASSERT_EQ(par, warp->getEffectInstance()->getTimeInvariantMetadata()->getPixelAspectRatio(-1));
    ASSERT_EQ(par, lens->getEffectInstance()->getTimeInvariantMetadata()->getPixelAspectRatio(-1));

    const RectD window(0., 0., kParitySourceWidth * par, kParitySourceHeight);
    const InverseWarpMapConstPtr warpMap = mapOf(warp, window);
    const InverseWarpMapConstPtr lensMap = mapOf(lens, window);
    ASSERT_TRUE(warpMap && lensMap);
    std::vector<double> xs, ys;
    gridPoints(RectD(-4., -4., kParitySourceWidth + 4., kParitySourceHeight + 4.), 48, &xs, &ys);

    InverseWarpChain warpOnly(RenderScale::identity, par);
    warpOnly.appendMap(warpMap, InverseWarpChain::eGpuStepSplineWarp);
    expectParity(*runtime, warpOnly, xs, ys, "SplineWarp at a pixel aspect ratio of 2");

    std::vector<double> sx = xs;
    std::vector<double> sy = ys;
    std::vector<double> J(xs.size() * 4, 0.);
    for (std::size_t i = 0; i < xs.size(); ++i) {
        J[i * 4] = 1.;
        J[i * 4 + 3] = 1.;
    }
    warpOnly.mapRow(xs.size(), sx.data(), sy.data(), J.data());
    double offDiagonal = 0.;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        if (std::isfinite(J[i * 4 + 1]) && std::isfinite(J[i * 4 + 2])) {
            offDiagonal = std::max(offDiagonal, std::min(std::fabs(J[i * 4 + 1]), std::fabs(J[i * 4 + 2])));
        }
    }
    EXPECT_GT(offDiagonal, 1e-2) << "the shear should give the derivative both off-diagonal terms";

    InverseWarpChain chain(RenderScale::identity, par);
    chain.appendMatrix(Transform::Matrix3x3(0.95, 0.1, 1.5, -0.08, 1.05, -2., 0., 0., 1.));
    chain.appendMap(warpMap, InverseWarpChain::eGpuStepSplineWarp);
    chain.appendMap(lensMap, InverseWarpChain::eGpuStepLens);
    expectParity(*runtime, chain, xs, ys, "matrix, SplineWarp and LensDistortion at a pixel aspect ratio of 2");
#endif
}

TEST_F(WarpChainGpuTest, MapsWithoutAGpuFormDoNotPack)
{
    InverseWarpChain chain(RenderScale::identity, 1.);
    chain.appendMatrix(Transform::Matrix3x3(1., 0., 2., 0., 1., -1., 0., 0., 1.));
    chain.appendMap(std::make_shared<WarpChainTestMap>(RenderScale::identity));
    EXPECT_EQ(InverseWarpChain::eGpuStepMatrix | InverseWarpChain::eGpuStepUnsupported, chain.getGpuSteps());
    std::vector<uint32_t> words(3, 7u);
    EXPECT_FALSE(chain.packGpu(&words));
    EXPECT_TRUE(words.empty());

    InverseWarpChain matrices(RenderScale::identity, 1.);
    matrices.appendMatrix(Transform::Matrix3x3(1., 0., 2., 0., 1., -1., 0., 0., 1.));
    EXPECT_TRUE(matrices.packGpu(&words));
    ASSERT_EQ(3u + 18u, words.size());
    EXPECT_EQ(1u, words[0]);
    EXPECT_EQ(InverseWarpChain::kGpuTagMatrix, words[1]);
    EXPECT_EQ(3u, words[2]);
}
