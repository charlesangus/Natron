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
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7),
 * src/iop/hlreconstruct/segmentation.c, by Hanno Schwalm.
 * darktable is distributed under the GNU GPL, version 3 or (at your option) any later version,
 * and so is this port: GPL-3.0-or-later.
 *
 *     This file is part of darktable,
 *     Copyright (C) 2022-2026 darktable developers.
 *
 *     darktable is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     darktable is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "RawSegmentation.h"

#include <algorithm>
#include <cstring>

NATRON_NAMESPACE_ENTER

namespace RawKernels {
namespace {
    struct Pos {
        int xpos;
        int ypos;
    };

    // darktable's fixed-size floodfill stack: a push past its end is dropped, as there.
    struct FloodStack {
        int pos = 0;
        int size = 0;
        std::vector<Pos> el;

        void push(int xpos,
                  int ypos)
        {
            if (pos >= size - 1) {
                return;
            }
            el[pos].xpos = xpos;
            el[pos].ypos = ypos;
            ++pos;
        }

        Pos pop()
        {
            if (pos > 0) {
                --pos;
            }

            return el[pos];
        }
    };

    void
    clearSegmentSlot(Segmentation* seg,
                     std::uint32_t id)
    {
        if (id > (std::uint32_t)(seg->slots - 1)) {
            return;
        }
        seg->size[id] = seg->xmin[id] = seg->xmax[id] = seg->ymin[id] = seg->ymax[id] = 0;
        seg->val1[id] = seg->val2[id] = 0.0f;
    }

