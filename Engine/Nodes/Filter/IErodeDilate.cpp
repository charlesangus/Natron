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

#include "IErodeDilate.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include <QString>

#include "Engine/AppManager.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/KnobReader.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuBlur.h"
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace IErodeDilateKernels {
namespace {
    // As ErodeDilateKernels' reduce(), and in the same argument order everywhere, so that the GPU
    // takes the same value even among NaNs and zeros of either sign.
    inline float
    reduce(float a,
           float b,
           bool takeMax)
    {
        return takeMax ? ((b > a) ? b : a) : ((b < a) ? b : a);
    }

    // a + t (b - a), a itself when the two are equal, so that equal extrema blend to themselves
    // exactly whatever t, infinities included.
    inline float
    blend(float a,
          float b,
          float t)
    {
        if ((a == b) || (t == 0.f)) {
            return a;
        }
        const float d = b - a;
        const float s = t * d;

        return a + s;
    }

    /// The half-widths of the ellipse rows of the last few element sizes a row asked for, by |dy|.
    class HalfWidths {
    public:
        const int* get(int rx,
                       int ry)
        {
            Entry& e = _entries[((unsigned)rx * 31u + (unsigned)ry) % kEntries];
            if (!e.valid || (e.rx != rx) || (e.ry != ry)) {
                e.valid = true;
                e.rx = rx;
                e.ry = ry;
                e.widths.resize((std::size_t)ry + 1);
                for (int dy = 0; dy <= ry; ++dy) {
                    e.widths[dy] = ellipseHalfWidth(rx, ry, dy);
                }
            }

            return e.widths.data();
        }

    private:
        static const unsigned kEntries = 16;

        struct Entry {
            bool valid = false;
            int rx = 0;
            int ry = 0;
            std::vector<int> widths;
        };

        Entry _entries[kEntries];
    };

    float
    elementExtremum(const float* plane,
                    const float* tables,
                    int width,
                    int height,
                    int x,
                    int y,
                    int rx,
                    int ry,
                    bool disc,
                    bool takeMax,
                    HalfWidths& halfWidths)
    {
        const std::size_t levelStride = (std::size_t)width * height;
        const int y1 = std::max(0, y - ry);
        const int y2 = std::min(height - 1, y + ry);
        const int* widths = disc ? halfWidths.get(rx, ry) : nullptr;
        float acc = 0.f;

        for (int yy = y1; yy <= y2; ++yy) {
            const int dy = (yy >= y) ? (yy - y) : (y - yy);
            const int hw = widths ? widths[dy] : rx;
            const int xa = std::max(0, x - hw);
            const int xb = std::min(width - 1, x + hw);
            const std::size_t row = (std::size_t)yy * width;
            const float q = rowQuery(plane + row, tables + row, levelStride, xa, xb, takeMax);
            acc = (yy == y1) ? q : reduce(acc, q, takeMax);
        }

        return acc;
    }
} // anonymous namespace

int
levelCount(int maxHalfWidth)
{
    return (int)std::bit_width((unsigned)(2 * std::max(0, maxHalfWidth) + 1));
}

void
buildRowTable(const float* row,
              int width,
              int levels,
              bool takeMax,
              float* tableRow,
              std::size_t levelStride)
{
    const float* prev = row;

    for (int k = 1; k < levels; ++k) {
        const int half = 1 << (k - 1);
        const int span = 1 << k;
        float* const cur = tableRow + (std::size_t)(k - 1) * levelStride;
        for (int j = 0; j + span <= width; ++j) {
            cur[j] = reduce(prev[j], prev[j + half], takeMax);
        }
        prev = cur;
    }
}

float
rowQuery(const float* row,
         const float* tableRow,
         std::size_t levelStride,
         int x1,
         int x2,
         bool takeMax)
{
    const int k = (int)std::bit_width((unsigned)(x2 - x1 + 1)) - 1;

    if (k == 0) {
        return row[x1];
    }
    const float* const level = tableRow + (std::size_t)(k - 1) * levelStride;

    return reduce(level[x1], level[x2 - (1 << k) + 1], takeMax);
}

int
ellipseHalfWidth(int rx,
                 int ry,
                 int dy)
{
    if (dy < 0) {
        dy = -dy;
    }
    if (dy > ry) {
        return -1;
    }
    if ((ry == 0) || (rx == 0)) {
        return rx;
    }
    // The largest d with d^2 ry^2 <= rx^2 (ry^2 - dy^2), from the float root, corrected exactly.
    const int64_t ry2 = (int64_t)ry * ry;
    const int64_t rhs = (int64_t)rx * rx * (ry2 - (int64_t)dy * dy);
    int d = std::min(rx, (int)(std::sqrt((double)rhs) / ry));
    while ((d < rx) && ((int64_t)(d + 1) * (d + 1) * ry2 <= rhs)) {
        ++d;
    }
    while ((d > 0) && ((int64_t)d * d * ry2 > rhs)) {
        --d;
    }

    return d;
}

void
filterRow(const float* plane,
          const float* tables,
          int width,
          int height,
          int y,
          int x1,
          int x2,
          const float* rx,
          const float* ry,
          bool disc,
          bool takeMax,
          float* out)
{
    HalfWidths halfWidths;

    for (int x = x1; x < x2; ++x) {
        const float rxf = rx[x - x1];
        const float ryf = ry[x - x1];
        const float fx = std::floor(rxf);
        const float fy = std::floor(ryf);
        const float tx = rxf - fx;
        const float ty = ryf - fy;
        const int r0x = (int)fx;
        const int r0y = (int)fy;
        const int r1x = (tx > 0.f) ? r0x + 1 : r0x;
        const int r1y = (ty > 0.f) ? r0y + 1 : r0y;

        const float v00 = elementExtremum(plane, tables, width, height, x, y, r0x, r0y, disc, takeMax, halfWidths);
        const float v10 = (r1x != r0x) ? elementExtremum(plane, tables, width, height, x, y, r1x, r0y, disc, takeMax, halfWidths) : v00;
        const float v01 = (r1y != r0y) ? elementExtremum(plane, tables, width, height, x, y, r0x, r1y, disc, takeMax, halfWidths) : v00;
        float v11;
        if ((r1x != r0x) && (r1y != r0y)) {
            v11 = elementExtremum(plane, tables, width, height, x, y, r1x, r1y, disc, takeMax, halfWidths);
        } else {
            v11 = (r1x != r0x) ? v10 : v01;
        }
        const float a = blend(v00, v10, tx);
        const float b = blend(v01, v11, tx);
        out[x - x1] = blend(a, b, ty);
    }
}

void
filterPlane(const float* src,
            int width,
            int height,
            const float* rx,
            const float* ry,
            bool disc,
            bool takeMax,
            float* dst)
{
    if ((width <= 0) || (height <= 0)) {
        return;
    }
    const std::size_t pixels = (std::size_t)width * height;
    float maxRx = 0.f;
    for (std::size_t i = 0; i < pixels; ++i) {
        maxRx = std::max(maxRx, rx[i]);
    }
    const int levels = levelCount((int)std::ceil(maxRx));
    std::vector<float> tables((std::size_t)std::max(0, levels - 1) * pixels);
    for (int y = 0; y < height; ++y) {
        const std::size_t row = (std::size_t)y * width;
        buildRowTable(src + row, width, levels, takeMax, tables.data() + row, pixels);
    }
    for (int y = 0; y < height; ++y) {
        const std::size_t row = (std::size_t)y * width;
        filterRow(src, tables.data(), width, height, y, 0, width, rx + row, ry + row, disc, takeMax, dst + row);
    }
}

namespace {
    inline bool
    isNaN(float v)
    {
        return (std::bit_cast<uint32_t>(v) & 0x7fffffffu) > 0x7f800000u;
    }

    // reduce() that skips a NaN first operand: over a run of values, from its first, the first of
    // the extrema of its values that are not NaN, or a NaN when all are. As the definition's rows
    // reduced from the top once the top row is not a NaN, and associative, so that a sparse table
    // over overlapping windows gives the same.
    inline float
    reduceSkip(float a,
               float b,
               bool takeMax)
    {
        return isNaN(a) ? b : reduce(a, b, takeMax);
    }

    void
    rowRadii(const TilePlan& plan,
             int r,
             std::vector<float>& work,
             const float** rx,
             const float** ry)
    {
        const std::size_t n = (std::size_t)plan.window.width();
        if (work.size() < 2 * n) {
            work.resize(2 * n);
        }
        plan.radii(r, work.data(), work.data() + n);
        *rx = work.data();
        *ry = work.data() + n;
    }

    int
    slotOf(const TilePlan& plan,
           int c,
           int radius)
    {
        const auto first = plan.slotRadius.begin() + plan.slotStart[c];
        const auto last = plan.slotRadius.begin() + plan.slotStart[c + 1];
        const auto it = std::lower_bound(first, last, radius);

        return (it != last) && (*it == radius) ? (int)(it - plan.slotRadius.begin()) : -1;
    }

    int
    clampInt(int v,
             int lo,
             int hi)
    {
        return std::min(std::max(v, lo), hi);
    }
} // anonymous namespace

PixelElements
pixelElements(float rx,
              float ry)
{
    PixelElements p;
    const float fx = std::floor(rx);
    const float fy = std::floor(ry);

    p.tx = rx - fx;
    p.ty = ry - fy;
    p.x[0] = (int)fx;
    p.y[0] = (int)fy;
    p.x[1] = (p.tx > 0.f) ? p.x[0] + 1 : p.x[0];
    p.y[1] = (p.ty > 0.f) ? p.y[0] + 1 : p.y[0];

    return p;
}

float
blendElements(const PixelElements& p,
              const float v[4])
{
    const float v00 = v[0];
    const float v10 = p.has(1) ? v[1] : v00;
    const float v01 = p.has(2) ? v[2] : v00;
    const float v11 = p.has(3) ? v[3] : ((p.x[1] != p.x[0]) ? v10 : v01);
    const float a = blend(v00, v10, p.tx);
    const float b = blend(v01, v11, p.tx);

    return blend(a, b, p.ty);
}

int
TilePlan::bandCount() const
{
    const int h = window.height();

    return (h <= 0) ? 0 : (h + bandRows - 1) / bandRows;
}

int
TilePlan::batchCount(int band) const
{
    const int ra = band * bandRows;
    const int rb = std::min(window.height(), ra + bandRows);
    const int rows = (rb - ra) + 2 * haloY;

    return (rb <= ra) ? 0 : (rows + batchRows - 1) / batchRows;
}

TilePlan
planTile(const RectI& window,
         const TileRadiiFn& radii,
         bool disc,
         bool takeMax,
         std::size_t budgetFloats)
{
    TilePlan plan;
    plan.window = window;
    plan.disc = disc;
    plan.takeMax = takeMax;
    plan.radii = radii;
    const int ww = std::max(0, window.width());
    const int wh = std::max(0, window.height());
    if (!ww || !wh) {
        plan.bandRows = 1;
        plan.columnBatch.clear();

        return plan;
    }
    const std::size_t budget = std::max(budgetFloats, (std::size_t)2);

    std::vector<float> rx((std::size_t)ww);
    std::vector<float> ry((std::size_t)ww);
    std::vector<std::vector<int>> columnRadii((std::size_t)ww);
    double foldRowsWork = 0.;
    double pixelElementsCount = 0.;
    float maxRx = 0.f;
    float maxRy = 0.f;
    for (int r = 0; r < wh; ++r) {
        radii(r, rx.data(), ry.data());
        for (int c = 0; c < ww; ++c) {
            maxRx = std::max(maxRx, rx[c]);
            maxRy = std::max(maxRy, ry[c]);
            const PixelElements p = pixelElements(rx[c], ry[c]);
            std::vector<int>& cr = columnRadii[c];
            for (int e = 0; e < 4; ++e) {
                if (!p.has(e)) {
                    continue;
                }
                plan.element[e] = true;
                foldRowsWork += 2. * p.radiusY(e) + 1.;
                pixelElementsCount += 1.;
                const int radius = p.radiusX(e);
                if (std::find(cr.begin(), cr.end(), radius) == cr.end()) {
                    cr.push_back(radius);
                }
            }
        }
    }
    plan.haloX = (int)std::ceil(maxRx);
    plan.haloY = (int)std::ceil(maxRy);
    plan.levels = levelCount(plan.haloX);
    plan.columnLevels = levelCount(plan.haloY);
    const int width = ww + 2 * plan.haloX;
    const int height = wh + 2 * plan.haloY;
    const double rowTableWork = (double)plan.levels * width;

    const std::size_t rowFloats = plan.rowTableFloats(width);
    plan.batchRows = rowFloats ? (int)std::min<std::size_t>((budget / 2) / rowFloats, (std::size_t)height) : height;
    plan.batchRows = std::max(1, plan.batchRows);
    const int extra = plan.extraElements();
    plan.bandRows = extra ? (int)std::min<std::size_t>((budget / 2) / ((std::size_t)extra * ww), (std::size_t)wh) : wh;
    plan.bandRows = std::max(1, plan.bandRows);
    double rowsCost = foldRowsWork;
    for (int b = 0; b < plan.bandCount(); ++b) {
        const int rows = std::min(wh - b * plan.bandRows, plan.bandRows) + 2 * plan.haloY;
        rowsCost += (double)rows * rowTableWork;
    }

    plan.slotStart.assign((std::size_t)ww + 1, 0);
    for (int c = 0; c < ww; ++c) {
        std::vector<int>& cr = columnRadii[c];
        std::sort(cr.begin(), cr.end());
        plan.slotStart[c + 1] = plan.slotStart[c] + (int)cr.size();
    }
    plan.slotRadius.reserve((std::size_t)plan.slotStart[ww]);
    for (int c = 0; c < ww; ++c) {
        plan.slotRadius.insert(plan.slotRadius.end(), columnRadii[c].begin(), columnRadii[c].end());
        std::vector<int>().swap(columnRadii[c]);
    }
    const std::size_t slotFloats = plan.columnFloats(height);
    const std::size_t maxSlots = budget / slotFloats;
    plan.columnsFit = !disc && (maxSlots > 0);
    double columnsCost = 0.;
    if (plan.columnsFit) {
        plan.columnBatch.push_back(0);
        for (int c = 0; c < ww && plan.columnsFit; ++c) {
            const int slots = plan.slotStart[c + 1] - plan.slotStart[c];
            if ((std::size_t)slots > maxSlots) {
                plan.columnsFit = false;
            } else if ((std::size_t)(plan.slotStart[c + 1] - plan.slotStart[plan.columnBatch.back()]) > maxSlots) {
                plan.columnBatch.push_back(c);
            }
        }
        plan.columnBatch.push_back(ww);
    }
    if (plan.columnsFit) {
        for (int b = 0; b < plan.columnBatchCount(); ++b) {
            const int ca = plan.columnBatch[b];
            const int cb = plan.columnBatch[b + 1];
            const int slots = plan.slotStart[cb] - plan.slotStart[ca];
            plan.batchSlots = std::max(plan.batchSlots, slots);
            columnsCost += (double)height * plan.levels * ((cb - ca) + 2. * plan.haloX);
            columnsCost += (double)slots * height * (plan.columnLevels + 1.);
        }
        columnsCost += 4. * pixelElementsCount;
    } else {
        plan.columnBatch.clear();
        plan.batchSlots = 0;
    }
    plan.strategy = (plan.columnsFit && (columnsCost < rowsCost)) ? TileStrategy::Columns : TileStrategy::Rows;

    return plan;
} // planTile

TilePlane
tilePlane(const TilePlan& plan,
          const float* src,
          int width,
          int height,
          int x0,
          int y0)
{
    TilePlane t;
    const int ww = std::max(0, plan.window.width());
    const int wh = std::max(0, plan.window.height());

    t.src = src;
    t.width = width;
    t.height = height;
    t.x0 = x0;
    t.y0 = y0;
    t.c0 = clampInt(x0 - plan.window.x1, 0, ww);
    t.c1 = clampInt(x0 + width - plan.window.x1, t.c0, ww);
    t.r0 = clampInt(y0 - plan.window.y1, 0, wh);
    t.r1 = clampInt(y0 + height - plan.window.y1, t.r0, wh);

    return t;
}

void
bandRange(const TilePlan& plan,
          const TilePlane& t,
          int band,
          int* ra,
          int* rb)
{
    *ra = clampInt(band * plan.bandRows, t.r0, t.r1);
    *rb = clampInt((band + 1) * plan.bandRows, *ra, t.r1);
}

void
batchRange(const TilePlan& plan,
           const TilePlane& t,
           int band,
           int batch,
           int* first,
           int* count)
{
    // The band's rows, unclipped, reach haloY beyond it; the batch's are the plane's among them.
    const int ra = band * plan.bandRows;
    const int a = t.y(plan, ra) - plan.haloY + batch * plan.batchRows;
    const int bandEnd = t.y(plan, std::min(plan.window.height(), ra + plan.bandRows)) + plan.haloY;
    const int b = std::min(a + plan.batchRows, bandEnd);

    *first = clampInt(a, 0, t.height);
    *count = clampInt(b, *first, t.height) - *first;
}

void
foldRows(const TilePlan& plan,
         const TilePlane& t,
         const float* tables,
         int first,
         int count,
         int r,
         const TileElements& out,
         std::vector<float>& work)
{
    if ((count <= 0) || (r < t.r0) || (r >= t.r1)) {
        return;
    }
    static thread_local HalfWidths halfWidths;
    const float* rx;
    const float* ry;
    rowRadii(plan, r, work, &rx, &ry);
    const int y = t.y(plan, r);
    const int last = first + count - 1;
    const std::size_t rowFloats = plan.rowTableFloats(t.width);
    const std::size_t width = (std::size_t)t.width;

    for (int c = t.c0; c < t.c1; ++c) {
        const PixelElements p = pixelElements(rx[c], ry[c]);
        const int x = t.x(plan, c);
        for (int e = 0; e < 4; ++e) {
            if (!p.has(e)) {
                continue;
            }
            const int erx = p.radiusX(e);
            const int ery = p.radiusY(e);
            const int y1 = std::max(0, y - ery);
            const int y2 = std::min(t.height - 1, y + ery);
            const int lo = std::max(y1, first);
            const int hi = std::min(y2, last);
            if (lo > hi) {
                continue;
            }
            const int* widths = plan.disc ? halfWidths.get(erx, ery) : nullptr;
            float* const acc = out.at(e, c, r);
            float a = (lo == y1) ? 0.f : *acc;
            for (int yy = lo; yy <= hi; ++yy) {
                const int dy = (yy >= y) ? (yy - y) : (y - yy);
                const int hw = widths ? widths[dy] : erx;
                const int xa = std::max(0, x - hw);
                const int xb = std::min(t.width - 1, x + hw);
                const float q = rowQuery(t.src + (std::size_t)yy * width, tables + (std::size_t)(yy - first) * rowFloats, width, xa, xb, plan.takeMax);
                a = (yy == y1) ? q : reduce(a, q, plan.takeMax);
            }
            *acc = a;
        }
    }
} // foldRows

void
finishRow(const TilePlan& plan,
          const TilePlane& t,
          int r,
          const TileElements& out,
          std::vector<float>& work)
{
    if ((r < t.r0) || (r >= t.r1)) {
        return;
    }
    const float* rx;
    const float* ry;
    rowRadii(plan, r, work, &rx, &ry);
    for (int c = t.c0; c < t.c1; ++c) {
        const PixelElements p = pixelElements(rx[c], ry[c]);
        float v[4] = { 0.f, 0.f, 0.f, 0.f };
        for (int e = 0; e < 4; ++e) {
            if (p.has(e)) {
                v[e] = *out.at(e, c, r);
            }
        }
        *out.at(0, c, r) = blendElements(p, v);
    }
}

void
columnBatchRange(const TilePlan& plan,
                 const TilePlane& t,
                 int b,
                 int* ca,
                 int* cb)
{
    *ca = clampInt(plan.columnBatch[b], t.c0, t.c1);
    *cb = clampInt(plan.columnBatch[b + 1], *ca, t.c1);
}

void
fillSpans(const TilePlan& plan,
          const TilePlane& t,
          int ca,
          int cb,
          int y,
          float* columns,
          std::vector<float>& work)
{
    if ((ca >= cb) || (y < 0) || (y >= t.height)) {
        return;
    }
    // The row's table over the batch's columns and their halo only: an entry depends on the
    // values it covers alone, so the spans are those of the whole row's table.
    const int xa = std::max(0, t.x(plan, ca) - plan.haloX);
    const int xb = std::min(t.width, t.x(plan, cb - 1) + plan.haloX + 1);
    const int width = xb - xa;
    const std::size_t floats = plan.rowTableFloats(width);
    if (work.size() < floats) {
        work.resize(floats);
    }
    const float* const row = t.src + (std::size_t)y * t.width + xa;
    buildRowTable(row, width, plan.levels, plan.takeMax, work.data(), (std::size_t)width);
    const std::size_t columnFloats = plan.columnFloats(t.height);
    const int s0 = plan.slotStart[ca];
    for (int c = ca; c < cb; ++c) {
        const int x = t.x(plan, c);
        for (int s = plan.slotStart[c]; s < plan.slotStart[c + 1]; ++s) {
            const int radius = plan.slotRadius[s];
            const int x1 = std::max(0, x - radius) - xa;
            const int x2 = std::min(t.width - 1, x + radius) - xa;
            columns[(std::size_t)(s - s0) * columnFloats + y] = rowQuery(row, work.data(), (std::size_t)width, x1, x2, plan.takeMax);
        }
    }
}

void
buildColumn(const TilePlan& plan,
            const TilePlane& t,
            float* column)
{
    const int height = t.height;
    const float* prev = column;

    for (int k = 1; k < plan.columnLevels; ++k) {
        const int half = 1 << (k - 1);
        const int span = 1 << k;
        float* const cur = column + (std::size_t)k * height;
        for (int j = 0; j + span <= height; ++j) {
            cur[j] = reduceSkip(prev[j], prev[j + half], plan.takeMax);
        }
        prev = cur;
    }
}

void
answerRow(const TilePlan& plan,
          const TilePlane& t,
          int ca,
          int cb,
          int r,
          const float* columns,
          const TileElements& out,
          std::vector<float>& work)
{
    if ((ca >= cb) || (r < t.r0) || (r >= t.r1)) {
        return;
    }
    const float* rx;
    const float* ry;
    rowRadii(plan, r, work, &rx, &ry);
    const int y = t.y(plan, r);
    const int height = t.height;
    const std::size_t columnFloats = plan.columnFloats(height);
    const int s0 = plan.slotStart[ca];

    for (int c = ca; c < cb; ++c) {
        const PixelElements p = pixelElements(rx[c], ry[c]);
        float v[4] = { 0.f, 0.f, 0.f, 0.f };
        for (int e = 0; e < 4; ++e) {
            if (!p.has(e)) {
                continue;
            }
            const float* const column = columns + (std::size_t)(slotOf(plan, c, p.radiusX(e)) - s0) * columnFloats;
            const int ery = p.radiusY(e);
            const int a = std::max(0, y - ery);
            const int b = std::min(height - 1, y + ery);
            // The definition keeps its top row when that is a NaN, and otherwise skips NaNs.
            if (isNaN(column[a])) {
                v[e] = column[a];
                continue;
            }
            const int k = (int)std::bit_width((unsigned)(b - a + 1)) - 1;
            const float* const level = column + (std::size_t)k * height;
            v[e] = (k == 0) ? column[a] : reduceSkip(level[a], level[b - (1 << k) + 1], plan.takeMax);
        }
        *out.at(0, c, r) = blendElements(p, v);
    }
}

void
filterTile(const TilePlan& plan,
           const float* src,
           int width,
           int height,
           float* dst)
{
    const TilePlane t = tilePlane(plan, src, width, height, 0, 0);
    std::vector<float> work;
    TileElements out;
    out.e[0] = dst;
    out.stride[0] = (std::size_t)width;
    out.c0[0] = t.x0 - plan.window.x1;
    out.r0[0] = t.y0 - plan.window.y1;

    if (plan.strategy == TileStrategy::Columns) {
        std::vector<float> columns;
        for (int b = 0; b < plan.columnBatchCount(); ++b) {
            int ca;
            int cb;
            columnBatchRange(plan, t, b, &ca, &cb);
            if (ca >= cb) {
                continue;
            }
            const int slots = plan.slotStart[cb] - plan.slotStart[ca];
            columns.resize((std::size_t)slots * plan.columnFloats(height));
            for (int y = 0; y < height; ++y) {
                fillSpans(plan, t, ca, cb, y, columns.data(), work);
            }
            for (int s = 0; s < slots; ++s) {
                buildColumn(plan, t, columns.data() + (std::size_t)s * plan.columnFloats(height));
            }
            for (int r = t.r0; r < t.r1; ++r) {
                answerRow(plan, t, ca, cb, r, columns.data(), out, work);
            }
        }

        return;
    }
    std::vector<float> tables;
    std::vector<float> extra;
    for (int band = 0; band < plan.bandCount(); ++band) {
        int ra;
        int rb;
        bandRange(plan, t, band, &ra, &rb);
        if (ra >= rb) {
            continue;
        }
        const std::size_t area = (std::size_t)(rb - ra) * (std::size_t)(t.c1 - t.c0);
        extra.resize(area * 3);
        for (int e = 1; e < 4; ++e) {
            out.e[e] = extra.data() + (std::size_t)(e - 1) * area;
            out.stride[e] = (std::size_t)(t.c1 - t.c0);
            out.c0[e] = t.c0;
            out.r0[e] = ra;
        }
        for (int batch = 0; batch < plan.batchCount(band); ++batch) {
            int first;
            int count;
            batchRange(plan, t, band, batch, &first, &count);
            const std::size_t rowFloats = plan.rowTableFloats(width);
            tables.resize(std::max<std::size_t>(1, (std::size_t)count * rowFloats));
            for (int y = first; y < first + count; ++y) {
                buildRowTable(src + (std::size_t)y * width, width, plan.levels, plan.takeMax, tables.data() + (std::size_t)(y - first) * rowFloats, (std::size_t)width);
            }
            for (int r = ra; r < rb; ++r) {
                foldRows(plan, t, tables.data(), first, count, r, out, work);
            }
        }
        for (int r = ra; r < rb; ++r) {
            finishRow(plan, t, r, out, work);
        }
    }
} // filterTile
} // namespace IErodeDilateKernels

