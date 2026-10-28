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

#ifndef Engine_Nodes_Filter_LensAberration_h
#define Engine_Nodes_Filter_LensAberration_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <vector>

#include "Engine/Nodes/Filter/BokehKernel.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

/// Which way the cat-eye's second aperture is offset: towards the frame's centre, or across.
enum class CatEyeOrientation {
    Radial,
    Tangential
};

struct LensAberrationParams {
    // How far the second aperture is offset at the format's corners, in kernel radii, and
    // proportionally nearer the centre; 0 leaves every kernel whole.
    double catEye = 0.;
    // Radial offsets narrow the bokeh across the radius, as optical vignetting does; tangential
    // ones along it, swirling the bokeh around the centre.
    CatEyeOrientation catEyeOrientation = CatEyeOrientation::Radial;
    // Channel c's circle of confusion is scaled by 1 + chromaticAberration * chromaticOffset[c],
    // red, green and blue, and by no less than 0.
    double chromaticAberration = 0.;
    double chromaticOffset[3] = { 1., 0., -1. };

    bool hasCatEye() const
    {
        return std::isfinite(catEye) && (catEye > 0.);
    }

    /// The circle of confusion scale of channel bit c, 0 red, 1 green, 2 blue; any other is the green's.
    double channelScale(int c) const
    {
        const int k = ((c >= 0) && (c < 3)) ? c : 1;
        const double scale = 1. + chromaticAberration * chromaticOffset[k];

        return std::isfinite(scale) ? std::max(0., scale) : 1.;
    }

    double maxScale() const
    {
        return std::max(channelScale(0), std::max(channelScale(1), channelScale(2)));
    }

    bool hasChromatic() const
    {
        return (channelScale(0) != 1.) || (channelScale(1) != 1.) || (channelScale(2) != 1.);
    }

    bool isActive() const
    {
        return hasCatEye() || hasChromatic();
    }

    bool operator==(const LensAberrationParams& o) const
    {
        return (catEye == o.catEye) && (catEyeOrientation == o.catEyeOrientation) && (chromaticAberration == o.chromaticAberration)
            && std::equal(chromaticOffset, chromaticOffset + 3, o.chromaticOffset);
    }

    bool operator!=(const LensAberrationParams& o) const
    {
        return !(*this == o);
    }
};

/**
 * @brief The cells a render's window is cut into, columns i1 .. i1 + columns - 1 and rows j1 ..
 * j1 + rows - 1 of the lens's grid: with the cat-eye, the cells it overlaps, otherwise the one
 * cell (0, 0) that covers everything.
 **/
struct LensCells {
    int i1 = 0;
    int j1 = 0;
    int columns = 1;
    int rows = 1;
};

/**
 * @brief Rows j1 .. j2 - 1 of a window's cells, whose kernels a defocus builds together, and the
 * window's pixel rows y1 .. y2 - 1 in them.
 **/
struct LensBand {
    int j1 = 0;
    int j2 = 1;
    int y1 = 0;
    int y2 = 0;

    int cellRows() const
    {
        return j2 - j1;
    }
};

/**
 * @brief The items [*first, *end) of nItems that line handles of nLines lines, nItems spread
 * evenly over them so that any run of lines, as one thread takes it, gets its share.
 **/
inline void
spreadItems(int line,
            int nLines,
            std::size_t nItems,
            std::size_t* first,
            std::size_t* end)
{
    const std::size_t lines = (std::size_t)std::max(1, nLines);

    *first = ((std::size_t)line * nItems + lines - 1) / lines;
    *end = ((std::size_t)(line + 1) * nItems + lines - 1) / lines;
}

/**
 * @brief The lens aberrations of the defocus filters at one render scale. Engine-free, so that the
 * CPU and the GPU share it.
 *
 * The cat-eye clips every kernel by a second aperture, a disc of the kernel's radius offset as a
 * function of the position in the format. The position is constant over the cells of a grid
 * anchored at the format's centre, kCellSize full-resolution pixels a side and scaled with the
 * render, so that a pixel's kernel depends neither on the render window nor on the scale. A
 * cell's position is its centre's offset from the format's centre in canonical coordinates over
 * the format's half-diagonal, 1 at the corners, its length clamped to 1 beyond them. The clip disc
 * is offset by catEye times the position, towards the centre (radial) or a quarter turn
 * counter-clockwise from that (tangential), in units of the kernel's radii, so that it squeezes
 * with an anamorphic kernel.
 *
 * Chromatic aberration scales each colour channel's circle of confusion; any other channel, the
 * alpha among them, takes the green's scale. Channels of one scale form a group, which shares its
 * kernels.
 **/
class LensAberration {
public:
    static constexpr double kCellSize = 32.;

    /**
     * @brief The aberrations of params over format, in full-resolution pixels of pixel aspect
     * ratio par, rendered at scale (scaleX, scaleY).
     **/
    LensAberration(const LensAberrationParams& params,
                   const RectI& format,
                   double par,
                   double scaleX,
                   double scaleY);

    bool hasCatEye() const
    {
        return _catEye;
    }

    bool hasChromatic() const
    {
        return _nGroups > 1;
    }

    /// The circle of confusion scale of channel bit c, 0 red, 1 green, 2 blue.
    double channelScale(int c) const WARN_UNUSED_RETURN;

    /// The largest channel scale.
    double maxScale() const WARN_UNUSED_RETURN;

    /// The number of groups, 1 to 3, and the group of channel bit c and its scale.
    int groupCount() const
    {
        return _nGroups;
    }

