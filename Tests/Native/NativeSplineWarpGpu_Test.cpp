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
#include <cstring>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "Native/NativeGpuParity.h"
#include "NativeParity.h"

#include "Engine/AppInstance.h"
#include "Engine/Bezier.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/GpuWeight.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarp.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpContext.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpField.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpGpu.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpSampling.h"
#include "Engine/Nodes/Transform/SplineWarp/SplineWarpTps.h"
#include "Engine/Nodes/Transform/Transform.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/RotoItem.h"
#include "Engine/ViewIdx.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/Gpu/GpuWrap.h"
#endif

NATRON_NAMESPACE_USING

// splinewarp.slang against SplineWarp's CPU resample: the same field, from SplineWarpField, read
// through getRow() and Resampler::resampleRowFromCoords() on the CPU and through getGpuTables() on
// the device; then the node rendered through the scheduler on both.

namespace {

const double kTime = 1.;

// ---------------------------------------------------------------------------------------------
// Kernel-level cases

SplineWarpSamplePair
makePair(double dx,
         double dy,
         double sx,
         double sy)
{
    SplineWarpSamplePair p;

    p.destination.x = dx;
    p.destination.y = dy;
    p.source.x = sx;
    p.source.y = sy;

    return p;
}

Point
makePoint(double x,
          double y)
{
    Point p;

    p.x = x;
    p.y = y;

    return p;
}

SplineWarpTps
translation(double tx,
            double ty)
{
    return SplineWarpTps::solve(std::vector<SplineWarpSamplePair>(1, makePair(0., 0., tx, ty)), 0.);
}

// Pairs on one affine map, which the solve reproduces exactly enough to give a uniform Jacobian.
SplineWarpTps
affineMap()
{
    const double a[2][2] = { { 1.05, 0.12 }, { -0.08, 0.93 } };
    const double b[2] = { 1.75, -2.5 };
    const double xy[6][2] = { { 0., 0. }, { 40., 2. }, { 6., 30. }, { 34., 28. }, { 18., 12. }, { 4., 22. } };
    std::vector<SplineWarpSamplePair> pairs;
    for (int i = 0; i < 6; ++i) {
        const double x = xy[i][0], y = xy[i][1];
        pairs.push_back(makePair(x, y, a[0][0] * x + a[0][1] * y + b[0], a[1][0] * x + a[1][1] * y + b[1]));
    }

    return SplineWarpTps::solve(pairs, 0.);
}

// A bend: one pair pulled against four held corners, so the Jacobian varies over the window.
SplineWarpTps
bendMap()
{
    std::vector<SplineWarpSamplePair> pairs;
    pairs.push_back(makePair(0., 0., 0., 0.));
    pairs.push_back(makePair(40., 0., 40., 0.));
    pairs.push_back(makePair(0., 32., 0., 32.));
    pairs.push_back(makePair(40., 32., 40., 32.));
    pairs.push_back(makePair(20., 16., 23.5, 13.25));

    return SplineWarpTps::solve(pairs, 0.);
}

std::vector<Point>
rectangle(double x1,
          double y1,
          double x2,
          double y2)
{
    std::vector<Point> p;

    p.push_back(makePoint(x1, y1));
    p.push_back(makePoint(x2, y1));
    p.push_back(makePoint(x2, y2));
    p.push_back(makePoint(x1, y2));

    return p;
}

std::vector<SplineWarpBoundaryShape>
squareBoundary()
{
    SplineWarpBoundaryShape b;

    b.curve = rectangle(10., 8., 30., 24.);
    b.feather = rectangle(6., 4., 34., 28.);
    b.fallOff = 1.;

    return std::vector<SplineWarpBoundaryShape>(1, b);
}

struct KernelCase {
    std::string name;
    std::shared_ptr<const SplineWarpField> field;
    RectI window;
    RectI srcBounds;
    int nComps = 4;
    std::vector<float> src;
    Resampler::FilterEnum filter = Resampler::eFilterCubic;
    bool clamp = false;
    bool blackOutside = true;
    // The inverse of a concatenated upstream matrix, row-major, when set.
    bool hasMatrix = false;
    double matrix[9] = { 1., 0., 0., 0., 1., 0., 0., 0., 1. };
    // With motion blur, the fields averaged in place of `field`, null for one that moves nothing.
    std::vector<std::shared_ptr<const SplineWarpField>> subFrames;
};

std::string
describe(const KernelCase& c)
{
    std::ostringstream s;
    s << c.name << " filter " << (int)c.filter << " clamp " << c.clamp << " blackOutside " << c.blackOutside << " matrix " << c.hasMatrix
      << " nComps " << c.nComps << " sub-frames " << c.subFrames.size();

    return s.str();
}

// A pattern that differs per pixel and per channel.
std::vector<float>
makeSource(const RectI& bounds,
           int nComps)
{
    std::vector<float> pixels((std::size_t)bounds.area() * nComps);
    for (int y = 0; y < bounds.height(); ++y) {
        for (int x = 0; x < bounds.width(); ++x) {
            for (int c = 0; c < nComps; ++c) {
                const unsigned h = (unsigned)(x * 73856093) ^ (unsigned)(y * 19349663) ^ (unsigned)(c * 83492791);
                pixels[((std::size_t)y * bounds.width() + x) * nComps + c] = 0.25f * x + 0.125f * y + 0.1f * c + (float)(h % 97) / 970.f;
            }
        }
    }

    return pixels;
}

KernelCase
makeCase(const std::string& name,
         const SplineWarpField& field,
         int nComps = 4)
{
    KernelCase c;
    c.name = name;
    c.field = std::make_shared<const SplineWarpField>(field);
    c.window = field.getWindow();
    // Smaller than what the warp reads at the window's far edges, so black outside matters.
    c.srcBounds = RectI(-3, -2, 39, 30);
    c.nComps = nComps;
    c.src = makeSource(c.srcBounds, nComps);

    return c;
}

// As SplineWarp's mapRowThrough.
void
mapRowThrough(const double m[9],
              int n,
              float* sx,
              float* sy,
              float* jacobian)
{
    for (int i = 0; i < n; ++i) {
        const double x = sx[i];
        const double y = sy[i];
        const double w = m[6] * x + m[7] * y + m[8];
        if (!(w > 0.)) {
            sx[i] = -1e9f;
            sy[i] = -1e9f;
            continue;
        }
        const double qx = (m[0] * x + m[1] * y + m[2]) / w;
        const double qy = (m[3] * x + m[4] * y + m[5]) / w;
        sx[i] = (float)qx;
        sy[i] = (float)qy;
        if (!jacobian) {
            continue;
        }
        const double d00 = (m[0] - qx * m[6]) / w;
        const double d01 = (m[1] - qx * m[7]) / w;
        const double d10 = (m[3] - qy * m[6]) / w;
        const double d11 = (m[4] - qy * m[7]) / w;
        float* J = &jacobian[(std::size_t)i * 4];
        const double j0 = J[0], j1 = J[1], j2 = J[2], j3 = J[3];
        J[0] = (float)(d00 * j0 + d01 * j2);
        J[1] = (float)(d00 * j1 + d01 * j3);
        J[2] = (float)(d10 * j0 + d11 * j2);
        J[3] = (float)(d10 * j1 + d11 * j3);
    }
}

// SplineWarp::renderOp's sampling of one plane over the field's window.
std::vector<float>
renderCpu(const KernelCase& c)
{
    const Resampler::SourceImage src(c.src.data(), c.srcBounds, c.nComps);
    const int width = c.window.width();
    const bool impulse = c.filter == Resampler::eFilterImpulse;
    std::vector<float> out((std::size_t)c.window.area() * c.nComps, 0.f);
    std::vector<float> sx(width);
    std::vector<float> sy(width);
    std::vector<float> jacobian((std::size_t)width * 4);
    float* const jac = impulse ? NULL : jacobian.data();
    const SplineWarpField identity;
    const auto sampleRow = [&](const SplineWarpField& field, int y, float* row) {
        field.getRow(y, c.window.x1, c.window.x2, sx.data(), sy.data(), jac);
        if (c.hasMatrix) {
            mapRowThrough(c.matrix, width, sx.data(), sy.data(), jac);
        }
        Resampler::resampleRowFromCoords(src, sx.data(), sy.data(), jac, width, c.filter, c.clamp, c.blackOutside, Resampler::eWrapNone,
                                         Resampler::eWrapNone, row, c.nComps);
        if (c.hasMatrix) {
            return;
        }
        const float cy = (float)(y + 0.5);
        for (int i = 0; i < width; ++i) {
            if ((sx[i] != (float)(c.window.x1 + i + 0.5)) || (sy[i] != cy)) {
                continue;
            }
            const float* pix = src.pixel(c.window.x1 + i, y);
            for (int k = 0; k < c.nComps; ++k) {
                row[(std::size_t)i * c.nComps + k] = pix ? pix[k] : 0.f;
            }
        }
    };
    const std::size_t rowSize = (std::size_t)width * c.nComps;
    std::vector<float> subRow(rowSize);
    for (int y = c.window.y1; y < c.window.y2; ++y) {
        float* row = &out[(std::size_t)(y - c.window.y1) * rowSize];
        if (c.subFrames.empty()) {
            sampleRow(*c.field, y, row);
            continue;
        }
        // As SplineWarp::renderOp: summed in sub-frame order, then divided.
        sampleRow(c.subFrames[0] ? *c.subFrames[0] : identity, y, row);
        for (std::size_t f = 1; f < c.subFrames.size(); ++f) {
            sampleRow(c.subFrames[f] ? *c.subFrames[f] : identity, y, subRow.data());
            for (std::size_t k = 0; k < rowSize; ++k) {
                row[k] += subRow[k];
            }
        }
        for (std::size_t k = 0; k < rowSize; ++k) {
            row[k] /= (float)c.subFrames.size();
        }
    }

    return out;
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

gpu::GpuWrapRect
wrapRect(const RectI& r)
{
    return gpu::GpuWrapRect { r.x1, r.y1, r.x2, r.y2 };
}

// The case through splinewarp_main as SplineWarp::renderOpGpu records it, with no mask and full mix.
bool
renderGpu(GpuParityRuntime& runtime,
          const KernelCase& c,
          std::vector<float>* out)
{
    gpu::GpuRuntime& vk = runtime.runtime();
    std::vector<float> src = c.src;
    std::vector<float> dst((std::size_t)c.window.area() * c.nComps, std::numeric_limits<float>::quiet_NaN());

    SplineWarpGpuParams params;
    params.filter = (uint32_t)c.filter;
    params.flags = kSplineWarpGpuSample | ((c.clamp && Resampler::filterHonoursClamp(c.filter)) ? kSplineWarpGpuClamp : 0u)
        | (c.blackOutside ? kSplineWarpGpuBlackOutside : 0u) | ((c.filter != Resampler::eFilterImpulse) ? kSplineWarpGpuJacobian : 0u);
    std::vector<uint32_t> words;
    SplineWarpFieldGpuLayout layout;
    if (c.subFrames.empty()) {
        c.field->getGpuTables(&words, &layout);
        if (!words.empty()) {
            setSplineWarpGpuField(layout, c.field->getWindow(), &params);
        }
    } else {
        // As SplineWarp::renderOpGpu: the sub-frame table, then each field's tables.
        words.assign(c.subFrames.size() * kSplineWarpGpuSubFrameWords, 0u);
        for (std::size_t i = 0; i < c.subFrames.size(); ++i) {
            std::vector<uint32_t> tables;
            SplineWarpFieldGpuLayout subLayout;
            if (c.subFrames[i]) {
                c.subFrames[i]->getGpuTables(&tables, &subLayout);
            }
            setSplineWarpGpuSubFrame(tables.empty() ? NULL : &subLayout, tables.empty() ? RectI() : c.subFrames[i]->getWindow(), (uint32_t)words.size(),
                                     &words[i * kSplineWarpGpuSubFrameWords]);
            words.insert(words.end(), tables.begin(), tables.end());
        }
        params.subFrames = (uint32_t)c.subFrames.size();
    }
    if (c.hasMatrix) {
        setSplineWarpGpuMatrix(c.matrix, &params);
    }
    std::vector<float> paramWords(sizeof(params) / sizeof(float));
    std::memcpy(paramWords.data(), &params, sizeof(params));

    gpu::GpuBuffer* srcBuffer = runtime.buffer(0, src.size());
    gpu::GpuBuffer* dstBuffer = runtime.buffer(1, dst.size());
    gpu::GpuBuffer* paramBuffer = runtime.buffer(2, paramWords.size());
    gpu::GpuBuffer* fieldBuffer = words.empty() ? NULL : runtime.buffer(3, words.size());
    if (!srcBuffer || !dstBuffer || !paramBuffer || (!words.empty() && !fieldBuffer)) {
        return false;
    }
    if (!transfer(vk, *srcBuffer, src.data(), src.size() * sizeof(float), true) || !transfer(vk, *dstBuffer, dst.data(), dst.size() * sizeof(float), true)
        || !transfer(vk, *paramBuffer, paramWords.data(), paramWords.size() * sizeof(float), true)
        || (fieldBuffer && !transfer(vk, *fieldBuffer, words.data(), words.size() * sizeof(uint32_t), true))) {
        return false;
    }

    gpu::GpuWrap wrap;
    wrap.src.buffer = srcBuffer;
    wrap.src.bounds = wrapRect(c.srcBounds);
    wrap.src.strideFloats = (uint32_t)(c.srcBounds.width() * c.nComps);
    wrap.src.nComps = (uint32_t)c.nComps;
    wrap.dst.buffer = dstBuffer;
    wrap.dst.bounds = wrapRect(c.window);
    wrap.dst.strideFloats = (uint32_t)(c.window.width() * c.nComps);
    wrap.dst.nComps = (uint32_t)c.nComps;
    wrap.output = wrapRect(c.window);
    wrap.nComps = (uint32_t)c.nComps;
    gpu::GpuWrapView fieldView;
    if (fieldBuffer) {
        fieldView.buffer = fieldBuffer;
        fieldView.bounds = gpu::GpuWrapRect { 0, 0, (int32_t)words.size(), 1 };
        fieldView.strideFloats = (uint32_t)words.size();
        fieldView.nComps = 1;
    }

    std::unique_ptr<gpu::GpuRecordContext> context;
    const gpu::GpuStatus created = vk.newRecordContext(context);
    EXPECT_TRUE(bool(created)) << created.message;
    if (!created) {
        return false;
    }
    const gpu::GpuStatus recorded = gpu::recordWrappedInputs(*context, gpu::GpuKernelId::splinewarp, wrap, paramBuffer,
                                                             std::span<const gpu::GpuWrapView>(&fieldView, 1));
    EXPECT_TRUE(bool(recorded)) << recorded.message;
    if (!recorded) {
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

void
expectParity(GpuParityRuntime& runtime,
             const KernelCase& c)
{
    const std::vector<float> cpu = renderCpu(c);
    std::vector<float> gpu;
    ASSERT_TRUE(renderGpu(runtime, c, &gpu)) << describe(c);
    for (std::size_t i = 0; i < gpu.size(); ++i) {
        ASSERT_TRUE(std::isfinite(gpu[i])) << describe(c) << ": float " << i << " is " << gpu[i];
    }
    const ImageDiff diff = compareImages(cpu, gpu, ParityTolerance::resampling());
    EXPECT_EQ(0u, diff.mismatches) << describe(c) << ": first at float " << diff.firstMismatchIndex << " gpu " << gpu[diff.firstMismatchIndex] << " cpu "
                                   << cpu[diff.firstMismatchIndex] << ", worst " << diff.worstAbsolute;
}

#endif // NATRON_HAVE_GPU

const Resampler::FilterEnum kFilters[] = { Resampler::eFilterImpulse, Resampler::eFilterBox, Resampler::eFilterBilinear, Resampler::eFilterCubic,
                                           Resampler::eFilterKeys, Resampler::eFilterSimon, Resampler::eFilterRifman, Resampler::eFilterMitchell,
                                           Resampler::eFilterParzen, Resampler::eFilterNotch };

const RectI kWindow(0, 0, 36, 28);

} // namespace

// A translation: every pixel samples the same offset, with a unit Jacobian.
TEST(SplineWarpGpuKernel, UniformFieldMatchesTheCpu)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const std::vector<SplineWarpBoundaryShape> none;
    const SplineWarpField field(translation(3.25, -1.75), none, kWindow, 1., 1., 4.);
    ASSERT_FALSE(field.isIdentity());
    for (int nComps : { 1, 3, 4 }) {
        for (Resampler::FilterEnum filter : { Resampler::eFilterImpulse, Resampler::eFilterBilinear, Resampler::eFilterCubic }) {
            for (bool blackOutside : { true, false }) {
                KernelCase c = makeCase("uniform", field, nComps);
                c.filter = filter;
                c.blackOutside = blackOutside;
                expectParity(*runtime, c);
            }
        }
    }
#endif
}

// An affine map at full and anisotropic scale: a uniform Jacobian other than the identity.
TEST(SplineWarpGpuKernel, AffineFieldMatchesTheCpu)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const std::vector<SplineWarpBoundaryShape> none;
    const SplineWarpTps map = affineMap();
    const SplineWarpField full(map, none, kWindow, 1., 1., 3.);
    const SplineWarpField scaled(map, none, RectI(0, 0, 18, 28), 0.5, 1., 3.);
    ASSERT_FALSE(full.isIdentity());
    ASSERT_FALSE(scaled.isIdentity());
    for (const SplineWarpField* field : { &full, &scaled }) {
        for (bool clamp : { false, true }) {
            KernelCase c = makeCase((field == &full) ? "affine" : "affine at scale (0.5, 1)", *field);
            c.filter = Resampler::eFilterCubic;
            c.clamp = clamp;
            expectParity(*runtime, c);
        }
    }
#endif
}

// Each filter, clamped and not, over a field whose Jacobian varies from pixel to pixel.
TEST(SplineWarpGpuKernel, EachFilterMatchesTheCpu)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const std::vector<SplineWarpBoundaryShape> none;
    const SplineWarpField field(bendMap(), none, kWindow, 1., 1., 2.);
    ASSERT_FALSE(field.isIdentity());
    for (Resampler::FilterEnum filter : kFilters) {
        for (bool clamp : { false, true }) {
            KernelCase c = makeCase("bend", field);
            c.filter = filter;
            c.clamp = clamp;
            expectParity(*runtime, c);
        }
    }
#endif
}