float
IErodeDilateAxis::at(float map) const
{
    if (map >= 1.f) {
        return white;
    }
    if (!(map > 0.f)) {
        return black;
    }
    // Separate statements, as the GPU's precise ones, so that neither contracts them.
    const float d = white - black;
    const float s = map * d;
    const float r = black + s;

    return std::min(std::max(r, std::min(black, white)), std::max(black, white));
}

float
IErodeDilateAxis::largest(const MapRange& range) const
{
    // at() is monotonic, so its largest value over the range is at one end.
    return std::max(at(range.min), at(range.max));
}

void
IErodeDilateParams::axes(const RenderScale& scale,
                         IErodeDilateAxis* x,
                         IErodeDilateAxis* y) const
{
    const OfxPointD rs = scale.toOfxPointD();

    // Like Erode's, the radii are not divided by the pixel aspect ratio.
    x->black = (float)std::max(0., map.sizeAtBlack[0] * rs.x);
    x->white = (float)std::max(0., sizeX * rs.x);
    y->black = (float)std::max(0., map.sizeAtBlack[1] * rs.y);
    y->white = (float)std::max(0., sizeY * rs.y);
}

namespace {
constexpr InputSpec kIErodeDilateInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
    kMapInputSpec,
};

template <ErodeDilateMode MODE>
struct IErodeDilateSpec {
    static constexpr bool kDilate = (MODE == ErodeDilateMode::Dilate);