    inline std::uint32_t
    testDilate(const std::uint32_t* img,
               std::size_t i,
               std::size_t w1,
               int radius)
    {
        std::uint32_t retval = 0;
        retval = img[i - w1 - 1] | img[i - w1] | img[i - w1 + 1] | img[i - 1] | img[i] | img[i + 1] | img[i + w1 - 1] | img[i + w1] | img[i + w1 + 1];
        if (retval || (radius < 2))
            return retval;

        const std::size_t w2 = 2 * w1;
        retval = img[i - w2 - 1] | img[i - w2] | img[i - w2 + 1] | img[i - w1 - 2] | img[i - w1 + 2] | img[i - 2] | img[i + 2] | img[i + w1 - 2] | img[i + w1 + 2] | img[i + w2 - 1] | img[i + w2] | img[i + w2 + 1];
        if (retval || (radius < 3))
            return retval;

        const std::size_t w3 = 3 * w1;
        retval = img[i - w3 - 2] | img[i - w3 - 1] | img[i - w3] | img[i - w3 + 1] | img[i - w3 + 2] | img[i - w2 - 3] | img[i - w2 - 2] | img[i - w2 + 2] | img[i - w2 + 3] | img[i - w1 - 3] | img[i - w1 + 3] | img[i - 3] | img[i + 3] | img[i + w1 - 3] | img[i + w1 + 3] | img[i + w2 - 3] | img[i + w2 - 2] | img[i + w2 + 2] | img[i + w2 + 3] | img[i + w3 - 2] | img[i + w3 - 1] | img[i + w3] | img[i + w3 + 1] | img[i + w3 + 2];
        if (retval || (radius < 4))
            return retval;

        const std::size_t w4 = 4 * w1;
        retval = img[i - w4 - 2] | img[i - w4 - 1] | img[i - w4] | img[i - w4 + 1] | img[i - w4 + 2] | img[i - w3 - 3] | img[i - w3 + 3] | img[i - w2 - 4] | img[i - w2 + 4] | img[i - w1 - 4] | img[i - w1 + 4] | img[i - 4] | img[i + 4] | img[i + w1 - 4] | img[i + w1 + 4] | img[i + w2 - 4] | img[i + w2 + 4] | img[i + w3 - 3] | img[i + w3 + 3] | img[i + w4 - 2] | img[i + w4 - 1] | img[i + w4] | img[i + w4 + 1] | img[i + w4 + 2];
        if (retval || (radius < 5))
            return retval;

        const std::size_t w5 = 5 * w1;
        retval = img[i - w5 - 2] | img[i - w5 - 1] | img[i - w5] | img[i - w5 + 1] | img[i - w5 + 2] | img[i - w4 - 4] | img[i - w4 - 3] | img[i - w4 + 3] | img[i - w4 + 4] | img[i - w3 - 4] | img[i - w3 + 4] | img[i - w2 - 5] | img[i - w2 + 5] | img[i - w1 - 5] | img[i - w1 + 5] | img[i - 5] | img[i + 5] | img[i + w1 - 5] | img[i + w1 + 5] | img[i + w2 - 5] | img[i + w2 + 5] | img[i + w3 - 4] | img[i + w3 + 4] | img[i + w4 - 4] | img[i + w4 - 3] | img[i + w4 + 3] | img[i + w4 + 4] | img[i + w5 - 2] | img[i + w5 - 1] | img[i + w5] | img[i + w5 + 1] | img[i + w5 + 2];
        if (retval || (radius < 6))
            return retval;

        const std::size_t w6 = 6 * w1;
        retval = img[i - w6 - 2] | img[i - w6 - 1] | img[i - w6] | img[i - w6 + 1] | img[i - w6 + 2] | img[i - w5 - 4] | img[i - w5 - 3] | img[i - w5 + 3] | img[i - w5 + 4] | img[i - w4 - 5] | img[i - w4 + 5] | img[i - w3 - 5] | img[i - w3 + 5] | img[i - w2 - 6] | img[i - w2 + 6] | img[i - w1 - 6] | img[i - w1 + 6] | img[i - 6] | img[i + 6] | img[i + w1 - 6] | img[i + w1 + 6] | img[i + w2 - 6] | img[i + w2 + 6] | img[i + w3 - 5] | img[i + w3 + 5] | img[i + w4 - 5] | img[i + w4 + 5] | img[i + w5 - 4] | img[i + w5 - 3] | img[i + w5 + 3] | img[i + w5 + 4] | img[i + w6 - 2] | img[i + w6 - 1] | img[i + w6] | img[i + w6 + 1] | img[i + w6 + 2];
        if (retval || (radius < 7))
            return retval;

        const std::size_t w7 = 7 * w1;
        retval = img[i - w7 - 3] | img[i - w7 - 2] | img[i - w7 - 1] | img[i - w7] | img[i - w7 + 1] | img[i - w7 + 2] | img[i - w7 + 3] | img[i - w6 - 4] | img[i - w6 - 3] | img[i - w6 + 3] | img[i - w6 + 4] | img[i - w5 - 6] | img[i - w5 - 5] | img[i - w5 + 5] | img[i - w5 + 6] | img[i - w4 - 6] | img[i - w4 + 6] | img[i - w3 - 7] | img[i - w3 - 6] | img[i - w3 + 6] | img[i - w3 + 7] | img[i - w2 - 7] | img[i - w2 + 7] | img[i - w1 - 7] | img[i - w1 + 7] | img[i - 7] | img[i + 7] | img[i + w1 - 7] | img[i + w1 + 7] | img[i + w2 - 7] | img[i + w2 + 7] | img[i + w3 - 7] | img[i + w3 - 6] | img[i + w3 + 6] | img[i + w3 + 7] | img[i + w4 - 6] | img[i + w4 + 6] | img[i + w5 - 6] | img[i + w5 - 5] | img[i + w5 + 5] | img[i + w5 + 6] | img[i + w6 - 4] | img[i + w6 - 3] | img[i + w6 + 3] | img[i + w6 + 4] | img[i + w7 - 3] | img[i + w7 - 2] | img[i + w7 - 1] | img[i + w7] | img[i + w7 + 1] | img[i + w7 + 2] | img[i + w7 + 3];
        if (retval || (radius < 8))
            return retval;

        const std::size_t w8 = 8 * w1;
        retval = img[i - w8 - 4] | img[i - w8 - 3] | img[i - w8 - 2] | img[i - w8 - 1] | img[i - w8] | img[i - w8 + 1] | img[i - w8 + 2] | img[i - w8 + 3] | img[i - w8 + 4] | img[i - w7 - 6] | img[i - w7 - 5] | img[i - w7 - 4] | img[i - w7 + 4] | img[i - w7 + 5] | img[i - w7 + 6] | img[i - w6 - 6] | img[i - w6 - 5] | img[i - w6 + 5] | img[i - w6 + 6] | img[i - w5 - 7] | img[i - w5 + 6] | img[i - w4 - 8] | img[i - w4 - 7] | img[i - w4 + 7] | img[i - w4 + 8] | img[i - w3 - 8] | img[i - w3 - 7] | img[i - w3 + 7] | img[i - w3 + 8] | img[i - w2 - 8] | img[i - w2 + 8] | img[i - w1 - 8] | img[i - w1 + 8] | img[i - 8] | img[i + 8] | img[i + w1 - 8] | img[i + w1 + 8] | img[i + w2 - 8] | img[i + w2 + 8] | img[i + w3 - 8] | img[i + w3 - 7] | img[i + w3 + 7] | img[i + w3 + 8] | img[i + w4 - 8] | img[i + w4 - 7] | img[i + w4 + 7] | img[i + w4 + 8] | img[i + w5 - 7] | img[i + w5 + 7] | img[i + w6 - 6] | img[i + w6 - 5] | img[i + w6 + 5] | img[i + w6 + 6] | img[i + w7 - 6] | img[i + w7 - 5] | img[i + w7 - 4] | img[i + w7 + 4] | img[i + w7 + 5] | img[i + w7 + 6] | img[i + w8 - 4] | img[i + w8 - 3] | img[i + w8 - 2] | img[i + w8 - 1] | img[i + w8] | img[i + w8 + 1] | img[i + w8 + 2] | img[i + w8 + 3] | img[i + w8 + 4];
        return retval;
    }

