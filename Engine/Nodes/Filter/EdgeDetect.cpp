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

/*
 * The finite-difference gradient schemes in this file follow CImg's CImg<float>::get_gradient()
 * (CImg 2.9.9, as bundled with openfx-misc), by David Tschumperle and contributors,
 * <http://cimg.eu>. CImg is distributed under the CeCILL-C licence, which is compatible with the
 * GNU GPL.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "EdgeDetect.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Filter/ErodeDilate.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Project.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuBlur.h"
#include "Engine/Gpu/GpuMorphology.h"
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {
// Keeps the erosion radius of an absurd erode size a representable int.
const double kMaxErodeRadius = 1e7;

bool
isGaussianFamily(BlurKernels::Filter filter)
{
    return (filter == BlurKernels::eFilterQuasiGaussian) || (filter == BlurKernels::eFilterGaussian);
}

int
boxIterations(BlurKernels::Filter filter)
{
    return (filter == BlurKernels::eFilterBox) ? 1 : ((filter == BlurKernels::eFilterTriangle) ? 2 : 3);
}

bool
isFiniteDifference(EdgeDetectFilterEnum filter)
{
    return (filter == eEdgeDetectFilterSimple) || (filter == eEdgeDetectFilterSobel) || (filter == eEdgeDetectFilterRotationInvariant);
}

// The parabola through (-1, Ip), (0, Ic), (1, In), evaluated at alpha.
double
parabola(double Ip,
         double Ic,
         double In,
         double alpha)
{
    const double a = ((In - Ic) + (Ip - Ic)) / 2;
    const double b = (In - Ip) / 2;
    const double c = Ic;

    return a * alpha * alpha + b * alpha + c;
}

// The extremum value of that parabola.
double
parabolaMaxValue(double Ip,
                 double Ic,
                 double In)
{
    const double a = ((In - Ic) + (Ip - Ic)) / 2;
    const double b = (In - Ip) / 2;
    const double c = Ic;

    return c - b * b / (4 * a);
}

using LineFilterPtr = std::shared_ptr<const BlurKernels::LineFilter>;

LineFilterPtr
makeFilter(BlurKernels::LineFilter filter)
{
    return std::make_shared<const BlurKernels::LineFilter>(std::move(filter));
}

// Filters each line the executor hands over with filter; none for a filter that leaves its lines
// unchanged.
std::function<PassLineFn()>
lineWorker(const LineFilterPtr& filter)
{
    if (filter->isIdentity()) {
        return std::function<PassLineFn()>();
    }

    return [filter]() -> PassLineFn {
        std::shared_ptr<BlurKernels::LineScratch> scratch = std::make_shared<BlurKernels::LineScratch>();

        return [filter, scratch](float* line, int length, std::ptrdiff_t stride, int /*count*/) {
            filter->apply(line, length, stride, *scratch);
        };
    };
}

std::function<PassLineFn()>
erodeWorker(int radius,
            bool takeMax)
{
    return [radius, takeMax]() -> PassLineFn {
        std::shared_ptr<ErodeDilateKernels::LineScratch> scratch = std::make_shared<ErodeDilateKernels::LineScratch>();

        return [radius, takeMax, scratch](float* line, int length, std::ptrdiff_t stride, int /*count*/) {
            if (length > 1) {
                ErodeDilateKernels::filterLine(line, length, stride, radius, takeMax, *scratch);
            }
        };
    };
}

// The scratch slots the passes use: the buffer, which holds the source, then the edge magnitude;
// the two gradient components, which keep the direction non-maxima suppression follows; and the
// suppression's output.
const int kSlotBuffer = 0;
const int kSlotGx = 1;
const int kSlotGy = 2;
const int kSlotSuppressed = 3;

// The rotation-invariant kernel's weights, rounded to float.
float
rotationInvariantA()
{
    return (float)(0.25f * (2 - std::sqrt(2.f)));
}

float
rotationInvariantB()
{
    return (float)(0.5f * (std::sqrt(2.f) - 1));
}

// The finite-difference stencil over the pre-blurred buffer: CImg<float>::get_gradient(0, scheme),
// centred differences (one-sided and halved at the ends), Sobel or rotation-invariant 3x3 kernels
// with repeated edges, all in float. A one-pixel-wide axis has a zero derivative.
std::function<PassRowFn(const PassScratch&)>
stencilWorker(EdgeDetectFilterEnum scheme)
{
    return [scheme](const PassScratch& scratch) -> PassRowFn {
        const float ra = rotationInvariantA();
        const float rb = rotationInvariantB();
        const int bufferWidth = scratch.width;
        const int bufferHeight = scratch.height;

        return [=](int line) {
            const int p = line / bufferHeight;
            const int row = line % bufferHeight;
            const float* blurred = scratch.plane(kSlotBuffer, p);
            float* gxRow = scratch.plane(kSlotGx, p) + (std::size_t)row * bufferWidth;
            float* gyRow = scratch.plane(kSlotGy, p) + (std::size_t)row * bufferWidth;
            const int rowP = std::max(0, row - 1);
            const int rowN = std::min(bufferHeight - 1, row + 1);
            const float* lineP = blurred + (std::size_t)rowP * bufferWidth;
            const float* lineC = blurred + (std::size_t)row * bufferWidth;
            const float* lineN = blurred + (std::size_t)rowN * bufferWidth;
            for (int i = 0; i < bufferWidth; ++i) {
                const int iP = std::max(0, i - 1);
                const int iN = std::min(bufferWidth - 1, i + 1);
                const float Ipp = lineP[iP];
                const float Icp = lineP[i];
                const float Inp = lineP[iN];
                const float Ipc = lineC[iP];
                const float Inc = lineC[iN];
                const float Ipn = lineN[iP];
                const float Icn = lineN[i];
                const float Inn = lineN[iN];
                float vx;
                float vy;
                if (scheme == eEdgeDetectFilterSobel) {
                    vx = -Ipp + Inp - 2 * Ipc + 2 * Inc - Ipn + Inn;
                    vy = -Ipp - 2 * Icp - Inp + Ipn + 2 * Icn + Inn;
                    vx = vx / 8;
                    vy = vy / 8;
                } else if (scheme == eEdgeDetectFilterRotationInvariant) {
                    vx = -ra * Ipp - rb * Ipc - ra * Ipn + ra * Inp + rb * Inc + ra * Inn;
                    vy = -ra * Ipp - rb * Icp - ra * Inp + ra * Ipn + rb * Icn + ra * Inn;
                } else {
                    vx = (Inc - Ipc) / 2;
                    vy = (Icn - Icp) / 2;
                }
                gxRow[i] = (bufferWidth > 1) ? vx : 0.f;
                gyRow[i] = (bufferHeight > 1) ? vy : 0.f;
            }
        };
    };
}

// The source copied into both gradient components, which the derivative blurs then filter.
std::function<PassRowFn(const PassScratch&)>
copyWorker()
{
    return [](const PassScratch& scratch) -> PassRowFn {
        return [&scratch](int line) {
            const int p = line / scratch.height;
            const int row = line % scratch.height;
            const std::size_t offset = (std::size_t)row * scratch.width;
            const float* source = scratch.plane(kSlotBuffer, p) + offset;

            std::copy(source, source + scratch.width, scratch.plane(kSlotGx, p) + offset);
            std::copy(source, source + scratch.width, scratch.plane(kSlotGy, p) + offset);
        };
    };
}

