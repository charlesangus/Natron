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
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Filter/EdgeAware/EdgeAwareCommon.h"
#include "Engine/Nodes/Filter/EdgeAware/PoissonHeal.h"
#include "Engine/Nodes/Filter/RetouchRender.h"

NATRON_NAMESPACE_USING

namespace RC = RetouchCore;

/*
 * Timings of the retouch's processing, printed per case. Normal runs use small images so that
 * they stay quick; NATRON_RETOUCH_BENCH=1 runs the 2K and 4K cases and checks the heal solver's
 * speed against darktable's relaxation.
 */
namespace {
bool
fullBench()
{
    const char* value = std::getenv("NATRON_RETOUCH_BENCH");

    return value && (std::strcmp(value, "0") != 0) && (value[0] != '\0');
}

// Runs the bodies on every hardware thread, as a render does.
void
threadedFor(int count,
            const std::function<void(int)>& body)
{
    const int threads = std::max(1, std::min(count, (int)std::thread::hardware_concurrency()));
    if (threads <= 1) {
        EdgeAware::serialFor(count, body);

        return;
    }
    std::atomic<int> next(0);
    std::vector<std::thread> pool;
    for (int t = 0; t < threads; ++t) {
        pool.emplace_back([&]() {
            for (int i = next++; i < count; i = next++) {
                body(i);
            }
        });
    }
    for (std::thread& thread : pool) {
        thread.join();
    }
}

std::vector<float>
benchImage(int width,
           int height)
{
    std::vector<float> image((std::size_t)width * height * 4);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float u = (float)x / width;
            const float v = (float)y / height;
            for (int c = 0; c < 4; ++c) {
                image[((std::size_t)y * width + x) * 4 + c] = 0.2f + 0.5f * u * (1.f + 0.2f * c) + 0.2f * v
                    + 0.04f * std::sin(0.9f * x + 1.7f * y + c) * std::cos(0.37f * y - 0.21f * x);
            }
        }
    }

    return image;
}

// A brush stroke of the given width and length in pixels, centred in the image, horizontal or at
// 45 degrees, its source a quarter of the image to the left.
RC::Shape
benchStroke(RC::AlgorithmEnum algorithm,
            int scale,
            int width,
            int height,
            float strokeWidth,
            float strokeLength,
            bool diagonal)
{
    const float half = 0.5f * strokeLength * (diagonal ? std::sqrt(0.5f) : 1.f);
    RC::BrushPoint a, b;
    a.x = 0.5f * width - half;
    a.y = 0.5f * height - (diagonal ? half : 0.f);
    a.radius = 0.5f * strokeWidth;
    a.hardness = 0.8f;
    b = a;
    b.x = 0.5f * width + half;
    b.y = 0.5f * height + (diagonal ? half : 0.f);
    RC::Shape shape;
    shape.settings.algorithm = algorithm;
    shape.settings.scale = scale;
    shape.maskRoi = RC::brushBounds({ a, b });
    RC::rasterizeBrush({ a, b }, shape.maskRoi, &shape.mask);
    shape.dx = 0.25f * width;
    shape.dy = 0.f;

    return shape;
}

double
millisecondsSince(const std::chrono::steady_clock::time_point& start)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

struct BenchSize {
    const char* name;
    int width;
    int height;
    float strokeWidth;
    float strokeLength;
};
} // anon namespace