    inline std::uint32_t
    testErode(const std::uint32_t* img,
              std::size_t i,
              std::size_t w1,
              int radius)
    {
        std::uint32_t retval = 1;
        retval = img[i - w1 - 1] & img[i - w1] & img[i - w1 + 1] & img[i - 1] & img[i] & img[i + 1] & img[i + w1 - 1] & img[i + w1] & img[i + w1 + 1];
        if ((retval == 0) || (radius < 2))
            return retval;

        const std::size_t w2 = 2 * w1;
        retval = img[i - w2 - 1] & img[i - w2] & img[i - w2 + 1] & img[i - w1 - 2] & img[i - w1 + 2] & img[i - 2] & img[i + 2] & img[i + w1 - 2] & img[i + w1 + 2] & img[i + w2 - 1] & img[i + w2] & img[i + w2 + 1];
        if ((retval == 0) || (radius < 3))
            return retval;

        const std::size_t w3 = 3 * w1;
        retval = img[i - w3 - 2] & img[i - w3 - 1] & img[i - w3] & img[i - w3 + 1] & img[i - w3 + 2] & img[i - w2 - 3] & img[i - w2 - 2] & img[i - w2 + 2] & img[i - w2 + 3] & img[i - w1 - 3] & img[i - w1 + 3] & img[i - 3] & img[i + 3] & img[i + w1 - 3] & img[i + w1 + 3] & img[i + w2 - 3] & img[i + w2 - 2] & img[i + w2 + 2] & img[i + w2 + 3] & img[i + w3 - 2] & img[i + w3 - 1] & img[i + w3] & img[i + w3 + 1] & img[i + w3 + 2];
        if ((retval == 0) || (radius < 4))
            return retval;

        const std::size_t w4 = 4 * w1;
        retval = img[i - w4 - 2] & img[i - w4 - 1] & img[i - w4] & img[i - w4 + 1] & img[i - w4 + 2] & img[i - w3 - 3] & img[i - w3 + 3] & img[i - w2 - 4] & img[i - w2 + 4] & img[i - w1 - 4] & img[i - w1 + 4] & img[i - 4] & img[i + 4] & img[i + w1 - 4] & img[i + w1 + 4] & img[i + w2 - 4] & img[i + w2 + 4] & img[i + w3 - 3] & img[i + w3 + 3] & img[i + w4 - 2] & img[i + w4 - 1] & img[i + w4] & img[i + w4 + 1] & img[i + w4 + 2];
        if ((retval == 0) || (radius < 5))
            return retval;

        const std::size_t w5 = 5 * w1;
        retval = img[i - w5 - 2] & img[i - w5 - 1] & img[i - w5] & img[i - w5 + 1] & img[i - w5 + 2] & img[i - w4 - 4] & img[i - w4 - 3] & img[i - w4 + 3] & img[i - w4 + 4] & img[i - w3 - 4] & img[i - w3 + 4] & img[i - w2 - 5] & img[i - w2 + 5] & img[i - w1 - 5] & img[i - w1 + 5] & img[i - 5] & img[i + 5] & img[i + w1 - 5] & img[i + w1 + 5] & img[i + w2 - 5] & img[i + w2 + 5] & img[i + w3 - 4] & img[i + w3 + 4] & img[i + w4 - 4] & img[i + w4 - 3] & img[i + w4 + 3] & img[i + w4 + 4] & img[i + w5 - 2] & img[i + w5 - 1] & img[i + w5] & img[i + w5 + 1] & img[i + w5 + 2];
        return retval;
    }