// What scales a gradient component so that a blurred unit step edge responds with exactly 1,
// applied to the finite-difference schemes too when their pre-blur is the Gaussian one.
struct Normalization {
    bool normalizeX = false;
    bool normalizeY = false;
    double normX = 1.;
    double normY = 1.;
};

Normalization
normalization(BlurKernels::Filter filter,
              double sx,
              double sy)
{
    Normalization n;

    switch (filter) {
    case BlurKernels::eFilterBox:
        n.normalizeX = (sx > 1.);
        n.normalizeY = (sy > 1.);
        n.normX = sx;
        n.normY = sy;
        break;
    case BlurKernels::eFilterTriangle: {
        // The box of size s has 2w+1 taps of 1/s and two end taps of f/s, f = (s - 2w - 1) / 2.
        // Squared, it gives the triangle's centre tap (2w + 1 + 2f^2) / s^2 and its next one
        // (2w + 2f) / s^2. The centred difference of a step between two pixels peaks at the mean
        // of those two taps, (2s - 1 - 2f(1 - f)) / (2s^2).
        const auto triangleNorm = [](double s) {
            const double f = (s - (2 * ((int)(s - 1) / 2) + 1)) / 2.;

            return 2 * s * s / (2 * s - 1 - 2 * f * (1 - f));
        };
        n.normalizeX = (sx > 1.);
        n.normalizeY = (sy > 1.);
        n.normX = triangleNorm(sx);
        n.normY = triangleNorm(sy);
        break;
    }
    case BlurKernels::eFilterQuadratic:
        n.normalizeX = (sx > 1.);
        n.normalizeY = (sy > 1.);
        n.normX = (12 * sx * sx * sx) / (9 * sx * sx - 4);
        n.normY = (12 * sy * sy * sy) / (9 * sy * sy - 4);
        break;
    case BlurKernels::eFilterGaussian:
        // The derivative of a unit step blurred by a Gaussian peaks at 1 / (sigma sqrt(2 pi)).
        n.normalizeX = (sx / 2.4 >= 0.1);
        n.normalizeY = (sy / 2.4 >= 0.1);
        n.normX = (sx / 2.4) * std::sqrt(2. * M_PI);
        n.normY = (sy / 2.4) * std::sqrt(2. * M_PI);
        break;
    case BlurKernels::eFilterQuasiGaussian:
        // Deriche's derivative kernel is c x exp(-alpha |x|) with alpha = 1.695 / sigma and
        // c = alpha^3 / 4, so a unit step peaks at alpha / 4.
        n.normalizeX = (sx / 2.4 >= 0.1);
        n.normalizeY = (sy / 2.4 >= 0.1);
        n.normX = 4. * (sx / 2.4) / 1.695;
        n.normY = 4. * (sy / 2.4) / 1.695;
        break;
    default:
        break;
    }

    return n;
}

// Normalisation and the channel combination of one row: the edge magnitude goes to the buffer,
// and the gradient components keep the direction non-maxima suppression follows.
std::function<PassRowFn(const PassScratch&)>
combineWorker(const Normalization& n,
              EdgeDetectMultiChannelEnum multiChannel)
{
    return [n, multiChannel](const PassScratch& scratch) -> PassRowFn {
        const int bufferWidth = scratch.width;
        const int nPlanes = scratch.nPlanes;
        const std::size_t planeSize = scratch.planeSize;
        const bool separate = (nPlanes == 1) || (multiChannel == eEdgeDetectMultiChannelSeparate);
        float* const buffer = scratch.buffers[kSlotBuffer];
        float* const gx = scratch.buffers[kSlotGx];
        float* const gy = scratch.buffers[kSlotGy];

        return [=](int row) {
            for (int i = 0; i < bufferWidth; ++i) {
                const std::size_t o = (std::size_t)row * bufferWidth + i;
                for (int p = 0; p < nPlanes; ++p) {
                    const std::size_t k = (std::size_t)p * planeSize + o;
                    if (n.normalizeX) {
                        gx[k] = knobValueAsFloat(gx[k] * n.normX);
                    }
                    if (n.normalizeY) {
                        gy[k] = knobValueAsFloat(gy[k] * n.normY);
                    }
                }
                if (separate) {
                    for (int p = 0; p < nPlanes; ++p) {
                        const std::size_t k = (std::size_t)p * planeSize + o;
                        const double gxd = gx[k];
                        const double gyd = gy[k];
                        const double sqe = gxd * gxd + gyd * gyd;
                        buffer[k] = static_cast<float>(std::sqrt(std::max(sqe, 0.)));
                    }
                } else if (multiChannel == eEdgeDetectMultiChannelRMS) {
                    // The direction is the sum of the channel gradients, each flipped to
                    // point the same way as the strongest channel's: opposite gradients
                    // would otherwise cancel and turn the suppression direction.
                    double sumsq = 0.;
                    double refx = 0.;
                    double refy = 0.;
                    double refsq = -1.;
                    for (int p = 0; p < nPlanes; ++p) {
                        const std::size_t k = (std::size_t)p * planeSize + o;
                        const double gxd = gx[k];
                        const double gyd = gy[k];
                        const double sq = gxd * gxd + gyd * gyd;
                        sumsq += sq;
                        if (sq > refsq) {
                            refsq = sq;
                            refx = gxd;
                            refy = gyd;
                        }
                    }
                    double gradx = 0.;
                    double grady = 0.;
                    for (int p = 0; p < nPlanes; ++p) {
                        const std::size_t k = (std::size_t)p * planeSize + o;
                        const double gxd = gx[k];
                        const double gyd = gy[k];
                        const double sign = (gxd * refx + gyd * refy < 0.) ? -1. : 1.;
                        gradx += sign * gxd;
                        grady += sign * gyd;
                    }
                    const double sqe = sumsq / nPlanes;
                    const double e = std::sqrt(std::max(sqe, 0.));
                    for (int p = 0; p < nPlanes; ++p) {
                        const std::size_t k = (std::size_t)p * planeSize + o;
                        buffer[k] = static_cast<float>(e);
                        gx[k] = static_cast<float>(gradx);
                        gy[k] = static_cast<float>(grady);
                    }
                } else if (multiChannel == eEdgeDetectMultiChannelMax) {
                    double maxsq = 0.;
                    double gradx = 0.;
                    double grady = 0.;
                    for (int p = 0; p < nPlanes; ++p) {
                        const std::size_t k = (std::size_t)p * planeSize + o;
                        const double gxd = gx[k];
                        const double gyd = gy[k];
                        const double sq = gxd * gxd + gyd * gyd;
                        if (sq > maxsq) {
                            maxsq = sq;
                            gradx = gxd;
                            grady = gyd;
                        }
                    }
                    const double e = std::sqrt(std::max(maxsq, 0.));
                    for (int p = 0; p < nPlanes; ++p) {
                        const std::size_t k = (std::size_t)p * planeSize + o;
                        buffer[k] = static_cast<float>(e);
                        gx[k] = static_cast<float>(gradx);
                        gy[k] = static_cast<float>(grady);
                    }
                } else {
                    // Di Zenzo: the square root of the largest eigenvalue of the
                    // structure tensor, whose eigenvector (Jxy, e1 - Jx) is the direction.
                    double Jx = 0.;
                    double Jy = 0.;
                    double Jxy = 0.;
                    for (int p = 0; p < nPlanes; ++p) {
                        const std::size_t k = (std::size_t)p * planeSize + o;
                        const double gxd = gx[k];
                        const double gyd = gy[k];
                        Jx += gxd * gxd;
                        Jy += gyd * gyd;
                        Jxy += gxd * gyd;
                    }
                    const double sqD = Jx * Jx - 2 * Jx * Jy + Jy * Jy + 4 * Jxy * Jxy;
                    const double D = std::sqrt(std::max(sqD, 0.));
                    const double e1 = (Jx + Jy + D) / 2;
                    const double e = std::sqrt(std::max(e1, 0.));
                    for (int p = 0; p < nPlanes; ++p) {
                        const std::size_t k = (std::size_t)p * planeSize + o;
                        buffer[k] = static_cast<float>(e);
                        gx[k] = static_cast<float>(Jxy);
                        gy[k] = static_cast<float>(e1 - Jx);
                    }
                }
            }
        };
    };
}