// RC::process on one brush stroke, for each image size, render scale, Scales setting, algorithm
// and stroke direction: prints the milliseconds of each render.
TEST(NativeRetouchBench, ProcessTimings)
{
    std::vector<BenchSize> sizes;
    if (fullBench()) {
        sizes.push_back({ "2K", 2048, 1080, 100.f, 800.f });
        sizes.push_back({ "4K", 4096, 2160, 100.f, 800.f });
    } else {
        sizes.push_back({ "small", 256, 136, 12.f, 100.f });
    }
    for (const BenchSize& size : sizes) {
        for (float renderScale : { 1.f, 0.5f }) {
            const int width = std::max(1, (int)std::lround(size.width * renderScale));
            const int height = std::max(1, (int)std::lround(size.height * renderScale));
            const std::vector<float> image = benchImage(width, height);
            for (int scales : { 0, 5 }) {
                for (RC::AlgorithmEnum algorithm : { RC::eAlgorithmClone, RC::eAlgorithmHeal }) {
                    for (bool diagonal : { false, true }) {
                        RC::Params params;
                        params.numScales = scales;
                        params.renderScale = renderScale;
                        const std::vector<RC::Shape> shapes(1, benchStroke(algorithm, 0, width, height, size.strokeWidth * renderScale, size.strokeLength * renderScale, diagonal));
                        std::vector<float> out = image;
                        const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
                        RC::process(out.data(), width, height, params, shapes, threadedFor);
                        const double ms = millisecondsSince(start);
                        std::printf("[ retouch  ] %s scale %.1f, Scales %d, %s, %s: %.1f ms\n", size.name, renderScale, scales,
                                    (algorithm == RC::eAlgorithmHeal) ? "heal" : "clone", diagonal ? "diagonal" : "horizontal", ms);
                        bool finite = true;
                        for (float v : out) {
                            finite = finite && std::isfinite(v);
                        }
                        EXPECT_TRUE(finite) << size.name;
                    }
                }
            }
        }
    }
}

// The heal of one stroke by the multigrid solver against darktable's 2000 relaxation sweeps. With
// NATRON_RETOUCH_BENCH set, on the 100 x 800 px stroke, where the relaxation stops unconverged,
// the multigrid residual is no larger and the solve at least 20 times faster.
TEST(NativeRetouchBench, MultigridHealAgainstRelaxation)
{
    const bool bench = fullBench();
    const int width = bench ? 1000 : 160;
    const int height = bench ? 300 : 48;
    const RC::Shape stroke = benchStroke(RC::eAlgorithmHeal, 0, width, height, bench ? 100.f : 16.f, bench ? 800.f : 120.f, false);
    const std::vector<float> src = benchImage(width, height);
    std::vector<float> dest(src.size());
    for (std::size_t i = 0; i < dest.size(); ++i) {
        dest[i] = 1.2f - 0.6f * src[i];
    }
    std::vector<float> mask((std::size_t)width * height, 0.f);
    for (int y = 0; y < stroke.maskRoi.height; ++y) {
        for (int x = 0; x < stroke.maskRoi.width; ++x) {
            const int ix = x + stroke.maskRoi.x;
            const int iy = y + stroke.maskRoi.y;
            if ((ix >= 1) && (ix < width - 1) && (iy >= 1) && (iy < height - 1)) {
                mask[(std::size_t)iy * width + ix] = stroke.mask[(std::size_t)y * stroke.maskRoi.width + x];
            }
        }
    }

    std::vector<float> relaxed = dest;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    EdgeAware::poissonHeal(src.data(), relaxed.data(), mask.data(), width, height, EdgeAware::kPoissonHealDefaultIterations, EdgeAware::serialFor);
    const double sorMs = millisecondsSince(start);

    std::vector<float> multigrid = dest;
    start = std::chrono::steady_clock::now();
    const int cycles = EdgeAware::poissonHealMultigrid(src.data(), multigrid.data(), mask.data(), width, height, 100, EdgeAware::serialFor);
    const double multigridMs = millisecondsSince(start);

    const double sorResidual = EdgeAware::poissonHealResidual(src.data(), relaxed.data(), mask.data(), width, height);
    const double multigridResidual = EdgeAware::poissonHealResidual(src.data(), multigrid.data(), mask.data(), width, height);
    std::printf("[ retouch  ] heal %dx%d: relaxation %.1f ms (residual %g), multigrid %.1f ms in %d cycles (residual %g), %.1fx\n",
                width, height, sorMs, sorResidual, multigridMs, cycles, multigridResidual, sorMs / std::max(multigridMs, 1e-3));
    EXPECT_GT(cycles, 0);
    if (bench) {
        EXPECT_LE(multigridResidual, sorResidual);
        EXPECT_GE(sorMs / std::max(multigridMs, 1e-3), 20.);
    }
}