// A boundary: pixels outside its feather are the Source's own, the rest warped.
TEST(SplineWarpGpuKernel, BoundaryFeatherMatchesTheCpu)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const SplineWarpField field(bendMap(), squareBoundary(), kWindow, 1., 1., 2.);
    ASSERT_FALSE(field.isIdentity());
    for (Resampler::FilterEnum filter : { Resampler::eFilterImpulse, Resampler::eFilterCubic, Resampler::eFilterNotch }) {
        KernelCase c = makeCase("boundary", field);
        c.filter = filter;
        expectParity(*runtime, c);
    }
#endif
}

// Without field tables every pixel is its own source: the Source copied, unsoftened by the filter.
TEST(SplineWarpGpuKernel, IdentityFieldCopiesTheSource)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const std::vector<SplineWarpBoundaryShape> none;
    const SplineWarpField field(translation(0., 0.), none, kWindow, 1., 1., 4.);
    ASSERT_TRUE(field.isIdentity());
    std::vector<uint32_t> words;
    SplineWarpFieldGpuLayout layout;
    field.getGpuTables(&words, &layout);
    EXPECT_TRUE(words.empty());
    KernelCase c = makeCase("identity", field);
    c.filter = Resampler::eFilterCubic;
    expectParity(*runtime, c);
