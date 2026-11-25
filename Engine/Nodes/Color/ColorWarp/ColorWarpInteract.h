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

#ifndef Engine_Nodes_Color_ColorWarp_ColorWarpInteract_h
#define Engine_Nodes_Color_ColorWarp_ColorWarpInteract_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Engine/KnobColorWarpPins.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The viewer side of ColorWarp: drawing the pins and the state of a pin drag.
 **/
class ColorWarpInteract {
public:
    // Half the side of a pin's swatch, in screen pixels.
    static constexpr double kPinHalfSize = 8.;

    /**
     * @brief The id of the picked pin whose swatch is under canonical (x, y), the last drawn first, or 0.
     * pixelScaleX and pixelScaleY are the canonical units per screen pixel.
     **/
    static int hitTest(const ColorWarpPinSet& pins,
                       double x,
                       double y,
                       double pixelScaleX,
                       double pixelScaleY) WARN_UNUSED_RETURN;

    /**
     * @brief Each picked pin as a swatch at its pick position, source on the left and its target at time
     * on the right, the selected pin outlined brighter; disabled pins are drawn faded.
     **/
    static void build(const ColorWarpPinSet& pins,
                      int selectedId,
                      double time,
                      double pixelScaleX,
                      double pixelScaleY,
                      OverlayDrawList* list);

    /**
     * @brief Moves the pin with id to (x, y) with a new source sampled at frame, keeping its
     * target - source shift in every key.
     **/
    static bool movePin(ColorWarpPinSet* pins,
                        int id,
                        const double source[3],
                        double x,
                        double y,
                        double frame);

    void beginDrag(int id,
                   const ColorWarpPinSet& before);
    void endDrag();

    bool isDragging() const
    {
        return _dragId != 0;
    }

    int dragId() const
    {
        return _dragId;
    }

    const ColorWarpPinSet& dragBefore() const
    {
        return _dragBefore;
    }

private:
    int _dragId = 0;
    ColorWarpPinSet _dragBefore;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Color_ColorWarp_ColorWarpInteract_h
