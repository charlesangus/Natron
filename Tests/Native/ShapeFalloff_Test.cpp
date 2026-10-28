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

#include "Tests/Native/NativeGpuParity.h"

#include "Engine/Nodes/Generator/ShapeFalloff.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuRuntime.h"
#endif

NATRON_NAMESPACE_USING

using namespace ShapeFalloff;

namespace {

const float kSqrtHalf = 0.70710678f;

ShapeEdge
edgeOf(float distance,
       float nx,
       float ny)
{
    ShapeEdge e;
    e.distance = distance;
    e.nx = nx;
    e.ny = ny;

    return e;
}

ShapeBox
boxOf(float x1,
      float y1,
      float x2,
      float y2)
{
    ShapeBox b;
    b.x1 = x1;
    b.y1 = y1;
    b.x2 = x2;
    b.y2 = y2;

    return b;
}

// The mean over an n by n grid of samples of the pixel footprint of f(offset along the normal).
template <typename F>
double
supersample(const ShapeEdge& e,
            float pixelW,
            float pixelH,
            int n,
            F f)
{
    double sum = 0.;
    for (int j = 0; j < n; ++j) {
        const double v = ((j + 0.5) / n - 0.5) * pixelH;
        for (int i = 0; i < n; ++i) {
            const double u = ((i + 0.5) / n - 0.5) * pixelW;
            sum += f(e.distance - (e.nx * u + e.ny * v));
        }
    }

    return sum / ((double)n * n);
}

} // namespace

TEST(ShapeFalloff, EaseEndpointsAndMidpoints)
{
    const float mid[kFalloffCount] = { 0.5f, 0.5f, 0.375f, 0.625f, 0.5f };
    for (int f = 0; f < kFalloffCount; ++f) {
        EXPECT_EQ(0.f, ease(0.f, f)) << f;
        EXPECT_EQ(1.f, ease(1.f, f)) << f;
        EXPECT_EQ(0.f, ease(-3.f, f)) << f;
        EXPECT_EQ(1.f, ease(7.f, f)) << f;
        EXPECT_FLOAT_EQ(mid[f], ease(0.5f, f)) << f;
    }
    EXPECT_FLOAT_EQ(0.25f * 0.25f * (3.f - 0.5f), ease(0.25f, kFalloffSmooth));
    EXPECT_FLOAT_EQ(2.f * 0.25f * 0.25f, ease(0.25f, kFalloffQuadratic));
    EXPECT_FLOAT_EQ(1.f - 2.f * 0.25f * 0.25f, ease(0.75f, kFalloffQuadratic));
    EXPECT_FLOAT_EQ(0.25f, ease(0.25f, kFalloffLinear));
}

TEST(ShapeFalloff, EaseIsMonotonicAndSymmetric)
{
    for (int f = 0; f < kFalloffCount; ++f) {
        float prev = ease(0.f, f);
        for (int i = 1; i <= 1000; ++i) {
            const float t = i / 1000.f;
            const float v = ease(t, f);
            EXPECT_GE(v, prev) << "falloff " << f << " at " << t;
            prev = v;
        }
    }
    for (int i = 0; i <= 100; ++i) {
        const float t = i / 100.f;
        for (int f : { kFalloffLinear, kFalloffSmooth, kFalloffQuadratic }) {
            EXPECT_NEAR(1.f - ease(t, f), ease(1.f - t, f), 1e-6f) << "falloff " << f << " at " << t;
        }
        EXPECT_NEAR(1.f - ease(t, kFalloffEaseOut), ease(1.f - t, kFalloffEaseIn), 1e-6f) << t;
    }
}