    static constexpr NodeSpec kNodeSpec = {
        .id = kDilate ? PLUGINID_NATRON_IDILATE : PLUGINID_NATRON_IERODE,
        .label = kDilate ? "IDilate" : "IErode",
        .description = kDilate ? "Dilate the input stream by a structuring element whose size varies per pixel with the Map input: each pixel takes the maximum of the pixels around it.\n"
                                 "\n"
                                 "Where the Map is white the element has the Size, where it is black the Size At Black (0 by default, which leaves those pixels exactly unchanged), "
                                 "and in between a size in between; a fractional size blends the results of the sizes either side of it, so the result changes smoothly with the Map. "
                                 "Map Channel picks the Map's channel in any of its layers, Invert Map inverts it, Map Gamma raises it to 1/gamma and Map Response picks a Linear or Smooth (smoothstep) curve from Size At Black to Size. "
                                 "Outside the Map image, the Map reads black, before Invert Map and the remap. With no Map connected, the element has the Size everywhere.\n"
                                 "\n"
                                 "The element is a rectangle of 2*size+1 pixels, as Dilate's, or a disc (an ellipse when the two sizes differ). At a whole Size where the Map is white, "
                                 "the rectangle gives Dilate's result. The Mask and Mix limit the result as usual."
                               : "Erode the input stream by a structuring element whose size varies per pixel with the Map input: each pixel takes the minimum of the pixels around it.\n"
                                 "\n"
                                 "Where the Map is white the element has the Size, where it is black the Size At Black (0 by default, which leaves those pixels exactly unchanged), "
                                 "and in between a size in between; a fractional size blends the results of the sizes either side of it, so the result changes smoothly with the Map. "
                                 "Map Channel picks the Map's channel in any of its layers, Invert Map inverts it, Map Gamma raises it to 1/gamma and Map Response picks a Linear or Smooth (smoothstep) curve from Size At Black to Size. "
                                 "Outside the Map image, the Map reads black, before Invert Map and the remap. With no Map connected, the element has the Size everywhere.\n"
                                 "\n"
                                 "The element is a rectangle of 2*size+1 pixels, as Erode's, or a disc (an ellipse when the two sizes differ). At a whole Size where the Map is white, "
                                 "the rectangle gives Erode's result. The Mask and Mix limit the result as usual.",
        .grouping = PLUGIN_GROUP_FILTER,
        .majorVersion = kDilate ? PLUGIN_MAJOR_NATRON_IDILATE : PLUGIN_MAJOR_NATRON_IERODE,
        .icon = kDilate ? NATRON_IMAGES_PATH "NativeNodes/IDilate.png" : NATRON_IMAGES_PATH "NativeNodes/IErode.png",
        .inputs = kIErodeDilateInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::all(),
        .op = OpKind::Passes,
    };
};