#endif
}

// The warp's sources through the inverse of a projective upstream matrix, the Jacobian chained.
TEST(SplineWarpGpuKernel, ConcatenatedMatrixMatchesTheCpu)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const std::vector<SplineWarpBoundaryShape> none;
    const SplineWarpField field(bendMap(), none, kWindow, 1., 1., 2.);
    const double inverse[9] = { 0.9, 0.1, 2.25, -0.05, 1.1, -1.5, 0.0015, -0.001, 1. };
    for (Resampler::FilterEnum filter : { Resampler::eFilterImpulse, Resampler::eFilterBilinear, Resampler::eFilterCubic, Resampler::eFilterParzen }) {
        KernelCase c = makeCase("matrix", field);
        c.filter = filter;
        c.hasMatrix = true;
        std::copy(inverse, inverse + 9, c.matrix);
        expectParity(*runtime, c);
    }
#endif
}

// Motion blur: several fields, one moving nothing and one bounded, averaged in one dispatch.
TEST(SplineWarpGpuKernel, AveragedSubFramesMatchTheCpu)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const std::vector<SplineWarpBoundaryShape> none;
    const SplineWarpField first(translation(0.75, -0.25), none, kWindow, 1., 1., 4.);
    std::vector<std::shared_ptr<const SplineWarpField>> subFrames;
    subFrames.push_back(std::make_shared<const SplineWarpField>(first));
    subFrames.push_back(std::shared_ptr<const SplineWarpField>());
    subFrames.push_back(std::make_shared<const SplineWarpField>(translation(2.5, 1.25), none, kWindow, 1., 1., 4.));
    subFrames.push_back(std::make_shared<const SplineWarpField>(bendMap(), squareBoundary(), kWindow, 1., 1., 2.));
    subFrames.push_back(std::make_shared<const SplineWarpField>(affineMap(), none, kWindow, 1., 1., 3.));
    for (int nComps : { 1, 4 }) {
        for (Resampler::FilterEnum filter : { Resampler::eFilterImpulse, Resampler::eFilterBilinear, Resampler::eFilterCubic }) {
            KernelCase c = makeCase("sub-frames", first, nComps);
            c.filter = filter;
            c.subFrames = subFrames;
            expectParity(*runtime, c);
        }
    }