// Non-maxima suppression along the gradient direction, with the parabolic peak value at the
// maxima; the buffer's border is cleared.
std::function<PassRowFn(const PassScratch&)>
suppressionWorker(EdgeDetectMultiChannelEnum multiChannel)
{
    return [multiChannel](const PassScratch& scratch) -> PassRowFn {
        const int bufferWidth = scratch.width;
        const int bufferHeight = scratch.height;
        const int nPlanes = scratch.nPlanes;
        const std::size_t planeSize = scratch.planeSize;
        const bool separate = (nPlanes == 1) || (multiChannel == eEdgeDetectMultiChannelSeparate);
        const int cmax = separate ? nPlanes : 1;
        const float* const eroded = scratch.buffers[kSlotBuffer];
        const float* const gx = scratch.buffers[kSlotGx];
        const float* const gy = scratch.buffers[kSlotGy];
        float* const out = scratch.buffers[kSlotSuppressed];

        return [=](int y) {
            for (int x = 0; x < bufferWidth; ++x) {
                const std::size_t o = (std::size_t)y * bufferWidth + x;
                if ((x == 0) || (x == bufferWidth - 1) || (y == 0) || (y == bufferHeight - 1)) {
                    for (int p = 0; p < nPlanes; ++p) {
                        out[(std::size_t)p * planeSize + o] = 0.f;
                    }
                    continue;
                }
                for (int c = 0; c < cmax; ++c) {
                    const std::size_t base = (std::size_t)c * planeSize;
                    const float* mag = eroded + base;
                    float value = 0.f;
                    float gradx = gx[base + o];
                    float grady = gy[base + o];
                    if ((gradx != 0.f) || (grady != 0.f)) {
                        const bool horiz = std::abs(gradx) >= std::abs(grady);
                        const std::size_t rowP = (std::size_t)(y - 1) * bufferWidth;
                        const std::size_t rowC = (std::size_t)y * bufferWidth;
                        const std::size_t rowN = (std::size_t)(y + 1) * bufferWidth;
                        float Ipp;
                        float Ipc;
                        float Ipn;
                        float Inp;
                        float Inc;
                        float Inn;
                        if (horiz) {
                            Ipp = mag[rowP + x - 1];
                            Ipc = mag[rowC + x - 1];
                            Ipn = mag[rowN + x - 1];
                            Inp = mag[rowP + x + 1];
                            Inc = mag[rowC + x + 1];
                            Inn = mag[rowN + x + 1];
                        } else {
                            std::swap(gradx, grady);
                            Ipp = mag[rowP + x - 1];
                            Ipc = mag[rowP + x];
                            Ipn = mag[rowP + x + 1];
                            Inp = mag[rowN + x - 1];
                            Inc = mag[rowN + x];
                            Inn = mag[rowN + x + 1];
                        }
                        const double alpha = grady / gradx;
                        const double Ip = parabola(Ipp, Ipc, Ipn, -alpha);
                        const double In = parabola(Inp, Inc, Inn, +alpha);
                        const double Icv = mag[rowC + x];
                        if ((Ip < Icv) && (Icv >= In)) {
                            value = static_cast<float>(parabolaMaxValue(Ip, Icv, In));
                        }
                    }
                    if (separate) {
                        out[base + o] = value;
                    } else {
                        for (int p = 0; p < nPlanes; ++p) {
                            out[(std::size_t)p * planeSize + o] = value;
                        }
                    }
                }
            }
        };
    };
}

Pass
atSlot(Pass pass,
       int slot)
{
    pass.slot = slot;

    return pass;
}

#ifdef NATRON_HAVE_GPU
// Mirrors kScheme*, kCombine* and kNormalize* in Kernels/edgedetect.slang.
const uint32_t kGpuSchemeSimple = 0;
const uint32_t kGpuSchemeSobel = 1;
const uint32_t kGpuSchemeRotationInvariant = 2;
const uint32_t kGpuCombineSeparate = 0;
const uint32_t kGpuCombineRMS = 1;
const uint32_t kGpuCombineMax = 2;
const uint32_t kGpuCombineTensor = 3;
const uint32_t kGpuNormalizeX = 1;
const uint32_t kGpuNormalizeY = 2;

// Mirrors EdgeParams in Kernels/edgedetect.slang, which has no padding between members.
struct EdgeDetectGpuParams {
    int32_t bufX1;
    int32_t bufY1;
    uint32_t bufWidth;
    uint32_t bufHeight;
    uint32_t channels;
    uint32_t processed;
    uint32_t mode;
    uint32_t flags;
    int32_t outX1;
    int32_t outY1;
    uint32_t outWidth;
    uint32_t outHeight;
    uint32_t dstOffset;
    uint32_t dstStride;
    int32_t srcX1;
    int32_t srcY1;
    int32_t srcX2;
    int32_t srcY2;
    uint32_t srcOffset;
    uint32_t srcStride;
    float normXHi;
    float normXLo;
    float normYHi;
    float normYLo;
    float ra;
    float rb;
};
static_assert(sizeof(EdgeDetectGpuParams) == 104);
static_assert(offsetof(EdgeDetectGpuParams, outX1) == 32);
static_assert(offsetof(EdgeDetectGpuParams, srcX1) == 56);
static_assert(offsetof(EdgeDetectGpuParams, normXHi) == 80);

// The passes of edgeDetectPasses() as the GPU recorder runs them.
struct EdgeDetectGpuPlan {
    bool finiteDifference = false;
    uint32_t scheme = kGpuSchemeSimple;
    // The finite-difference schemes' pre-blur, absent when it leaves every line unchanged.
    bool preBlur = false;
    gpu::GpuLineFilter preBlurX;
    gpu::GpuLineFilter preBlurY;
    // The derivative blurs of the x and y gradient components, rows then columns.
    gpu::GpuLineFilter gxRows;
    gpu::GpuLineFilter gxColumns;
    gpu::GpuLineFilter gyRows;
    gpu::GpuLineFilter gyColumns;
    Normalization normalization;
    EdgeDetectMultiChannelEnum multiChannel = eEdgeDetectMultiChannelTensor;
    // Rows then columns, as the CPU's erosion passes.
    std::vector<gpu::GpuMorphPass> erosion;
    int erodeX = 0;
    int erodeY = 0;
    bool nms = false;
};