/**
 * @brief The floats, per plane, a render's tile buffers may take beside its scratch planes: the
 * row tables of a batch and the fractional elements of a band (Rows), or a batch's slot columns
 * (Columns). See IErodeDilateKernels::TileStrategy.
 **/
const std::size_t kTileBudgetFloats = (std::size_t)1 << 22;

/**
 * @brief What the passes of one render share: the tile's plan, and its buffers for every plane.
 * The first pass sizes the buffers for the plane job it runs in, as IBlur's state does; every
 * buffer is written before it is read, so they start uninitialised.
 **/
struct MorphologyState {
    std::shared_ptr<const MapPlane> map;
    IErodeDilateKernels::TilePlan plan;

    std::mutex mutex;
    std::unique_ptr<float[]> tables;
    std::size_t tablesCapacity = 0;
    std::unique_ptr<float[]> extra;
    std::size_t extraCapacity = 0;

    std::size_t planeTables(const PassScratch& scratch) const
    {
        if (plan.strategy == IErodeDilateKernels::TileStrategy::Columns) {
            return (std::size_t)plan.batchSlots * plan.columnFloats(scratch.height);
        }

        return (std::size_t)plan.batchRows * plan.rowTableFloats(scratch.width);
    }

    std::size_t bandArea(const IErodeDilateKernels::TilePlane& t) const
    {
        return (std::size_t)plan.bandRows * (std::size_t)(t.c1 - t.c0);
    }

    IErodeDilateKernels::TilePlane plane(const PassScratch& scratch,
                                         int p) const
    {
        return IErodeDilateKernels::tilePlane(plan, scratch.plane(0, p), scratch.width, scratch.height, scratch.rect.x1, scratch.rect.y1);
    }

