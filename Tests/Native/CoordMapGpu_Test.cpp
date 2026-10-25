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

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Tests/Native/NativeGpuParity.h"
#include "Tests/NativeParity.h"

#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/RectI.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuResample.h"
#include "Engine/Gpu/GpuRuntime.h"
#endif

NATRON_NAMESPACE_USING

// coordmap_test.slang's entry against Resampler::resampleRowFromCoords: the same source, per-pixel
// coordinates and Jacobians, sampled on the CPU and on the device.

namespace {

// One sampling of a coordinate grid of width x height destination pixels.
struct CoordCase {
    std::string name;
    RectI bounds;
    int nComps = 4;
    std::vector<float> src;
    int width = 0;
    int height = 0;
    // sx, sy, then Jxx, Jxy, Jyx, Jyy per pixel, row by row.
    std::vector<float> coords;
    bool hasJacobian = false;
    Resampler::FilterEnum filter = Resampler::eFilterImpulse;
    bool clamp = false;
    bool blackOutside = true;
    Resampler::WrapMode wrapU = Resampler::eWrapNone;
    Resampler::WrapMode wrapV = Resampler::eWrapNone;
    // The source's bounds when null.
    RectI domain;
};

const int kCoordWords = 6;

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

// A ramp: x / 4 in the first channel, y / 4 in the second, their sum in the others.
std::vector<float>
makeRamp(const RectI& bounds,
         int nComps)
{
    std::vector<float> pixels((std::size_t)bounds.area() * nComps);
    for (int y = 0; y < bounds.height(); ++y) {
        for (int x = 0; x < bounds.width(); ++x) {
            float* p = &pixels[((std::size_t)y * bounds.width() + x) * nComps];
            for (int c = 0; c < nComps; ++c) {
                p[c] = (c == 0) ? x / 4.f : (c == 1) ? y / 4.f
                                                     : (x + y) / 8.f;
            }
        }
    }

    return pixels;
}

void
setCoord(CoordCase& c,
         int i,
         int j,
         float sx,
         float sy,
         float Jxx = 0.f,
         float Jxy = 0.f,
         float Jyx = 0.f,
         float Jyy = 0.f)
{
    float* w = &c.coords[((std::size_t)j * c.width + i) * kCoordWords];
    w[0] = sx;
    w[1] = sy;
    w[2] = Jxx;
    w[3] = Jxy;
    w[4] = Jyx;
    w[5] = Jyy;
}

void
resizeGrid(CoordCase& c,
           int width,
           int height)
{
    c.width = width;
    c.height = height;
    c.coords.assign((std::size_t)width * height * kCoordWords, 0.f);
}

std::vector<float>
renderCpu(const CoordCase& c)
{
    const Resampler::SourceImage src(c.src.data(), c.bounds, c.nComps);
    std::vector<float> out((std::size_t)c.width * c.height * c.nComps, 0.f);
    std::vector<float> sx(c.width);
    std::vector<float> sy(c.width);
    std::vector<float> jacobian((std::size_t)c.width * 4);
    for (int j = 0; j < c.height; ++j) {
        for (int i = 0; i < c.width; ++i) {
            const float* w = &c.coords[((std::size_t)j * c.width + i) * kCoordWords];
            sx[i] = w[0];
            sy[i] = w[1];
            for (int k = 0; k < 4; ++k) {
                jacobian[(std::size_t)i * 4 + k] = w[2 + k];
            }
        }
        Resampler::resampleRowFromCoords(src, sx.data(), sy.data(), c.hasJacobian ? jacobian.data() : 0, c.width, c.filter, c.clamp,
                                         c.blackOutside, c.wrapU, c.wrapV, &out[(std::size_t)j * c.width * c.nComps], c.nComps,
                                         c.domain.isNull() ? 0 : &c.domain);
    }

    return out;
}

const char*
wrapName(Resampler::WrapMode mode)
{
    switch (mode) {
    case Resampler::eWrapNone:
        return "none";
    case Resampler::eWrapClamp:
        return "clamp";
    case Resampler::eWrapRepeat:
        return "repeat";
    case Resampler::eWrapMirror:
        return "mirror";
    }

    return "?";
}

std::string
describe(const CoordCase& c)
{
    std::ostringstream s;
    s << c.name << " filter " << (int)c.filter << " clamp " << c.clamp << " blackOutside " << c.blackOutside << " wrap " << wrapName(c.wrapU)
      << "/" << wrapName(c.wrapV) << " jacobian " << c.hasJacobian << " nComps " << c.nComps;

    return s.str();
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

// Mirrors the bits coordmap_test_main reads from ResampleParams::flags and ::states.
const uint32_t kFlagClamp = 1;
const uint32_t kFlagBlackOutside = 2;
const uint32_t kFlagSrcValid = 4;
const uint32_t kStateJacobian = 16;

bool
renderGpu(GpuParityRuntime& runtime,
          const CoordCase& c,
          std::vector<float>* out)
{
    gpu::GpuRuntime& vk = runtime.runtime();
    std::vector<float> src = c.src;
    std::vector<float> coords = c.coords;
    std::vector<float> dst((std::size_t)c.width * c.height * c.nComps, std::numeric_limits<float>::quiet_NaN());
    const RectI domain = c.domain.isNull() ? RectI() : c.domain;
    const int32_t domainWords[4] = { domain.x1, domain.y1, domain.x2, domain.y2 };
    std::vector<float> params(4);
    std::memcpy(params.data(), domainWords, sizeof(domainWords));

    gpu::GpuBuffer* srcBuffer = runtime.buffer(0, src.size());
    gpu::GpuBuffer* dstBuffer = runtime.buffer(1, dst.size());
    gpu::GpuBuffer* paramBuffer = runtime.buffer(2, params.size());
    gpu::GpuBuffer* coordBuffer = runtime.buffer(3, coords.size());
    if (!srcBuffer || !dstBuffer || !paramBuffer || !coordBuffer) {
        return false;
    }
    if (!transfer(vk, *srcBuffer, src.data(), src.size() * sizeof(float), true) || !transfer(vk, *dstBuffer, dst.data(), dst.size() * sizeof(float), true)
        || !transfer(vk, *paramBuffer, params.data(), params.size() * sizeof(float), true)
        || !transfer(vk, *coordBuffer, coords.data(), coords.size() * sizeof(float), true)) {
        return false;
    }

    gpu::GpuResampleParams p {};
    p.width = (uint32_t)c.width;
    p.height = (uint32_t)c.height;
    p.dstOffset = 0;
    p.dstStride = (uint32_t)(c.width * c.nComps);
    p.srcOffset = 0;
    p.srcStride = (uint32_t)(c.bounds.width() * c.nComps);
    p.srcX1 = c.bounds.x1;
    p.srcY1 = c.bounds.y1;
    p.srcX2 = c.bounds.x2;
    p.srcY2 = c.bounds.y2;
    p.nComps = (uint32_t)c.nComps;
    p.filter = (uint32_t)c.filter;
    p.flags = kFlagSrcValid | ((c.clamp && Resampler::filterHonoursClamp(c.filter)) ? kFlagClamp : 0u) | (c.blackOutside ? kFlagBlackOutside : 0u);
    p.states = (uint32_t)c.wrapU | ((uint32_t)c.wrapV << 2) | (c.hasJacobian ? kStateJacobian : 0u);

    std::unique_ptr<gpu::GpuRecordContext> context;
    const gpu::GpuStatus created = vk.newRecordContext(context);
    EXPECT_TRUE(bool(created)) << created.message;
    if (!created) {
        return false;
    }
    const gpu::GpuStatus dispatched = context->dispatch(gpu::GpuKernelId::coordmap_test, { srcBuffer, dstBuffer, paramBuffer, coordBuffer }, p,
                                                        { p.width, p.height, 1 });
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

// Samples the case on both sides and compares them, impulse ones exactly when `exactImpulse`.
void
expectParity(GpuParityRuntime& runtime,
             const CoordCase& c,
             bool exactImpulse = false)
{
    const std::vector<float> cpu = renderCpu(c);
    std::vector<float> gpu;
    ASSERT_TRUE(renderGpu(runtime, c, &gpu)) << describe(c);
    for (std::size_t i = 0; i < gpu.size(); ++i) {
        ASSERT_TRUE(std::isfinite(gpu[i])) << describe(c) << ": float " << i << " is " << gpu[i];
    }
    const bool exact = exactImpulse && (c.filter == Resampler::eFilterImpulse);
    const ImageDiff diff = compareImages(cpu, gpu, exact ? ParityTolerance::exact() : ParityTolerance::resampling());
    EXPECT_EQ(0u, diff.mismatches) << describe(c) << ": first at float " << diff.firstMismatchIndex << " gpu " << gpu[diff.firstMismatchIndex] << " cpu "
                                   << cpu[diff.firstMismatchIndex] << ", worst " << diff.worstAbsolute;
}

#endif // NATRON_HAVE_GPU

const Resampler::WrapMode kWraps[] = { Resampler::eWrapNone, Resampler::eWrapClamp, Resampler::eWrapRepeat, Resampler::eWrapMirror };

} // namespace

// Pixel centres sample the source back unchanged, with and without a unit Jacobian.
TEST(CoordMapGpu, IdentityMapCopiesTheSource)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    for (int nComps : { 1, 3, 4 }) {
        CoordCase c;
        c.name = "identity";
        c.bounds = RectI(1, 2, 8, 7);
        c.nComps = nComps;
        c.src = makeSource(c.bounds, nComps);
        resizeGrid(c, c.bounds.width(), c.bounds.height());
        for (int j = 0; j < c.height; ++j) {
            for (int i = 0; i < c.width; ++i) {
                setCoord(c, i, j, c.bounds.x1 + i + 0.5f, c.bounds.y1 + j + 0.5f, 1.f, 0.f, 0.f, 1.f);
            }
        }
        for (bool hasJacobian : { false, true }) {
            for (Resampler::WrapMode wrap : kWraps) {
                c.hasJacobian = hasJacobian;
                c.wrapU = wrap;
                c.wrapV = wrap;
                c.filter = Resampler::eFilterImpulse;
                std::vector<float> gpu;
                ASSERT_TRUE(renderGpu(*runtime, c, &gpu)) << describe(c);
                const ImageDiff diff = compareImages(c.src, gpu, ParityTolerance::exact());
                EXPECT_EQ(0u, diff.mismatches) << describe(c) << ": first at float " << diff.firstMismatchIndex;
                expectParity(*runtime, c, true);
            }
        }
    }
#endif
}

// Coordinates spanning several periods on each side of a 4x4 ramp whose bounds are off the origin,
// for every wrap, with the black-outside and Jacobian variants. No coordinate lies within a tenth
// of a pixel of a pixel edge, before or after wrapping, so impulse sampling picks the same pixel.
TEST(CoordMapGpu, WrapModesMatchTheCpu)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    CoordCase c;
    c.name = "wrap";
    c.bounds = RectI(2, 1, 6, 5);
    c.nComps = 4;
    c.src = makeRamp(c.bounds, c.nComps);
    resizeGrid(c, 24, 24);
    for (int j = 0; j < c.height; ++j) {
        for (int i = 0; i < c.width; ++i) {
            setCoord(c, i, j, c.bounds.x1 - 6.3f + 0.6f * i, c.bounds.y1 - 6.1f + 0.6f * j, 0.6f, 0.05f, -0.05f, 0.6f);
        }
    }
    const Resampler::FilterEnum filters[] = { Resampler::eFilterImpulse, Resampler::eFilterBox, Resampler::eFilterBilinear, Resampler::eFilterCubic,
                                              Resampler::eFilterKeys };
    const Resampler::WrapMode pairs[][2] = {
        { Resampler::eWrapNone, Resampler::eWrapNone },
        { Resampler::eWrapClamp, Resampler::eWrapClamp },
        { Resampler::eWrapRepeat, Resampler::eWrapRepeat },
        { Resampler::eWrapMirror, Resampler::eWrapMirror },
        { Resampler::eWrapRepeat, Resampler::eWrapMirror },
        { Resampler::eWrapClamp, Resampler::eWrapNone },
    };
    for (Resampler::FilterEnum filter : filters) {
        for (const Resampler::WrapMode* pair : pairs) {
            for (bool blackOutside : { true, false }) {
                for (bool hasJacobian : { false, true }) {
                    c.filter = filter;
                    c.clamp = true;
                    c.wrapU = pair[0];
                    c.wrapV = pair[1];
                    c.blackOutside = blackOutside;
                    c.hasJacobian = hasJacobian;
                    expectParity(*runtime, c, true);
                }
            }
        }
    }

