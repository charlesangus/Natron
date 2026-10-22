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

#ifndef Serialization_Yaml_YamlScalars_h
#define Serialization_Yaml_YamlScalars_h

#include "Global/Macros.h"

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief Thrown when a YAML document cannot be encoded or does not decode into the data model.
 * The message names what was expected and, for decoding, where in the document.
 **/
class YamlError
    : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * @brief Thrown for a document written by a newer Natron, with a higher formatVersion than this
 * version reads. The message says so in words meant for the user.
 **/
class YamlNewerFormatError
    : public YamlError {
public:
    using YamlError::YamlError;
};

/**
 * @brief Non-fatal findings of a decode, such as keys the decoder does not know: they are
 * skipped and reported here so a newer file still loads.
 **/
typedef std::vector<std::string> YamlWarnings;

/**
 * @brief The shortest text that reads back as exactly the same double (std::to_chars), with
 * the YAML forms .inf, -.inf and .nan for the non-finite values.
 **/
std::string formatYamlDouble(double value);

/**
 * @brief The same for a float, shortest at float precision.
 **/
std::string formatYamlFloat(float value);

/**
 * @brief Parses the whole of text as a double (std::from_chars), accepting the YAML forms of
 * infinity and NaN and a leading '+'. Returns nothing when text is not a number.
 **/
std::optional<double> parseYamlDouble(std::string_view text);
std::optional<float> parseYamlFloat(std::string_view text);

/**
 * @brief Parses the whole of text as a decimal integer, nothing when it is not one or does not
 * fit.
 **/
std::optional<std::int64_t> parseYamlInteger(std::string_view text);

/**
 * @brief The token a curve gives a keyframe interpolation (the value of KeyframeTypeEnum):
 * K constant, L linear, S smooth, R Catmull-Rom, C cubic, H horizontal, F free, X broken and
 * N none. Encoding an interpolation outside that range throws YamlError.
 **/
std::string_view keyframeInterpolationToken(int interpolation);
std::optional<int> keyframeInterpolationFromToken(std::string_view token);

// The KeyframeTypeEnum values the curve grammar needs to know: only free and broken keys store
// derivatives, every other kind has its derivatives computed from the neighbouring keys.
inline constexpr int kYamlKeyframeConstant = 0;
inline constexpr int kYamlKeyframeLinear = 1;
inline constexpr int kYamlKeyframeFree = 6;
inline constexpr int kYamlKeyframeBroken = 7;
inline constexpr int kYamlKeyframeNone = 8;

inline bool
keyframeStoresDerivatives(int interpolation)
{
    return (interpolation == kYamlKeyframeFree) || (interpolation == kYamlKeyframeBroken);
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Yaml_YamlScalars_h
