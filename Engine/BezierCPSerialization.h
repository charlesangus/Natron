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

#ifndef Engine_BezierCPSerialization_h
#define Engine_BezierCPSerialization_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/BezierCP.h"
#include "Engine/BezierCPPrivate.h"

#if !defined(Q_MOC_RUN) && !defined(SBK_RUN)
GCC_DIAG_UNUSED_LOCAL_TYPEDEFS_OFF
// clang-format off
GCC_DIAG_OFF(unused-parameter)
#include <boost/archive/basic_archive.hpp>
#include <boost/serialization/base_object.hpp>
#include <boost/serialization/export.hpp>
#include <boost/serialization/list.hpp>
#include <boost/serialization/shared_ptr.hpp>
#include <boost/serialization/split_member.hpp>
#include <boost/serialization/version.hpp>
#include <boost/serialization/map.hpp>
GCC_DIAG_UNUSED_LOCAL_TYPEDEFS_ON
GCC_DIAG_ON(unused-parameter)
// clang-format on
#endif

#include "Serialization/CurveSerialization.h"
#include "Serialization/BoostXml/CurveXml.h"

#include "Engine/EngineFwd.h"

#define BEZIER_CP_INTRODUCES_OFFSET 2
#define BEZIER_CP_FIX_BUG_CURVE_POINTER 3
#define BEZIER_CP_REMOVE_OFFSET 4
#define BEZIER_CP_ADD_BROKEN 5
#define BEZIER_CP_VERSION BEZIER_CP_ADD_BROKEN

NATRON_NAMESPACE_ENTER

template<class Archive>
void
BezierCP::serialize(Archive &ar,
                    const unsigned int file_version)
{
    boost::serialization::split_member(ar, *this, file_version);
}

template<class Archive>
void
BezierCP::save(Archive & ar,
               const unsigned int version) const
{
    Q_UNUSED(version);
    Serialization::CurveSerialization xCurveData, yCurveData, leftXCurveData, leftYCurveData, rightXCurveData, rightYCurveData;
    _imp->curveX->toSerialization(&xCurveData);
    _imp->curveY->toSerialization(&yCurveData);
    _imp->curveLeftBezierX->toSerialization(&leftXCurveData);
    _imp->curveLeftBezierY->toSerialization(&leftYCurveData);
    _imp->curveRightBezierX->toSerialization(&rightXCurveData);
    _imp->curveRightBezierY->toSerialization(&rightYCurveData);
    ar & ::boost::serialization::make_nvp("X", _imp->x);
    ar & ::boost::serialization::make_nvp("X_animation", xCurveData);
    ar & ::boost::serialization::make_nvp("Y", _imp->y);
    ar & ::boost::serialization::make_nvp("Y_animation", yCurveData);
    ar & ::boost::serialization::make_nvp("Left_X", _imp->leftX);
    ar & ::boost::serialization::make_nvp("Left_X_animation", leftXCurveData);
    ar & ::boost::serialization::make_nvp("Left_Y", _imp->leftY);
    ar & ::boost::serialization::make_nvp("Left_Y_animation", leftYCurveData);
    ar & ::boost::serialization::make_nvp("Right_X", _imp->rightX);
    ar & ::boost::serialization::make_nvp("Right_X_animation", rightXCurveData);
    ar & ::boost::serialization::make_nvp("Right_Y", _imp->rightY);
    ar & ::boost::serialization::make_nvp("Right_Y_animation", rightYCurveData);
    ar & ::boost::serialization::make_nvp("Broken", _imp->broken);
}