uint32_t
gpuCombineMode(EdgeDetectMultiChannelEnum multiChannel)
{
    switch (multiChannel) {
    case eEdgeDetectMultiChannelRMS:
        return kGpuCombineRMS;
    case eEdgeDetectMultiChannelMax:
        return kGpuCombineMax;
    case eEdgeDetectMultiChannelTensor:
        return kGpuCombineTensor;
    case eEdgeDetectMultiChannelSeparate:
    default:
        return kGpuCombineSeparate;
    }
}

uint32_t
gpuScheme(EdgeDetectFilterEnum filter)
{
    if (filter == eEdgeDetectFilterSobel) {
        return kGpuSchemeSobel;
    }
    if (filter == eEdgeDetectFilterRotationInvariant) {
        return kGpuSchemeRotationInvariant;
    }

    return kGpuSchemeSimple;
}

// A double as a float and the float nearest to what that leaves.
void
splitDouble(double v,
            float* hi,
            float* lo)
{
    *hi = (float)v;
    *lo = (float)(v - (double)*hi);
}

// A tightly packed scratch buffer of nComps floats per pixel of rect.
gpu::GpuBlurImage
scratchImage(gpu::GpuBuffer* buffer,
             const RectI& rect,
             int nComps)
{
    gpu::GpuBlurImage image;

    image.buffer = buffer;
    image.bounds = PassExec::toGpuBlurRect(rect);
    image.strideFloats = (uint32_t)(rect.width() * nComps);

    return image;
}

// The float index of pixel (x, y) of image, or false when the image exceeds the kernels' 32-bit
// indices.
bool
gpuFloatIndex(const gpu::GpuBlurImage& image,
              int x,
              int y,
              int nComps,
              uint32_t* out)
{
    const uint64_t index = image.offsetFloats + (uint64_t)(y - image.bounds.y1) * image.strideFloats + (uint64_t)(x - image.bounds.x1) * nComps;
    const uint64_t end = image.offsetFloats + (uint64_t)(image.bounds.y2 - image.bounds.y1) * image.strideFloats;
    if ((index > UINT32_MAX) || (end > UINT32_MAX)) {
        return false;
    }
    *out = (uint32_t)index;

    return true;
}

// Points a dispatch's output at rect of dst.
bool
setGpuOutput(const gpu::GpuBlurImage& dst,
             const RectI& rect,
             int nComps,
             EdgeDetectGpuParams* p)
{
    p->outX1 = rect.x1;
    p->outY1 = rect.y1;
    p->outWidth = (uint32_t)std::max(0, rect.width());
    p->outHeight = (uint32_t)std::max(0, rect.height());
    p->dstStride = dst.strideFloats;

    return PassExec::isEmptyRect(rect) || gpuFloatIndex(dst, rect.x1, rect.y1, nComps, &p->dstOffset);
}

bool
dispatchEdgeDetect(gpu::GpuRecordContext& ctx,
                   gpu::GpuKernelId kernel,
                   const gpu::GpuBuffer* in,
                   const gpu::GpuBuffer* gx,
                   const gpu::GpuBuffer* gy,
                   const gpu::GpuBuffer* out,
                   const EdgeDetectGpuParams& p,
                   uint32_t width,
                   uint32_t height)
{
    if (!width || !height) {
        return true;
    }

    return (bool)ctx.dispatch(kernel, { in, gx, gy, out }, p, { width, height, 1 });
}

// A separable line filter of the whole buffer, from src to dst, which may be the same buffer.
bool
recordBufferLines(gpu::GpuRecordContext& ctx,
                  const gpu::GpuBuffer* window,
                  const gpu::GpuBlurImage& src,
                  const gpu::GpuBlurImage& dst,
                  int nComps,
                  const gpu::GpuLineFilter& filterX,
                  const gpu::GpuLineFilter& filterY)
{
    gpu::GpuLineBlur blur;

    blur.src = src;
    blur.srcValid = src.bounds;
    blur.dst = dst;
    blur.output = dst.bounds;
    blur.domain = dst.bounds;
    blur.window = window;
    blur.channels = (uint32_t)nComps;
    blur.filterX = filterX;
    blur.filterY = filterY;

    return (bool)gpu::recordLineBlur(ctx, blur);
}

