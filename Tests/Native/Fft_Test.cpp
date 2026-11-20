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
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Fft/Fft.h"

NATRON_NAMESPACE_USING

namespace {

using Fft::Complex;

const double kPi = 3.14159265358979323846;

std::vector<Complex>
randomComplex(std::size_t count,
              uint32_t seed)
{
    std::vector<Complex> out(count);
    uint32_t state = seed;
    const auto next = [&state]() {
        state = state * 1664525u + 1013904223u;
        return (float)(state >> 8) / 8388608.f - 1.f;
    };
    for (Complex& c : out) {
        const float re = next();
        c = Complex(re, next());
    }

    return out;
}

} // namespace

TEST(Fft, RefusesEmptySizesAndTheWrongForward)
{
    EXPECT_THROW(Fft::Plan2D(0, 8), std::invalid_argument);
    EXPECT_THROW(Fft::Plan2D(8, -1), std::invalid_argument);

    std::vector<Complex> c(16);
    std::vector<float> r(16);
    const Fft::Plan2D complexPlan(4, 4, Fft::Kind::complexToComplex);
    const Fft::Plan2D realPlan(4, 4, Fft::Kind::realToComplex);
    EXPECT_THROW(complexPlan.forwardReal(r.data(), c.data()), std::logic_error);
    EXPECT_THROW(realPlan.forward(c.data(), c.data()), std::logic_error);
}

TEST(Fft, AbortAfterTheRowPassReportsAborted)
{
    // Under the parallel threshold each pass is one band: polls are row band, between the passes, column band.
    const int size = 64;
    const Fft::Plan2D plan(size, size);
    const std::vector<Complex> input = randomComplex((std::size_t)size * size, 7);
    std::vector<Complex> full(input.size());
    ASSERT_TRUE(plan.forward(input.data(), full.data(), [] { return false; }));

    int polls = 0;
    std::vector<Complex> out(input.size());
    EXPECT_FALSE(plan.forward(input.data(), out.data(), [&] { return ++polls == 2; }));
    EXPECT_EQ(polls, 2);
    EXPECT_NE(out, full);

    EXPECT_FALSE(plan.forward(input.data(), out.data(), [] { return true; }));
}

TEST(Fft, DeltaTransformsToAConstant)
{
    const int w = 64, h = 32;
    const Fft::Plan2D plan(w, h);
    std::vector<Complex> data((std::size_t)w * h, Complex(0.f, 0.f));
    data[0] = Complex(1.f, 0.f);
    plan.forward(data.data(), data.data());
    for (std::size_t i = 0; i < data.size(); ++i) {
        ASSERT_NEAR(1.f, data[i].real(), 1e-6f) << "at " << i;
        ASSERT_NEAR(0.f, data[i].imag(), 1e-6f) << "at " << i;
    }

    std::vector<float> power(data.size());
    Fft::powerSpectrum(data.data(), power.data(), w, h);
    const float n = (float)(w * h);
    EXPECT_NEAR(1.f / (n * n), power[123], 1e-12f);
}

// A real 2-D cosine of frequency (fx, fy) has two peaks of N / 2, at (fx, fy) and its mirror (w - fx, h - fy).
TEST(Fft, SinusoidPeaksAtItsFrequency)
{
    const int w = 128, h = 64, fx = 5, fy = 3;
    const std::size_t n = (std::size_t)w * h;
    std::vector<float> real(n);
    std::vector<Complex> cplx(n);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float v = (float)std::cos(2. * kPi * ((double)fx * x / w + (double)fy * y / h));
            real[(std::size_t)y * w + x] = v;
            cplx[(std::size_t)y * w + x] = Complex(v, 0.f);
        }
    }

    std::vector<Complex> fromReal(n);
    Fft::Plan2D(w, h, Fft::Kind::realToComplex).forwardReal(real.data(), fromReal.data());
    std::vector<Complex> fromComplex(n);
    Fft::Plan2D(w, h).forward(cplx.data(), fromComplex.data());

    const std::size_t peak = (std::size_t)fy * w + fx;
    const std::size_t mirror = (std::size_t)(h - fy) * w + (w - fx);
    const float half = (float)n / 2.f;
    for (const std::vector<Complex>* spectrum : { &fromReal, &fromComplex }) {
        for (std::size_t i = 0; i < n; ++i) {
            const float expected = (i == peak || i == mirror) ? half : 0.f;
            ASSERT_NEAR(expected, std::abs((*spectrum)[i]), 1e-2f) << "at " << i % w << ", " << i / w;
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        ASSERT_NEAR(fromComplex[i].real(), fromReal[i].real(), 1e-2f) << "at " << i;
        ASSERT_NEAR(fromComplex[i].imag(), fromReal[i].imag(), 1e-2f) << "at " << i;
    }
}

