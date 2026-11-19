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

#ifndef NATRON_ENGINE_FFT_FFT_H
#define NATRON_ENGINE_FFT_FFT_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <complex>
#include <cstddef>

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuFft.h"
#endif

NATRON_NAMESPACE_ENTER

namespace Fft {

using Complex = std::complex<float>;

// What a plan's forward transform reads: interleaved complex samples, or real ones.
enum class Kind {
    complexToComplex,
    realToComplex,
};

/**
 * @brief A 2-D discrete Fourier transform of width x height samples, stored row by row with x fastest.
 *
 * The forward transform has the kernel exp(-2 pi i (kx x / width + ky y / height)) and is unscaled; the inverse has
 * the opposite sign and scales by 1 / (width * height), so inverse(forward(x)) gives x back. Spectra hold the full
 * width x height frequencies in the same layout, the zero frequency at index 0 (see fftshift()).
 *
 * The CPU transforms take any size. The GPU transforms take powers of two up to gpu::kGpuFftMaxSize along each axis
 * (see supportsGpu()); callers pad other sizes.
 **/
class Plan2D {
public:
    // Throws std::invalid_argument unless both sizes are at least 1.
    Plan2D(int width, int height, Kind kind = Kind::complexToComplex);

    int width() const { return _width; }
    int height() const { return _height; }
    Kind kind() const { return _kind; }
    std::size_t sampleCount() const { return (std::size_t)_width * _height; }

    // A complexToComplex plan's forward transform. `in` may equal `out`.
    void forward(const Complex* in, Complex* out) const;

    // A realToComplex plan's forward transform, writing the whole Hermitian spectrum. `in` and `out` must not overlap.
    void forwardReal(const float* in, Complex* out) const;

    // The inverse of either kind's forward transform, scaled by 1 / sampleCount(). `in` may equal `out`.
    void inverse(const Complex* in, Complex* out) const;

    bool supportsGpu() const;

#ifdef NATRON_HAVE_GPU
    // The GPU versions of forward(), forwardReal() and inverse() on device buffers: complex buffers hold
    // 2 * sampleCount() floats, real ones sampleCount(). The source may be the destination. Dispatches that wrote the
    // source must be ordered before the call, and dispatches that read the destination after it, with
    // GpuRecordContext::computeBarrier(). A plan supportsGpu() refuses is refused here.
    gpu::GpuStatus recordForward(gpu::GpuRecordContext& ctx, const gpu::GpuBuffer& in, const gpu::GpuBuffer& out) const;
    gpu::GpuStatus recordForwardReal(gpu::GpuRecordContext& ctx, const gpu::GpuBuffer& in, const gpu::GpuBuffer& out) const;
    gpu::GpuStatus recordInverse(gpu::GpuRecordContext& ctx, const gpu::GpuBuffer& in, const gpu::GpuBuffer& out) const;
#endif

private:
    int _width;
    int _height;
    Kind _kind;
};

// Swaps the half-spaces of a width x height image so the zero frequency moves from index 0 to (width / 2, height / 2),
// in place. Odd sizes put the zero frequency at (width / 2, height / 2) rounded down, as the usual fftshift does.
void fftshift(Complex* data, int width, int height);
void fftshift(float* data, int width, int height);

// |F|^2 / N^2 for every sample of a spectrum of N = width * height samples, so a constant image of value a has a
// power spectrum of a^2 at the zero frequency.
void powerSpectrum(const Complex* spectrum, float* power, int width, int height);

// spectrum[i] *= transfer[i] for the `count` samples, the frequency-domain form of a convolution.
void multiply(Complex* spectrum, const Complex* transfer, std::size_t count);

} // namespace Fft

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_FFT_FFT_H
