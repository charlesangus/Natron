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

#include "LensAberration.h"

#include <cmath>

NATRON_NAMESPACE_ENTER

namespace {
double
finiteOr(double value,
         double fallback)
{
    return std::isfinite(value) ? value : fallback;
}
} // anonymous namespace

LensAberration::LensAberration(const LensAberrationParams& params,
                               const RectI& format,
                               double par,
                               double scaleX,
                               double scaleY)
{
    _par = (std::isfinite(par) && (par > 0.)) ? par : 1.;
    _scaleX = (std::isfinite(scaleX) && (scaleX > 0.)) ? scaleX : 1.;
    _scaleY = (std::isfinite(scaleY) && (scaleY > 0.)) ? scaleY : 1.;
    _centreX = ((double)format.x1 + (double)format.x2) / 2.;
    _centreY = ((double)format.y1 + (double)format.y2) / 2.;
    const double halfDiagonal = std::hypot(std::max(0, format.x2 - format.x1) * _par / 2., std::max(0, format.y2 - format.y1) / 2.);
    _halfDiagonal = (halfDiagonal > 0.) ? halfDiagonal : 1.;
    _amount = std::max(0., finiteOr(params.catEye, 0.));
    _catEye = _amount > 0.;
    _orientation = params.catEyeOrientation;

    for (int c = 0; c < 3; ++c) {
        _scales[c] = params.channelScale(c);
    }
    _nGroups = 0;
    for (int c = 0; c < 3; ++c) {
        int g = 0;
        while ((g < _nGroups) && (_groupScales[g] != _scales[c])) {
            ++g;
        }
        if (g == _nGroups) {
            _groupScales[_nGroups++] = _scales[c];
        }
        _groupOfChannel[c] = g;
    }
}

double
LensAberration::channelScale(int c) const
{
    return _scales[((c >= 0) && (c < 3)) ? c : 1];
}

double
LensAberration::maxScale() const
{
    return std::max(_scales[0], std::max(_scales[1], _scales[2]));
}

int
LensAberration::groupOf(int c) const
{
    return _groupOfChannel[((c >= 0) && (c < 3)) ? c : 1];
}

double
LensAberration::groupScale(int group) const
{
    return _groupScales[std::clamp(group, 0, _nGroups - 1)];
}

int
LensAberration::columnStart(int i) const
{
    return (int)std::ceil((_centreX + (i - 0.5) * kCellSize) * _scaleX - 0.5);
}

int
LensAberration::rowStart(int j) const
{
    return (int)std::ceil((_centreY + (j - 0.5) * kCellSize) * _scaleY - 0.5);
}

int
LensAberration::cellColumn(int x) const
{
    // The estimate, then exact against columnStart(), which defines the cells.
    int i = (int)std::floor(((x + 0.5) / _scaleX - _centreX) / kCellSize + 0.5);
    while (columnStart(i + 1) <= x) {
        ++i;
    }
    while (columnStart(i) > x) {
        --i;
    }

    return i;
}

int
LensAberration::cellRow(int y) const
{
    int j = (int)std::floor(((y + 0.5) / _scaleY - _centreY) / kCellSize + 0.5);
    while (rowStart(j + 1) <= y) {
        ++j;
    }
    while (rowStart(j) > y) {
        --j;
    }

    return j;
}

LensCells
LensAberration::cells(const RectI& window) const
{
    LensCells cells;
    if (!_catEye || (window.x1 >= window.x2) || (window.y1 >= window.y2)) {
        return cells;
    }
    cells.i1 = cellColumn(window.x1);
    cells.j1 = cellRow(window.y1);
    cells.columns = cellColumn(window.x2 - 1) - cells.i1 + 1;
    cells.rows = cellRow(window.y2 - 1) - cells.j1 + 1;

    return cells;
}

std::vector<LensBand>
LensAberration::bands(const RectI& window,
                      int minRows) const
{
    std::vector<LensBand> list;
    if ((window.x1 >= window.x2) || (window.y1 >= window.y2)) {
        return list;
    }
    if (!_catEye) {
        LensBand band;
        band.y1 = window.y1;
        band.y2 = window.y2;
        list.push_back(band);

        return list;
    }
    const LensCells all = cells(window);
    int j = all.j1;
    while (j < all.j1 + all.rows) {
        LensBand band;
        band.j1 = j;
        band.y1 = std::max(window.y1, rowStart(j));
        do {
            ++j;
        } while ((j < all.j1 + all.rows) && (std::min(window.y2, rowStart(j)) - band.y1 < minRows));
        band.j2 = j;
        band.y2 = std::min(window.y2, rowStart(j));
        list.push_back(band);
    }

    return list;
}

void
LensAberration::cellPosition(int i,
                             int j,
                             double* x,
                             double* y) const
{
    // The centre is the format's centre plus whole cells: in canonical units, i cells of
    // kCellSize full-resolution pixels of aspect par.
    double px = i * kCellSize * _par / _halfDiagonal;
    double py = j * kCellSize / _halfDiagonal;
    const double length = std::hypot(px, py);
    if (length > 1.) {
        px /= length;
        py /= length;
    }
    *x = px;
    *y = py;
}

BokehClip
LensAberration::clip(int i,
                     int j) const
{
    BokehClip clip;
    if (!_catEye || ((i == 0) && (j == 0))) {
        return clip;
    }
    double px = 0.;
    double py = 0.;
    cellPosition(i, j, &px, &py);
    if (_orientation == CatEyeOrientation::Radial) {
        clip.offsetX = -_amount * px;
        clip.offsetY = -_amount * py;
    } else {
        clip.offsetX = _amount * py;
        clip.offsetY = -_amount * px;
    }
    clip.radius = 1.;

    return clip;
}

BokehRunCache::Runs
BokehRunCache::runs(const BokehShapeParams& shape,
                    double radiusX,
                    double radiusY)
{
    BokehShapeParams base = shape;
    base.clip = BokehClip();
    const Key key(radiusX, radiusY, shape.clip.offsetX, shape.clip.offsetY, shape.clip.radius);
    {
        const std::lock_guard<std::mutex> lock(_mutex);
        if (base != _shape) {
            _runs.clear();
            _bytes = 0;
            _shape = base;
        }
        const std::map<Key, Runs>::const_iterator found = _runs.find(key);
        if (found != _runs.end()) {
            return found->second;
        }
    }
    const Runs built = std::make_shared<const std::vector<BokehRun>>(BokehKernel::gatherKernel(shape, radiusX, radiusY).runs());
    const std::size_t bytes = built->size() * sizeof(BokehRun) + sizeof(Key) + sizeof(Runs);

    const std::lock_guard<std::mutex> lock(_mutex);
    if ((base != _shape) || (bytes > _budget)) {
        return built;
    }
    if (_bytes + bytes > _budget) {
        _runs.clear();
        _bytes = 0;
    }
    if (_runs.emplace(key, built).second) {
        _bytes += bytes;
    }

    return built;
}

NATRON_NAMESPACE_EXIT
