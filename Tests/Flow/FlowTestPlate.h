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

#ifndef Tests_Flow_FlowTestPlate_h
#define Tests_Flow_FlowTestPlate_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <random>
#include <thread>
#include <vector>

#include "Engine/Flow/FlowField.h"
#include "Engine/Flow/FlowPyramid.h"

NATRON_NAMESPACE_ENTER

namespace FlowTest {
/**
 * @brief Fixed-seed value noise over the whole plane, about 0..1, with features from 4 to 32 pixels.
 * It is defined at any position, so a moved frame is sampled exactly and carries no resampling error.
 **/
class NoiseTexture {
public:
    explicit NoiseTexture(std::uint32_t seed)
        : _lattice(kSize * kSize)
    {
        // Raw engine output, since distributions differ between standard libraries.
        std::mt19937 engine(seed);
        for (float& value : _lattice) {
            value = static_cast<float>(engine()) / 4294967296.f;
        }
    }

    float operator()(double x,
                     double y) const
    {
        const double amplitudes[] = { 0.4, 0.3, 0.2, 0.1 };
        const double wavelengths[] = { 32., 16., 8., 4. };
        double sum = 0.;
        for (int octave = 0; octave < 4; ++octave) {
            sum += amplitudes[octave] * lattice(x / wavelengths[octave] + 17.3 * octave, y / wavelengths[octave] + 5.9 * octave);
        }
        return static_cast<float>(sum);
    }

private:
    static const int kSize = 256;

    double at(int x,
              int y) const
    {
        return _lattice[static_cast<std::size_t>(y & (kSize - 1)) * kSize + (x & (kSize - 1))];
    }

    double lattice(double x,
                   double y) const
    {
        const double fx = std::floor(x);
        const double fy = std::floor(y);
        const int ix = static_cast<int>(fx);
        const int iy = static_cast<int>(fy);
        const double tx = x - fx;
        const double ty = y - fy;
        const double sx = tx * tx * (3. - 2. * tx);
        const double sy = ty * ty * (3. - 2. * ty);
        const double top = at(ix, iy) + sx * (at(ix + 1, iy) - at(ix, iy));
        const double bottom = at(ix, iy + 1) + sx * (at(ix + 1, iy + 1) - at(ix, iy + 1));
        return top + sy * (bottom - top);
    }

    std::vector<float> _lattice;
};

/// A single-component frame sampled from a function of position.
struct Frame {
    int width = 0;
    int height = 0;
    std::vector<float> pixels;

    Frame(int w,
          int h,
          const std::function<float(double, double)>& function)
        : width(w)
        , height(h)
        , pixels(static_cast<std::size_t>(w) * h)
    {
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                pixels[static_cast<std::size_t>(y) * w + x] = function(x, y);
            }
        }
    }

    FlowImageView view() const
    {
        FlowImageView image;
        image.pixels = pixels.data();
        image.width = width;
        image.height = height;
        image.nComps = 1;
        image.rowElements = width;
        return image;
    }

    FlowPyramid pyramid() const
    {
        FlowPyramid result;
        result.build(view(), eFlowChannelsLuminance, 0, FlowContext());
        return result;
    }
};

/// A context whose ParallelFor runs on several threads, for checking results do not depend on them.
inline FlowContext
threadedContext()
{
    FlowContext context;
    context.parallel = [](int count, const std::function<void(int)>& body) {
        std::atomic<int> next(0);
        std::vector<std::thread> threads;
        for (int t = 0; t < 3; ++t) {
            threads.emplace_back([&]() {
                for (int i = next++; i < count; i = next++) {
                    body(i);
                }
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
    };
    return context;
}

/// A displacement at a position: the ground truth a field is measured against.
using TrueFlow = std::function<void(int x, int y, float& u, float& v)>;

/// Mean endpoint error over the pixels where inside(x, y) holds.
inline double
meanEndpointError(const FlowField& flow,
                  const TrueFlow& truth,
                  const std::function<bool(int, int)>& inside)
{
    double sum = 0.;
    long count = 0;
    for (int y = 0; y < flow.height; ++y) {
        for (int x = 0; x < flow.width; ++x) {
            if (!inside(x, y)) {
                continue;
            }
            float u, v;
            truth(x, y, u, v);
            const double du = flow.u.at(x, y) - u;
            const double dv = flow.v.at(x, y) - v;
            sum += std::sqrt(du * du + dv * dv);
            ++count;
        }
    }
    return count ? sum / static_cast<double>(count) : 0.;
}

/// Mean endpoint error, leaving out a margin at the frame's edges.
inline double
meanEndpointError(const FlowField& flow,
                  const TrueFlow& truth,
                  int margin)
{
    return meanEndpointError(flow, truth, [&](int x, int y) {
        return x >= margin && y >= margin && x < flow.width - margin && y < flow.height - margin;
    });
}

inline TrueFlow
translation(float tx,
            float ty)
{
    return [tx, ty](int, int, float& u, float& v) {
        u = tx;
        v = ty;
    };
}

/// The flow of a rotation of `degrees` about the frame's centre.
inline TrueFlow
rotation(int width,
         int height,
         double degrees)
{
    const double angle = degrees * 3.14159265358979323846 / 180.;
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    const double cx = 0.5 * (width - 1);
    const double cy = 0.5 * (height - 1);
    return [=](int x, int y, float& u, float& v) {
        const double dx = x - cx;
        const double dy = y - cy;
        u = static_cast<float>(c * dx - s * dy - dx);
        v = static_cast<float>(s * dx + c * dy - dy);
    };
}

/// The frame a rotation of `degrees` about the centre carries the texture to.
inline Frame
rotatedFrame(const NoiseTexture& texture,
             int width,
             int height,
             double degrees)
{
    const double angle = degrees * 3.14159265358979323846 / 180.;
    const double c = std::cos(angle);
    const double s = std::sin(angle);
    const double cx = 0.5 * (width - 1);
    const double cy = 0.5 * (height - 1);
    return Frame(width, height, [&](double x, double y) {
        const double dx = x - cx;
        const double dy = y - cy;
        return texture(cx + c * dx + s * dy, cy - s * dx + c * dy);
    });
}

/// A frame of the texture displaced by (tx, ty).
inline Frame
translatedFrame(const NoiseTexture& texture,
                int width,
                int height,
                double tx,
                double ty)
{
    return Frame(width, height, [&](double x, double y) { return texture(x - tx, y - ty); });
}
} // namespace FlowTest

NATRON_NAMESPACE_EXIT

#endif // Tests_Flow_FlowTestPlate_h
