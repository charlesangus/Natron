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

#include "Fft.h"

#include <algorithm>
#include <stdexcept>

#include <atomic>

#include <pocketfft_hdronly.h>

#include "Engine/PoolParallelFor.h"
#include "Engine/RectI.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace Fft {

namespace {

    const qint64 kMinParallelSamples = 128 * 128;

    const std::ptrdiff_t kComplexBytes = (std::ptrdiff_t)sizeof(Complex);

    bool
    shouldStop(const AbortCheck& abortCheck,
               std::atomic<bool>& aborted)
    {
        if (aborted.load()) {
            return true;
        }
        if (abortCheck && abortCheck()) {
            aborted.store(true);
            return true;
        }
        return false;
    }

    void
    forEachLineBand(int count,
                    int length,
                    const std::function<void(int first, int last)>& fn)
    {
        forEachRowBand(RectI(0, 0, length, count), kMinParallelSamples, fn);
    }

    // pocketfft is called once per band with nthreads = 1, so its own thread pool never starts and the engine's pool is
    // the only one splitting the work.
    bool
    transformColumns(int width,
                     int height,
                     bool forward,
                     const Complex* in,
                     Complex* out,
                     float scale,
                     const AbortCheck& abortCheck)
    {
        const pocketfft::stride_t strides { width * kComplexBytes, kComplexBytes };
        std::atomic<bool> aborted { false };
        forEachLineBand(width, height, [&](int x1, int x2) {
            if (shouldStop(abortCheck, aborted)) {
                return;
            }
            const pocketfft::shape_t shape { (std::size_t)height, (std::size_t)(x2 - x1) };
            pocketfft::c2c<float>(shape, strides, strides, { 0 }, forward, in + x1, out + x1, scale, 1);
        });
        return !aborted.load();
    }

    bool
    transformComplex(int width,
                     int height,
                     bool forward,
                     const Complex* in,
                     Complex* out,
                     float scale,
                     const AbortCheck& abortCheck)
    {
        const pocketfft::stride_t strides { width * kComplexBytes, kComplexBytes };
        std::atomic<bool> aborted { false };
        forEachLineBand(height, width, [&](int y1, int y2) {
            if (shouldStop(abortCheck, aborted)) {
                return;
            }
            const pocketfft::shape_t shape { (std::size_t)(y2 - y1), (std::size_t)width };
            const std::size_t offset = (std::size_t)y1 * width;
            pocketfft::c2c<float>(shape, strides, strides, { 1 }, forward, in + offset, out + offset, scale, 1);
        });
        if (aborted.load() || (abortCheck && abortCheck())) {
            return false;
        }
        return transformColumns(width, height, forward, out, out, 1.f, abortCheck);
    }

} // namespace

Plan2D::Plan2D(int width,
               int height,
               Kind kind)
    : _width(width)
    , _height(height)
    , _kind(kind)
{
    if ((width < 1) || (height < 1)) {
        throw std::invalid_argument("Fft::Plan2D: width and height must be at least 1");
    }
}

bool
Plan2D::forward(const Complex* in,
                Complex* out,
                const AbortCheck& abortCheck) const
{
    if (_kind != Kind::complexToComplex) {
        throw std::logic_error("Fft::Plan2D::forward: the plan transforms real input, use forwardReal()");
    }
    return transformComplex(_width, _height, true, in, out, 1.f, abortCheck);
}