/// The pass list on the GPU over the CPU's buffer, io.domain. The buffer slot, the two gradient
/// components and, when the suppression reads an eroded magnitude, a fourth buffer are scratch;
/// the last step writes io.filtered over io.output, zero outside the buffer as the CPU writes it.
PassGpuRecorder
edgeDetectGpuRecorder(const std::shared_ptr<const EdgeDetectGpuPlan>& plan)
{
    return [plan](const PassGpuIO& io) -> StatusEnum {
        gpu::GpuRecordContext& ctx = io.context;
        const int nComps = io.nComps;
        const RectI& bufferRect = io.domain;
        const bool hasBuffer = !PassExec::isEmptyRect(bufferRect);
        const uint64_t floats = hasBuffer ? (uint64_t)bufferRect.width() * bufferRect.height() * nComps : 0;
        if (floats > UINT32_MAX) {
            return eStatusFailed;
        }
        const VkDeviceSize bytes = (VkDeviceSize)std::max<uint64_t>(floats, 4) * sizeof(float);
        gpu::GpuBuffer* magnitude = nullptr;
        gpu::GpuBuffer* gx = nullptr;
        gpu::GpuBuffer* gy = nullptr;
        if (!ctx.scratch(bytes, magnitude) || !magnitude || !ctx.scratch(bytes, gx) || !gx || !ctx.scratch(bytes, gy) || !gy) {
            return eStatusFailed;
        }

        // The CPU's buffer holds the processed channels only, and combines those.
        uint32_t processed = 0;
        int nProcessed = 0;
        int firstProcessed = -1;
        for (int c = 0; c < nComps; ++c) {
            if (io.channels[pixelKernelChannelBit(nComps, c)]) {
                processed |= 1u << c;
                ++nProcessed;
                if (firstProcessed < 0) {
                    firstProcessed = c;
                }
            }
        }
        const bool separate = (nProcessed <= 1) || (plan->multiChannel == eEdgeDetectMultiChannelSeparate);

        EdgeDetectGpuParams base {};
        base.channels = (uint32_t)nComps;
        base.processed = processed;
        if (hasBuffer) {
            base.bufX1 = bufferRect.x1;
            base.bufY1 = bufferRect.y1;
            base.bufWidth = (uint32_t)bufferRect.width();
            base.bufHeight = (uint32_t)bufferRect.height();
        }
        const gpu::GpuBlurImage filtered = PassExec::toGpuBlurImage(io.filtered);

        if (!hasBuffer) {
            // Every pixel of the window lies outside the empty buffer, so the combination writes zero.
            EdgeDetectGpuParams p = base;
            if (!setGpuOutput(filtered, io.output, nComps, &p)) {
                return eStatusFailed;
            }

            return dispatchEdgeDetect(ctx, gpu::GpuKernelId::edgedetect_combine, magnitude, gx, gy, filtered.buffer, p, p.outWidth, p.outHeight) ? eStatusOK : eStatusFailed;
        }
        const gpu::GpuBlurImage magnitudeImage = scratchImage(magnitude, bufferRect, nComps);

        const gpu::GpuBuffer* sourceBuffer = io.source.buffer;
        EdgeDetectGpuParams fill = base;
        if (!sourceBuffer || PassExec::isEmptyRect(io.sourceValid)) {
            // The kernel still binds a source, which the empty source rect keeps unread.
            sourceBuffer = gx;
        } else {
            const gpu::GpuBlurImage source = PassExec::toGpuBlurImage(io.source);
            fill.srcX1 = io.sourceValid.x1;
            fill.srcY1 = io.sourceValid.y1;
            fill.srcX2 = io.sourceValid.x2;
            fill.srcY2 = io.sourceValid.y2;
            fill.srcStride = source.strideFloats;
            if (!gpuFloatIndex(source, io.sourceValid.x1, io.sourceValid.y1, nComps, &fill.srcOffset)) {
                return eStatusFailed;
            }
        }
        if (!dispatchEdgeDetect(ctx, gpu::GpuKernelId::edgedetect_fill, sourceBuffer, gx, gy, magnitude, fill, base.bufWidth, base.bufHeight)) {
            return eStatusFailed;
        }
        ctx.computeBarrier();

        const bool lines = !plan->finiteDifference || plan->preBlur;
        gpu::GpuBuffer* window = nullptr;
        if (lines && !gpu::recordFilterWindow(ctx, PassExec::toGpuBlurRect(io.output), PassExec::toGpuBlurRect(io.rod), io.haloX, io.haloY, window)) {
            return eStatusFailed;
        }
        if (plan->finiteDifference) {
            if (plan->preBlur) {
                if (!recordBufferLines(ctx, window, magnitudeImage, magnitudeImage, nComps, plan->preBlurX, plan->preBlurY)) {
                    return eStatusFailed;
                }
                ctx.computeBarrier();
            }
            EdgeDetectGpuParams stencil = base;
            stencil.mode = plan->scheme;
            stencil.ra = rotationInvariantA();
            stencil.rb = rotationInvariantB();
            if (!dispatchEdgeDetect(ctx, gpu::GpuKernelId::edgedetect_stencil, magnitude, gx, gy, magnitude, stencil, base.bufWidth, base.bufHeight)) {
                return eStatusFailed;
            }
        } else {
            if (!recordBufferLines(ctx, window, magnitudeImage, scratchImage(gx, bufferRect, nComps), nComps, plan->gxRows, plan->gxColumns)) {
                return eStatusFailed;
            }
            if (!recordBufferLines(ctx, window, magnitudeImage, scratchImage(gy, bufferRect, nComps), nComps, plan->gyRows, plan->gyColumns)) {
                return eStatusFailed;
            }
        }
        ctx.computeBarrier();

        // Without erosion or suppression the combination is the result, needed over the window only.
        const bool erodes = !plan->erosion.empty();
        const bool keepsBuffer = erodes || plan->nms;
        const Normalization& n = plan->normalization;
        EdgeDetectGpuParams combine = base;
        combine.mode = separate ? kGpuCombineSeparate : gpuCombineMode(plan->multiChannel);
        combine.flags = (n.normalizeX ? kGpuNormalizeX : 0u) | (n.normalizeY ? kGpuNormalizeY : 0u);
        splitDouble(n.normX, &combine.normXHi, &combine.normXLo);
        splitDouble(n.normY, &combine.normYHi, &combine.normYLo);
        const gpu::GpuBlurImage& combineDst = keepsBuffer ? magnitudeImage : filtered;
        if (!setGpuOutput(combineDst, keepsBuffer ? bufferRect : io.output, nComps, &combine)) {
            return eStatusFailed;
        }
        if (!dispatchEdgeDetect(ctx, gpu::GpuKernelId::edgedetect_combine, magnitude, gx, gy, combineDst.buffer, combine, combine.outWidth, combine.outHeight)) {
            return eStatusFailed;
        }
        if (!keepsBuffer) {
            return eStatusOK;
        }
        ctx.computeBarrier();

        const gpu::GpuBuffer* suppressionInput = magnitude;
        if (erodes) {
            gpu::GpuMorphology morph;
            morph.src = magnitudeImage;
            morph.rod = PassExec::toGpuBlurRect(bufferRect);
            morph.passes = plan->erosion;
            morph.channels = (uint32_t)nComps;
            gpu::GpuBuffer* eroded = nullptr;
            if (plan->nms) {
                if (!ctx.scratch(bytes, eroded) || !eroded) {
                    return eStatusFailed;
                }
                morph.dst = scratchImage(eroded, bufferRect, nComps);
                morph.output = morph.rod;
            } else {
                // Each eroded line clamps its windows to the buffer, which the window grown by the
                // radius and clipped to the buffer reaches wherever the window needs it.
                morph.dst = filtered;
                morph.output = PassExec::toGpuBlurRect(io.output);
                morph.haloX = plan->erodeX;
                morph.haloY = plan->erodeY;
            }
            if (!gpu::recordMorphology(ctx, morph)) {
                return eStatusFailed;
            }
            if (!plan->nms) {
                return eStatusOK;
            }
            ctx.computeBarrier();
            suppressionInput = eroded;
        }

        EdgeDetectGpuParams suppression = base;
        suppression.mode = separate ? 1u : 0u;
        suppression.flags = separate ? 0u : (uint32_t)std::max(0, firstProcessed);
        if (!setGpuOutput(filtered, io.output, nComps, &suppression)) {
            return eStatusFailed;
        }

        return dispatchEdgeDetect(ctx, gpu::GpuKernelId::edgedetect_nms, suppressionInput, gx, gy, filtered.buffer, suppression, suppression.outWidth, suppression.outHeight) ? eStatusOK : eStatusFailed;
    };
} // edgeDetectGpuRecorder
#endif // NATRON_HAVE_GPU

