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

#include "GlobalModeKind.h"

#include <cctype>
#include <string>
#include <vector>

#include "Engine/Knob.h"

NATRON_NAMESPACE_ENTER

namespace {

std::vector<std::string>
splitWords(const std::string& text)
{
    std::vector<std::string> words;
    std::string current;
    unsigned char previous = 0;

    for (unsigned char c : text) {
        const bool alnum = std::isalnum(c) != 0;
        const bool hump = (previous != 0) && ((std::isupper(c) && std::islower(previous)) || ((std::isdigit(c) != 0) != (std::isdigit(previous) != 0)));
        const bool boundary = !alnum || hump;
        if (boundary && !current.empty()) {
            words.push_back(current);
            current.clear();
        }
        if (alnum) {
            current.push_back(static_cast<char>(std::tolower(c)));
        }
        previous = alnum ? c : 0;
    }
    if (!current.empty()) {
        words.push_back(current);
    }

    return words;
}

ValueRoleEnum
valueRoleFromWord(const std::string& word)
{
    if ((word == "translate") || (word == "position") || (word == "offset")) {
        return eValueRoleTranslate;
    }
    if ((word == "rotate") || (word == "rotation") || (word == "angle")) {
        return eValueRoleRotate;
    }
    if (word == "scale") {
        return eValueRoleScale;
    }
    if (word == "size") {
        return eValueRoleSize;
    }
    if (word == "skew") {
        return eValueRoleSkew;
    }

    return eValueRoleNone;
}

ValueRoleEnum
valueRoleFromText(const std::string& text)
{
    for (const std::string& word : splitWords(text)) {
        const ValueRoleEnum role = valueRoleFromWord(word);
        if (role != eValueRoleNone) {
            return role;
        }
    }

    return eValueRoleNone;
}

GlobalModeKindEnum
kindForRole(ValueRoleEnum role)
{
    switch (role) {
    case eValueRoleTranslate:
        return eGlobalModeKindTranslate;
    case eValueRoleRotate:
        return eGlobalModeKindRotate;
    case eValueRoleScale:
        return eGlobalModeKindScale;
    case eValueRoleSize:
        return eGlobalModeKindSize;
    case eValueRoleSkew:
        return eGlobalModeKindSkew;
    case eValueRoleNone:
        break;
    }

    return eGlobalModeKindIneligible;
}

} // namespace

bool
isGlobalModeDimensionWritable(const KnobIPtr& knob,
                              int dimension)
{
    return knob && knob->isEnabled(dimension) && knob->getExpression(dimension).empty() && !knob->isSlave(dimension);
}

bool
hasGlobalModeWritableDimension(const KnobIPtr& knob)
{
    for (int d = 0; knob && (d < knob->getDimension()); ++d) {
        if (isGlobalModeDimensionWritable(knob, d)) {
            return true;
        }
    }

    return false;
}

bool
isGlobalModeKnobEditable(const KnobIPtr& knob)
{
    if (dynamic_cast<const KnobColor*>(knob.get())) {
        return knob->isEnabled(0);
    }

    return hasGlobalModeWritableDimension(knob);
}

ValueRoleEnum
valueRoleFromName(const std::string& scriptName,
                  const std::string& label)
{
    const ValueRoleEnum fromName = valueRoleFromText(scriptName);

    return (fromName != eValueRoleNone) ? fromName : valueRoleFromText(label);
}

GlobalModeKindEnum
classifyGlobalModeKind(const KnobIPtr& knob)
{
    if (!knob) {
        return eGlobalModeKindIneligible;
    }
    if (dynamic_cast<const KnobColor*>(knob.get())) {
        return eGlobalModeKindColor;
    }

    const KnobDouble* asDouble = dynamic_cast<const KnobDouble*>(knob.get());
    const KnobInt* asInt = dynamic_cast<const KnobInt*>(knob.get());
    if (!asDouble && !asInt) {
        return eGlobalModeKindIneligible;
    }

    if (!hasGlobalModeWritableDimension(knob)) {
        return eGlobalModeKindIneligible;
    }

    const ValueRoleEnum explicitRole = asDouble ? asDouble->getValueRole() : asInt->getValueRole();
    if (explicitRole != eValueRoleNone) {
        return kindForRole(explicitRole);
    }

    const ValueRoleEnum namedRole = valueRoleFromName(knob->getName(), knob->getLabel());
    if (namedRole != eValueRoleNone) {
        return kindForRole(namedRole);
    }

    if (asDouble && asDouble->getIsSpatial() && (knob->getDimension() == 2)) {
        return eGlobalModeKindGenericSpatial2D;
    }

    return eGlobalModeKindGeneric;
}

NATRON_NAMESPACE_EXIT