TEST(ShapeFalloff, RampT)
{
    EXPECT_FLOAT_EQ(0.5f, rampT(5.f, 7.f, 0.f, 0.f, 10.f, 0.f));
    EXPECT_EQ(0.f, rampT(-3.f, 1.f, 0.f, 0.f, 10.f, 0.f));
    EXPECT_EQ(1.f, rampT(20.f, 1.f, 0.f, 0.f, 10.f, 0.f));
    EXPECT_FLOAT_EQ(0.25f, rampT(3.f, 25.f, 3.f, 20.f, 3.f, 40.f));
    EXPECT_FLOAT_EQ(0.5f, rampT(2.f, 2.f, 0.f, 0.f, 4.f, 4.f));
    EXPECT_FLOAT_EQ(0.5f, rampT(4.f, 0.f, 0.f, 0.f, 4.f, 4.f));
    EXPECT_FLOAT_EQ(0.75f, rampT(1.f, 1.f, 4.f, 4.f, 0.f, 0.f));
    // Coincident points: a step with everything on p1's side, never a division by zero.
    EXPECT_EQ(1.f, rampT(-50.f, 3.f, 7.f, 7.f, 7.f, 7.f));
    EXPECT_EQ(1.f, rampT(7.f, 7.f, 7.f, 7.f, 7.f, 7.f));
}

TEST(ShapeFalloff, EllipseEdge)
{
    const ShapeBox box = boxOf(0.f, 0.f, 20.f, 10.f);
    ShapeEdge e = ellipseEdge(20.f, 5.f, box);
    EXPECT_NEAR(0.f, e.distance, 1e-6f);
    EXPECT_FLOAT_EQ(1.f, e.nx);
    EXPECT_FLOAT_EQ(0.f, e.ny);
    e = ellipseEdge(15.f, 5.f, box);
    EXPECT_NEAR(5.f, e.distance, 1e-5f);
    e = ellipseEdge(10.f, 8.f, box);
    EXPECT_NEAR(2.f, e.distance, 1e-5f);
    EXPECT_FLOAT_EQ(1.f, e.ny);
    e = ellipseEdge(10.f, 0.f, box);
    EXPECT_NEAR(0.f, e.distance, 1e-6f);
    EXPECT_FLOAT_EQ(-1.f, e.ny);
    e = ellipseEdge(25.f, 5.f, box);
    EXPECT_NEAR(-5.f, e.distance, 1e-5f);
    e = ellipseEdge(10.f, 5.f, box);
    EXPECT_NEAR(5.f, e.distance, 1e-5f);
    // On the edge at 45 degrees of the parametric angle.
    e = ellipseEdge(10.f + 10.f * kSqrtHalf, 5.f + 5.f * kSqrtHalf, box);
    EXPECT_NEAR(0.f, e.distance, 1e-5f);
    EXPECT_NEAR(1.f, e.nx * e.nx + e.ny * e.ny, 1e-6f);
    // The ellipse normal at (a cos u, b sin u) is along (b cos u, a sin u).
    EXPECT_NEAR(1.f / std::sqrt(5.f), e.nx, 1e-6f);
    EXPECT_NEAR(2.f / std::sqrt(5.f), e.ny, 1e-6f);

    EXPECT_EQ(kFarOutside, ellipseEdge(0.f, 0.f, boxOf(0.f, 0.f, 0.f, 10.f)).distance);
    EXPECT_EQ(kFarOutside, ellipseEdge(0.f, 0.f, boxOf(0.f, 5.f, 10.f, 5.f)).distance);
}