    // A wrap domain smaller than the source.
    c.name = "wrap domain";
    c.domain = RectI(3, 2, 5, 4);
    for (Resampler::WrapMode wrap : kWraps) {
        c.filter = Resampler::eFilterBilinear;
        c.wrapU = wrap;
        c.wrapV = wrap;
        c.blackOutside = true;
        c.hasJacobian = false;
        expectParity(*runtime, c, true);
    }
#endif
}

// NaN and infinite coordinates are outside under every wrap but clamp, which brings infinities to
// the edge; NaN is outside under clamp too. Non-finite Jacobian entries read zero.
TEST(CoordMapGpu, NonFiniteCoordinatesReadOutside)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    CoordCase c;
    c.name = "non-finite";
    c.bounds = RectI(0, 0, 4, 4);
    c.nComps = 4;
    c.src = makeRamp(c.bounds, c.nComps);
    for (float* p = c.src.data(); p < c.src.data() + c.src.size(); ++p) {
        *p += 0.5f;
    }
    resizeGrid(c, 8, 1);
    setCoord(c, 0, 0, nan, 1.5f, 1.f, 0.f, 0.f, 1.f);
    setCoord(c, 1, 0, 1.5f, nan, 1.f, 0.f, 0.f, 1.f);
    setCoord(c, 2, 0, inf, 1.5f, 1.f, 0.f, 0.f, 1.f);
    setCoord(c, 3, 0, 1.5f, -inf, 1.f, 0.f, 0.f, 1.f);
    setCoord(c, 4, 0, nan, nan, 1.f, 0.f, 0.f, 1.f);
    setCoord(c, 5, 0, 1.7f, 2.2f, nan, 0.f, 0.f, 4.f);
    setCoord(c, 6, 0, 2.3f, 1.4f, inf, -inf, 5.f, nan);
    setCoord(c, 7, 0, 2.5f, 2.5f, 1.f, 0.f, 0.f, 1.f);
    for (Resampler::WrapMode wrap : kWraps) {
        for (bool blackOutside : { true, false }) {
            for (bool hasJacobian : { false, true }) {
                for (Resampler::FilterEnum filter : { Resampler::eFilterImpulse, Resampler::eFilterBilinear, Resampler::eFilterCubic }) {
                    c.wrapU = wrap;
                    c.wrapV = wrap;
                    c.blackOutside = blackOutside;
                    c.hasJacobian = hasJacobian;
                    c.filter = filter;
                    expectParity(*runtime, c);

                    std::vector<float> gpu;
                    ASSERT_TRUE(renderGpu(*runtime, c, &gpu)) << describe(c);
                    for (int i = 0; i < 5; ++i) {
                        const bool atEdge = (wrap == Resampler::eWrapClamp) && ((i == 2) || (i == 3));
                        for (int k = 0; k < c.nComps; ++k) {
                            const float v = gpu[(std::size_t)i * c.nComps + k];
                            if (atEdge) {
                                EXPECT_NE(0.f, v) << describe(c) << ": pixel " << i;
                            } else {
                                EXPECT_EQ(0.f, v) << describe(c) << ": pixel " << i;
                            }
                        }
                    }
                }
            }
        }
    }