#endif
}

// ---------------------------------------------------------------------------------------------
// Node-level cases

namespace {

SplineWarpContextPtr
contextOf(const NodePtr& node)
{
    return node ? std::dynamic_pointer_cast<SplineWarpContext>(node->getCurveContext()) : SplineWarpContextPtr();
}

StagedImageEffect*
effectOf(const NodePtr& node)
{
    return node ? dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get()) : NULL;
}

// A closed Bezier through points, keyed at kTime.
BezierPtr
makeCurve(const SplineWarpContextPtr& ctx,
          const std::vector<std::pair<double, double>>& points)
{
    BezierPtr bezier = ctx->makeBezier(points[0].first, points[0].second, kRotoBezierBaseName, kTime, false);
    if (!bezier) {
        return bezier;
    }
    for (std::size_t i = 1; i < points.size(); ++i) {
        bezier->addControlPoint(points[i].first, points[i].second, kTime);
    }
    bezier->setCurveFinished(true);

    return bezier;
}

// A quadrilateral at (x, y), scaled by s about its centre, then moved by (dx, dy).
BezierPtr
makeQuad(const SplineWarpContextPtr& ctx,
         double x,
         double y,
         double size,
         double dx = 0.,
         double dy = 0.,
         double s = 1.)
{
    const double base[4][2] = { { x, y }, { x + size, y }, { x + size * 1.2, y + size }, { x - size * 0.1, y + size * 0.8 } };
    const double cx = x + size * 0.5;
    const double cy = y + size * 0.5;
    std::vector<std::pair<double, double>> points;
    for (int i = 0; i < 4; ++i) {
        points.push_back(std::make_pair(cx + s * (base[i][0] - cx) + dx, cy + s * (base[i][1] - cy) + dy));
    }

    return makeCurve(ctx, points);
}