TEST(ShapeFalloff, RoundedBoxEdge)
{
    const ShapeBox box = boxOf(0.f, 0.f, 100.f, 50.f);
    ShapeEdge e = roundedBoxEdge(50.f, 25.f, box, 10.f, 10.f);
    EXPECT_NEAR(25.f, e.distance, 1e-5f);
    EXPECT_FLOAT_EQ(0.f, e.nx);
    EXPECT_FLOAT_EQ(1.f, e.ny);
    e = roundedBoxEdge(5.f, 25.f, box, 10.f, 10.f);
    EXPECT_NEAR(5.f, e.distance, 1e-5f);
    EXPECT_FLOAT_EQ(-1.f, e.nx);
    e = roundedBoxEdge(105.f, 25.f, box, 10.f, 10.f);
    EXPECT_NEAR(-5.f, e.distance, 1e-5f);
    EXPECT_FLOAT_EQ(1.f, e.nx);
    e = roundedBoxEdge(30.f, 48.f, box, 10.f, 10.f);
    EXPECT_NEAR(2.f, e.distance, 1e-5f);
    EXPECT_FLOAT_EQ(1.f, e.ny);

    // In a corner: the circle of radius 10 centred 10 in from the corner.
    e = roundedBoxEdge(5.f, 5.f, box, 10.f, 10.f);
    EXPECT_NEAR(10.f - 5.f * std::sqrt(2.f), e.distance, 1e-5f);
    EXPECT_NEAR(-kSqrtHalf, e.nx, 1e-6f);
    EXPECT_NEAR(-kSqrtHalf, e.ny, 1e-6f);
    e = roundedBoxEdge(100.f - 10.f + 10.f * kSqrtHalf, 50.f - 10.f + 10.f * kSqrtHalf, box, 10.f, 10.f);
    EXPECT_NEAR(0.f, e.distance, 1e-5f);
    EXPECT_NEAR(kSqrtHalf, e.nx, 1e-6f);
    EXPECT_NEAR(kSqrtHalf, e.ny, 1e-6f);
    // The box corner itself lies outside the rounded corner.
    e = roundedBoxEdge(0.f, 0.f, box, 10.f, 10.f);
    EXPECT_NEAR(10.f - 10.f * std::sqrt(2.f), e.distance, 1e-5f);
    // No rounding: the corner point is on the edge.
    EXPECT_FLOAT_EQ(0.f, roundedBoxEdge(0.f, 0.f, box, 0.f, 10.f).distance);
    EXPECT_FLOAT_EQ(0.f, roundedBoxEdge(0.f, 0.f, box, -4.f, -4.f).distance);

    // A radius past half the shorter side is clamped to it, 25: the left side becomes a half
    // disc of radius 25 centred at (25, 25).
    e = roundedBoxEdge(25.f - 25.f * kSqrtHalf, 25.f + 25.f * kSqrtHalf, box, 100.f, 80.f);
    EXPECT_NEAR(0.f, e.distance, 1e-5f);
    e = roundedBoxEdge(10.f, 25.f, box, 100.f, 80.f);
    EXPECT_NEAR(10.f, e.distance, 1e-5f);
    EXPECT_FLOAT_EQ(-1.f, e.nx);
    e = roundedBoxEdge(5.f, 45.f, box, 100.f, 80.f);
    const float r = std::sqrt(20.f * 20.f + 20.f * 20.f);
    EXPECT_NEAR(25.f - r, e.distance, 1e-5f);

    EXPECT_EQ(kFarOutside, roundedBoxEdge(0.f, 0.f, boxOf(3.f, 0.f, 3.f, 10.f), 0.f, 0.f).distance);
}

