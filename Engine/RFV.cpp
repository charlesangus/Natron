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

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Project.h"

#include "Serialization/KnobSerialization.h"

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

bool
isAppAttributeName(const AppInstancePtr& app,
                   const std::string& name)
{
    bool found = false;
    PyObject* mainModule = NATRON_PYTHON_NAMESPACE::getMainModule();
    PyObject* appObject = (app && mainModule) ? PyObject_GetAttrString(mainModule, app->getAppIDString().c_str()) : 0;

    if (appObject) {
        found = PyObject_HasAttrString((PyObject*)Py_TYPE(appObject), name.c_str());
        Py_DECREF(appObject);
    } else {
        PyErr_Clear();
    }
    if (!found) {
        PyObject* module = PyImport_ImportModule(NATRON_ENGINE_PYTHON_MODULE_NAME);
        PyObject* appClass = module ? PyObject_GetAttrString(module, "App") : 0;
        if (appClass) {
            found = PyObject_HasAttrString(appClass, name.c_str());
            Py_DECREF(appClass);
        } else {
            PyErr_Clear();
        }
        Py_XDECREF(module);
    }

    return found;
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

bool
isNodeVariable(const KnobIPtr& knob)
{
    if (!knob || !dynamic_cast<EffectInstance*>(knob->getHolder())) {
        return false;
    }
    KnobGroup* parent = dynamic_cast<KnobGroup*>(knob->getParentKnob().get());

    return parent && parent->isRFVGroup();
}

bool
canPromoteToProject(const KnobIPtr& knob,
                    std::string* reason)
{
    std::string error;
    AppInstancePtr app = (knob && knob->getHolder()) ? knob->getHolder()->getApp() : AppInstancePtr();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();

    if (!isNodeVariable(knob) || !isVariableKnobType(knob) || !isValidVariableName(knob->getName()) || !project) {
        error = "Only a variable of a node's Variables group can be added to the project variables";
    } else {
        const std::string& name = knob->getName();
        KnobIPtr existing = project->getKnobByName(name);
        if (existing && (existing->getParentKnob() == project->getRFVGroup())) {
            error = "The project already declares a variable named " + name;
        } else if (existing) {
            error = "The project already has a parameter named " + name;
        } else if (isAppAttributeName(app, name)) {
            error = name + " is an attribute of the App class, so it cannot be a project variable";
        }
    }
    if (reason) {
        *reason = error;
    }

    return error.empty();
}

KnobIPtr
promoteToProject(const KnobIPtr& knob,
                 std::string* error)
{
    if (!canPromoteToProject(knob, error)) {
        return KnobIPtr();
    }
    AppInstancePtr app = knob->getHolder()->getApp();
    ProjectPtr project = app->getProject();
    KnobGroupPtr group = project->getRFVGroup();

    // A user knob's serialization carries its definition, values and animation; its expressions
    // and links are applied by the caller, so the copy has none.
    Serialization::KnobSerialization serialization;
    knob->toSerialization(&serialization);
    KnobIPtr copy = createUserKnobFromSerialization(project.get(), serialization);
    if (!copy || !group || !group->addKnob(copy)) {
        if (copy) {
            project->deleteKnob(copy.get(), false);
        }
        if (error) {
            *error = "The project variables group refused " + knob->getName();
        }

        return KnobIPtr();
    }
    project->recreateUserKnobs(true);
    app->refreshProjectParamsInPython();

    return copy;
}

} // namespace RFV

NATRON_NAMESPACE_EXIT
