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

#ifndef NATRON_ENGINE_GLOBALMODESTROKE_H
#define NATRON_ENGINE_GLOBALMODESTROKE_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <vector>

#include "Engine/GlobalModeKind.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Stroke rates at gear 1 (no modifier held), gathered here so tuning them is not a hunt
 * through widget code. Kinds not listed move by the knob's own increment per pixel.
 **/
struct GlobalModeValueRates {
    double translateUnitsPerPixel = 1.0;
    double rotateDegreesPerPixel = 0.25;
    double scaleFractionPerPixel = 0.005;
};

/**
 * @brief Every dimension moves together, rather than one locked dimension.
 **/
const int kGlobalModeLockAll = -1;

/**
 * @brief What a stroke edits: the values it starts from, one per dimension (skewX then skewY for
 * a skew stroke, which spans both knobs), each dimension's increment, whether the knob holds
 * integers, and the locked dimension or kGlobalModeLockAll. A lock at or beyond the dimension
 * count is ignored and every dimension moves.
 **/
struct GlobalModeValueStrokeSpec {
    GlobalModeKindEnum kind = eGlobalModeKindGeneric;
    std::vector<double> increments;
    bool isInt = false;
    int lockDimension = kGlobalModeLockAll;
};

/**
 * @brief One click-drag stroke on a numeric knob, as ColorGlobalModeStroke is for colour: the
 * accumulated pointer travel is applied to the values the stroke started from rather than step
 * by step, so many small moves land where one big move would and a drag back returns to the
 * start; an integer knob therefore steps by whole units once enough travel has built up. Each
 * move's pixels are scaled by the gear held during that move (gear() in ColorWheelMath.h).
 *
 * dx and dy are screen pixels, y pointing down. Translate, skew and genericSpatial2D map x to
 * the first dimension and up to the second, and any later dimension to right-or-up travel. The
 * other kinds move with right-or-up travel on either axis: rotate turns counter-clockwise,
 * scale grows by scaleFractionPerPixel compounded per pixel, and size and generic add their
 * increment per pixel. Colour and ineligible kinds leave the start untouched.
 **/
class GlobalModeValueStroke {
public:
    GlobalModeValueStroke();

    void begin(const std::vector<double>& start);
    void addMove(double dx, double dy, double gearFactor);

    std::vector<double> evaluate(const GlobalModeValueStrokeSpec& spec, const GlobalModeValueRates& rates) const;

    const std::vector<double>& getStart() const
    {
        return _start;
    }

    double getTotalDx() const
    {
        return _dx;
    }

    double getTotalDy() const
    {
        return _dy;
    }

private:
    std::vector<double> _start;
    double _dx;
    double _dy;
};

/**
 * @brief The transform-group knob a key switches to. Center drags as translate does.
 **/
enum GlobalModeGroupMemberEnum {
    eGlobalModeGroupMemberTranslate,
    eGlobalModeGroupMemberRotate,
    eGlobalModeGroupMemberScale,
    eGlobalModeGroupMemberSkew,
    eGlobalModeGroupMemberCenter
};

enum GlobalModeValueKeyActionEnum {
    eGlobalModeValueKeyNone,
    eGlobalModeValueKeyLockAll,
    eGlobalModeValueKeyLockDimension,
    eGlobalModeValueKeySwitchMember
};

struct GlobalModeValueKeyAction {
    GlobalModeValueKeyActionEnum action = eGlobalModeValueKeyNone;
    int dimension = kGlobalModeLockAll;
    GlobalModeGroupMemberEnum member = eGlobalModeGroupMemberTranslate;
};

/**
 * @brief Maps an unmodified key press (a Qt::Key) to its value-knob action. On a lone knob
 * Q/W/E/R lock dimensions 1 to 4 and A moves them all. In a transform group Q/W/E/R/T switch to
 * translate/rotate/scale/skew/center, 1/2 lock x/y and A moves both. dimensionCount is the
 * active knob's (2 for skew); a lock at or beyond it maps to none, as does any other key.
 **/
GlobalModeValueKeyAction classifyGlobalModeValueKey(int key, bool inGroup, int dimensionCount);

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_GLOBALMODESTROKE_H