    void
    dilating(const std::uint32_t* img,
             std::uint32_t* o,
             int w1,
             int height,
             int border,
             int radius,
             const ParallelFor& parallel)
    {
        parallel(height - 2 * border, [&](int r) {
            const int row = border + r;
            for (int col = border; col < w1 - border; ++col) {
                const std::size_t i = (std::size_t)row * w1 + col;
                o[i] = testDilate(img, i, w1, radius) ? 1 : 0;
            }
        });
    }

    void
    eroding(const std::uint32_t* img,
            std::uint32_t* o,
            int w1,
            int height,
            int border,
            int radius,
            const ParallelFor& parallel)
    {
        parallel(height - 2 * border, [&](int r) {
            const int row = border + r;
            for (int col = border; col < w1 - border; ++col) {
                const std::size_t i = (std::size_t)row * w1 + col;
                o[i] = testErode(img, i, w1, radius) ? 1 : 0;
            }
        });
    }

    // The bottom band starts one row higher than the top band's mirror and leaves the last row
    // alone, as darktable's _intimage_borderfill() does: the segmentation never reads that row.
    void
    intimageBorderfill(std::uint32_t* d,
                       int width,
                       int height,
                       std::uint32_t val,
                       int border)
    {
        const std::size_t di = (std::size_t)(height - border - 1) * width;
        for (std::size_t i = 0; i < (std::size_t)border * width; ++i) {
            d[i] = d[i + di] = val;
        }
        for (std::size_t row = border; row < (std::size_t)(height - border); ++row) {
            const std::size_t j = row * width;
            const std::size_t dj = width - border;
            for (std::size_t i = 0; i < (std::size_t)border; ++i) {
                d[j + i] = d[j + i + dj] = val;
            }
        }
    }

    // Marks the unsegmented location rp as bordering segment id, growing its rectangle.
    inline void
    markBorder(std::uint32_t* d,
               std::size_t rp,
               int xp,
               int yp,
               std::uint32_t id,
               int* minX,
               int* maxX,
               int* minY,
               int* maxY)
    {
        *minX = std::min(*minX, xp);
        *maxX = std::max(*maxX, xp);
        *minY = std::min(*minY, yp);
        *maxY = std::max(*maxY, yp);
        d[rp] = kSegmentIdMask | id;
    }