    int groupOf(int c) const WARN_UNUSED_RETURN;
    double groupScale(int group) const WARN_UNUSED_RETURN;

    /// The grid's column and row of the pixel at x, y at the render's scale.
    int cellColumn(int x) const WARN_UNUSED_RETURN;
    int cellRow(int y) const WARN_UNUSED_RETURN;

    /// The first pixel at the render's scale of grid column i, and of grid row j.
    int columnStart(int i) const WARN_UNUSED_RETURN;
    int rowStart(int j) const WARN_UNUSED_RETURN;

    /// The cells over window.
    LensCells cells(const RectI& window) const WARN_UNUSED_RETURN;

    /**
     * @brief window's cells cut into bands of whole cell rows, bottom to top, each at least
     * minRows pixel rows high where the window allows, so that a band's rows keep the threads
     * busy. Without the cat-eye, one band of the whole window.
     **/
    std::vector<LensBand> bands(const RectI& window, int minRows) const WARN_UNUSED_RETURN;

    /// The position of the centre of cell (i, j): x right and y up, 1 at the corners.
    void cellPosition(int i, int j, double* x, double* y) const;

    /// The second aperture of cell (i, j); no clip at the format's centre or without the cat-eye.
    BokehClip clip(int i, int j) const WARN_UNUSED_RETURN;

    /// The grid row of pixel row y: 0 without the cat-eye.
    int rowOf(int y) const
    {
        return _catEye ? cellRow(y) : 0;
    }

    /**
     * @brief Calls fn(xa, xb, i) for each piece [xa, xb) of [x1, x2) in one grid column i, left to
     * right; without the cat-eye, once for the whole range in column 0.
     **/
    template <typename F>
    void forEachSegment(int x1,
                        int x2,
                        F fn) const
    {
        if (x1 >= x2) {
            return;
        }
        if (!_catEye) {
            fn(x1, x2, 0);

            return;
        }
        int x = x1;
        for (int i = cellColumn(x1); x < x2; ++i) {
            const int end = std::min(x2, columnStart(i + 1));
            if (end > x) {
                fn(x, end, i);
                x = end;
            }
        }
    }

private:
    bool _catEye = false;
    double _amount = 0.;
    CatEyeOrientation _orientation = CatEyeOrientation::Radial;
    double _scales[3] = { 1., 1., 1. };
    int _groupOfChannel[3] = { 0, 0, 0 };
    double _groupScales[3] = { 1., 1., 1. };
    int _nGroups = 1;
    // The format's centre in full-resolution pixels, its canonical half-diagonal, and the scale.
    double _centreX = 0.;
    double _centreY = 0.;
    double _halfDiagonal = 1.;
    double _par = 1.;
    double _scaleX = 1.;
    double _scaleY = 1.;
};

/**
 * @brief The runs of BokehKernel::gatherKernel() for one shape by radii and clip, kept across
 * renders. Past budget bytes of runs it starts over, so that a frame needing more kernels than
 * that rebuilds them rather than holding them all.
 **/
class BokehRunCache {
public:
    using Runs = std::shared_ptr<const std::vector<BokehRun>>;

    static constexpr std::size_t kDefaultBudget = (std::size_t)64 << 20;

    explicit BokehRunCache(std::size_t budget = kDefaultBudget)
        : _budget(budget)
    {
    }

    /// The runs of shape's gatherKernel() at the radii, built on a miss outside the lock.
    Runs runs(const BokehShapeParams& shape, double radiusX, double radiusY) WARN_UNUSED_RETURN;

private:
    using Key = std::tuple<double, double, double, double, double>;

    std::mutex _mutex;
    std::size_t _budget;
    std::size_t _bytes = 0;
    // The shape with its clip at the default, which every entry shares.
    BokehShapeParams _shape;
    std::map<Key, Runs> _runs;
};

/**
 * @brief The kernels' runs of one band of a window's cells at a time, by group, band cell row and
 * column: what a defocus holds while it gathers the band, so that a frame never holds all its
 * cells' kernels at once.
 **/
struct LensBandRuns {
    LensCells cells;
    int groups = 1;
    std::vector<BokehRunCache::Runs> runs;

    std::size_t size(const LensBand& band) const
    {
        return (std::size_t)groups * (std::size_t)band.cellRows() * (std::size_t)cells.columns;
    }

    /// Room for the largest of bands.
    void reserve(const std::vector<LensBand>& bands)
    {
        std::size_t largest = 0;
        for (const LensBand& band : bands) {
            largest = std::max(largest, size(band));
        }
        runs.assign(largest, BokehRunCache::Runs());
    }

    /// The group and cell of band's kernel k, k < size(band).
    void item(const LensBand& band,
              std::size_t k,
              int* group,
              int* i,
              int* j) const
    {
        const std::size_t columns = (std::size_t)cells.columns;
        const std::size_t rest = k / columns;
        *i = cells.i1 + (int)(k % columns);
        *j = band.j1 + (int)(rest % (std::size_t)band.cellRows());
        *group = (int)(rest / (std::size_t)band.cellRows());
    }

    /// The runs of group's kernel in cell (i, j) of band, built.
    const std::vector<BokehRun>& at(const LensBand& band,
                                    int group,
                                    int i,
                                    int j) const
    {
        return *runs[((std::size_t)group * (std::size_t)band.cellRows() + (std::size_t)(j - band.j1)) * (std::size_t)cells.columns + (std::size_t)(i - cells.i1)];
    }
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_LensAberration_h