template<class Archive>
void
BezierCP::load(Archive & ar,
               const unsigned int version)
{
    bool createdDuringToRC2Or3 = appPTR->wasProjectCreatedDuringRC2Or3();

    if ( (version >= BEZIER_CP_FIX_BUG_CURVE_POINTER) || !createdDuringToRC2Or3 ) {
        ar & ::boost::serialization::make_nvp("X", _imp->x);
        Serialization::CurveSerialization xCurveData;
        ar & ::boost::serialization::make_nvp("X_animation", xCurveData);
        Curve xCurve;
        xCurve.fromSerialization(xCurveData);
        _imp->curveX->clone(xCurve);

        if (version < BEZIER_CP_FIX_BUG_CURVE_POINTER) {
            Serialization::CurveSerialization curveBugData;
            ar & ::boost::serialization::make_nvp("Y", curveBugData);
        } else {
            ar & ::boost::serialization::make_nvp("Y", _imp->y);
        }

        Serialization::CurveSerialization yCurveData;
        ar & ::boost::serialization::make_nvp("Y_animation", yCurveData);
        Curve yCurve;
        yCurve.fromSerialization(yCurveData);
        _imp->curveY->clone(yCurve);

        ar & ::boost::serialization::make_nvp("Left_X", _imp->leftX);
        Serialization::CurveSerialization leftXCurveData, leftYCurveData, rightXCurveData, rightYCurveData;
        ar & ::boost::serialization::make_nvp("Left_X_animation", leftXCurveData);
        ar & ::boost::serialization::make_nvp("Left_Y", _imp->leftY);
        ar & ::boost::serialization::make_nvp("Left_Y_animation", leftYCurveData);
        ar & ::boost::serialization::make_nvp("Right_X", _imp->rightX);
        ar & ::boost::serialization::make_nvp("Right_X_animation", rightXCurveData);
        ar & ::boost::serialization::make_nvp("Right_Y", _imp->rightY);
        ar & ::boost::serialization::make_nvp("Right_Y_animation", rightYCurveData);

        Curve leftCurveX, leftCurveY, rightCurveX, rightCurveY;
        leftCurveX.fromSerialization(leftXCurveData);
        leftCurveY.fromSerialization(leftYCurveData);
        rightCurveX.fromSerialization(rightXCurveData);
        rightCurveY.fromSerialization(rightYCurveData);

        _imp->curveLeftBezierX->clone(leftCurveX);
        _imp->curveLeftBezierY->clone(leftCurveY);
        _imp->curveRightBezierX->clone(rightCurveX);
        _imp->curveRightBezierY->clone(rightCurveY);

        if (version >= BEZIER_CP_ADD_BROKEN) {
            ar & ::boost::serialization::make_nvp("Broken", _imp->broken);
        } else {
            // For older projects, infer if the tangents are broken.
            // Fixes https://github.com/NatronGitHub/Natron/issues/202

            // The vector between the two bezier points
            double vx = _imp->rightX - _imp->leftX;
            double vy = _imp->rightY - _imp->leftY;
            double v = std::sqrt(vx * vx + vy *vy);
            // The normal vector
            double nx = vy / v;
            double ny = -vx / v;
            // The (signed) distance from the position to the segment
            double d = nx * (_imp->x - _imp->leftX) + ny * (_imp->y - _imp->leftY);
            _imp->broken = (std::abs(d) < 1e-4);
        }
    } else {
        // Projects from this era (RC2/RC3, before BEZIER_CP_FIX_BUG_CURVE_POINTER)
        // archived the animation curves through a tracked boost shared_ptr rather
        // than by value, and even stored one under the "Y" tag by mistake. Curve
        // itself no longer has a serialize of its own, so the pointee type read
        // here is the plain-data CurveSerialization instead; the wire shape boost
        // emits for a shared_ptr<T> does not depend on what T is, so this reads
        // these old archives identically to reading a shared_ptr<Curve>.
        ar & ::boost::serialization::make_nvp("X", _imp->x);
        std::shared_ptr<Serialization::CurveSerialization> xCurveData, yCurveData, leftXCurveData, leftYCurveData, rightXCurveData, rightYCurveData;
        ar & ::boost::serialization::make_nvp("X_animation", xCurveData);
        Curve xCurve;
        if (xCurveData) {
            xCurve.fromSerialization(*xCurveData);
        }
        _imp->curveX->clone(xCurve);

        std::shared_ptr<Serialization::CurveSerialization> curveBugData;
        ar & ::boost::serialization::make_nvp("Y", curveBugData);
        ar & ::boost::serialization::make_nvp("Y_animation", yCurveData);
        Curve yCurve;
        if (yCurveData) {
            yCurve.fromSerialization(*yCurveData);
        }
        _imp->curveY->clone(yCurve);

        ar & ::boost::serialization::make_nvp("Left_X", _imp->leftX);
        ar & ::boost::serialization::make_nvp("Left_X_animation", leftXCurveData);
        ar & ::boost::serialization::make_nvp("Left_Y", _imp->leftY);
        ar & ::boost::serialization::make_nvp("Left_Y_animation", leftYCurveData);
        ar & ::boost::serialization::make_nvp("Right_X", _imp->rightX);
        ar & ::boost::serialization::make_nvp("Right_X_animation", rightXCurveData);
        ar & ::boost::serialization::make_nvp("Right_Y", _imp->rightY);
        ar & ::boost::serialization::make_nvp("Right_Y_animation", rightYCurveData);

        Curve leftCurveX, leftCurveY, rightCurveX, rightCurveY;
        if (leftXCurveData) {
            leftCurveX.fromSerialization(*leftXCurveData);
        }
        if (leftYCurveData) {
            leftCurveY.fromSerialization(*leftYCurveData);
        }
        if (rightXCurveData) {
            rightCurveX.fromSerialization(*rightXCurveData);
        }
        if (rightYCurveData) {
            rightCurveY.fromSerialization(*rightYCurveData);
        }

        _imp->curveLeftBezierX->clone(leftCurveX);
        _imp->curveLeftBezierY->clone(leftCurveY);
        _imp->curveRightBezierX->clone(rightCurveX);
        _imp->curveRightBezierY->clone(rightCurveY);
    }
    if ( (version >= BEZIER_CP_INTRODUCES_OFFSET) && (version < BEZIER_CP_REMOVE_OFFSET) ) {
        int offsetTime;
        ar & ::boost::serialization::make_nvp("OffsetTime", offsetTime);
    }
} // BezierCP::load

NATRON_NAMESPACE_EXIT

BOOST_CLASS_VERSION(NATRON_NAMESPACE::BezierCP, BEZIER_CP_VERSION)


#endif // Engine_BezierCPSerialization_h