bool
Plan2D::forwardReal(const float* in,
                    Complex* out,
                    const AbortCheck& abortCheck) const
{
    if (_kind != Kind::realToComplex) {
        throw std::logic_error("Fft::Plan2D::forwardReal: the plan transforms complex input, use forward()");
    }
    const int width = _width;
    const int half = width / 2 + 1;
    const pocketfft::stride_t inStrides { width * (std::ptrdiff_t)sizeof(float), (std::ptrdiff_t)sizeof(float) };
    const pocketfft::stride_t outStrides { width * kComplexBytes, kComplexBytes };
    std::atomic<bool> aborted { false };
    forEachLineBand(_height, width, [&](int y1, int y2) {
        if (shouldStop(abortCheck, aborted)) {
            return;
        }
        const pocketfft::shape_t shape { (std::size_t)(y2 - y1), (std::size_t)width };
        const std::size_t offset = (std::size_t)y1 * width;
        pocketfft::r2c<float>(shape, inStrides, outStrides, (std::size_t)1, true, in + offset, out + offset, 1.f, 1);
        // r2c writes frequencies 0 to width / 2; a real row's spectrum has X[width - k] = conj(X[k]).
        for (int y = y1; y < y2; ++y) {
            Complex* row = out + (std::size_t)y * width;
            for (int k = half; k < width; ++k) {
                row[k] = std::conj(row[width - k]);
            }
        }
    });
    if (aborted.load() || (abortCheck && abortCheck())) {
        return false;
    }
    return transformColumns(width, _height, true, out, out, 1.f, abortCheck);
}

bool
Plan2D::inverse(const Complex* in,
                Complex* out,
                const AbortCheck& abortCheck) const
{
    return transformComplex(_width, _height, false, in, out, 1.f / (float)sampleCount(), abortCheck);
}

bool
Plan2D::supportsGpu() const
{
#ifdef NATRON_HAVE_GPU
    return gpu::gpuFftSupportsSize((uint32_t)_width, (uint32_t)_height);
#else
    return false;
#endif
}

#ifdef NATRON_HAVE_GPU

gpu::GpuStatus
Plan2D::recordForward(gpu::GpuRecordContext& ctx,
                      const gpu::GpuBuffer& in,
                      const gpu::GpuBuffer& out) const
{
    if (_kind != Kind::complexToComplex) {
        return { VK_ERROR_INITIALIZATION_FAILED, "Fft::Plan2D::recordForward: the plan transforms real input" };
    }
    return gpu::recordFft2D(ctx, { &in, &out, (uint32_t)_width, (uint32_t)_height, false, false });
}

gpu::GpuStatus
Plan2D::recordForwardReal(gpu::GpuRecordContext& ctx,
                          const gpu::GpuBuffer& in,
                          const gpu::GpuBuffer& out) const
{
    if (_kind != Kind::realToComplex) {
        return { VK_ERROR_INITIALIZATION_FAILED, "Fft::Plan2D::recordForwardReal: the plan transforms complex input" };
    }
    return gpu::recordFft2D(ctx, { &in, &out, (uint32_t)_width, (uint32_t)_height, false, true });
}

gpu::GpuStatus
Plan2D::recordInverse(gpu::GpuRecordContext& ctx,
                      const gpu::GpuBuffer& in,
                      const gpu::GpuBuffer& out) const
{
    return gpu::recordFft2D(ctx, { &in, &out, (uint32_t)_width, (uint32_t)_height, true, false });
}

#endif // NATRON_HAVE_GPU

namespace {

    template <typename T>
    void
    shift(T* data,
          int width,
          int height)
    {
        const int dx = width / 2;
        const int dy = height / 2;
        if (dx > 0) {
            for (int y = 0; y < height; ++y) {
                T* row = data + (std::size_t)y * width;
                std::rotate(row, row + (width - dx), row + width);
            }
        }
        if (dy > 0) {
            const std::size_t total = (std::size_t)width * height;
            std::rotate(data, data + (std::size_t)(height - dy) * width, data + total);
        }
    }

} // namespace

void
fftshift(Complex* data,
         int width,
         int height)
{
    shift(data, width, height);
}

void
fftshift(float* data,
         int width,
         int height)
{
    shift(data, width, height);
}

void
powerSpectrum(const Complex* spectrum,
              float* power,
              int width,
              int height)
{
    const double n = (double)width * height;
    const float norm = (float)(1. / (n * n));
    const std::size_t count = (std::size_t)width * height;
    for (std::size_t i = 0; i < count; ++i) {
        power[i] = std::norm(spectrum[i]) * norm;
    }
}

void
multiply(Complex* spectrum,
         const Complex* transfer,
         std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i) {
        spectrum[i] *= transfer[i];
    }
}

} // namespace Fft

NATRON_NAMESPACE_EXIT
