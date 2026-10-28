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

#ifndef Engine_BezierCPPrivate_h
#define Engine_BezierCPPrivate_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <QMutex>
#include <QReadWriteLock>

#include <memory>
#include <string>

#include "Engine/Curve.h"
#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Where a control point linked to a track reads its centre from. Replaced as a whole,
 * never edited in place, so readers can hold a copy without the lock.
 **/
struct BezierCPTrackLink {
    TrackMarkerWPtr marker;
    // The names saved with the point, also used to resolve it on load.
    std::string trackerName, trackName;
    // The centre used while the track cannot be resolved.
    double frozenX, frozenY;
    // Whether the track was found unresolvable by the last updateTrackLinkState().
    bool lost;

    BezierCPTrackLink()
        : marker()
        , trackerName()
        , trackName()
        , frozenX(0)
        , frozenY(0)
        , lost(false)
    {
    }
};

typedef std::shared_ptr<const BezierCPTrackLink> BezierCPTrackLinkConstPtr;

/**
 * @brief The point and its tangents relative to the linked track's centre.
 **/
struct BezierCPTrackOffsets {
    double x, y, leftX, leftY, rightX, rightY;

    BezierCPTrackOffsets()
        : x(0)
        , y(0)
        , leftX(0)
        , leftY(0)
        , rightX(0)
        , rightY(0)
    {
    }
};

struct BezierCPPrivate
{
    BezierWPtr holder;

    ///the animation curves for the position in the 2D plane
    CurvePtr curveX, curveY;
    CurvePtr guiCurveX, guiCurveY;
    double x, y; //< used when there is no keyframe
    double guiX, guiY;
    bool broken; //< true if both tangents are independent

    ///the animation curves for the derivatives
    ///They do not need to be protected as Curve is a thread-safe class.
    CurvePtr curveLeftBezierX, curveRightBezierX, curveLeftBezierY, curveRightBezierY;
    CurvePtr guiCurveLeftBezierX, guiCurveRightBezierX, guiCurveLeftBezierY, guiCurveRightBezierY;
    mutable QMutex staticPositionMutex; //< protects the  leftX,rightX,leftY,rightY
    double leftX, rightX, leftY, rightY; //< used when there is no keyframe
    double guiLeftX, guiRightX, guiLeftY, guiRightY; //< used when there is no keyframe

    // Null when the point follows no track. Protected by staticPositionMutex, like the offsets.
    BezierCPTrackLinkConstPtr trackLink;
    BezierCPTrackOffsets trackOffsets, guiTrackOffsets;

    BezierCPPrivate(const BezierPtr& curve)
        : holder(curve)
        , curveX()
        , curveY()
        , guiCurveX()
        , guiCurveY()
        , x(0)
        , y(0)
        , guiX(0)
        , guiY(0)
        , broken(false)
        , curveLeftBezierX()
        , curveRightBezierX()
        , curveLeftBezierY()
        , curveRightBezierY()
        , guiCurveLeftBezierX()
        , guiCurveRightBezierX()
        , guiCurveLeftBezierY()
        , guiCurveRightBezierY()
        , staticPositionMutex()
        , leftX(0)
        , rightX(0)
        , leftY(0)
        , rightY(0)
        , guiLeftX(0)
        , guiRightX(0)
        , guiLeftY(0)
        , guiRightY(0)
        , trackLink()
        , trackOffsets()
        , guiTrackOffsets()
    {
        curveX= std::make_shared<Curve>();
        curveY= std::make_shared<Curve>();
        guiCurveX= std::make_shared<Curve>();
        guiCurveY= std::make_shared<Curve>();
        curveLeftBezierX= std::make_shared<Curve>();
        curveRightBezierX= std::make_shared<Curve>();
        curveLeftBezierY= std::make_shared<Curve>();
        curveRightBezierY= std::make_shared<Curve>();
        guiCurveLeftBezierX= std::make_shared<Curve>();
        guiCurveRightBezierX= std::make_shared<Curve>();
        guiCurveLeftBezierY= std::make_shared<Curve>();
        guiCurveRightBezierY= std::make_shared<Curve>();
    }
};

NATRON_NAMESPACE_EXIT

#endif // Engine_BezierCPPrivate_h_