    void reserve(const PassScratch& scratch)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        const std::size_t nPlanes = (std::size_t)scratch.nPlanes;
        const std::size_t size = std::max<std::size_t>(1, planeTables(scratch) * nPlanes);
        if (tablesCapacity < size) {
            tables.reset(new float[size]);
            tablesCapacity = size;
        }
        if (plan.strategy == IErodeDilateKernels::TileStrategy::Rows) {
            const std::size_t extraSize = std::max<std::size_t>(1, (std::size_t)plan.extraElements() * bandArea(plane(scratch, 0)) * nPlanes);
            if (extraCapacity < extraSize) {
                extra.reset(new float[extraSize]);
                extraCapacity = extraSize;
            }
        }
    }

    IErodeDilateKernels::TileElements elements(const PassScratch& scratch,
                                               const IErodeDilateKernels::TilePlane& t,
                                               int p,
                                               int ra) const
    {
        IErodeDilateKernels::TileElements out;
        out.e[0] = scratch.plane(1, p);
        out.stride[0] = (std::size_t)scratch.width;
        out.c0[0] = t.x0 - plan.window.x1;
        out.r0[0] = t.y0 - plan.window.y1;
        const std::size_t area = bandArea(t);
        const std::size_t count = (std::size_t)plan.extraElements();
        std::size_t index = 0;
        for (int e = 1; e < 4; ++e) {
            if (extra && plan.element[e]) {
                out.e[e] = extra.get() + ((std::size_t)p * count + index++) * area;
            }
            out.stride[e] = (std::size_t)(t.c1 - t.c0);
            out.c0[e] = t.c0;
            out.r0[e] = ra;
        }

        return out;
    }
};

using MorphologyStatePtr = std::shared_ptr<MorphologyState>;

/// The items of line y of lines that its share of count items covers, *first to *end - 1: the
/// items spread evenly over the lines, so that the threads, which take the lines in chunks, share
/// them whatever part of the plane the items lie in.
void
lineItems(int y,
          int lines,
          int count,
          int* first,
          int* end)
{
    const int64_t n = std::max(0, count);
    *first = (int)(((int64_t)y * n + lines - 1) / lines);
    *end = (int)(((int64_t)(y + 1) * n + lines - 1) / lines);
}

using TileItemFn = std::function<void(const MorphologyState& s, const PassScratch& scratch, int p, const IErodeDilateKernels::TilePlane& t, int item, std::vector<float>& work)>;
using TileCountFn = std::function<int(const MorphologyState& s, const IErodeDilateKernels::TilePlane& t)>;

std::function<PassRowFn(const PassScratch&)>
tileWorker(const MorphologyStatePtr& state,
           TileCountFn count,
           TileItemFn fn)
{
    return [state, count, fn](const PassScratch& scratch) -> PassRowFn {
        state->reserve(scratch);
        const MorphologyState* s = state.get();
        const PassScratch* buffers = &scratch;
        std::shared_ptr<std::vector<float>> work = std::make_shared<std::vector<float>>();

        return [s, buffers, count, fn, work](int line) {
            const int height = buffers->height;
            const int p = line / height;
            const IErodeDilateKernels::TilePlane t = s->plane(*buffers, p);
            int first;
            int end;
            lineItems(line % height, height, count(*s, t), &first, &end);
            for (int item = first; item < end; ++item) {
                fn(*s, *buffers, p, t, item, *work);
            }
        };
    };
}

/// Row y of plane p of slot 1, outside the window: slot 0's pixels, which only the halo holds and
/// nothing reads.
std::function<PassRowFn(const PassScratch&)>
copyWorker(const MorphologyStatePtr& state)
{
    return [state](const PassScratch& scratch) -> PassRowFn {
        const MorphologyState* s = state.get();
        const PassScratch* buffers = &scratch;

        return [s, buffers](int line) {
            const int height = buffers->height;
            const int width = buffers->width;
            const int p = line / height;
            const int y = line % height;
            const IErodeDilateKernels::TilePlane t = s->plane(*buffers, p);
            const std::size_t row = (std::size_t)y * width;
            const float* const src = buffers->plane(0, p) + row;
            float* const dst = buffers->plane(1, p) + row;
            const int r = y - t.y(s->plan, 0);
            if ((r < t.r0) || (r >= t.r1) || (t.c0 == t.c1)) {
                std::copy(src, src + width, dst);

                return;
            }
            const int x1 = t.x(s->plan, t.c0);
            const int x2 = t.x(s->plan, t.c1);
            std::copy(src, src + x1, dst);
            std::copy(src + x2, src + width, dst + x2);
        };
    };
}

void
tilePasses(const MorphologyStatePtr& state,
           int haloX,
           int haloY,
           bool grow,
           PassList* list)
{
    namespace K = IErodeDilateKernels;
    const K::TilePlan& plan = state->plan;
    std::vector<Pass> passes;
    const auto add = [&passes, &state](const TileCountFn& count,
                                       const TileItemFn& fn,
                                       std::size_t taps) {
        Pass pass = Pass::point(true, tileWorker(state, count, fn));
        pass.tapsPerSample = std::max<std::size_t>(1, taps);
        passes.push_back(pass);
    };
    const std::size_t levels = (std::size_t)plan.levels;
    const std::size_t foldTaps = 4 * (2 * (std::size_t)plan.haloY + 1);

    if (plan.strategy == K::TileStrategy::Columns) {
        for (int b = 0; b < plan.columnBatchCount(); ++b) {
            add([](const MorphologyState&, const K::TilePlane& t) { return t.height; },
                [b](const MorphologyState& s, const PassScratch& scratch, int p, const K::TilePlane& t, int y, std::vector<float>& work) {
                    int ca;
                    int cb;
                    K::columnBatchRange(s.plan, t, b, &ca, &cb);
                    K::fillSpans(s.plan, t, ca, cb, y, s.tables.get() + (std::size_t)p * s.planeTables(scratch), work);
                },
                levels);
            add([b](const MorphologyState& s, const K::TilePlane& t) {
                int ca;
                int cb;
                K::columnBatchRange(s.plan, t, b, &ca, &cb);

                return s.plan.slotStart[cb] - s.plan.slotStart[ca]; },
                [](const MorphologyState& s, const PassScratch& scratch, int p, const K::TilePlane& t, int slot, std::vector<float>&) {
                    K::buildColumn(s.plan, t, s.tables.get() + (std::size_t)p * s.planeTables(scratch) + (std::size_t)slot * s.plan.columnFloats(t.height));
                },
                (std::size_t)plan.columnLevels);
            add([](const MorphologyState&, const K::TilePlane& t) { return t.r1 - t.r0; },
                [b](const MorphologyState& s, const PassScratch& scratch, int p, const K::TilePlane& t, int i, std::vector<float>& work) {
                    int ca;
                    int cb;
                    K::columnBatchRange(s.plan, t, b, &ca, &cb);
                    K::answerRow(s.plan, t, ca, cb, t.r0 + i, s.tables.get() + (std::size_t)p * s.planeTables(scratch), s.elements(scratch, t, p, 0), work);
                },
                8);
        }
    } else {
        for (int band = 0; band < plan.bandCount(); ++band) {
            const auto bandRows = [band](const MorphologyState& s, const K::TilePlane& t) {
                int ra;
                int rb;
                K::bandRange(s.plan, t, band, &ra, &rb);

                return rb - ra;
            };
            for (int batch = 0; batch < plan.batchCount(band); ++batch) {
                add([band, batch](const MorphologyState& s, const K::TilePlane& t) {
                    int first;
                    int count;
                    K::batchRange(s.plan, t, band, batch, &first, &count);

                    return count; },
                    [band, batch](const MorphologyState& s, const PassScratch& scratch, int p, const K::TilePlane& t, int i, std::vector<float>&) {
                        int first;
                        int count;
                        K::batchRange(s.plan, t, band, batch, &first, &count);
                        const int y = first + i;
                        float* const tableRow = s.tables.get() + (std::size_t)p * s.planeTables(scratch) + (std::size_t)i * s.plan.rowTableFloats(t.width);
                        K::buildRowTable(t.src + (std::size_t)y * t.width, t.width, s.plan.levels, s.plan.takeMax, tableRow, (std::size_t)t.width);
                    },
                    levels);
                add(bandRows, [band, batch](const MorphologyState& s, const PassScratch& scratch, int p, const K::TilePlane& t, int i, std::vector<float>& work) {
                    int ra;
                    int rb;
                    K::bandRange(s.plan, t, band, &ra, &rb);
                    int first;
                    int count;
                    K::batchRange(s.plan, t, band, batch, &first, &count);
                    K::foldRows(s.plan, t, s.tables.get() + (std::size_t)p * s.planeTables(scratch), first, count, ra + i, s.elements(scratch, t, p, ra), work); }, foldTaps);
            }
            add(bandRows, [band](const MorphologyState& s, const PassScratch& scratch, int p, const K::TilePlane& t, int i, std::vector<float>& work) {
                int ra;
                int rb;
                K::bandRange(s.plan, t, band, &ra, &rb);
                K::finishRow(s.plan, t, ra + i, s.elements(scratch, t, p, ra), work); }, 4);
        }
    }
    Pass copy = Pass::point(true, copyWorker(state));
    passes.push_back(copy);

    Pass& first = passes.front();
    first.kind = PassKind::Gather;
    first.haloX = haloX;
    first.haloY = haloY;
    first.growX = grow ? haloX : 0;
    first.growY = grow ? haloY : 0;
    for (Pass& pass : passes) {
        pass.slot = 1;
        list->passes.push_back(std::move(pass));
    }
} // tilePasses
#ifdef NATRON_HAVE_GPU
// The largest radius, in pixels, the GPU records: its ellipse arithmetic is in 32-bit integers.
// A larger element stays on the CPU.
const int kGpuMaxRadius = 255;