TEST(Fft, InverseOfForwardRoundTrips)
{
    const int w = 1024, h = 1024;
    const std::vector<Complex> input = randomComplex((std::size_t)w * h, 7u);
    const Fft::Plan2D plan(w, h);
    std::vector<Complex> spectrum(input.size());
    plan.forward(input.data(), spectrum.data());
    std::vector<Complex> back(input.size());
    plan.inverse(spectrum.data(), back.data());
    float worst = 0.f;
    for (std::size_t i = 0; i < input.size(); ++i) {
        worst = std::max(worst, std::abs(back[i] - input[i]));
    }
    EXPECT_LT(worst, 1e-5f);

    // Odd, non-power-of-two sizes, in place.
    const int ow = 45, oh = 37;
    const std::vector<Complex> odd = randomComplex((std::size_t)ow * oh, 11u);
    std::vector<Complex> inPlace = odd;
    const Fft::Plan2D oddPlan(ow, oh);
    oddPlan.forward(inPlace.data(), inPlace.data());
    oddPlan.inverse(inPlace.data(), inPlace.data());
    for (std::size_t i = 0; i < odd.size(); ++i) {
        ASSERT_LT(std::abs(inPlace[i] - odd[i]), 1e-5f) << "at " << i;
    }
}

TEST(Fft, FftshiftCentresTheZeroFrequency)
{
    for (const int w : { 4, 5 }) {
        for (const int h : { 2, 3 }) {
            std::vector<float> data((std::size_t)w * h, 0.f);
            data[0] = 1.f;
            data[1] = 2.f;
            Fft::fftshift(data.data(), w, h);
            EXPECT_EQ(1.f, data[(std::size_t)(h / 2) * w + w / 2]) << w << "x" << h;
            EXPECT_EQ(2.f, data[(std::size_t)(h / 2) * w + (w / 2 + 1) % w]) << w << "x" << h;
        }
    }
}

// Multiplying by the spectrum of a delta at (1, 2) shifts the image circularly by (1, 2).
TEST(Fft, MultiplyConvolves)
{
    const int w = 16, h = 8;
    const std::size_t n = (std::size_t)w * h;
    const std::vector<Complex> image = randomComplex(n, 3u);
    std::vector<Complex> kernel(n, Complex(0.f, 0.f));
    kernel[(std::size_t)2 * w + 1] = Complex(1.f, 0.f);
    const Fft::Plan2D plan(w, h);
    std::vector<Complex> spectrum(n);
    plan.forward(image.data(), spectrum.data());
    plan.forward(kernel.data(), kernel.data());
    Fft::multiply(spectrum.data(), kernel.data(), n);
    plan.inverse(spectrum.data(), spectrum.data());
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const Complex expected = image[(std::size_t)((y + h - 2) % h) * w + (x + w - 1) % w];
            ASSERT_LT(std::abs(spectrum[(std::size_t)y * w + x] - expected), 1e-5f) << x << ", " << y;
        }
    }
}

// The far field of a disc of diameter D is the Airy pattern, whose first dark ring lies 1.22 N / D frequency samples
// from the centre of an N x N transform.
TEST(Fft, DiscPowerSpectrumHasTheAiryFirstDarkRing)
{
    const int n = 1024;
    const double diameter = 64.;
    const double radius = diameter / 2.;
    const double centre = n / 2;
    std::vector<float> aperture((std::size_t)n * n, 0.f);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const double dx = x - centre, dy = y - centre;
            if (dx * dx + dy * dy <= radius * radius) {
                aperture[(std::size_t)y * n + x] = 1.f;
            }
        }
    }
    std::vector<Complex> spectrum(aperture.size());
    Fft::Plan2D(n, n, Fft::Kind::realToComplex).forwardReal(aperture.data(), spectrum.data());
    std::vector<float> power(aperture.size());
    Fft::powerSpectrum(spectrum.data(), power.data(), n, n);
    Fft::fftshift(power.data(), n, n);

    const std::size_t row = (std::size_t)(n / 2) * n;
    const int c = n / 2;
    int firstMinimum = -1;
    for (int k = 1; k < n / 4; ++k) {
        if (power[row + c + k] < power[row + c + k - 1] && power[row + c + k] <= power[row + c + k + 1]) {
            firstMinimum = k;
            break;
        }
    }
    const double expected = 1.22 * n / diameter;
    ASSERT_GT(firstMinimum, 0);
    EXPECT_NEAR(expected, (double)firstMinimum, 1.) << "first dark ring at " << firstMinimum;
    // The ring is dark: far below the central peak.
    EXPECT_LT(power[row + c + firstMinimum], power[row + c] * 5e-3f);
}