BezierPtr
makeRectangle(const SplineWarpContextPtr& ctx,
              double x1,
              double y1,
              double x2,
              double y2)
{
    std::vector<std::pair<double, double>> points;
    points.push_back(std::make_pair(x1, y1));
    points.push_back(std::make_pair(x2, y1));
    points.push_back(std::make_pair(x2, y2));
    points.push_back(std::make_pair(x1, y2));

    return makeCurve(ctx, points);
}

// A link that moves, grows and shears a quadrilateral, so the field is neither uniform nor affine.
bool
linkBend(const SplineWarpContextPtr& ctx)
{
    BezierPtr from = makeQuad(ctx, 18., 13., 20.);
    BezierPtr to = makeQuad(ctx, 18., 13., 20., 2.75, -1.5, 1.15);
    if (!from || !to) {
        return false;
    }

    return bool(ctx->linkWithAlignment(from, to, 0., false));
}

void
expectFramesMatch(const EndToEndResult& result,
                  const std::string& what)
{
    ASSERT_EQ(result.cpu.size(), result.gpu.size()) << what;
    const ImageDiff diff = compareImages(result.cpu, result.gpu, ParityTolerance::resampling());
    EXPECT_EQ(0u, diff.mismatches) << what << ": worst " << diff.worstAbsolute << ", first at float " << diff.firstMismatchIndex;
}

