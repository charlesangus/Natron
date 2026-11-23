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

#include "ColorWarpInteract.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "Engine/OverlayDrawList.h"

NATRON_NAMESPACE_ENTER

namespace {

// The swatches show working-space values; a 2.2 gamma is close enough to tell colours apart.
OverlayColor
swatchColor(const double rgb[3],
            float alpha)
{
    float c[3];

    for (int i = 0; i < 3; ++i) {
        const double v = std::isfinite(rgb[i]) ? (std::min)(1., (std::max)(0., rgb[i])) : 0.;
        c[i] = (float)std::pow(v, 1. / 2.2);
    }

    return OverlayColor::fromRGBA(c[0], c[1], c[2], alpha);
}

void
addRect(OverlayDrawList* list,
        double x0,
        double y0,
        double x1,
        double y1,
        const OverlayColor& color)
{
    const std::vector<OverlayDrawList::Point> points = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };

    list->addPolygon(points, color);
}

} // anonymous namespace

int
ColorWarpInteract::hitTest(const ColorWarpPinSet& pins,
                           double x,
                           double y,
                           double pixelScaleX,
                           double pixelScaleY)
{
    const double hx = (kPinHalfSize + 2.) * pixelScaleX;
    const double hy = (kPinHalfSize + 2.) * pixelScaleY;

    for (std::size_t i = pins.pins.size(); i > 0; --i) {
        const ColorWarpPinData& pin = pins.pins[i - 1];
        if ((std::abs(x - pin.position[0]) <= hx) && (std::abs(y - pin.position[1]) <= hy)) {
            return pin.id;
        }
    }

    return 0;
}

void
ColorWarpInteract::build(const ColorWarpPinSet& pins,
                         int selectedId,
                         double time,
                         double pixelScaleX,
                         double pixelScaleY,
                         OverlayDrawList* list)
{
    const double hx = kPinHalfSize * pixelScaleX;
    const double hy = kPinHalfSize * pixelScaleY;

    for (const ColorWarpPinData& pin : pins.pins) {
        const bool selected = (pin.id == selectedId);
        const float alpha = pin.enabled ? 1.f : 0.4f;
        const double x = pin.position[0];
        const double y = pin.position[1];
        double target[3];
        for (int c = 0; c < 3; ++c) {
            target[c] = pin.target[c].valueAt(time);
        }
        addRect(list, x - hx, y - hy, x, y + hy, swatchColor(pin.source, alpha));
        addRect(list, x, y - hy, x + hx, y + hy, swatchColor(target, alpha));
        const std::vector<OverlayDrawList::Point> outline = { { x - hx, y - hy }, { x + hx, y - hy }, { x + hx, y + hy }, { x - hx, y + hy } };
        const OverlayColor outlineColor = selected ? OverlayColor::fromRGBA(1.f, 1.f, 1.f, 1.f) : OverlayColor::fromNode(alpha);
        list->addPolyline(outline, true, outlineColor, selected ? 3.f : OverlayDrawList::kDefaultLineWidth);
        list->addText(x + hx, y + hy, pin.name, outlineColor, 4., 4.);
    }
}

bool
ColorWarpInteract::movePin(ColorWarpPinSet* pins,
                           int id,
                           const double source[3],
                           double x,
                           double y,
                           double frame)
{
    const int index = pins->indexOf(id);

    if (index < 0) {
        return false;
    }
    ColorWarpPinData& pin = pins->pins[index];
    for (int c = 0; c < 3; ++c) {
        pin.target[c].offset(source[c] - pin.source[c]);
        pin.source[c] = source[c];
    }
    pin.position[0] = x;
    pin.position[1] = y;
    pin.frame = frame;

    return true;
}

void
ColorWarpInteract::beginDrag(int id,
                             const ColorWarpPinSet& before)
{
    _dragId = id;
    _dragBefore = before;
}

void
ColorWarpInteract::endDrag()
{
    _dragId = 0;
    _dragBefore = ColorWarpPinSet();
}

NATRON_NAMESPACE_EXIT