    bool
    floodfillSegmentize(int yin,
                        int xin,
                        Segmentation* seg,
                        int w,
                        int h,
                        std::uint32_t id,
                        FloodStack* stack)
    {
        if (id >= (std::uint32_t)(seg->slots - 2)) {
            return false;
        }
        const int border = seg->border;
        std::uint32_t* d = seg->data.data();
        int minX = xin;
        int maxX = xin;
        int minY = yin;
        int maxY = yin;
        int cnt = 0;
        stack->pos = 0;
        clearSegmentSlot(seg, id);

        stack->push(xin, yin);
        while (stack->pos) {
            const Pos coord = stack->pop();
            const int x = coord.xpos;
            const int y = coord.ypos;
            if (d[(std::size_t)y * w + x] != 1) {
                continue;
            }
            const int yUp = y - 1;
            const int yDown = y + 1;
            bool lastXUp = false;
            bool lastXDown = false;
            bool firstXUp = false;
            bool firstXDown = false;
            d[(std::size_t)y * w + x] = id;
            ++cnt;
            if ((yUp >= border) && (d[(std::size_t)yUp * w + x] == 1)) {
                stack->push(x, yUp);
                firstXUp = lastXUp = true;
            } else {
                const std::size_t rp = (std::size_t)yUp * w + x;
                // darktable tests the column here where the other upward tests check the row.
                if ((x > border + 1) && (d[rp] == 0)) {
                    markBorder(d, rp, x, yUp, id, &minX, &maxX, &minY, &maxY);
                }
            }

            if ((yDown < h - border) && (d[(std::size_t)yDown * w + x] == 1)) {
                stack->push(x, yDown);
                firstXDown = lastXDown = true;
            } else {
                const std::size_t rp = (std::size_t)yDown * w + x;
                if ((yDown < h - border - 2) && (d[rp] == 0)) {
                    markBorder(d, rp, x, yDown, id, &minX, &maxX, &minY, &maxY);
                }
            }

            int xr = x + 1;
            while ((xr < w - border) && (d[(std::size_t)y * w + xr] == 1)) {
                d[(std::size_t)y * w + xr] = id;
                ++cnt;
                if ((yUp >= border) && (d[(std::size_t)yUp * w + xr] == 1)) {
                    if (!lastXUp) {
                        stack->push(xr, yUp);
                        lastXUp = true;
                    }
                } else {
                    const std::size_t rp = (std::size_t)yUp * w + xr;
                    if ((yUp > border + 1) && (d[rp] == 0)) {
                        markBorder(d, rp, xr, yUp, id, &minX, &maxX, &minY, &maxY);
                    }
                    lastXUp = false;
                }

                if ((yDown < h - border) && (d[(std::size_t)yDown * w + xr] == 1)) {
                    if (!lastXDown) {
                        stack->push(xr, yDown);
                        lastXDown = true;
                    }
                } else {
                    const std::size_t rp = (std::size_t)yDown * w + xr;
                    if ((yDown < h - border - 2) && (d[rp] == 0)) {
                        markBorder(d, rp, xr, yDown, id, &minX, &maxX, &minY, &maxY);
                    }
                    lastXDown = false;
                }
                ++xr;
            }

            {
                const std::size_t rp = (std::size_t)y * w + xr;
                if ((xr < w - border - 2) && (d[rp] == 0)) {
                    markBorder(d, rp, xr, y, id, &minX, &maxX, &minY, &maxY);
                }
            }

            int xl = x - 1;
            lastXUp = firstXUp;
            lastXDown = firstXDown;
            while ((xl >= border) && (d[(std::size_t)y * w + xl] == 1)) {
                d[(std::size_t)y * w + xl] = id;
                ++cnt;
                if ((yUp >= border) && (d[(std::size_t)yUp * w + xl] == 1)) {
                    if (!lastXUp) {
                        stack->push(xl, yUp);
                        lastXUp = true;
                    }
                } else {
                    const std::size_t rp = (std::size_t)yUp * w + xl;
                    if ((yUp > border + 1) && (d[rp] == 0)) {
                        markBorder(d, rp, xl, yUp, id, &minX, &maxX, &minY, &maxY);
                    }
                    lastXUp = false;
                }

                if ((yDown < h - border) && (d[(std::size_t)yDown * w + xl] == 1)) {
                    if (!lastXDown) {
                        stack->push(xl, yDown);
                        lastXDown = true;
                    }
                } else {
                    const std::size_t rp = (std::size_t)yDown * w + xl;
                    if ((yDown < h - border - 2) && (d[rp] == 0)) {
                        markBorder(d, rp, xl, yDown, id, &minX, &maxX, &minY, &maxY);
                    }
                    lastXDown = false;
                }
                --xl;
            }

            d[(std::size_t)y * w + x] = id;

            {
                const std::size_t rp = (std::size_t)y * w + xl;
                if ((xl > border + 1) && (d[rp] == 0)) {
                    markBorder(d, rp, xl, y, id, &minX, &maxX, &minY, &maxY);
                }
            }
        }

        // Segments of fewer than four locations are dropped, with their border marks, so that
        // noise does not oversegmentize the plane.
        const bool success = cnt > 3;
        if (!success) {
            for (int row = minY; row <= maxY; ++row) {
                for (int col = minX; col <= maxX; ++col) {
                    const std::size_t loc = (std::size_t)w * row + col;
                    if (d[loc] == id) {
                        d[loc] = 1;
                    } else if (d[loc] == (id | kSegmentIdMask)) {
                        d[loc] = 0;
                    }
                }
            }
        } else {
            seg->size[id] = cnt;
            seg->xmin[id] = minX;
            seg->xmax[id] = maxX;
            seg->ymin[id] = minY;
            seg->ymax[id] = maxY;
            seg->nr += 1;
            clearSegmentSlot(seg, id + 1);
        }

        return success;
    } // floodfillSegmentize
} // namespace

void
Segmentation::init(int w,
                   int h,
                   int b,
                   int islots)
{
    slots = std::max(256, std::min(islots, (int)kSegmentIdMask - 2));
    data.assign((std::size_t)w * (std::size_t)h, 0);
    size.assign(slots, 0);
    xmin.assign(slots, 0);
    xmax.assign(slots, 0);
    ymin.assign(slots, 0);
    ymax.assign(slots, 0);
    val1.assign(slots, 0.f);
    val2.assign(slots, 0.f);
    nr = 2;
    border = b;
    width = w;
    height = h;
}

void
segmentizePlane(Segmentation* seg)
{
    const int width = seg->width;
    const int height = seg->height;
    FloodStack stack;
    stack.size = (int)((std::size_t)width * height / 32);
    stack.el.resize(std::max(stack.size, 1));
    const int border = seg->border;
    std::uint32_t id = 2;

    for (int row = border; row < height - border; ++row) {
        for (int col = border; col < width - border; ++col) {
            if (id >= (std::uint32_t)(seg->slots - 2)) {
                return;
            }
            if (seg->data[(std::size_t)width * row + col] == 1) {
                if (floodfillSegmentize(row, col, seg, width, height, id, &stack)) {
                    ++id;
                }
            }
        }
    }
}

void
combineSegments(Segmentation* seg,
                int radius,
                std::uint32_t* tmp,
                const ParallelFor& parallel)
{
    std::uint32_t* img = seg->data.data();
    const int width = seg->width;
    const int height = seg->height;
    const int border = seg->border;

    intimageBorderfill(img, width, height, 0, border);
    dilating(img, tmp, width, height, border, radius, parallel);
    if (radius > 3) {
        intimageBorderfill(tmp, width, height, 1, border);
        eroding(tmp, img, width, height, border, radius - 3, parallel);
    } else {
        std::memcpy(img, tmp, (std::size_t)width * height * sizeof(std::uint32_t));
    }
    intimageBorderfill(img, width, height, 0, border);
}
} // namespace RawKernels

NATRON_NAMESPACE_EXIT