TEST(ShapeFalloff, PixelCoverageHandValues)
{
    EXPECT_FLOAT_EQ(0.5f, pixelCoverage(edgeOf(0.f, 1.f, 0.f), 1.f, 1.f));
    EXPECT_FLOAT_EQ(0.75f, pixelCoverage(edgeOf(0.25f, 1.f, 0.f), 1.f, 1.f));
    EXPECT_FLOAT_EQ(0.25f, pixelCoverage(edgeOf(-0.25f, 0.f, -1.f), 1.f, 1.f));
    EXPECT_EQ(1.f, pixelCoverage(edgeOf(0.5f, 1.f, 0.f), 1.f, 1.f));
    EXPECT_EQ(0.f, pixelCoverage(edgeOf(-0.5f, 1.f, 0.f), 1.f, 1.f));
    // A pixel two units wide.
    EXPECT_FLOAT_EQ(0.75f, pixelCoverage(edgeOf(0.5f, 1.f, 0.f), 2.f, 1.f));
    EXPECT_FLOAT_EQ(0.75f, pixelCoverage(edgeOf(0.125f, 0.f, 1.f), 2.f, 0.5f));
    // A diagonal edge through the centre halves the pixel; one cutting a corner triangle of legs
    // 0.6 covers 0.18.
    EXPECT_FLOAT_EQ(0.5f, pixelCoverage(edgeOf(0.f, kSqrtHalf, kSqrtHalf), 1.f, 1.f));
    EXPECT_NEAR(0.18f, pixelCoverage(edgeOf((0.6f - 1.f) * kSqrtHalf, kSqrtHalf, kSqrtHalf), 1.f, 1.f), 1e-6f);
    EXPECT_NEAR(0.82f, pixelCoverage(edgeOf((1.f - 0.6f) * kSqrtHalf, kSqrtHalf, kSqrtHalf), 1.f, 1.f), 1e-6f);
}

// The coverage of a half-plane, and the band averaged over the pixel, against a brute-force
// supersampled pixel, at many angles, footprints and distances.
TEST(ShapeFalloff, CoverageMatchesSupersampling)
{
    const int n = 100;
    for (int angle = 0; angle < 12; ++angle) {
        const float th = angle * 0.5236f + 0.01f;
        const float nx = std::cos(th);
        const float ny = std::sin(th);
        for (float pw : { 1.f, 0.5f, 2.f }) {
            for (float ph : { 1.f, 3.f }) {
                for (int k = -20; k <= 20; ++k) {
                    const ShapeEdge e = edgeOf(k * 0.11f, nx, ny);
                    const double ref = supersample(e, pw, ph, n, [](double d) { return d >= 0. ? 1. : 0.; });
                    EXPECT_NEAR(ref, pixelCoverage(e, pw, ph), 1.0 / n) << "angle " << th << " d " << e.distance;
                    for (float s : { 0.3f, 1.f, 4.f }) {
                        const double band = supersample(e, pw, ph, n, [s](double d) { return std::min(1., std::max(0., d / s)); });
                        EXPECT_NEAR(band, bandCoverage(e, s, pw, ph), 1e-3) << "angle " << th << " d " << e.distance << " s " << s;
                    }
                }
            }
        }
    }
}

TEST(ShapeFalloff, BandCoverage)
{
    const ShapeEdge axis = edgeOf(0.f, 1.f, 0.f);
    EXPECT_EQ(pixelCoverage(axis, 1.f, 1.f), bandCoverage(axis, 0.f, 1.f, 1.f));
    EXPECT_EQ(pixelCoverage(edgeOf(0.3f, 0.6f, 0.8f), 1.f, 1.f), bandCoverage(edgeOf(0.3f, 0.6f, 0.8f), 0.f, 1.f, 1.f));
    // Linear across the band away from its ends, so the average is the centre's value.
    EXPECT_FLOAT_EQ(0.5f, bandCoverage(edgeOf(5.f, 1.f, 0.f), 10.f, 1.f, 1.f));
    EXPECT_FLOAT_EQ(0.25f, bandCoverage(edgeOf(2.5f, 0.f, 1.f), 10.f, 1.f, 1.f));
    // At the edge: the mean of max(-u, 0) / 10 over u in [-0.5, 0.5].
    EXPECT_FLOAT_EQ(0.0125f, bandCoverage(axis, 10.f, 1.f, 1.f));
    EXPECT_EQ(0.f, bandCoverage(edgeOf(-0.5f, 1.f, 0.f), 10.f, 1.f, 1.f));
    EXPECT_EQ(1.f, bandCoverage(edgeOf(10.5f, 1.f, 0.f), 10.f, 1.f, 1.f));
    EXPECT_EQ(1.f, bandCoverage(edgeOf(1e6f, 1.f, 0.f), 10.f, 1.f, 1.f));
    EXPECT_EQ(0.f, bandCoverage(edgeOf(kFarOutside, 1.f, 0.f), 10.f, 1.f, 1.f));

    float prev = 0.f;
    for (int i = -200; i <= 1200; ++i) {
        const float v = bandCoverage(edgeOf(i * 0.01f, 0.6f, 0.8f), 7.f, 1.f, 1.f);
        EXPECT_GE(v, prev) << i;
        prev = v;
    }
    EXPECT_EQ(1.f, prev);
}

