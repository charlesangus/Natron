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

#ifndef Serialization_Yaml_CurveYaml_h
#define Serialization_Yaml_CurveYaml_h

#include "Global/Macros.h"

#include <string>
#include <string_view>

#include "Serialization/CurveSerialization.h"
#include "Serialization/Yaml/YamlScalars.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief The compact curve form: one flow sequence holding, for each keyframe, its interpolation
 * token when it differs from the previous keyframe's (always for the first), its time and value,
 * then its right derivative for a free or broken keyframe and its left derivative for a broken
 * one, e.g. [L, 0, 1, 10, 2, X, 20, 3, 0.5, -0.5].
 *
 * A free or broken keyframe whose tangent weights are not both the default takes the token FW or
 * XW instead, and adds its right then its left weight after the derivatives, e.g.
 * [FW, 0, 1, 0.5, 0.6, 0.2]. Weights outside [kKeyFrameMinTangentWeight,
 * kKeyFrameMaxTangentWeight], or NaN, are a decode error.
 *
 * Only free and broken keyframes keep derivatives: every other interpolation derives them from
 * the neighbouring keys, so a decoded curve has zero derivatives there and the loader recomputes
 * them. A free keyframe decodes with its left derivative equal to its right one. Times must
 * increase. Malformed input throws YamlError.
 **/
std::string encodeCurveYaml(const CurveSerialization& curve);
CurveSerialization decodeCurveYaml(std::string_view text);

/**
 * @brief The curve as the format keeps it: derivatives zeroed and weights reset to the default
 * where they are derived, and a free keyframe's left derivative set to its right one. Encoding then decoding gives exactly this.
 **/
CurveSerialization canonicalCurveForYaml(const CurveSerialization& curve);

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Yaml_CurveYaml_h
