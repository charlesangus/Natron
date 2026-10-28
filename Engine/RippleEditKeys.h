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

#ifndef Engine_RippleEditKeys_h
#define Engine_RippleEditKeys_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

NATRON_NAMESPACE_ENTER

enum RippleEditModeEnum {
    eRippleEditModeAll = 0,
    eRippleEditModeFromStart,
    eRippleEditModeToEnd,
    eRippleEditModeRange
};

/**
 * @brief Which keyframes of a shape a ripple edit carries to, besides the time being edited.
 *
 * From start covers the keys at or before the edit time, to end the keys at or after it, and
 * range the keys in [rangeFirst, rangeLast] (bounds included, in either order).
 **/
struct RippleEditKeys {
    bool enabled = false;
    RippleEditModeEnum mode = eRippleEditModeAll;
    double rangeFirst = 0.;
    double rangeLast = 0.;

    bool includes(double keyTime,
                  double editTime) const
    {
        if (!enabled) {
            return false;
        }
        switch (mode) {
        case eRippleEditModeAll:

            return true;
        case eRippleEditModeFromStart:

            return keyTime <= editTime;
        case eRippleEditModeToEnd:

            return keyTime >= editTime;
        case eRippleEditModeRange: {
            double lo = rangeFirst < rangeLast ? rangeFirst : rangeLast;
            double hi = rangeFirst < rangeLast ? rangeLast : rangeFirst;

            return (keyTime >= lo) && (keyTime <= hi);
        }
        }

        return true;
    }
};

NATRON_NAMESPACE_EXIT

#endif // Engine_RippleEditKeys_h