TEST(ShapeFalloff, ShapeTAndWeight)
{
    const ShapeBox box = boxOf(0.f, 0.f, 20.f, 10.f);
    EXPECT_EQ(1.f, ellipseT(10.f, 5.f, box, 4.f, 1.f, 1.f));
    EXPECT_EQ(1.f, ellipseT(13.f, 5.f, box, 4.f, 1.f, 1.f));
    EXPECT_FLOAT_EQ(0.5f, ellipseT(18.f, 5.f, box, 4.f, 1.f, 1.f));
    EXPECT_FLOAT_EQ(0.5f, ellipseT(20.f, 5.f, box, 0.f, 1.f, 1.f));
    EXPECT_EQ(0.f, ellipseT(25.f, 5.f, box, 0.f, 1.f, 1.f));

    const ShapeBox rect = boxOf(0.f, 0.f, 100.f, 50.f);
    EXPECT_FLOAT_EQ(0.5f, roundedBoxT(4.f, 25.f, rect, 0.f, 0.f, 8.f, 1.f, 1.f));
    EXPECT_FLOAT_EQ(0.5f, roundedBoxT(0.f, 25.f, rect, 10.f, 10.f, 0.f, 1.f, 1.f));
    EXPECT_EQ(1.f, roundedBoxT(50.f, 25.f, rect, 10.f, 10.f, 8.f, 1.f, 1.f));
    EXPECT_EQ(0.f, roundedBoxT(0.5f, 0.5f, rect, 10.f, 10.f, 0.f, 1.f, 1.f));

    const ShapeEdge e = edgeOf(1.5f, 0.f, 1.f);
    for (int f = 0; f < kFalloffCount; ++f) {
        EXPECT_FLOAT_EQ(ease(bandCoverage(e, 6.f, 1.f, 1.f), f), shapeWeight(e, 6.f, f, 1.f, 1.f)) << f;
    }
    EXPECT_FLOAT_EQ(ease(0.25f, kFalloffSmooth), shapeWeight(e, 6.f, kFalloffSmooth, 1.f, 1.f));
}

#ifdef NATRON_HAVE_GPU

