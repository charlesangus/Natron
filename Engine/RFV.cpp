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

#include "RFV.h"

#include <map>
#include <string>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"

NATRON_NAMESPACE_ENTER

namespace RFV {

bool
isVariableKnobType(const KnobIPtr& knob)
{
    if (!knob) {
        return false;
    }
    KnobI* k = knob.get();

    return !dynamic_cast<KnobGroup*>(k) && !dynamic_cast<KnobPage*>(k) && !dynamic_cast<KnobButton*>(k) && !dynamic_cast<KnobSeparator*>(k);
}

bool
isValidVariableName(const std::string& name)
{
    if (name.empty() || NATRON_PYTHON_NAMESPACE::isKeyword(name)) {
        return false;
    }
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        const bool letter = ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || (c == '_');
        const bool digit = (c >= '0') && (c <= '9');
        if (!letter && (!digit || (i == 0))) {
            return false;
        }
    }

    return true;
}

std::vector<VariableDeclaration>
getDeclaredVariables(const KnobHolder* holder,
                     std::vector<std::string>* errors)
{
    std::vector<VariableDeclaration> declarations;
    if (!holder) {
        return declarations;
    }

    const std::vector<KnobIPtr> knobs = holder->getKnobs_mt_safe();
    std::map<std::string, int> nameCounts;
    for (std::size_t i = 0; i < knobs.size(); ++i) {
        ++nameCounts[knobs[i]->getName()];
    }

    for (std::size_t i = 0; i < knobs.size(); ++i) {
        KnobGroup* group = dynamic_cast<KnobGroup*>(knobs[i].get());
        if (!group || !group->isRFVGroup()) {
            continue;
        }
        const std::vector<KnobIPtr> children = group->getChildren();
        for (std::size_t c = 0; c < children.size(); ++c) {
            const KnobIPtr& child = children[c];
            const std::string& name = child->getName();
            std::string error;
            if (!isVariableKnobType(child)) {
                error = name + ": a " + child->typeName() + " parameter cannot be a variable";
            } else if (!isValidVariableName(name)) {
                error = name + ": a variable name must be a valid Python identifier";
            } else if (nameCounts[name] > 1) {
                error = name + ": another parameter has the same name";
            }
            if (!error.empty()) {
                if (errors) {
                    errors->push_back(error);
                }
                continue;
            }
            VariableDeclaration declaration;
            declaration.name = name;
            declaration.knob = child;
            declarations.push_back(declaration);
        }
    }

    return declarations;
} // getDeclaredVariables

} // namespace RFV

NATRON_NAMESPACE_EXIT
