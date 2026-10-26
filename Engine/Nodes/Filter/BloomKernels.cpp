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

#include "BloomKernels.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuBlur.h"
#include "Engine/Gpu/GpuMorphology.h"
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace BloomKernels {
namespace {
    using LineFilterPtr = std::shared_ptr<const BlurKernels::LineFilter>;

    // A blur runs on the deepest level where its standard deviation is still this many samples.
    const double kLevelMinSigma = 2.5;
    // Deeper than any blur of at most kMaxBlurSize goes, and shallow enough that upTaps()'s
    // 2 * factor stays exact in float.
    const int kMaxLevels = 20;
    // A bound no halo of a blur of at most kMaxBlurSize reaches; a halo is clamped to it all the
    // same, so the regions of interest grown by it stay inside int.
    const int64_t kMaxHalo = int64_t(1) << 24;
    // The [1 3 3 1] / 8 downsampling kernel, applied to samples 2j - 1 to 2j + 2 for sample j.
    const float kDown[4] = { 0.125f, 0.375f, 0.375f, 0.125f };

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

    int
    floorDiv(int a,
             int b)
    {
        const int q = a / b;

        return ((a % b != 0) && ((a < 0) != (b < 0))) ? (q - 1) : q;
    }

    int
    ceilDiv(int a,
            int b)
    {
        return -floorDiv(-a, b);
    }

    /// The size of blur i, size * ratio^i, capped at kMaxBlurSize.
    double
    blurSize(double size,
             double ratio,
             int i)
    {
        return std::min(size * std::pow(ratio, (double)i), kMaxBlurSize);
    }

    /// The deepest level along an axis for a blur of standard deviation sigma.
    int
    levelFor(double sigma)
    {
        int k = 0;

        while ((k < kMaxLevels) && (std::ldexp(sigma, -(k + 1)) >= kLevelMinSigma)) {
            ++k;
        }

        return k;
    }

    /// The standard deviation left to blur at level k, in level samples: the downsampling's own
    /// variance, (1 - 4^-k) / 4 level samples squared, taken out.
    double
    levelSigma(double sigma,
               int k)
    {
        const double s = std::ldexp(sigma, -k);
        const double v = s * s - 0.25 * (1. - std::ldexp(1., -2 * k));

        return std::sqrt(std::max(0., v));
    }

    /// The level samples, and their weights, that pixel x reads when upsampling by factor: a
    /// Catmull-Rom spline through four samples, or the sample itself at factor 1. bloom.slang's
    /// upTaps() computes the same in float.
    struct Taps {
        int i0 = 0;
        int n = 1;
        float w[4] = { 1.f, 0.f, 0.f, 0.f };
    };

    Taps
    upTaps(int factor,
           int x)
    {
        Taps t;

        if (factor == 1) {
            t.i0 = x;

            return t;
        }
        // Level sample i is centred at factor * i + factor / 2, so pixel x sits at level position
        // (2x + 1 - factor) / (2 factor); the fraction is exact in float.
        const int num = 2 * x + 1 - factor;
        const int den = 2 * factor;
        const int i = floorDiv(num, den);
        const float f = (float)(num - i * den) / (float)den;
        const float f2 = f * f;
        const float f3 = f2 * f;
        t.i0 = i - 1;
        t.n = 4;
        t.w[0] = 0.5f * (-f3 + 2.f * f2 - f);
        t.w[1] = 0.5f * (3.f * f3 - 5.f * f2 + 2.f);
        t.w[2] = 0.5f * (-3.f * f3 + 4.f * f2 + f);
        t.w[3] = 0.5f * (f3 - f2);

        return t;
    }

    /// Index i of a line of n samples as read past its ends: the end sample, or -1 for zero.
    inline int
    resolveIndex(int i,
                 int n,
                 bool nearest)
    {
        if ((i >= 0) && (i < n)) {
            return i;
        }
        if (!nearest || (n <= 0)) {
            return -1;
        }

        return (i < 0) ? 0 : (n - 1);
    }

    struct Geometry {
        int width = 0;
        int height = 0;
        std::size_t planeSize = 0;
    };

    Geometry
    geometryOf(const RectI& r)
    {
        Geometry g;

        g.width = std::max(0, r.width());
        g.height = std::max(0, r.height());
        g.planeSize = (std::size_t)g.width * g.height;

        return g;
    }

    /// Items [begin(line), begin(line + 1)) of n, for line in [0, lines): every item once.
    inline int
    itemsBegin(int line,
               int n,
               int lines)
    {
        return (int)(((int64_t)line * n) / lines);
    }
} // anonymous namespace

struct Plan {
    struct Blur {
        // The pyramid level the blur runs on, and how many times smaller it is along each axis.
        int level = 0;
        int factorX = 1;
        int factorY = 1;
        LineFilterPtr filterX;
        LineFilterPtr filterY;
        int radiusX = 0;
        int radiusY = 0;
#ifdef NATRON_HAVE_GPU
        // The FIR Gaussian's half kernels, or the other filters.
        std::vector<float> weightsX;
        std::vector<float> weightsY;
        gpu::GpuLineFilter lineX;
        gpu::GpuLineFilter lineY;
#endif
    };

    bool fir = false;
    bool nearest = true;
    int count = 1;
    // The axis each level after the first halves (0 for x, 1 for y).
    std::vector<int> steps;
    std::vector<Blur> blurs;
    int haloX = 0;
    int haloY = 0;
    double sigma = 0.;
    bool recursive = false;
};

namespace {
    /// The rect of each level when the first is r0: the samples of each whose kernel reaches into the
    /// previous level.
    std::vector<RectI>
    levelRects(const Plan& plan,
               const RectI& r0)
    {
        std::vector<RectI> rects(1, r0);

        for (int axis : plan.steps) {
            RectI r = rects.back();
            if (!PassExec::isEmptyRect(r)) {
                if (axis == 0) {
                    r.x1 = ceilDiv(r.x1 - 2, 2);
                    r.x2 = floorDiv(r.x2, 2) + 1;
                } else {
                    r.y1 = ceilDiv(r.y1 - 2, 2);
                    r.y2 = floorDiv(r.y2, 2) + 1;
                }
            }
            rects.push_back(r);
        }

        return rects;
    }

    /// The levels after the first and the blurred copy of the level a blur runs on, for the scratch
    /// of one plane job. They are rewritten whole by every job, so a job of the same shape reuses them.
    class CpuLevels {
    public:
        void prepare(const Plan& plan,
                     const PassScratch& scratch)
        {
            std::lock_guard<std::mutex> lock(_mutex);

            if (_ready && (_rect.x1 == scratch.rect.x1) && (_rect.y1 == scratch.rect.y1) && (_rect.x2 == scratch.rect.x2) && (_rect.y2 == scratch.rect.y2) && (_nPlanes == scratch.nPlanes)) {
                return;
            }
            _rect = scratch.rect;
            _nPlanes = scratch.nPlanes;
            _rects = levelRects(plan, scratch.rect);
            _geometry.clear();
            _storage.assign(_rects.size(), std::vector<float>());
            std::size_t workPlane = 0;
            for (std::size_t m = 0; m < _rects.size(); ++m) {
                const Geometry g = geometryOf(_rects[m]);
                _geometry.push_back(g);
                if (m > 0) {
                    _storage[m].resize(g.planeSize * _nPlanes);
                }
                workPlane = std::max(workPlane, g.planeSize);
            }
            _work.resize(workPlane * _nPlanes);
            _ready = true;
        }

        const RectI& rect(int m) const
        {
            return _rects[m];
        }

        const Geometry& geometry(int m) const
        {
            return _geometry[m];
        }

        float* level(const PassScratch& scratch,
                     int input,
                     int m,
                     int p)
        {
            if (m == 0) {
                return scratch.plane(input, p);
            }

            return _storage[m].data() + (std::size_t)p * _geometry[m].planeSize;
        }

        // The blurred copy of level m.
        float* work(int m,
                    int p)
        {
            return _work.data() + (std::size_t)p * _geometry[m].planeSize;
        }

    private:
        std::mutex _mutex;
        bool _ready = false;
        RectI _rect;
        int _nPlanes = 0;
        std::vector<RectI> _rects;
        std::vector<Geometry> _geometry;
        std::vector<std::vector<float>> _storage;
        std::vector<float> _work;
    };

    using CpuLevelsPtr = std::shared_ptr<CpuLevels>;

    /// Level m from level m - 1, along the axis plan.steps[m - 1].
    std::function<PassRowFn(const PassScratch&)>
    downWorker(const PlanPtr& plan,
               const CpuLevelsPtr& levels,
               int input,
               int m)
    {
        return [plan, levels, input, m](const PassScratch& scratch) -> PassRowFn {
            levels->prepare(*plan, scratch);

            return [&scratch, plan, levels, input, m](int line) {
                const int lines = scratch.height;
                const int p = line / lines;
                const int r = line % lines;
                const RectI& src = levels->rect(m - 1);
                const RectI& dst = levels->rect(m);
                const Geometry& gs = levels->geometry(m - 1);
                const Geometry& gd = levels->geometry(m);
                const float* in = levels->level(scratch, input, m - 1, p);
                float* out = levels->level(scratch, input, m, p);
                const bool nearest = plan->nearest;
                const int rowEnd = itemsBegin(r + 1, gd.height, lines);

                for (int row = itemsBegin(r, gd.height, lines); row < rowEnd; ++row) {
                    float* o = out + (std::size_t)row * gd.width;
                    if (plan->steps[m - 1] == 0) {
                        const float* i = in + (std::size_t)row * gs.width;
                        for (int j = 0; j < gd.width; ++j) {
                            const int base = 2 * (dst.x1 + j) - 1 - src.x1;
                            float v = 0.f;
                            for (int t = 0; t < 4; ++t) {
                                const int k = resolveIndex(base + t, gs.width, nearest);
                                v += kDown[t] * ((k < 0) ? 0.f : i[k]);
                            }
                            o[j] = v;
                        }
                    } else {
                        const int base = 2 * (dst.y1 + row) - 1 - src.y1;
                        const float* rows[4];
                        for (int t = 0; t < 4; ++t) {
                            const int k = resolveIndex(base + t, gs.height, nearest);
                            rows[t] = (k < 0) ? NULL : (in + (std::size_t)k * gs.width);
                        }
                        for (int x = 0; x < gd.width; ++x) {
                            float v = 0.f;
                            for (int t = 0; t < 4; ++t) {
                                v += kDown[t] * (rows[t] ? rows[t][x] : 0.f);
                            }
                            o[x] = v;
                        }
                    }
                }
            };
        };
    } // downWorker

    /// The rows of the level blur b runs on, copied into the work buffer and filtered.
    std::function<PassRowFn(const PassScratch&)>
    rowsWorker(const PlanPtr& plan,
               const CpuLevelsPtr& levels,
               int input,
               int b)
    {
        return [plan, levels, input, b](const PassScratch& scratch) -> PassRowFn {
            levels->prepare(*plan, scratch);
            std::shared_ptr<BlurKernels::LineScratch> lineScratch = std::make_shared<BlurKernels::LineScratch>();

            return [&scratch, plan, levels, input, b, lineScratch](int line) {
                const Plan::Blur& blur = plan->blurs[b];
                const int lines = scratch.height;
                const int p = line / lines;
                const int r = line % lines;
                const Geometry& g = levels->geometry(blur.level);
                const float* in = levels->level(scratch, input, blur.level, p);
                float* out = levels->work(blur.level, p);
                const int rowEnd = itemsBegin(r + 1, g.height, lines);

                for (int row = itemsBegin(r, g.height, lines); row < rowEnd; ++row) {
                    const std::size_t offset = (std::size_t)row * g.width;
                    std::copy(in + offset, in + offset + g.width, out + offset);
                    if (!blur.filterX->isIdentity()) {
                        blur.filterX->apply(out + offset, g.width, 1, *lineScratch);
                    }
                }
            };
        };
    }

    /// The columns of the work buffer filtered, kColumnBlock adjacent ones at a time.
    std::function<PassRowFn(const PassScratch&)>
    columnsWorker(const PlanPtr& plan,
                  const CpuLevelsPtr& levels,
                  int b)
    {
        return [plan, levels, b](const PassScratch& scratch) -> PassRowFn {
            levels->prepare(*plan, scratch);
            std::shared_ptr<BlurKernels::LineScratch> lineScratch = std::make_shared<BlurKernels::LineScratch>();

            return [&scratch, plan, levels, b, lineScratch](int line) {
                const Plan::Blur& blur = plan->blurs[b];
                const int lines = scratch.height;
                const int p = line / lines;
                const int r = line % lines;
                const Geometry& g = levels->geometry(blur.level);
                const int block = BlurKernels::LineFilter::kColumnBlock;
                const int blocks = (g.width + block - 1) / block;
                float* data = levels->work(blur.level, p);
                const int blockEnd = itemsBegin(r + 1, blocks, lines);

                for (int k = itemsBegin(r, blocks, lines); k < blockEnd; ++k) {
                    const int c0 = k * block;
                    blur.filterY->applyColumns(data + c0, g.height, g.width, std::min(block, g.width - c0), *lineScratch);
                }
            };
        };
    }

    /// The blurred level brought back to full resolution and added into slot acc; the last blur
    /// divides the sum by the count.
    std::function<PassRowFn(const PassScratch&)>
    upAddWorker(const PlanPtr& plan,
                const CpuLevelsPtr& levels,
                int acc,
                int b)
    {
        return [plan, levels, acc, b](const PassScratch& scratch) -> PassRowFn {
            levels->prepare(*plan, scratch);
            const Plan::Blur& blur = plan->blurs[b];
            std::shared_ptr<std::vector<Taps>> columns = std::make_shared<std::vector<Taps>>(scratch.width);
            for (int x = 0; x < scratch.width; ++x) {
                (*columns)[x] = upTaps(blur.factorX, scratch.rect.x1 + x);
            }
            std::shared_ptr<std::vector<float>> blend = std::make_shared<std::vector<float>>(levels->geometry(blur.level).width);

            return [&scratch, plan, levels, acc, b, columns, blend](int line) {
                const Plan::Blur& blur = plan->blurs[b];
                const int p = line / scratch.height;
                const int r = line % scratch.height;
                const RectI& v = levels->rect(blur.level);
                const Geometry& g = levels->geometry(blur.level);
                const float* blurred = levels->work(blur.level, p);
                const bool nearest = plan->nearest;
                const bool first = (b == 0);
                const bool last = (b + 1 == (int)plan->blurs.size());
                const float divisor = (float)plan->count;
                std::vector<float>& tmp = *blend;

                const Taps ty = upTaps(blur.factorY, scratch.rect.y1 + r);
                std::fill(tmp.begin(), tmp.end(), 0.f);
                for (int j = 0; j < ty.n; ++j) {
                    const int k = resolveIndex(ty.i0 + j - v.y1, g.height, nearest);
                    if (k < 0) {
                        continue;
                    }
                    const float* row = blurred + (std::size_t)k * g.width;
                    for (int x = 0; x < g.width; ++x) {
                        tmp[x] += ty.w[j] * row[x];
                    }
                }

                float* out = scratch.plane(acc, p) + (std::size_t)r * scratch.width;
                for (int x = 0; x < scratch.width; ++x) {
                    const Taps& tx = (*columns)[x];
                    float sum = 0.f;
                    for (int i = 0; i < tx.n; ++i) {
                        const int k = resolveIndex(tx.i0 + i - v.x1, g.width, nearest);
                        if (k >= 0) {
                            sum += tx.w[i] * tmp[k];
                        }
                    }
                    float a = first ? 0.f : out[x];
                    a += sum;
                    if (last) {
                        a = a / divisor;
                    }
                    out[x] = a;
                }
            };
        };
    } // upAddWorker
} // anonymous namespace

Settings
normalised(const Settings& settings)
{
    Settings s = settings;

    s.sizeX = std::max(0., s.sizeX);
    s.sizeY = std::max(0., s.sizeY);
    s.count = std::min(std::max(1, s.count), kMaxCount);
    s.ratio = std::min(s.ratio, kMaxRatio);
    if (!(s.ratio > 1.)) {
        s.count = 1;
    }
    if (s.count == 1) {
        s.ratio = 1.;
    }

    return s;
}

bool
isIdentity(const Settings& settings)
{
    const Settings s = normalised(settings);
    const double x = blurSize(s.sizeX, s.ratio, s.count - 1);
    const double y = blurSize(s.sizeY, s.ratio, s.count - 1);

    if (isGaussianFamily(s.filter)) {
        return ((float)(x / 2.4) < 0.1) && ((float)(y / 2.4) < 0.1);
    }
    if (s.filter == BlurKernels::eFilterFIRGaussian) {
        return !BlurKernels::LineFilter::firGaussianRadius(x / 2.4) && !BlurKernels::LineFilter::firGaussianRadius(y / 2.4);
    }

    return (x <= 1) && (y <= 1);
}

PlanPtr
makePlan(const Settings& settings,
         bool nearest)
{
    const Settings s = normalised(settings);
    std::shared_ptr<Plan> plan = std::make_shared<Plan>();

    plan->fir = (s.filter == BlurKernels::eFilterFIRGaussian);
    plan->nearest = nearest;
    plan->count = s.count;
    plan->recursive = isGaussianFamily(s.filter);
    int levelX = 0;
    int levelY = 0;
    for (int i = 0; i < s.count; ++i) {
        const double px = blurSize(s.sizeX, s.ratio, i);
        const double py = blurSize(s.sizeY, s.ratio, i);
        Plan::Blur blur;
        int64_t haloX = 0;
        int64_t haloY = 0;
        if (plan->fir) {
            const double sigmaX = px / 2.4;
            const double sigmaY = py / 2.4;
            // The blurs grow with i, so their levels do too: the pyramid is one staircase of steps.
            const int kx = std::max(levelX, levelFor(sigmaX));
            const int ky = std::max(levelY, levelFor(sigmaY));
            for (; levelX < kx; ++levelX) {
                plan->steps.push_back(0);
            }
            for (; levelY < ky; ++levelY) {
                plan->steps.push_back(1);
            }
            blur.level = (int)plan->steps.size();
            blur.factorX = 1 << kx;
            blur.factorY = 1 << ky;
            const double ex = levelSigma(sigmaX, kx);
            const double ey = levelSigma(sigmaY, ky);
            blur.filterX = std::make_shared<const BlurKernels::LineFilter>(BlurKernels::LineFilter::firGaussian(ex, 0, nearest));
            blur.filterY = std::make_shared<const BlurKernels::LineFilter>(BlurKernels::LineFilter::firGaussian(ey, 0, nearest));
            blur.radiusX = BlurKernels::LineFilter::firGaussianRadius(ex);
            blur.radiusY = BlurKernels::LineFilter::firGaussianRadius(ey);
            // A pixel reads four level samples about its own and their blur, and a level sample
            // spans under three of its spacings of the pixels.
            haloX = (blur.factorX == 1) ? blur.radiusX : (int64_t)blur.factorX * (blur.radiusX + 4);
            haloY = (blur.factorY == 1) ? blur.radiusY : (int64_t)blur.factorY * (blur.radiusY + 4);
            plan->sigma = std::max(plan->sigma, std::max(ex, ey));
#ifdef NATRON_HAVE_GPU
            blur.weightsX = BlurKernels::LineFilter::firGaussianWeights(ex);
            blur.weightsY = BlurKernels::LineFilter::firGaussianWeights(ey);
#endif
        } else if (isGaussianFamily(s.filter)) {
            blur.filterX = std::make_shared<const BlurKernels::LineFilter>(BlurKernels::LineFilter::forFilter(s.filter, (float)(px / 2.4), 0, nearest));
            blur.filterY = std::make_shared<const BlurKernels::LineFilter>(BlurKernels::LineFilter::forFilter(s.filter, (float)(py / 2.4), 0, nearest));
            haloX = blur.filterX->isIdentity() ? 0 : std::max<int64_t>(3, (int64_t)std::ceil(px * 1.5));
            haloY = blur.filterY->isIdentity() ? 0 : std::max<int64_t>(3, (int64_t)std::ceil(py * 1.5));
            plan->sigma = std::max(plan->sigma, std::max(px, py) / 2.4);
        } else {
            const int iter = boxIterations(s.filter);
            blur.filterX = std::make_shared<const BlurKernels::LineFilter>(BlurKernels::LineFilter::forFilter(s.filter, (float)px, 0, nearest));
            blur.filterY = std::make_shared<const BlurKernels::LineFilter>(BlurKernels::LineFilter::forFilter(s.filter, (float)py, 0, nearest));
            haloX = blur.filterX->isIdentity() ? 0 : iter * (int64_t)(std::floor((px - 1) / 2) + 1);
            haloY = blur.filterY->isIdentity() ? 0 : iter * (int64_t)(std::floor((py - 1) / 2) + 1);
            plan->sigma = std::max(plan->sigma, std::max(px, py) / 2.4);
        }
#ifdef NATRON_HAVE_GPU
        if (!plan->fir) {
            blur.lineX = BlurKernels::toGpuLineFilter(*blur.filterX);
            blur.lineY = BlurKernels::toGpuLineFilter(*blur.filterY);
        }
#endif
        plan->haloX = std::max(plan->haloX, (int)std::min(haloX, kMaxHalo));
        plan->haloY = std::max(plan->haloY, (int)std::min(haloY, kMaxHalo));
        plan->blurs.push_back(blur);
    }

    return plan;
} // makePlan

int
haloX(const Plan& plan)
{
    return plan.haloX;
}

int
haloY(const Plan& plan)
{
    return plan.haloY;
}

void
appendPasses(const PlanPtr& plan,
             int input,
             int acc,
             int growX,
             int growY,
             std::vector<Pass>* passes)
{
    // The halo, the spread and the cost hint, for the whole bloom.
    Pass extent;
    extent.haloX = plan->haloX;
    extent.haloY = plan->haloY;
    extent.growX = growX;
    extent.growY = growY;
    extent.sigma = plan->sigma;
    extent.recursive = plan->recursive;
    passes->push_back(extent);

    const CpuLevelsPtr levels = std::make_shared<CpuLevels>();
    for (int m = 1; m <= (int)plan->steps.size(); ++m) {
        passes->push_back(Pass::point(true, downWorker(plan, levels, input, m)));
    }
    for (int b = 0; b < (int)plan->blurs.size(); ++b) {
        passes->push_back(Pass::point(true, rowsWorker(plan, levels, input, b)));
        if (!plan->blurs[b].filterY->isIdentity()) {
            passes->push_back(Pass::point(true, columnsWorker(plan, levels, b)));
        }
        passes->push_back(Pass::point(true, upAddWorker(plan, levels, acc, b)));
    }
}

#ifdef NATRON_HAVE_GPU
namespace {
    // Mirrors BloomParams in Kernels/bloom.slang, which has no padding between members.
    struct BloomGpuParams {
        int32_t inX1;
        int32_t inY1;
        uint32_t inOffset;
        uint32_t inStride;
        int32_t validX1;
        int32_t validY1;
        int32_t validX2;
        int32_t validY2;
        int32_t clampX1;
        int32_t clampY1;
        int32_t clampX2;
        int32_t clampY2;
        int32_t outX1;
        int32_t outY1;
        uint32_t outWidth;
        uint32_t outHeight;
        uint32_t outOffset;
        uint32_t outStride;
        int32_t domX1;
        int32_t domY1;
        int32_t domX2;
        int32_t domY2;
        uint32_t channels;
        uint32_t nearest;
        uint32_t axis;
        uint32_t factorX;
        uint32_t factorY;
        uint32_t first;
        float divisor;
        uint32_t pad0;
    };
    static_assert(sizeof(BloomGpuParams) == 120);
    static_assert(offsetof(BloomGpuParams, clampX1) == 32);
    static_assert(offsetof(BloomGpuParams, domX1) == 72);
    static_assert(offsetof(BloomGpuParams, channels) == 88);
    static_assert(offsetof(BloomGpuParams, divisor) == 112);

    bool
    dispatchBloom(gpu::GpuRecordContext& ctx,
                  gpu::GpuKernelId kernel,
                  const gpu::GpuBuffer* in,
                  const gpu::GpuBuffer* out,
                  const BloomGpuParams& p)
    {
        if (!p.outWidth || !p.outHeight) {
            return true;
        }
        if (!ctx.dispatch(kernel, { in, out }, p, { p.outWidth, p.outHeight, 1 })) {
            return false;
        }
        ctx.computeBarrier();

        return true;
    }

    void
    setRect(const RectI& r,
            int32_t* x1,
            int32_t* y1,
            int32_t* x2,
            int32_t* y2)
    {
        *x1 = r.x1;
        *y1 = r.y1;
        *x2 = r.x2;
        *y2 = r.y2;
    }

    /// The params reading a tightly packed buffer over in and writing one over out.
    BloomGpuParams
    packedParams(const RectI& in,
                 const RectI& out,
                 int nComps)
    {
        BloomGpuParams p {};

        p.channels = (uint32_t)nComps;
        p.inX1 = in.x1;
        p.inY1 = in.y1;
        p.inStride = (uint32_t)(std::max(0, in.width()) * nComps);
        setRect(in, &p.validX1, &p.validY1, &p.validX2, &p.validY2);
        setRect(in, &p.clampX1, &p.clampY1, &p.clampX2, &p.clampY2);
        p.outX1 = out.x1;
        p.outY1 = out.y1;
        p.outWidth = (uint32_t)std::max(0, out.width());
        p.outHeight = (uint32_t)std::max(0, out.height());
        p.outStride = (uint32_t)(std::max(0, out.width()) * nComps);

        return p;
    }

    /// The level samples pixels [a, b) read when upsampling by factor, clamped to the level's [v1, v2):
    /// every sample a read may land on.
    void
    neededRange(int factor,
                int a,
                int b,
                int v1,
                int v2,
                int32_t* x1,
                int32_t* x2)
    {
        const Taps lo = upTaps(factor, a);
        const Taps hi = upTaps(factor, b - 1);

        *x1 = std::min(std::max(lo.i0, v1), v2 - 1);
        *x2 = std::min(std::max(hi.i0 + hi.n - 1, v1), v2 - 1) + 1;
    }
} // anonymous namespace

bool
gpuScratch(gpu::GpuRecordContext& ctx,
           const RectI& rect,
           int nComps,
           gpu::GpuBuffer** buffer)
{
    const uint64_t floats = PassExec::isEmptyRect(rect) ? 0 : (uint64_t)rect.width() * rect.height() * nComps;
    if (floats > UINT32_MAX) {
        return false;
    }
    gpu::GpuBuffer* out = nullptr;
    if (!ctx.scratch((VkDeviceSize)std::max<uint64_t>(floats, 1) * sizeof(float), out) || !out) {
        return false;
    }
    *buffer = out;

    return true;
}

gpu::GpuBlurImage
gpuScratchImage(const gpu::GpuBuffer* buffer,
                const RectI& rect,
                int nComps)
{
    gpu::GpuBlurImage image;

    image.buffer = buffer;
    image.bounds = PassExec::toGpuBlurRect(rect);
    image.strideFloats = (uint32_t)(std::max(0, rect.width()) * nComps);

    return image;
}

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

StatusEnum
recordFill(const PassGpuIO& io,
           bool nearest,
           gpu::GpuBuffer** buffer)
{
    gpu::GpuRecordContext& ctx = io.context;
    const int nComps = io.nComps;

    if (!gpuScratch(ctx, io.domain, nComps, buffer)) {
        return eStatusFailed;
    }
    if (PassExec::isEmptyRect(io.domain)) {
        return eStatusOK;
    }
    BloomGpuParams p = packedParams(RectI(), io.domain, nComps);
    // The source's pixels that exist, the first at inOffset; the kernel reads none when empty.
    p.validX1 = p.validY1 = p.validX2 = p.validY2 = 0;
    setRect(io.rod, &p.clampX1, &p.clampY1, &p.clampX2, &p.clampY2);
    p.nearest = nearest ? 1u : 0u;
    const gpu::GpuBuffer* source = *buffer;
    if (io.source.buffer && !PassExec::isEmptyRect(io.sourceValid)) {
        const gpu::GpuBlurImage image = PassExec::toGpuBlurImage(io.source);
        source = io.source.buffer;
        p.inX1 = io.sourceValid.x1;
        p.inY1 = io.sourceValid.y1;
        p.inStride = image.strideFloats;
        setRect(io.sourceValid, &p.validX1, &p.validY1, &p.validX2, &p.validY2);
        if (!gpuFloatIndex(image, io.sourceValid.x1, io.sourceValid.y1, nComps, &p.inOffset)) {
            return eStatusFailed;
        }
    }

    return dispatchBloom(ctx, gpu::GpuKernelId::bloom_copy, source, *buffer, p) ? eStatusOK : eStatusFailed;
}

StatusEnum
recordGpu(const Plan& plan,
          const PassGpuIO& io,
          const gpu::GpuBuffer* input,
          const gpu::GpuBlurImage& dst)
{
    gpu::GpuRecordContext& ctx = io.context;
    const int nComps = io.nComps;
    const RectI& domain = io.domain;
    const RectI& output = io.output;

    if (PassExec::isEmptyRect(output)) {
        return eStatusOK;
    }
    // The upsampling into dst over the output; the level and its rect are set per blur.
    BloomGpuParams up = packedParams(RectI(), output, nComps);
    up.validX1 = up.validY1 = up.validX2 = up.validY2 = 0;
    up.outStride = dst.strideFloats;
    if (!gpuFloatIndex(dst, output.x1, output.y1, nComps, &up.outOffset)) {
        return eStatusFailed;
    }
    setRect(domain, &up.domX1, &up.domY1, &up.domX2, &up.domY2);
    up.nearest = plan.nearest ? 1u : 0u;
    up.factorX = up.factorY = 1;
    up.first = 1;
    if (PassExec::isEmptyRect(domain)) {
        // Every pixel of the output lies outside the empty domain, so the kernel writes zero.
        return dispatchBloom(ctx, gpu::GpuKernelId::bloom_up_add, input, dst.buffer, up) ? eStatusOK : eStatusFailed;
    }

    const std::vector<RectI> rects = levelRects(plan, domain);
    std::vector<const gpu::GpuBuffer*> levels(rects.size(), input);
    for (std::size_t m = 1; m < rects.size(); ++m) {
        gpu::GpuBuffer* level = nullptr;
        if (!gpuScratch(ctx, rects[m], nComps, &level)) {
            return eStatusFailed;
        }
        BloomGpuParams down = packedParams(rects[m - 1], rects[m], nComps);
        down.nearest = up.nearest;
        down.axis = (uint32_t)plan.steps[m - 1];
        if (!dispatchBloom(ctx, gpu::GpuKernelId::bloom_down, levels[m - 1], level, down)) {
            return eStatusFailed;
        }
        levels[m] = level;
    }

    // Per blur, the level samples the output reads (blurred) and those its blur reads (padded).
    const std::size_t nBlurs = plan.blurs.size();
    std::vector<RectI> blurredRects(nBlurs);
    std::vector<RectI> paddedRects(nBlurs);
    uint64_t blurredFloats = 1;
    uint64_t paddedFloats = 1;
    for (std::size_t b = 0; b < nBlurs; ++b) {
        const Plan::Blur& blur = plan.blurs[b];
        const RectI& v = rects[blur.level];
        RectI r;
        if (plan.fir) {
            neededRange(blur.factorX, output.x1, output.x2, v.x1, v.x2, &r.x1, &r.x2);
            neededRange(blur.factorY, output.y1, output.y2, v.y1, v.y2, &r.y1, &r.y2);
        } else {
            r = domain;
        }
        blurredRects[b] = r;
        paddedRects[b] = RectI(r.x1 - blur.radiusX, r.y1 - blur.radiusY, r.x2 + blur.radiusX, r.y2 + blur.radiusY);
        blurredFloats = std::max(blurredFloats, (uint64_t)r.width() * r.height() * nComps);
        paddedFloats = std::max(paddedFloats, (uint64_t)paddedRects[b].width() * paddedRects[b].height() * nComps);
    }
    if ((blurredFloats > UINT32_MAX) || (paddedFloats > UINT32_MAX)) {
        return eStatusFailed;
    }
    gpu::GpuBuffer* blurred = nullptr;
    gpu::GpuBuffer* padded = nullptr;
    if (!ctx.scratch((VkDeviceSize)blurredFloats * sizeof(float), blurred) || !blurred) {
        return eStatusFailed;
    }
    if (plan.fir && (!ctx.scratch((VkDeviceSize)paddedFloats * sizeof(float), padded) || !padded)) {
        return eStatusFailed;
    }
    gpu::GpuBuffer* window = nullptr;
    if (!plan.fir && !gpu::recordFilterWindow(ctx, PassExec::toGpuBlurRect(output), PassExec::toGpuBlurRect(io.rod), io.haloX, io.haloY, window)) {
        return eStatusFailed;
    }

    for (std::size_t b = 0; b < nBlurs; ++b) {
        const Plan::Blur& blur = plan.blurs[b];
        const RectI& v = rects[blur.level];
        const RectI& r = blurredRects[b];
        const gpu::GpuBuffer* source = blurred;
        if (plan.fir) {
            const RectI& padRect = paddedRects[b];
            BloomGpuParams pad = packedParams(v, padRect, nComps);
            pad.nearest = up.nearest;
            if (!dispatchBloom(ctx, gpu::GpuKernelId::bloom_copy, levels[blur.level], padded, pad)) {
                return eStatusFailed;
            }
            gpu::GpuSeparableBlur sep;
            sep.src = gpuScratchImage(padded, padRect, nComps);
            sep.srcValid = sep.src.bounds;
            sep.dst = gpuScratchImage(blurred, r, nComps);
            sep.output = sep.dst.bounds;
            sep.domain = sep.src.bounds;
            sep.channels = (uint32_t)nComps;
            sep.weightsX = blur.weightsX;
            sep.weightsY = blur.weightsY;
            if (!gpu::recordSeparableBlur(ctx, sep)) {
                return eStatusFailed;
            }
            ctx.computeBarrier();
        } else if (blur.filterX->isIdentity() && blur.filterY->isIdentity()) {
            source = input;
        } else {
            gpu::GpuLineBlur line;
            line.src = gpuScratchImage(input, domain, nComps);
            line.srcValid = line.src.bounds;
            line.dst = gpuScratchImage(blurred, domain, nComps);
            line.output = line.dst.bounds;
            line.domain = line.dst.bounds;
            line.window = window;
            line.channels = (uint32_t)nComps;
            line.filterX = blur.lineX;
            line.filterY = blur.lineY;
            if (!gpu::recordLineBlur(ctx, line)) {
                return eStatusFailed;
            }
            ctx.computeBarrier();
        }

        BloomGpuParams add = up;
        add.inX1 = r.x1;
        add.inY1 = r.y1;
        add.inStride = (uint32_t)(r.width() * nComps);
        setRect(v, &add.validX1, &add.validY1, &add.validX2, &add.validY2);
        add.factorX = (uint32_t)blur.factorX;
        add.factorY = (uint32_t)blur.factorY;
        add.first = (b == 0) ? 1u : 0u;
        add.divisor = (b + 1 == nBlurs) ? (float)plan.count : 0.f;
        if (!dispatchBloom(ctx, gpu::GpuKernelId::bloom_up_add, source, dst.buffer, add)) {
            return eStatusFailed;
        }
    }

    return eStatusOK;
} // recordGpu
#endif // NATRON_HAVE_GPU
} // namespace BloomKernels

NATRON_NAMESPACE_EXIT