#ifdef NATRON_HAVE_GPU
// Caps every context's storage buffer range for the scope.
class StorageRangeCap {
public:
    explicit StorageRangeCap(std::uint64_t bytes)
    {
        gpu::GpuRecordContext::setMaxStorageBufferBytesForTests(bytes);
    }

    ~StorageRangeCap()
    {
        gpu::GpuRecordContext::setMaxStorageBufferBytesForTests(0);
    }

    StorageRangeCap(const StorageRangeCap&) = delete;
    StorageRangeCap& operator=(const StorageRangeCap&) = delete;
};
#endif

} // namespace

class NativeSplineWarpGpuTest
    : public BaseTest {
protected:
    // A 64x48 project format, the parity source's size.
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeSplineWarpGpu64x48", 1.));
    }

    NodePtr makeSource()
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
        EXPECT_TRUE(bool(source));
        if (source) {
            setParitySourceOrigin(source, 0, 0);
        }

        return source;
    }

    NodePtr makeNode(const char* id,
                     int major,
                     const NodePtr& input)
    {
        NodePtr node = createNode(QString::fromUtf8(id), major);

        EXPECT_TRUE(bool(node)) << id;
        if (node && input) {
            connectNodes(input, node, 0, true);
        }

        return node;
    }

    // A SplineWarp reading input with linkBend()'s link, solved by the mesh when mesh is set.
    NodePtr makeWarp(const NodePtr& input,
                     bool mesh)
    {
        NodePtr node = makeNode(PLUGINID_NATRON_SPLINEWARP, PLUGIN_MAJOR_NATRON_SPLINEWARP, input);
        if (!node) {
            return node;
        }
        SplineWarpContextPtr ctx = contextOf(node);
        EXPECT_TRUE(bool(ctx));
        if (!ctx || !linkBend(ctx)) {
            ADD_FAILURE() << "no link";

            return NodePtr();
        }
        if (mesh) {
            EXPECT_TRUE(setKnobValues(node, kSplineWarpParamSolver, { 1. }));
        }

        return node;
    }

    // Renders warp alone through the scheduler, expecting it on the GPU and matching the CPU frame.
    void expectWarpRunsOnTheGpu(const NodePtr& source,
                                const NodePtr& warp,
                                const std::string& what)
    {
        const RectI window = paritySourceWindow(source, kTime, 0);
        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ warp }), warp, window, kTime);
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << what << ": " << result.error;
        EXPECT_TRUE(result.ranInRegion(warp)) << what;
        EXPECT_EQ(1u, result.gpuTasksPerNode.count(warp->getScriptName_mt_safe())) << what << ": the region fell back to the CPU";
        expectFramesMatch(result, what);
    }
};

