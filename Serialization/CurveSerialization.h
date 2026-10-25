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

#ifndef Serialization_CurveSerialization_h
#define Serialization_CurveSerialization_h

#include "Global/Macros.h"

#include <vector>

#include "Serialization/SerializationFwd.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief A tangent weight is its handle's extent in time as a fraction of the neighbouring
 * segment's span. These mirror Engine's Interpolation constants, which this directory cannot
 * include; Engine/Curve.cpp checks that they agree.
 **/
inline constexpr double kKeyFrameDefaultTangentWeight = 1. / 3.;
inline constexpr double kKeyFrameMinTangentWeight = 0.01;
inline constexpr double kKeyFrameMaxTangentWeight = 0.99;

/**
 * @brief Plain-data equivalent of a Curve keyframe. The interpolation is stored
 * as the underlying int value of Engine's KeyframeTypeEnum rather than that enum
 * itself, since Global/Enums.h (where it is defined) pulls in Qt and this
 * directory may not depend on Qt.
 **/
struct KeyFrameSerialization {
    double time;
    double value;
    int interpolation;
    double leftDerivative;
    double rightDerivative;
    double leftWeight;
    double rightWeight;

    KeyFrameSerialization()
        : time(0.)
        , value(0.)
        , interpolation(0)
        , leftDerivative(0.)
        , rightDerivative(0.)
        , leftWeight(kKeyFrameDefaultTangentWeight)
        , rightWeight(kKeyFrameDefaultTangentWeight)
    {
    }

    bool operator==(const KeyFrameSerialization&) const = default;
};

/**
 * @brief Plain-data equivalent of a Curve: the ordered set of keyframes it holds.
 **/
struct CurveSerialization {
    std::vector<KeyFrameSerialization> keyframes;

    bool operator==(const CurveSerialization&) const = default;
};

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_CurveSerialization_h