/// The passes of one render at scale; params carries sizeX already divided by the pixel aspect
/// ratio. The halo and the region of definition's spread are CImgBlurPlugin::getRoI() and
/// getRegionOfDefinition() with first-order derivatives on both axes, which are never an
/// identity, plus the erosion radius and the suppression's pixel in the halo only.
PassList
edgeDetectPasses(const EdgeDetectParams& params,
                 const RenderScale& scale)
{
    const OfxPointD rs = scale.toOfxPointD();
    const double sx = rs.x * params.sizeX;
    const double sy = rs.y * params.sizeY;
    const BlurKernels::Filter filter = EdgeDetect::blurFilter(params.filter);
    // EdgeDetect always uses CImg's Neumann (nearest) boundary condition.
    const bool neumann = true;

    // The derivative order is the one added to each delta.
    int haloX;
    int haloY;
    int growX;
    int growY;
    if (isGaussianFamily(filter)) {
        haloX = growX = std::max(3, (int)std::ceil(sx * 1.5)) + 1;
        haloY = growY = std::max(3, (int)std::ceil(sy * 1.5)) + 1;
    } else {
        const int iter = boxIterations(filter);
        haloX = iter * static_cast<int>(std::floor((sx - 1) / 2) + 1) + 1;
        haloY = iter * static_cast<int>(std::floor((sy - 1) / 2) + 1) + 1;
        growX = iter * (int)std::ceil((sx - 1) / 2) + 1;
        growY = iter * (int)std::ceil((sy - 1) / 2) + 1;
    }
    if (!params.expandRoD) {
        growX = growY = 0;
    }

    PassList list;
    list.boundary = PassExec::Boundary::Nearest;
    list.scratchSlots = 3;
    // What gpuCostHint() reports: the blur's sigma, recursive for the Gaussians.
    const auto withCost = [&](Pass pass, double size, const LineFilterPtr& lineFilter) {
        pass.sigma = size / 2.4;
        pass.recursive = isGaussianFamily(filter) && !lineFilter->isIdentity();

        return pass;
    };
#ifdef NATRON_HAVE_GPU
    std::shared_ptr<EdgeDetectGpuPlan> gpuPlan = std::make_shared<EdgeDetectGpuPlan>();
    gpuPlan->multiChannel = params.multiChannel;
    gpuPlan->nms = params.nms;
#endif

    if (isFiniteDifference(params.filter)) {
        // A Gaussian pre-blur, then finite differences, which read one pixel around each.
        const float sigmax = knobValueAsFloat(sx / 2.4);
        const float sigmay = knobValueAsFloat(sy / 2.4);
        LineFilterPtr preBlurX = makeFilter(BlurKernels::LineFilter());
        LineFilterPtr preBlurY = preBlurX;
        if (!((sigmax < 0.1) && (sigmay < 0.1))) {
            preBlurX = makeFilter(BlurKernels::LineFilter::forFilter(filter, sigmax, 0, neumann));
            preBlurY = makeFilter(BlurKernels::LineFilter::forFilter(filter, sigmay, 0, neumann));
        }
        list.passes.push_back(withCost(Pass::lineX(haloX - 1, std::max(0, growX - 1), lineWorker(preBlurX)), sx, preBlurX));
        list.passes.push_back(withCost(Pass::lineY(haloY - 1, std::max(0, growY - 1), lineWorker(preBlurY)), sy, preBlurY));
        Pass stencil = Pass::gather(1, 1, true, stencilWorker(params.filter));
        stencil.growX = stencil.growY = params.expandRoD ? 1 : 0;
        list.passes.push_back(stencil);
#ifdef NATRON_HAVE_GPU
        gpuPlan->finiteDifference = true;
        gpuPlan->scheme = gpuScheme(params.filter);
        gpuPlan->preBlur = !preBlurX->isIdentity() || !preBlurY->isIdentity();
        gpuPlan->preBlurX = BlurKernels::toGpuLineFilter(*preBlurX);
        gpuPlan->preBlurY = BlurKernels::toGpuLineFilter(*preBlurY);
#endif
    } else {
        // The x and y gradient components as derivative blurs, each run as a horizontal pass then
        // a vertical one, over a copy of the source each.
        LineFilterPtr gxRows;
        LineFilterPtr gxColumns;
        LineFilterPtr gyRows;
        LineFilterPtr gyColumns;
        if (isGaussianFamily(filter)) {
            const float sigmax = knobValueAsFloat(sx / 2.4);
            const float sigmay = knobValueAsFloat(sy / 2.4);
            gxRows = makeFilter(BlurKernels::LineFilter::forFilter(filter, sigmax, 1, neumann));
            gxColumns = makeFilter(BlurKernels::LineFilter::forFilter(filter, sigmay, 0, neumann));
            gyRows = makeFilter(BlurKernels::LineFilter::forFilter(filter, sigmax, 0, neumann));
            gyColumns = makeFilter(BlurKernels::LineFilter::forFilter(filter, sigmay, 1, neumann));
        } else {
            // CImg's box filter is the identity at size 0, but the derivative is still wanted there.
            const double boxX = (sx <= 0.) ? 1e-8 : sx;
            const double boxY = (sy <= 0.) ? 1e-8 : sy;
            gxRows = makeFilter(BlurKernels::LineFilter::forFilter(filter, knobValueAsFloat(boxX), 1, neumann));
            gxColumns = makeFilter(BlurKernels::LineFilter::forFilter(filter, knobValueAsFloat(sy), 0, neumann));
            gyRows = makeFilter(BlurKernels::LineFilter::forFilter(filter, knobValueAsFloat(sx), 0, neumann));
            gyColumns = makeFilter(BlurKernels::LineFilter::forFilter(filter, knobValueAsFloat(boxY), 1, neumann));
        }
        list.passes.push_back(Pass::point(true, copyWorker()));
        list.passes.push_back(atSlot(withCost(Pass::lineX(haloX, growX, lineWorker(gxRows)), sx, gxRows), kSlotGx));
        list.passes.push_back(atSlot(withCost(Pass::lineY(haloY, growY, lineWorker(gxColumns)), sy, gxColumns), kSlotGx));
        list.passes.push_back(atSlot(withCost(Pass::lineX(0, 0, lineWorker(gyRows)), sx, gyRows), kSlotGy));
        list.passes.push_back(atSlot(withCost(Pass::lineY(0, 0, lineWorker(gyColumns)), sy, gyColumns), kSlotGy));
#ifdef NATRON_HAVE_GPU
        gpuPlan->gxRows = BlurKernels::toGpuLineFilter(*gxRows);
        gpuPlan->gxColumns = BlurKernels::toGpuLineFilter(*gxColumns);
        gpuPlan->gyRows = BlurKernels::toGpuLineFilter(*gyRows);
        gpuPlan->gyColumns = BlurKernels::toGpuLineFilter(*gyColumns);
#endif
    }

    const Normalization norm = normalization(filter, sx, sy);
    list.passes.push_back(Pass::point(false, combineWorker(norm, params.multiChannel)));
#ifdef NATRON_HAVE_GPU
    gpuPlan->normalization = norm;
    bool gpuFits = true;
#endif

    int erodeX = 0;
    int erodeY = 0;
    EdgeDetect::getErodeRadius(scale, params, &erodeX, &erodeY);
    const bool takeMax = (params.erodeSize < 0.);
    if (params.erodeSize != 0.) {
        // Erosion (positive size) or dilation (negative size) by a (2 rx + 1) x (2 ry + 1)
        // rectangle, rows then columns.
        if (erodeX > 0) {
            list.passes.push_back(Pass::lineX(erodeX, 0, erodeWorker(erodeX, takeMax)));
        }
        if (erodeY > 0) {
            list.passes.push_back(Pass::lineY(erodeY, 0, erodeWorker(erodeY, takeMax)));
        }
#ifdef NATRON_HAVE_GPU
        gpuFits = (erodeX <= (int)gpu::kGpuMorphMaxRadius) && (erodeY <= (int)gpu::kGpuMorphMaxRadius);
        if (erodeX > 0) {
            gpuPlan->erosion.push_back(gpu::GpuMorphPass { (uint32_t)erodeX, false, takeMax });
            gpuPlan->erodeX = erodeX;
        }
        if (erodeY > 0) {
            gpuPlan->erosion.push_back(gpu::GpuMorphPass { (uint32_t)erodeY, true, takeMax });
            gpuPlan->erodeY = erodeY;
        }
#endif
    }

    if (params.nms) {
        Pass suppression = Pass::gather(1, 1, false, suppressionWorker(params.multiChannel));
        suppression.growX = suppression.growY = 0;
        list.passes.push_back(suppression);
        list.scratchSlots = 4;
        list.resultSlot = kSlotSuppressed;
    }

#ifdef NATRON_HAVE_GPU
    if (gpuFits) {
        list.gpu = edgeDetectGpuRecorder(gpuPlan);
    }
#endif

    return list;
} // edgeDetectPasses