namespace {

// shapefalloff_test.slang's ShapeFalloffTestParams.
struct ShapeFalloffTestParams {
    uint32_t count;
    uint32_t mode;
    int32_t falloff;
    float softness;
    ShapeBox box;
    float cornerX;
    float cornerY;
    float pixelW;
    float pixelH;
    float p0x;
    float p0y;
    float p1x;
    float p1y;
};
static_assert(std::is_trivially_copyable<ShapeFalloffTestParams>::value, "pushed to the GPU");
static_assert(sizeof(ShapeFalloffTestParams) == 64, "must match shapefalloff_test.slang");

enum Mode : uint32_t {
    kModeEase = 0,
    kModeRamp = 1,
    kModeEllipseEdge = 2,
    kModeRoundedBoxEdge = 3,
    kModePixelCoverage = 4,
    kModeBandCoverage = 5,
    kModeEllipseT = 6,
    kModeRoundedBoxT = 7,
    kModeWeight = 8,
};

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

// Runs the test entry on four floats per case, giving four floats per case.
bool
deviceRun(GpuParityRuntime& runtime,
          ShapeFalloffTestParams params,
          std::vector<float> input,
          std::vector<float>* output)
{
    gpu::GpuRuntime& vk = runtime.runtime();
    const uint32_t count = (uint32_t)(input.size() / 4);
    params.count = count;
    gpu::GpuBuffer* in = runtime.buffer(0, input.size());
    gpu::GpuBuffer* out = runtime.buffer(1, input.size());
    if (!in || !out || !transfer(vk, *in, input.data(), input.size() * sizeof(float), true)) {
        return false;
    }
    std::unique_ptr<gpu::GpuRecordContext> context;
    const gpu::GpuStatus created = vk.newRecordContext(context);
    EXPECT_TRUE(bool(created)) << created.message;
    if (!created) {
        return false;
    }
    const gpu::GpuStatus dispatched = context->dispatch(gpu::GpuKernelId::shapefalloff_test, { in, out }, params, { count, 1u, 1u });
    EXPECT_TRUE(bool(dispatched)) << dispatched.message;
    if (!dispatched) {
        return false;
    }
    const gpu::GpuStatus submitted = context->submitAndWait();
    EXPECT_TRUE(bool(submitted)) << submitted.message;
    context.reset();
    if (!submitted) {
        return false;
    }
    output->assign(input.size(), 0.f);

    return transfer(vk, *out, output->data(), output->size() * sizeof(float), false);
}

// The CPU's four outputs for a case, as the test entry computes them.
void
cpuRun(const ShapeFalloffTestParams& p,
       const float* in,
       float* out)
{
    out[0] = out[1] = out[2] = out[3] = 0.f;
    const ShapeEdge e = edgeOf(in[0], in[1], in[2]);
    switch (p.mode) {
    case kModeEase:
        out[0] = ease(in[0], p.falloff);
        break;
    case kModeRamp:
        out[0] = rampT(in[0], in[1], p.p0x, p.p0y, p.p1x, p.p1y);
        break;
    case kModeEllipseEdge:
    case kModeRoundedBoxEdge: {
        const ShapeEdge s = p.mode == kModeEllipseEdge ? ellipseEdge(in[0], in[1], p.box) : roundedBoxEdge(in[0], in[1], p.box, p.cornerX, p.cornerY);
        out[0] = s.distance;
        out[1] = s.nx;
        out[2] = s.ny;
        break;
    }
    case kModePixelCoverage:
        out[0] = pixelCoverage(e, p.pixelW, p.pixelH);
        break;
    case kModeBandCoverage:
        out[0] = bandCoverage(e, p.softness, p.pixelW, p.pixelH);
        break;
    case kModeEllipseT:
        out[0] = ellipseT(in[0], in[1], p.box, p.softness, p.pixelW, p.pixelH);
        break;
    case kModeRoundedBoxT:
        out[0] = roundedBoxT(in[0], in[1], p.box, p.cornerX, p.cornerY, p.softness, p.pixelW, p.pixelH);
        break;
    default:
        out[0] = shapeWeight(e, p.softness, p.falloff, p.pixelW, p.pixelH);
        break;
    }
}

ShapeFalloffTestParams
baseParams(uint32_t mode)
{
    ShapeFalloffTestParams p;
    std::memset(&p, 0, sizeof(p));
    p.mode = mode;
    p.box = boxOf(-30.f, 12.f, 170.f, 87.f);
    p.cornerX = 14.f;
    p.cornerY = 9.f;
    p.softness = 6.5f;
    p.pixelW = 1.f;
    p.pixelH = 1.f;
    p.p0x = 13.f;
    p.p0y = -4.f;
    p.p1x = 211.f;
    p.p1y = 97.f;

    return p;
}

} // namespace