TEST_F(NativeSplineWarpGpuTest, EligibilityFollowsTheSupportedSettings)
{
#ifdef NATRON_HAVE_GPU
    const bool supported = true;
#else
    const bool supported = false;
#endif
    resetProject();
    NodePtr source = makeSource();
    NodePtr warp = makeWarp(source, false);
    StagedImageEffect* effect = effectOf(warp);
    ASSERT_TRUE(effect != NULL);

    GpuQuery q;
    q.time = kTime;
    q.nComps = 4;
    q.bitDepth = eImageBitDepthFloat;
    for (int nComps : { 1, 3, 4 }) {
        GpuQuery c = q;
        c.nComps = nComps;
        EXPECT_EQ(supported, effect->canRenderOnGpu(c)) << nComps << " components";
    }
    for (ImageBitDepthEnum depth : { eImageBitDepthByte, eImageBitDepthShort, eImageBitDepthHalf }) {
        GpuQuery c = q;
        c.bitDepth = depth;
        EXPECT_FALSE(effect->canRenderOnGpu(c)) << "bit depth " << (int)depth;
    }
    GpuQuery draft = q;
    draft.draftMode = true;
    EXPECT_EQ(supported, effect->canRenderOnGpu(draft)) << "draft";
    GpuQuery concatenated = q;
    concatenated.hasConcatenatedInput = true;
    EXPECT_FALSE(effect->canRenderOnGpu(concatenated)) << "a transform concatenated from upstream";
}

// A Filter, heavier the farther a link moves its curve at the rendered scale.
TEST_F(NativeSplineWarpGpuTest, WeightScalesWithTheDisplacement)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr still = makeNode(PLUGINID_NATRON_SPLINEWARP, PLUGIN_MAJOR_NATRON_SPLINEWARP, source);
    NodePtr moved = makeNode(PLUGINID_NATRON_SPLINEWARP, PLUGIN_MAJOR_NATRON_SPLINEWARP, source);
    ASSERT_TRUE(still && moved);
    SplineWarpContextPtr ctx = contextOf(moved);
    ASSERT_TRUE(bool(ctx));
    BezierPtr from = makeQuad(ctx, 10., 10., 12.);
    BezierPtr to = makeQuad(ctx, 10., 10., 12., 32., 0.);
    ASSERT_TRUE(from && to);
    ASSERT_TRUE(bool(ctx->linkWithAlignment(from, to, 0., false)));

    const GpuWeight none = effectOf(still)->gpuWeight(kTime, ViewIdx(0), 0);
    const GpuWeight full = effectOf(moved)->gpuWeight(kTime, ViewIdx(0), 0);
    const GpuWeight half = effectOf(moved)->gpuWeight(kTime, ViewIdx(0), 1);
    EXPECT_EQ(GpuWeight::Class::Filter, none.weightClass);
    EXPECT_EQ(GpuWeight::Class::Filter, full.weightClass);
    EXPECT_DOUBLE_EQ(kGpuFilterWeight, none.weight);
    // A translated curve pairs each sample with itself moved, up to rounding.
    EXPECT_NEAR(gpuSizeScaledWeight(32.).weight, full.weight, 1e-9);
    EXPECT_NEAR(gpuSizeScaledWeight(16.).weight, half.weight, 1e-9);
}

TEST_F(NativeSplineWarpGpuTest, TpsFrameMatchesTheCpuForEachFilter)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr warp = makeWarp(source, false);
    ASSERT_TRUE(source && warp);
    for (Resampler::FilterEnum filter : kFilters) {
        ASSERT_TRUE(setKnobValues(warp, kResamplerParamFilterType, { (double)filter }));
        for (bool clamp : { false, true }) {
            ASSERT_TRUE(setKnobValues(warp, kResamplerParamFilterClamp, { clamp ? 1. : 0. }));
            std::ostringstream what;
            what << "TPS filter " << (int)filter << " clamp " << clamp;
            expectWarpRunsOnTheGpu(source, warp, what.str());
        }
    }
}

TEST_F(NativeSplineWarpGpuTest, MeshFrameMatchesTheCpu)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr warp = makeWarp(source, true);
    ASSERT_TRUE(source && warp);
    for (bool blackOutside : { true, false }) {
        ASSERT_TRUE(setKnobValues(warp, kResamplerParamFilterBlackOutside, { blackOutside ? 1. : 0. }));
        expectWarpRunsOnTheGpu(source, warp, blackOutside ? "mesh, black outside" : "mesh");
    }
}