EdgeDetectParams
atContext(const EdgeDetectParams& params,
          const PassContext& context)
{
    EdgeDetectParams out = params;

    out.par = context.pixelAspectRatio;
    if (out.par != 0.) {
        out.sizeX /= out.par;
    }

    return out;
}
} // anonymous namespace

void
EdgeDetectOp::createKnobs(KnobHolder* holder,
                          const KnobPagePtr& page)
{
    KnobChoicePtr filterKnob = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr("Filter"));
    filterKnob->setName(kEdgeDetectParamFilter);
    filterKnob->setHintToolTip(EffectInstance::tr("Edge detection filter. If the blur size is not zero, it is used as the kernel size for quasi-Gaussian, Gaussian, box, triangle and quadratic filters. For the simple, rotation-invariant and Sobel filters, the image is pre-blurred with a Gaussian filter."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kEdgeDetectParamFilterSimple, EffectInstance::tr("Simple").toStdString(), EffectInstance::tr("Gradient is estimated by centered finite differences.").toStdString()));
        options.push_back(ChoiceOption(kEdgeDetectParamFilterSobel, EffectInstance::tr("Sobel").toStdString(), EffectInstance::tr("Compute gradient using the Sobel 3x3 filter.").toStdString()));
        options.push_back(ChoiceOption(kEdgeDetectParamFilterRotationInvariant, EffectInstance::tr("Rotation Invariant").toStdString(), EffectInstance::tr("Compute gradient using a 3x3 rotation-invariant filter.").toStdString()));
        options.push_back(ChoiceOption(kEdgeDetectParamFilterQuasiGaussian, EffectInstance::tr("Quasi-Gaussian").toStdString(), EffectInstance::tr("Quasi-Gaussian filter (0-order recursive Deriche filter, faster) - IIR (infinite support / impulsional response).").toStdString()));
        options.push_back(ChoiceOption(kEdgeDetectParamFilterGaussian, EffectInstance::tr("Gaussian").toStdString(), EffectInstance::tr("Gaussian filter (Van Vliet recursive Gaussian filter, more isotropic, slower) - IIR (infinite support / impulsional response).").toStdString()));
        options.push_back(ChoiceOption(kEdgeDetectParamFilterBox, EffectInstance::tr("Box").toStdString(), EffectInstance::tr("Box filter - FIR (finite support / impulsional response).").toStdString()));
        options.push_back(ChoiceOption(kEdgeDetectParamFilterTriangle, EffectInstance::tr("Triangle").toStdString(), EffectInstance::tr("Triangle/tent filter - FIR (finite support / impulsional response).").toStdString()));
        options.push_back(ChoiceOption(kEdgeDetectParamFilterQuadratic, EffectInstance::tr("Quadratic").toStdString(), EffectInstance::tr("Quadratic filter - FIR (finite support / impulsional response).").toStdString()));
        filterKnob->populateChoices(options);
    }
    filterKnob->setDefaultValue((int)eEdgeDetectFilterGaussian);
    page->addKnob(filterKnob);
    filter = filterKnob;

    KnobChoicePtr multiChannelKnob = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr("Multi-Channel"));
    multiChannelKnob->setName(kEdgeDetectParamMultiChannel);
    multiChannelKnob->setHintToolTip(EffectInstance::tr("Operation used to combine multi-channel (e.g. color) gradients into an edge detector. This parameter has no effect if a single channel (e.g. alpha) is processed."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kEdgeDetectParamMultiChannelSeparate, EffectInstance::tr("Separate").toStdString(), EffectInstance::tr("The gradient magnitude is computed in each channel separately, and the output is a color edge image.").toStdString()));
        options.push_back(ChoiceOption(kEdgeDetectParamMultiChannelRMS, EffectInstance::tr("RMS").toStdString(), EffectInstance::tr("The RMS of per-channel gradients magnitudes is computed.").toStdString()));
        options.push_back(ChoiceOption(kEdgeDetectParamMultiChannelMax, EffectInstance::tr("Max").toStdString(), EffectInstance::tr("The maximum per-channel gradient magnitude is computed.").toStdString()));
        options.push_back(ChoiceOption(kEdgeDetectParamMultiChannelTensor, EffectInstance::tr("Tensor").toStdString(), EffectInstance::tr("The tensor gradient norm is computed. See Silvano Di Zenzo, A note on the gradient of a multi-image, CVGIP 33, 116-125 (1986).").toStdString()));
        multiChannelKnob->populateChoices(options);
    }
    multiChannelKnob->setDefaultValue((int)eEdgeDetectMultiChannelTensor);
    page->addKnob(multiChannelKnob);
    multiChannel = multiChannelKnob;

    KnobDoublePtr blurSizeKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Blur Size"));
    blurSizeKnob->setName(kEdgeDetectParamBlurSize);
    blurSizeKnob->setHintToolTip(EffectInstance::tr("Size of the blur kernel applied before edge detection."));
    blurSizeKnob->setMinimum(0.);
    blurSizeKnob->setMaximum(DBL_MAX);
    blurSizeKnob->setDisplayMinimum(0.);
    blurSizeKnob->setDisplayMaximum(100.);
    blurSizeKnob->setIncrement(0.1);
    blurSizeKnob->setDecimals(1);
    blurSizeKnob->setDefaultValue(0.);
    page->addKnob(blurSizeKnob);
    blurSize = blurSizeKnob;

    KnobDoublePtr erodeSizeKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Erode Size"));
    erodeSizeKnob->setName(kEdgeDetectParamErodeSize);
    erodeSizeKnob->setHintToolTip(EffectInstance::tr("Size of the erosion performed after edge detection."));
    erodeSizeKnob->setMinimum(-DBL_MAX);
    erodeSizeKnob->setMaximum(DBL_MAX);
    erodeSizeKnob->setDisplayMinimum(-10.);
    erodeSizeKnob->setDisplayMaximum(10.);
    erodeSizeKnob->setIncrement(0.1);
    erodeSizeKnob->setDecimals(1);
    erodeSizeKnob->setDefaultValue(0.);
    page->addKnob(erodeSizeKnob);
    erodeSize = erodeSizeKnob;

    KnobBoolPtr nmsKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Non-Maxima Suppression"));
    nmsKnob->setName(kEdgeDetectParamNMS);
    nmsKnob->setHintToolTip(EffectInstance::tr("Perform non-maxima suppression (after edge detection and erosion): only values that are maximal in the direction orthogonal to the contour are kept. For multi-channel images, the contour direction estimation depends on the multi-channel operation."));
    nmsKnob->setDefaultValue(false);
    page->addKnob(nmsKnob);
    nms = nmsKnob;

    KnobBoolPtr expandRoDKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Expand RoD"));
    expandRoDKnob->setName(kEdgeDetectParamExpandRoD);
    expandRoDKnob->setHintToolTip(EffectInstance::tr("Expand the source region of definition by 1.5*size (3.6*sigma)."));
    expandRoDKnob->setDefaultValue(true);
    expandRoDKnob->setAddNewLine(false);
    page->addKnob(expandRoDKnob);
    expandRoD = expandRoDKnob;

    KnobBoolPtr cropToFormatKnob = AppManager::createKnob<KnobBool>(holder, EffectInstance::tr("Crop To Format"));
    cropToFormatKnob->setName(kEdgeDetectParamCropToFormat);
    cropToFormatKnob->setHintToolTip(EffectInstance::tr("If the source is inside the format and the effect extends it outside of the format, crop it to avoid unnecessary calculations. To avoid unwanted crops, only the borders that were inside of the format in the source clip will be cropped."));
    cropToFormatKnob->setDefaultValue(true);
    page->addKnob(cropToFormatKnob);
    cropToFormat = cropToFormatKnob;
} // EdgeDetectOp::createKnobs

