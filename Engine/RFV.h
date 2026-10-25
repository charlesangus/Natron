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

#ifndef NATRON_ENGINE_RFV_H
#define NATRON_ENGINE_RFV_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Reverse-Flow Variables (RFVs): declaration. A variable is a direct child of a group knob
 * flagged with KnobGroup::setAsRFVGroup(); its script name is the variable name. Only the
 * ReverseFlowVariableSet node and the project carry such a group. These are pure functions over
 * the knobs; they hold no render state.
 **/
namespace RFV {

struct VariableDeclaration {
    std::string name;
    KnobIPtr knob;
};

/**
 * @brief Whether the knob holds a value and can therefore be a variable: groups, pages, buttons
 * and separators cannot.
 **/
bool isVariableKnobType(const KnobIPtr& knob);

/**
 * @brief Whether the name is a valid Python identifier: ASCII letters, digits and underscores,
 * not starting with a digit, and not a Python keyword.
 **/
bool isValidVariableName(const std::string& name);

/**
 * @brief The variables the holder declares, in the order of its RFV groups' children. A child
 * whose type cannot be a variable, whose name is not a valid identifier, or whose name another
 * knob of the holder also has, is left out and described in errors.
 **/
std::vector<VariableDeclaration> getDeclaredVariables(const KnobHolder* holder, std::vector<std::string>* errors = nullptr);

} // namespace RFV

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_RFV_H