// Mirror kDisc, kTakeMax and kMap* in Kernels/ierodedilate.slang.
const uint32_t kGpuDisc = 1;
const uint32_t kGpuTakeMax = 2;
const uint32_t kGpuMapConnected = 1;
const uint32_t kGpuMapRead = 2;
const uint32_t kGpuMapInvert = 4;
const uint32_t kGpuMapSmooth = 8;

// Mirrors MorphParams in Kernels/ierodedilate.slang, which has no padding between members.
struct IErodeDilateGpuParams {
    int32_t domX1;
    int32_t domY1;
    uint32_t domWidth;
    uint32_t domHeight;
    uint32_t channels;
    uint32_t comp;
    uint32_t level;
    uint32_t flags;
    int32_t srcX1;
    int32_t srcY1;
    int32_t srcX2;
    int32_t srcY2;
    uint32_t srcOffset;
    uint32_t srcStride;
    int32_t outX1;
    int32_t outY1;
    uint32_t outWidth;
    uint32_t outHeight;
    uint32_t dstOffset;
    uint32_t dstStride;
};
static_assert(sizeof(IErodeDilateGpuParams) == 80);
static_assert(offsetof(IErodeDilateGpuParams, srcX1) == 32);
static_assert(offsetof(IErodeDilateGpuParams, outX1) == 56);

// Mirrors MorphConstants in Kernels/ierodedilate.slang, which has no padding between members.
struct IErodeDilateGpuConstants {
    uint32_t mapFlags;
    uint32_t mapChannel;
    uint32_t mapChannels;
    uint32_t mapOffset;
    uint32_t mapStride;
    int32_t mapX1;
    int32_t mapY1;
    int32_t mapX2;
    int32_t mapY2;
    float exponent;
    uint32_t remapPad;
    float outside;
    float blackRx;
    float whiteRx;
    float blackRy;
    float whiteRy;
};
static_assert(sizeof(IErodeDilateGpuConstants) == 64);
static_assert(offsetof(IErodeDilateGpuConstants, exponent) == 36);
static_assert(offsetof(IErodeDilateGpuConstants, blackRx) == 48);

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

// Every kernel of Kernels/ierodedilate.slang binds these, in this order.
struct IErodeDilateGpuBuffers {
    const gpu::GpuBuffer* source = nullptr;
    const gpu::GpuBuffer* map = nullptr;
    const gpu::GpuBuffer* constants = nullptr;
    const gpu::GpuBuffer* work = nullptr;
    const gpu::GpuBuffer* out = nullptr;
};

bool
dispatchIErodeDilate(gpu::GpuRecordContext& ctx,
                     gpu::GpuKernelId kernel,
                     const IErodeDilateGpuBuffers& b,
                     const IErodeDilateGpuParams& p,
                     uint32_t width,
                     uint32_t height)
{
    if (!width || !height) {
        return true;
    }

    return (bool)ctx.dispatch(kernel, { b.source, b.map, b.constants, b.work, b.out }, p, { width, height, 1 });
}

/**
 * @brief The passes of IErodeDilateNode::passes() as the GPU records them over io.domain, the
 * CPU's scratch rect, one component at a time: the component into level 0 of the work buffer,
 * the table's levels one dispatch each, and the filter of every output pixel into io.filtered.
 * The Map's channel is mapChannel of io.inputs[Map], or none when that is negative. Fails, so that
 * the CPU renders, when a buffer exceeds the kernels' 32-bit indices.
 **/
StatusEnum
recordIErodeDilateGpu(const PassGpuIO& io,
                      const IErodeDilateParams& params,
                      const RenderScale& scale,
                      bool takeMax,
                      bool mapConnected,
                      int mapChannel)
{
    gpu::GpuRecordContext& ctx = io.context;
    const RectI& domain = io.domain;
    const int nComps = io.nComps;
    if (PassExec::isEmptyRect(domain) || (nComps < 1) || (nComps > 4) || !io.filtered.buffer) {
        return eStatusFailed;
    }
    IErodeDilateAxis axisX;
    IErodeDilateAxis axisY;
    params.axes(scale, &axisX, &axisY);
    MapRange full;
    full.min = 0.f;
    full.max = 1.f;
    const int levels = IErodeDilateKernels::levelCount((int)std::ceil(axisX.largest(full)));
    const uint64_t width = (uint64_t)domain.width();
    const uint64_t height = (uint64_t)domain.height();
    const uint64_t workFloats = (uint64_t)levels * width * height;
    if (workFloats > UINT32_MAX) {
        return eStatusFailed;
    }

    IErodeDilateGpuConstants k {};
    k.mapFlags = (mapConnected ? kGpuMapConnected : 0u) | (params.map.invert ? kGpuMapInvert : 0u) | (params.map.smooth ? kGpuMapSmooth : 0u);
    k.exponent = params.map.exponent;
    k.outside = mapConnected ? params.map.remap(0.f) : 1.f;
    k.blackRx = axisX.black;
    k.whiteRx = axisX.white;
    k.blackRy = axisY.black;
    k.whiteRy = axisY.white;

    gpu::GpuBuffer* constants = nullptr;
    gpu::GpuBuffer* work = nullptr;
    if (!ctx.scratch(sizeof(k), constants) || !constants || !ctx.scratch(workFloats * sizeof(float), work) || !work) {
        return eStatusFailed;
    }

    IErodeDilateGpuBuffers buffers;
    buffers.source = constants;
    buffers.map = constants;
    buffers.constants = constants;
    buffers.work = work;
    buffers.out = io.filtered.buffer;

    const GpuImageView map = (kIErodeDilateInputMap < (int)io.inputs.size()) ? io.inputs[kIErodeDilateInputMap] : GpuImageView();
    if (mapConnected && map.buffer && (mapChannel >= 0) && (mapChannel < map.nComps) && !PassExec::isEmptyRect(map.bounds)) {
        const gpu::GpuBlurImage image = PassExec::toGpuBlurImage(map);
        if (!gpuFloatIndex(image, map.bounds.x1, map.bounds.y1, map.nComps, &k.mapOffset)) {
            return eStatusFailed;
        }
        k.mapFlags |= kGpuMapRead;
        k.mapChannel = (uint32_t)mapChannel;
        k.mapChannels = (uint32_t)map.nComps;
        k.mapStride = image.strideFloats;
        k.mapX1 = map.bounds.x1;
        k.mapY1 = map.bounds.y1;
        k.mapX2 = map.bounds.x2;
        k.mapY2 = map.bounds.y2;
        buffers.map = map.buffer;
    }
    if (!ctx.updateBuffer(*constants, 0, std::as_bytes(std::span<const IErodeDilateGpuConstants>(&k, 1)))) {
        return eStatusFailed;
    }

    IErodeDilateGpuParams base {};
    base.domX1 = domain.x1;
    base.domY1 = domain.y1;
    base.domWidth = (uint32_t)width;
    base.domHeight = (uint32_t)height;
    base.channels = (uint32_t)nComps;
    base.flags = (params.disc ? kGpuDisc : 0u) | (takeMax ? kGpuTakeMax : 0u);
    const gpu::GpuBlurImage filtered = PassExec::toGpuBlurImage(io.filtered);
    base.outX1 = io.output.x1;
    base.outY1 = io.output.y1;
    base.outWidth = (uint32_t)std::max(0, io.output.width());
    base.outHeight = (uint32_t)std::max(0, io.output.height());
    base.dstStride = filtered.strideFloats;
    if (!PassExec::isEmptyRect(io.output) && !gpuFloatIndex(filtered, io.output.x1, io.output.y1, nComps, &base.dstOffset)) {
        return eStatusFailed;
    }
    if (io.source.buffer && !PassExec::isEmptyRect(io.sourceValid)) {
        const gpu::GpuBlurImage source = PassExec::toGpuBlurImage(io.source);
        base.srcX1 = io.sourceValid.x1;
        base.srcY1 = io.sourceValid.y1;
        base.srcX2 = io.sourceValid.x2;
        base.srcY2 = io.sourceValid.y2;
        base.srcStride = source.strideFloats;
        if (!gpuFloatIndex(source, io.sourceValid.x1, io.sourceValid.y1, nComps, &base.srcOffset)) {
            return eStatusFailed;
        }
        buffers.source = io.source.buffer;
    }

    for (int c = 0; c < nComps; ++c) {
        IErodeDilateGpuParams p = base;
        p.comp = (uint32_t)c;
        if (!dispatchIErodeDilate(ctx, gpu::GpuKernelId::ierodedilate_load, buffers, p, base.domWidth, base.domHeight)) {
            return eStatusFailed;
        }
        ctx.computeBarrier();
        for (int level = 1; level < levels; ++level) {
            p.level = (uint32_t)level;
            if (!dispatchIErodeDilate(ctx, gpu::GpuKernelId::ierodedilate_level, buffers, p, base.domWidth, base.domHeight)) {
                return eStatusFailed;
            }
            ctx.computeBarrier();
        }
        if (!dispatchIErodeDilate(ctx, gpu::GpuKernelId::ierodedilate_filter, buffers, p, base.outWidth, base.outHeight)) {
            return eStatusFailed;
        }
        // The next component overwrites the table the filter reads.
        if (c < nComps - 1) {
            ctx.computeBarrier();
        }
    }

    return eStatusOK;
} // recordIErodeDilateGpu
#endif // NATRON_HAVE_GPU
} // anonymous namespace

