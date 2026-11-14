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

#include "RawSuperpixel.h"

#include <algorithm>

NATRON_NAMESPACE_ENTER

namespace RawSuperpixel {
namespace {
    struct Box {
        int x1, y1, x2, y2;

        bool empty() const
        {
            return (x1 >= x2) || (y1 >= y2);
        }

        Box clipped(const Box& c) const
        {
            return Box { std::max(x1, c.x1), std::max(y1, c.y1), std::min(x2, c.x2), std::min(y2, c.y2) };
        }

        Box grown(int g) const
        {
            return Box { x1 - g, y1 - g, x2 + g, y2 + g };
        }
    };

    struct Sums {
        float sum[3] = { 0.f, 0.f, 0.f };
        int count[3] = { 0, 0, 0 };
    };

    void
    addPhotosites(const RawNodeSupport::TopDownBuffer& source,
                  const RawLayout& layout,
                  const Box& box,
                  const bool wanted[3],
                  Sums* sums)
    {
        for (int y = box.y1; y < box.y2; ++y) {
            for (int x = box.x1; x < box.x2; ++x) {
                const int c = (int)layout.colourAt(x, y);
                if ((c < 3) && wanted[c]) {
                    sums->sum[c] += source.data[source.offset(x, y)];
                    ++sums->count[c];
                }
            }
        }
    }

    void
    renderPixel(const RawNodeSupport::TopDownBuffer& source,
                const RawLayout& layout,
                const Box& clip,
                const Box& footprint,
                ModeEnum mode,
                float rgb[3])
    {
        rgb[0] = rgb[1] = rgb[2] = 0.f;
        const Box f = footprint.clipped(clip);
        if (f.empty()) {
            return;
        }
        if ((mode == eModeBoxMono) || (mode == eModeBoxRgb)) {
            const int channels = (mode == eModeBoxRgb) ? 3 : 1;
            float sum[3] = { 0.f, 0.f, 0.f };
            for (int y = f.y1; y < f.y2; ++y) {
                for (int x = f.x1; x < f.x2; ++x) {
                    const float* p = &source.data[source.offset(x, y)];
                    for (int c = 0; c < channels; ++c) {
                        sum[c] += p[c];
                    }
                }
            }
            const float n = (float)((f.x2 - f.x1) * (f.y2 - f.y1));
            for (int c = 0; c < 3; ++c) {
                rgb[c] = sum[(channels == 3) ? c : 0] / n;
            }

            return;
        }
        const bool all[3] = { true, true, true };
        Sums sums;
        addPhotosites(source, layout, f, all, &sums);
        if (mode == eModePhotositeColor) {
            const float n = (float)((f.x2 - f.x1) * (f.y2 - f.y1));
            for (int c = 0; c < 3; ++c) {
                rgb[c] = sums.sum[c] / n;
            }

            return;
        }
        for (int c = 0; c < 3; ++c) {
            if (sums.count[c] > 0) {
                continue;
            }
            bool only[3] = { false, false, false };
            only[c] = true;
            for (int g = 1; (g <= layout.period()) && (sums.count[c] == 0); ++g) {
                addPhotosites(source, layout, f.grown(g).clipped(clip), only, &sums);
            }
        }
        for (int c = 0; c < 3; ++c) {
            rgb[c] = (sums.count[c] > 0) ? sums.sum[c] / (float)sums.count[c] : 0.f;
        }
    }
} // namespace

RectI
sourceRegion(const RectI& window,
             unsigned int mipmapLevel,
             int period,
             const RectI& rod)
{
    const int size = 1 << mipmapLevel;
    const int grow = std::max(0, period);

    return RectI(window.x1 * size - grow, window.y1 * size - grow, window.x2 * size + grow, window.y2 * size + grow).intersect(rod);
}

void
render(const RawNodeSupport::TopDownBuffer& source,
       const RawLayout& layout,
       const RectI& rod,
       unsigned int mipmapLevel,
       ModeEnum mode,
       RawNodeSupport::TopDownBuffer* out,
       const RawKernels::ParallelFor& parallel)
{
    const RectI& window = out->rect;
    const int size = 1 << mipmapLevel;
    const Box clip = Box { rod.x1, rod.y1, rod.x2, rod.y2 }.clipped(Box { source.rect.x1, source.rect.y1, source.rect.x2, source.rect.y2 });

    parallel(window.height(), [&](int r) {
        const int y = window.y1 + r;
        for (int x = window.x1; x < window.x2; ++x) {
            float rgb[3];
            renderPixel(source, layout, clip, Box { x * size, y * size, (x + 1) * size, (y + 1) * size }, mode, rgb);
            float* q = &out->data[out->offset(x, y)];
            for (int c = 0; c < 3; ++c) {
                q[c] = rgb[c];
            }
        }
    });
}
} // namespace RawSuperpixel

NATRON_NAMESPACE_EXIT