EdgeDetectOp::Params
EdgeDetectOp::bake(const KnobReader& reader) const
{
    Params params;

    params.sizeX = params.sizeY = reader.value(blurSize);
    params.erodeSize = reader.value(erodeSize);
    const int f = reader.value(filter);
    if ((f >= (int)eEdgeDetectFilterSimple) && (f <= (int)eEdgeDetectFilterQuadratic)) {
        params.filter = (EdgeDetectFilterEnum)f;
    }
    const int m = reader.value(multiChannel);
    if ((m >= (int)eEdgeDetectMultiChannelSeparate) && (m <= (int)eEdgeDetectMultiChannelTensor)) {
        params.multiChannel = (EdgeDetectMultiChannelEnum)m;
    }
    params.nms = reader.value(nms);
    params.expandRoD = reader.value(expandRoD);
    params.cropToFormat = reader.value(cropToFormat);

    return params;
}

bool
EdgeDetectOp::isIdentity(const Params& /*params*/,
                         const PassContext& /*context*/) const
{
    return false;
}

PassList
EdgeDetectOp::passes(const Params& params,
                     const PassContext& context) const
{
    return edgeDetectPasses(atContext(params, context), context.scale);
}

EdgeDetect::EdgeDetect(NodePtr node)
    : ImageNode<EdgeDetectOp>(node)
{
}

EdgeDetect::~EdgeDetect()
{
}

void
EdgeDetect::addAcceptedComponents(int inputNb,
                                  std::list<ImageLayerDesc>* comps)
{
    ImageNode<EdgeDetectOp>::addAcceptedComponents(inputNb, comps);
    if (!isInputMask(inputNb)) {
        comps->push_back(ImageLayerDesc::getXYComponents());
    }
}

BlurKernels::Filter
EdgeDetect::blurFilter(EdgeDetectFilterEnum filter)
{
    switch (filter) {
    case eEdgeDetectFilterQuasiGaussian:
        return BlurKernels::eFilterQuasiGaussian;
    case eEdgeDetectFilterBox:
        return BlurKernels::eFilterBox;
    case eEdgeDetectFilterTriangle:
        return BlurKernels::eFilterTriangle;
    case eEdgeDetectFilterQuadratic:
        return BlurKernels::eFilterQuadratic;
    case eEdgeDetectFilterSimple:
    case eEdgeDetectFilterSobel:
    case eEdgeDetectFilterRotationInvariant:
    case eEdgeDetectFilterGaussian:
    default:
        return BlurKernels::eFilterGaussian;
    }
}

void
EdgeDetect::getErodeRadius(const RenderScale& scale,
                           const EdgeDetectParams& params,
                           int* rx,
                           int* ry)
{
    const OfxPointD rs = scale.toOfxPointD();
    const double par = (params.par > 0.) ? params.par : 1.;
    const double e = std::abs(params.erodeSize);

    *rx = (int)std::min(kMaxErodeRadius, std::floor(std::max(0., e / par) * rs.x));
    *ry = (int)std::min(kMaxErodeRadius, std::floor(std::max(0., e) * rs.y));
}

RectI
EdgeDetect::getSourceRoI(const RectI& rect,
                         const RenderScale& scale,
                         const EdgeDetectParams& params)
{
    return passListSourceRoI(rect, edgeDetectPasses(params, scale));
}

bool
EdgeDetect::clipRegionOfDefinition(double time,
                                   const RenderScale& scale,
                                   ViewIdx view,
                                   const EffectInstancePtr& source,
                                   const RectI& srcRoD,
                                   RectI* rod)
{
    const EdgeDetectParams params = bakeParams(op(), time, view);
    if (!params.cropToFormat) {
        return false;
    }
    RectI format;
    EffectInstancePtr formatSource = source ? source->getNearestNonIdentity(time) : EffectInstancePtr();
    if (formatSource) {
        format = formatSource->getRenderMetadata(time, view)->getOutputFormat();
    } else {
        Format projectFormat;
        getApp()->getProject()->getProjectDefaultFormat(&projectFormat);
        format = projectFormat;
    }
    const OfxPointD rs = scale.toOfxPointD();
    RectI enclosing;
    enclosing.x1 = (int)std::floor(format.x1 * rs.x);
    enclosing.x2 = (int)std::ceil(format.x2 * rs.x);
    enclosing.y1 = (int)std::floor(format.y1 * rs.y);
    enclosing.y2 = (int)std::ceil(format.y2 * rs.y);
    RectI inner;
    inner.x1 = (int)std::ceil(format.x1 * rs.x);
    inner.x2 = (int)std::floor(format.x2 * rs.x);
    inner.y1 = (int)std::ceil(format.y1 * rs.y);
    inner.y2 = (int)std::floor(format.y2 * rs.y);
    if (PassExec::isEmptyRect(inner)) {
        return false;
    }
    // Only the borders the source had inside its format are clamped back to it.
    bool changed = false;
    if ((rod->x1 < inner.x1) && (srcRoD.x1 >= enclosing.x1)) {
        rod->x1 = inner.x1;
        changed = true;
    }
    if ((rod->x2 > inner.x2) && (srcRoD.x2 <= enclosing.x2)) {
        rod->x2 = inner.x2;
        changed = true;
    }
    if ((rod->y1 < inner.y1) && (srcRoD.y1 >= enclosing.y1)) {
        rod->y1 = inner.y1;
        changed = true;
    }
    if ((rod->y2 > inner.y2) && (srcRoD.y2 <= enclosing.y2)) {
        rod->y2 = inner.y2;
        changed = true;
    }

    return changed;
} // EdgeDetect::clipRegionOfDefinition

NATRON_NAMESPACE_EXIT