template <ErodeDilateMode MODE>
IErodeDilateNode<MODE>::IErodeDilateNode(NodePtr node)
    : StagedImageEffect(node)
    , _map()
    , _size()
    , _shape()
    , _expandRoD()
{
}

template <ErodeDilateMode MODE>
IErodeDilateNode<MODE>::~IErodeDilateNode()
{
}

template <ErodeDilateMode MODE>
const NodeSpec*
IErodeDilateNode<MODE>::nodeSpec() const
{
    return staticNodeSpec<IErodeDilateSpec<MODE>::kNodeSpec>();
}

template <ErodeDilateMode MODE>
const ImagePipelineSpec&
IErodeDilateNode<MODE>::imagePipelineSpec() const
{
    return staticImagePipelineSpec<IErodeDilateSpec<MODE>::kPipeline, IErodeDilateSpec<MODE>::kNodeSpec>();
}

template <ErodeDilateMode MODE>
std::string
IErodeDilateNode<MODE>::getInputHint(int inputNb) const
{
    switch (inputNb) {
    case kIErodeDilateInputSource:
        return kDilate ? tr("The image to dilate.").toStdString() : tr("The image to erode.").toStdString();
    case kIErodeDilateInputMask:
        return tr("The mask the result is mixed with the source through.").toStdString();
    case kIErodeDilateInputMap:
        return tr("The Map: the element has the Size where it is white and the Size At Black where it is black.").toStdString();
    }

    return std::string();
}

template <ErodeDilateMode MODE>
void
IErodeDilateNode<MODE>::addAcceptedComponents(int inputNb,
                                              std::list<ImageLayerDesc>* comps)
{
    StagedImageEffect::addAcceptedComponents(inputNb, comps);
    if (!isInputMask(inputNb)) {
        comps->push_back(ImageLayerDesc::getXYComponents());
    }
}

template <ErodeDilateMode MODE>
void
IErodeDilateNode<MODE>::initializeKnobs()
{
    const KnobPagePtr page = createControlsPage();

    _map.createSizeAtBlackKnob(this, page, 2);

    KnobDoublePtr sizeKnob = AppManager::createKnob<KnobDouble>(this, tr("Size"), 2);
    sizeKnob->setName(kIErodeDilateParamSize);
    sizeKnob->setHintToolTip(tr("Half the width and height of the structuring element where the Map is white, in pixel units (>=0): the rectangle is 2*size+1 pixels wide, the disc's radius is size. A fractional size blends the results of the whole sizes either side of it."));
    sizeKnob->setSpatial(true);
    sizeKnob->setCanAutoFoldDimensions(true);
    for (int d = 0; d < 2; ++d) {
        sizeKnob->setMinimum(0., d);
        sizeKnob->setMaximum(kMapInputMaxSize, d);
        sizeKnob->setDisplayMinimum(0., d);
        sizeKnob->setDisplayMaximum(100., d);
        sizeKnob->setIncrement(0.1, d);
        sizeKnob->setDecimals(1, d);
        sizeKnob->setDefaultValue(1., d);
    }
    page->addKnob(sizeKnob);
    _size = sizeKnob;

    KnobChoicePtr shapeKnob = AppManager::createKnob<KnobChoice>(this, tr("Shape"));
    shapeKnob->setName(kIErodeDilateParamShape);
    shapeKnob->setHintToolTip(tr("The shape of the structuring element."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kIErodeDilateParamShapeSquare, tr("Square").toStdString(), tr("A rectangle of 2*size+1 pixels, as Erode's and Dilate's.").toStdString()));
        options.push_back(ChoiceOption(kIErodeDilateParamShapeDisc, tr("Disc").toStdString(), tr("The pixels within the ellipse of half-axes the two sizes: a disc when they are equal.").toStdString()));
        shapeKnob->populateChoices(options);
    }
    shapeKnob->setDefaultValue(0);
    page->addKnob(shapeKnob);
    _shape = shapeKnob;

    _map.createKnobs(this, page);

    KnobBoolPtr expandRoDKnob = AppManager::createKnob<KnobBool>(this, tr("Expand RoD"));
    expandRoDKnob->setName(kIErodeDilateParamExpandRoD);
    if (kDilate) {
        expandRoDKnob->setHintToolTip(tr("Expand the source region of definition by the larger of the two sizes."));
    } else {
        expandRoDKnob->setHintToolTip(tr("Erosion never expands the source region of definition; kept for symmetry with IDilate."));
    }
    expandRoDKnob->setDefaultValue(true);
    page->addKnob(expandRoDKnob);
    _expandRoD = expandRoDKnob;

    addStageKnobs(page);

    _map.declareLayerKnob(getNode(), kIErodeDilateInputMap);
} // IErodeDilateNode::initializeKnobs

template <ErodeDilateMode MODE>
IErodeDilateParams
IErodeDilateNode<MODE>::getParams(double time,
                                  ViewIdx view) const
{
    const KnobReader reader(time, view);
    IErodeDilateParams params;

    params.sizeX = MapInput::clampSize(reader.value(_size, 0));
    params.sizeY = MapInput::clampSize(reader.value(_size, 1));
    params.disc = (reader.value(_shape) == 1);
    params.expandRoD = reader.value(_expandRoD);
    params.map = _map.bake(reader);

    return params;
}