TEST_F(NativeSplineWarpGpuTest, BoundaryFrameMatchesTheCpu)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr warp = makeWarp(source, false);
    ASSERT_TRUE(source && warp);
    SplineWarpContextPtr ctx = contextOf(warp);
    ASSERT_TRUE(bool(ctx));
    BezierPtr boundary = makeRectangle(ctx, 14., 10., 50., 38.);
    ASSERT_TRUE(bool(boundary));
    ctx->setBoundary(boundary, true);
    expectWarpRunsOnTheGpu(source, warp, "TPS with a boundary");
    ASSERT_TRUE(setKnobValues(warp, kSplineWarpParamSolver, { 1. }));
    expectWarpRunsOnTheGpu(source, warp, "mesh with a boundary");
}

// A Transform concatenated into the warp resamples once on the CPU, the GPU path taking no
// concatenated source; the frame matches the CPU one.
TEST_F(NativeSplineWarpGpuTest, ConcatenatingATransformMatchesTheCpuFrame)
{
    resetProject();
    NodePtr source = makeSource();
    NodePtr transform = makeNode(PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM, source);
    ASSERT_TRUE(source && transform);
    ASSERT_TRUE(setKnobValues(transform, kTransformNodeParamCenter, { 32., 24. }));
    ASSERT_TRUE(setKnobValues(transform, kTransformNodeParamRotate, { 12. }));
    ASSERT_TRUE(setKnobValues(transform, kTransformNodeParamScale, { 0.9, 0.9 }));
    for (bool mesh : { false, true }) {
        NodePtr warp = makeWarp(transform, mesh);
        ASSERT_TRUE(bool(warp));
        InputMatrixMap transforms;
        warp->getEffectInstance()->tryConcatenateTransforms(kTime, false, ViewIdx(0), RenderScale::identity, &transforms);
        ASSERT_EQ(1u, transforms.count(kSplineWarpInputSource));

        const RectI window = paritySourceWindow(source, kTime, 0);
        const EndToEndResult result = renderEndToEnd(std::vector<NodePtr>({ transform, warp }), warp, window, kTime);
        if (result.skipped) {
            return;
        }
        ASSERT_TRUE(result.ok) << result.error;
        EXPECT_FALSE(result.ranInRegion(warp)) << (mesh ? "mesh" : "TPS");
        expectFramesMatch(result, mesh ? "Transform -> SplineWarp (mesh)" : "Transform -> SplineWarp (TPS)");
    }
}

// With motion blur the fields of every sub-frame average on the GPU as on the CPU.
TEST_F(NativeSplineWarpGpuTest, MotionBlurFrameMatchesTheCpu)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    for (bool mesh : { false, true }) {
        NodePtr warp = makeWarp(source, mesh);
        ASSERT_TRUE(bool(warp));
        KnobDouble* amount = dynamic_cast<KnobDouble*>(warp->getKnobByName(kSplineWarpParamAmount).get());
        ASSERT_TRUE(amount != NULL);
        amount->setValueAtTime(kTime, 0., ViewSpec::all(), 0);
        amount->setValueAtTime(kTime + 1., 1., ViewSpec::all(), 0);
        ASSERT_TRUE(setKnobValues(warp, kSplineWarpParamMotionBlur, { 1. }));
        ASSERT_TRUE(setKnobValues(warp, kSplineWarpParamShutter, { 1. }));
        SplineWarp* effect = dynamic_cast<SplineWarp*>(warp->getEffectInstance().get());
        ASSERT_TRUE(effect != NULL);
        ASSERT_EQ((std::size_t)Resampler::motionBlurMinSamples(1.), effect->getRenderTimes(kTime, ViewIdx(0)).size());
        expectWarpRunsOnTheGpu(source, warp, mesh ? "motion blur, mesh" : "motion blur, TPS");
    }
}

// A field finer than a dispatch can bind renders on the CPU; without the cap the same one runs on
// the GPU.
TEST_F(NativeSplineWarpGpuTest, OversizedFieldFallsBackToTheCpu)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    resetProject();
    NodePtr source = makeSource();
    NodePtr warp = makeWarp(source, false);
    ASSERT_TRUE(source && warp);
    // A grid point per pixel: over the 64x48 window about 110 KB of tables, past the 80 KB of
    // scratch a 100000-byte range leaves, while each 64x48 RGBA image (48 KB) still fits.
    ASSERT_TRUE(setKnobValues(warp, kSplineWarpParamFieldResolution, { 1. }));
    expectWarpRunsOnTheGpu(source, warp, "a fine field");

    const RectI window = paritySourceWindow(source, kTime, 0);
    EndToEndResult result;
    {
        const StorageRangeCap cap(100000);
        result = renderEndToEnd(std::vector<NodePtr>({ warp }), warp, window, kTime);
    }
    if (result.skipped) {
        return;
    }
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(0u, result.gpuTasksPerNode.count(warp->getScriptName_mt_safe())) << "the oversized field ran on the GPU";
    expectFramesMatch(result, "an oversized field");
#endif
}