// Every helper on the GPU against the C++, within 1e-6 (relative above 1 for distances).
TEST(ShapeFalloffGpu, MatchesCpu)
{
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    std::mt19937 rng(20261028u);
    std::uniform_real_distribution<float> unit(0.f, 1.f);
    const std::size_t count = 4096;
    // Points over the box and a margin round it, edges at random angles and distances near the
    // pixel and the band, and t over and beyond [0, 1].
    std::vector<float> points(count * 4, 0.f);
    std::vector<float> edges(count * 4, 0.f);
    std::vector<float> ts(count * 4, 0.f);
    for (std::size_t i = 0; i < count; ++i) {
        points[i * 4 + 0] = -50.f + 240.f * unit(rng);
        points[i * 4 + 1] = -10.f + 120.f * unit(rng);
        const float th = 6.2831853f * unit(rng);
        edges[i * 4 + 0] = -3.f + 14.f * unit(rng);
        edges[i * 4 + 1] = std::cos(th);
        edges[i * 4 + 2] = std::sin(th);
        ts[i * 4 + 0] = -0.2f + 1.4f * unit(rng);
    }

    struct Case {
        std::string name;
        ShapeFalloffTestParams params;
        const std::vector<float>* input;
    };
    std::vector<Case> cases;
    for (int f = 0; f < kFalloffCount; ++f) {
        ShapeFalloffTestParams p = baseParams(kModeEase);
        p.falloff = f;
        cases.push_back({ "ease " + std::to_string(f), p, &ts });
        p = baseParams(kModeWeight);
        p.falloff = f;
        cases.push_back({ "weight " + std::to_string(f), p, &edges });
    }
    cases.push_back({ "ramp", baseParams(kModeRamp), &points });
    ShapeFalloffTestParams coincident = baseParams(kModeRamp);
    coincident.p1x = coincident.p0x;
    coincident.p1y = coincident.p0y;
    cases.push_back({ "ramp coincident", coincident, &points });
    cases.push_back({ "ellipse edge", baseParams(kModeEllipseEdge), &points });
    cases.push_back({ "rounded box edge", baseParams(kModeRoundedBoxEdge), &points });
    for (float s : { 0.f, 0.4f, 6.5f }) {
        for (float pw : { 1.f, 0.25f }) {
            const std::string suffix = " softness " + std::to_string(s) + " pixel " + std::to_string(pw);
            for (uint32_t mode : { kModePixelCoverage, kModeBandCoverage }) {
                ShapeFalloffTestParams p = baseParams(mode);
                p.softness = s;
                p.pixelW = pw;
                p.pixelH = 1.f / pw;
                cases.push_back({ (mode == kModePixelCoverage ? "coverage" : "band") + suffix, p, &edges });
            }
            for (uint32_t mode : { kModeEllipseT, kModeRoundedBoxT }) {
                ShapeFalloffTestParams p = baseParams(mode);
                p.softness = s;
                p.pixelW = pw;
                p.pixelH = pw;
                cases.push_back({ (mode == kModeEllipseT ? "ellipseT" : "roundedBoxT") + suffix, p, &points });
            }
        }
    }

    for (const Case& c : cases) {
        std::vector<float> got;
        ASSERT_TRUE(deviceRun(*runtime, c.params, *c.input, &got)) << c.name;
        std::size_t mismatches = 0;
        float worst = 0.f;
        for (std::size_t i = 0; i < count; ++i) {
            float expected[4];
            cpuRun(c.params, &(*c.input)[i * 4], expected);
            for (int k = 0; k < 4; ++k) {
                const float diff = std::fabs(expected[k] - got[i * 4 + k]);
                const float tolerance = 1e-6f * std::max(1.f, std::fabs(expected[k]));
                worst = std::max(worst, diff);
                if (!(diff <= tolerance)) {
                    if (mismatches < 5) {
                        ADD_FAILURE() << c.name << " case " << i << " output " << k << ": cpu " << expected[k] << " gpu " << got[i * 4 + k];
                    }
                    ++mismatches;
                }
            }
        }
        EXPECT_EQ(0u, mismatches) << c.name << ", worst difference " << worst;
    }
}

#else // NATRON_HAVE_GPU

TEST(ShapeFalloffGpu, MatchesCpu)
{
    reportNoGpu("built without GPU support");
}

#endif // NATRON_HAVE_GPU