template <ErodeDilateMode MODE>
PassList
IErodeDilateNode<MODE>::passes(const IErodeDilateParams& params,
                               const RenderScale& scale,
                               const MapRange& range,
                               const std::shared_ptr<const MapPlane>& map,
                               const RectI& roi)
{
    PassList list;
    IErodeDilateAxis axisX;
    IErodeDilateAxis axisY;
    params.axes(scale, &axisX, &axisY);
    const float rx = axisX.largest(range);
    const float ry = axisY.largest(range);

    if ((rx <= 0.f) && (ry <= 0.f)) {
        return list;
    }
    const int haloX = (int)std::ceil(rx);
    const int haloY = (int)std::ceil(ry);

    list.scratchSlots = 2;
    list.resultSlot = 1;

    if (!map) {
        Pass halo = Pass::gather(haloX, haloY, true, std::function<PassRowFn(const PassScratch&)>());
        halo.slot = 1;
        // Up to four elements of two lookups per row.
        halo.tapsPerSample = 8 * (2 * (std::size_t)haloY + 1);
        // An erosion never spreads the region of definition.
        if (!kDilate) {
            halo.growX = 0;
            halo.growY = 0;
        }
        list.passes.push_back(halo);

        return list;
    }
    MorphologyStatePtr state = std::make_shared<MorphologyState>();
    state->map = map;
    const std::shared_ptr<const MapPlane> plane = map;
    const int x1 = roi.x1;
    const int y1 = roi.y1;
    const int width = roi.width();
    const IErodeDilateKernels::TileRadiiFn radii = [plane, axisX, axisY, x1, y1, width](int r, float* rxRow, float* ryRow) {
        float last = plane->at(x1, y1 + r);
        float lastX = axisX.at(last);
        float lastY = axisY.at(last);
        for (int c = 0; c < width; ++c) {
            const float m = plane->at(x1 + c, y1 + r);
            if (m != last) {
                last = m;
                lastX = axisX.at(m);
                lastY = axisY.at(m);
            }
            rxRow[c] = lastX;
            ryRow[c] = lastY;
        }
    };
    state->plan = IErodeDilateKernels::planTile(roi, radii, params.disc, kDilate, kTileBudgetFloats);
    tilePasses(state, haloX, haloY, kDilate, &list);

    return list;
} // IErodeDilateNode::passes

template <ErodeDilateMode MODE>
std::string
IErodeDilateNode<MODE>::mapNotConnectedMessage()
{
    return kDilate ? tr("The Map input is not connected: the dilation has the Size everywhere.").toStdString()
                   : tr("The Map input is not connected: the erosion has the Size everywhere.").toStdString();
}

template <ErodeDilateMode MODE>
PassList
IErodeDilateNode<MODE>::opPasses(double time,
                                 ViewIdx view,
                                 const RenderScale& scale) const
{
    // A disconnected Map reads 1 everywhere; a connected one may hold any value.
    MapRange range;
    range.min = getInput(kIErodeDilateInputMap) ? 0.f : 1.f;
    range.max = 1.f;
    const IErodeDilateParams params = getParams(time, view);
    PassList list = passes(params, scale, range, std::shared_ptr<const MapPlane>(), RectI());

#ifdef NATRON_HAVE_GPU
    bool capped = false;
    for (const Pass& pass : list.passes) {
        capped = capped || (pass.haloX > kGpuMaxRadius) || (pass.haloY > kGpuMaxRadius);
    }
    if (!list.empty() && !capped) {
        const IErodeDilateNode* self = this;
        list.gpu = [self, time, view, params, scale](const PassGpuIO& io) -> StatusEnum {
            return self->recordGpu(io, time, view, params, scale);
        };
    }
#endif

    return list;
}

template <ErodeDilateMode MODE>
StatusEnum
IErodeDilateNode<MODE>::recordGpu(const PassGpuIO& io,
                                  double time,
                                  ViewIdx view,
                                  const IErodeDilateParams& params,
                                  const RenderScale& scale) const
{
#ifdef NATRON_HAVE_GPU
    const bool connected = (bool)getInput(kIErodeDilateInputMap);
    ImageLayerDesc layer;
    int channel = -1;
    if (!connected || !_map.resolveChannel(const_cast<IErodeDilateNode*>(this), kIErodeDilateInputMap, time, view, &layer, &channel)) {
        channel = -1;
    }

    return recordIErodeDilateGpu(io, params, scale, kDilate, connected, channel);
#else
    Q_UNUSED(io);
    Q_UNUSED(time);
    Q_UNUSED(view);
    Q_UNUSED(params);
    Q_UNUSED(scale);

    return eStatusFailed;
#endif
}

template <ErodeDilateMode MODE>
bool
IErodeDilateNode<MODE>::readsGpuInput(int inputNb,
                                      const ImageLayerDesc& /*plane*/,
                                      double time,
                                      ViewIdx view) const
{
    if (inputNb != kIErodeDilateInputMap) {
        return true;
    }
    ImageLayerDesc layer;
    int channel = -1;

    return _map.resolveChannel(const_cast<IErodeDilateNode*>(this), kIErodeDilateInputMap, time, view, &layer, &channel);
}

template <ErodeDilateMode MODE>
GpuWeight
IErodeDilateNode<MODE>::gpuWeight(double time,
                                  ViewIdx view,
                                  unsigned int mipmapLevel) const
{
    IErodeDilateAxis x;
    IErodeDilateAxis y;

    getParams(time, view).axes(RenderScale::fromMipmapLevel(mipmapLevel), &x, &y);

    return gpuSizeScaledWeight(std::floor(std::max({ x.black, x.white, y.black, y.white })));
}

template <ErodeDilateMode MODE>
bool
IErodeDilateNode<MODE>::opPassesInputLayer(int inputNb,
                                           const ImageLayerDesc& plane,
                                           double time,
                                           ViewIdx view,
                                           ImageLayerDesc* layer) const
{
    if (inputNb != kIErodeDilateInputMap) {
        *layer = plane;

        return true;
    }
    int channel = -1;

    return _map.resolveChannel(const_cast<IErodeDilateNode*>(this), kIErodeDilateInputMap, time, view, layer, &channel);
}

template <ErodeDilateMode MODE>
bool
IErodeDilateNode<MODE>::isOpIdentity(double time,
                                     const RenderScale& scale,
                                     const RectI& /*roi*/,
                                     ViewIdx view)
{
    return opPasses(time, view, scale).empty();
}

template <ErodeDilateMode MODE>
StatusEnum
IErodeDilateNode<MODE>::renderOp(const RenderActionArgs& args,
                                 const StagePlan& plan)
{
    const PassList full = opPasses(args.time, args.view, args.mappedScale);

    if (full.empty()) {
        return runPassesCpu(*this, plan, args, full);
    }
    const IErodeDilateParams params = getParams(args.time, args.view);
    // Only the output pixels' radii matter, so the Map is read over the render window alone, and
    // the tile's halo is that of its largest radius there.
    std::shared_ptr<MapPlane> map = std::make_shared<MapPlane>();
    const StatusEnum status = _map.read(this, kIErodeDilateInputMap, params.map, args.time, args.view, args.mappedScale, args.roi, map.get());
    if (status != eStatusOK) {
        return status;
    }
    const MapRange range = map->tileRange(args.roi);

    return runPassesCpu(*this, plan, args, passes(params, args.mappedScale, range, map, args.roi));
}

template <ErodeDilateMode MODE>
bool
IErodeDilateNode<MODE>::clipRegionOfDefinition(double time,
                                               const RenderScale& /*scale*/,
                                               ViewIdx view,
                                               const EffectInstancePtr& /*source*/,
                                               const RectI& srcRoD,
                                               RectI* rod)
{
    const KnobReader reader(time, view);

    if (reader.value(_expandRoD) || (*rod == srcRoD)) {
        return false;
    }
    *rod = srcRoD;

    return true;
}

template <ErodeDilateMode MODE>
void
IErodeDilateNode<MODE>::onEffectCreated(bool /*mayCreateFileDialog*/,
                                        const CreateNodeArgs& /*args*/)
{
    refreshMapWarning();
}

template <ErodeDilateMode MODE>
void
IErodeDilateNode<MODE>::onInputChanged(int /*inputNo*/)
{
    refreshMapWarning();
}

template <ErodeDilateMode MODE>
void
IErodeDilateNode<MODE>::refreshMapWarning()
{
    const NodePtr node = getNode();

    if (!node) {
        return;
    }
    const std::string message = mapNotConnectedMessage();
    if (!getInput(kIErodeDilateInputMap)) {
        node->setPersistentMessage(eMessageTypeWarning, message);

        return;
    }
    // Only our own warning is taken back: anything posted since has replaced it.
    QString current;
    int type = 0;
    node->getPersistentMessage(&current, &type, false);
    if (current == QString::fromUtf8(message.c_str())) {
        node->clearPersistentMessage(false);
    }
}

template class IErodeDilateNode<ErodeDilateMode::Erode>;
template class IErodeDilateNode<ErodeDilateMode::Dilate>;

NATRON_NAMESPACE_EXIT
