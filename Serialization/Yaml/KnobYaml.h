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

#ifndef Serialization_Yaml_KnobYaml_h
#define Serialization_Yaml_KnobYaml_h

#include "Global/Macros.h"

#include <list>
#include <string>
#include <string_view>
#include <vector>

#include "Serialization/KnobSerialization.h"
#include "Serialization/Yaml/YamlScalars.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief Knobs as a YAML map keyed by script name, in the order given, which should be the
 * knobs toSerialization() asked to save: the encoder writes every knob it is given.
 *
 * A knob is written in the shortest form that holds it:
 * - its value, a scalar for one saved dimension and a flow sequence for several, each
 *   dimension being a value, `~` for none, or a map of `curve` (the compact curve form),
 *   `expr` and `hasRet`, `link` ("<fully.qualified.Node>.knob[.dim]", plus `track` for a track
 *   knob), `value` and `enabled: false`;
 * - a choice knob's option ID in place of its index when the index points at that ID;
 * - a string knob's animation as `{curve: [time, "string", ...]}`;
 * - a map with `value` and the knob-level keys (`secret`, `alias`, `option`, `strings`,
 *   `curves` for a parametric knob's curves, and `user` for a user knob's definition) when it
 *   needs any of them.
 *
 * Strings are always double-quoted, so the decoder tells a string from a number without the
 * knob's type. What a plug-in defines is not written: the type and dimension of a non-user
 * knob, its defaults, label and help, and the entries of its choice menu. A value under a curve,
 * an expression or a link is not written either, since it is recomputed.
 **/
std::string encodeKnobsYaml(const std::list<KnobSerialization>& knobs);

/**
 * @brief Reads knobs written by encodeKnobsYaml. Unknown keys are skipped and listed in
 * warnings; malformed values throw YamlError.
 *
 * A non-user knob comes back without its type (typeName empty, dimension 0) and with each
 * value typed from its YAML form (bool, int, double or string): completeKnobSerialization() must
 * be called with the live knob's type before the knob is restored. User knobs are complete.
 **/
std::list<KnobSerialization> decodeKnobsYaml(std::string_view text, YamlWarnings* warnings = nullptr);

/**
 * @brief Gives a decoded knob the type and dimension of the knob it is restored into, and
 * converts what the YAML form left undetermined: values to that type's storage, a choice
 * option ID to its index in choiceEntries (the live knob's entries, or the user knob's own;
 * 0 when absent), and the empty dimensions, parametric curves and string animation the type
 * always has. A type and dimension already set are kept, and calling it again changes nothing.
 **/
void completeKnobSerialization(KnobSerialization* knob, const std::string& typeName, int dimension, const std::vector<std::string>* choiceEntries = nullptr);

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Yaml_KnobYaml_h