#endif
}

// Minifying Jacobians, axis-aligned and rotated, supersample as interpolateSuper does on the CPU,
// for every filter, with the filter's clamp on and off.
TEST(CoordMapGpu, JacobianMinificationMatchesTheCpu)
{
#ifndef NATRON_HAVE_GPU
    reportNoGpu("built without the GPU backend");
#else
    std::unique_ptr<GpuParityRuntime> runtime;
    if (!createRuntime(runtime)) {
        return;
    }
    CoordCase c;
    c.name = "minify";
    c.bounds = RectI(-3, 2, 45, 38);
    c.nComps = 4;
    c.src = makeSource(c.bounds, c.nComps);
    resizeGrid(c, 14, 12);

    struct Scale {
        const char* name;
        double a, b, cc, d;
    };
    const double angle = 0.3;
    const Scale scales[] = {
        { "scale 3", 3., 0., 0., 3. },
        { "scale 2x5", 2., 0., 0., 5. },
        { "rotate scale 6", 6. * std::cos(angle), -6. * std::sin(angle), 6. * std::sin(angle), 6. * std::cos(angle) },
        { "shear", 1.2, 2.5, -0.4, 1.7 },
    };
    for (const Scale& s : scales) {
        // An affine map about the source's centre, sampled at the destination's pixel centres.
        const double cx = (c.bounds.x1 + c.bounds.x2) / 2.;
        const double cy = (c.bounds.y1 + c.bounds.y2) / 2.;
        for (int j = 0; j < c.height; ++j) {
            for (int i = 0; i < c.width; ++i) {
                const double x = i + 0.5 - c.width / 2.;
                const double y = j + 0.5 - c.height / 2.;
                setCoord(c, i, j, (float)(cx + s.a * x + s.b * y), (float)(cy + s.cc * x + s.d * y), (float)s.a, (float)s.b, (float)s.cc, (float)s.d);
            }
        }
        for (int f = 0; f <= (int)Resampler::eFilterNotch; ++f) {
            for (bool clamp : { false, true }) {
                for (bool blackOutside : { true, false }) {
                    c.name = std::string("minify ") + s.name;
                    c.filter = (Resampler::FilterEnum)f;
                    c.clamp = clamp;
                    c.blackOutside = blackOutside;
                    c.hasJacobian = true;
                    c.wrapU = Resampler::eWrapNone;
                    c.wrapV = Resampler::eWrapNone;
                    expectParity(*runtime, c);
                }
            }
        }
    }
#endif
}
