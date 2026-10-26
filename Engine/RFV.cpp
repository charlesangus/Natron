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

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/RFV/ReverseFlowVariableSet.h"
#include "Engine/Project.h"
#include "Engine/RenderContext.h"
#include "Engine/UndoKnobRef.h"

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
isPluginControl(const KnobIPtr& knob)
{
    return knob && knob->isDeclaredByPlugin() && !knob->isUserKnob();
}

bool
isAdmittedInRFVGroup(const KnobGroup& group,
                     const KnobIPtr& knob)
{
    if (isPluginControl(knob)) {
        return group.isDeclaredByPlugin() && (group.getHolder() == knob->getHolder());
    }

    return isVariableKnobType(knob);
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
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    // A top-level node is app.<script name>, whether or not Python has been told of it yet.
    if (project && project->getNodeByName(name)) {
        return true;
    }
    // A project parameter declared as app.<name> holds that name itself; the caller decides about
    // those. Any other attribute of that name was set by a script.
    KnobIPtr projectKnob = project ? project->getKnobByName(name) : KnobIPtr();
    const bool isProjectParam = projectKnob && isDeclaredOnApp(*projectKnob);

    bool found = false;
    PythonGILLocker pgl;
    PyObject* mainModule = NATRON_PYTHON_NAMESPACE::getMainModule();
    PyObject* appObject = (app && mainModule) ? PyObject_GetAttrString(mainModule, app->getAppIDString().c_str()) : 0;

    if (appObject) {
        found = PyObject_HasAttrString((PyObject*)Py_TYPE(appObject), name.c_str());
        if (!found && !isProjectParam) {
            found = PyObject_HasAttrString(appObject, name.c_str());
        }
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

bool
isDeclaredOnApp(const KnobI& knob)
{
    if (knob.isDeclaredByPlugin()) {
        return true;
    }
    KnobGroup* parent = dynamic_cast<KnobGroup*>(knob.getParentKnob().get());

    return parent && parent->isRFVGroup();
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
            if (isPluginControl(child)) {
                continue;
            }
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

std::vector<VariableDeclaration>
getExistingVariables(const KnobHolder* holder)
{
    std::vector<VariableDeclaration> existing;
    AppInstancePtr app = holder ? holder->getApp() : AppInstancePtr();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    if (!project) {
        return existing;
    }

    std::vector<VariableDeclaration> candidates = getDeclaredVariables(project.get());
    NodesList nodes;
    project->getNodes_recursive(nodes, true);
    for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        EffectInstancePtr effect = (*it)->getEffectInstance();
        if (!effect || (effect.get() == holder)) {
            continue;
        }
        const std::vector<VariableDeclaration> declared = getDeclaredVariables(effect.get());
        candidates.insert(candidates.end(), declared.begin(), declared.end());
    }

    std::set<std::string> seen;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const std::string& name = candidates[i].name;
        if (!seen.insert(name).second || holder->getKnobByName(name)) {
            continue;
        }
        existing.push_back(candidates[i]);
    }

    return existing;
} // getExistingVariables

namespace {

    template <typename T>
    bool
    setCurrentValueOf(const KnobIPtr& source,
                      const KnobIPtr& target)
    {
        Knob<T>* from = dynamic_cast<Knob<T>*>(source.get());
        Knob<T>* to = dynamic_cast<Knob<T>*>(target.get());
        if (!from || !to) {
            return false;
        }
        const double time = source->getCurrentTime();
        const int dimensions = std::min(from->getDimension(), to->getDimension());
        for (int i = 0; i < dimensions; ++i) {
            const T value = from->getValueAtTime(time, i);
            to->removeAnimation(ViewSpec::all(), i);
            to->setValue(value, ViewSpec::all(), i, true);
        }

        return true;
    }

    KnobGroupPtr
    rfvGroupOf(const KnobHolder* holder)
    {
        const std::vector<KnobIPtr> knobs = holder->getKnobs_mt_safe();
        for (std::size_t i = 0; i < knobs.size(); ++i) {
            KnobGroupPtr group = std::dynamic_pointer_cast<KnobGroup>(knobs[i]);
            if (group && group->isRFVGroup()) {
                return group;
            }
        }

        return KnobGroupPtr();
    }

} // namespace

KnobIPtr
addExistingVariable(KnobHolder* holder,
                    const std::string& name,
                    std::string* error)
{
    std::string why;
    KnobIPtr source;
    const std::vector<VariableDeclaration> existing = getExistingVariables(holder);
    for (std::size_t i = 0; i < existing.size(); ++i) {
        if (existing[i].name == name) {
            source = existing[i].knob;
            break;
        }
    }
    KnobGroupPtr group = holder ? rfvGroupOf(holder) : KnobGroupPtr();
    KnobIPtr knob;
    if (!source) {
        why = "No other variable named " + name + " can be added here";
    } else if (!group) {
        why = "There is no variables group to add " + name + " to";
    } else {
        // A user knob's serialization carries its definition and defaults; the value is set below.
        Serialization::KnobSerialization serialization;
        source->toSerialization(&serialization);
        knob = createUserKnobFromSerialization(holder, serialization);
        if (!knob) {
            why = "A " + source->typeName() + " parameter cannot be copied";
        } else if (!group->addKnob(knob)) {
            holder->deleteKnob(knob.get(), false);
            knob.reset();
            why = "The variables group refused " + name;
        }
    }
    if (error) {
        *error = why;
    }
    if (!knob) {
        return KnobIPtr();
    }

    knob->beginChanges();
    if (!setCurrentValueOf<double>(source, knob) && !setCurrentValueOf<int>(source, knob) && !setCurrentValueOf<bool>(source, knob)) {
        setCurrentValueOf<std::string>(source, knob);
    }
    knob->endChanges();
    holder->recreateUserKnobs(true);

    return knob;
} // addExistingVariable

bool
canAddAsVariable(const KnobHolder* holder,
                 const KnobIPtr& knob,
                 std::string* reason)
{
    std::string error;
    const Project* project = dynamic_cast<const Project*>(holder);
    const std::string owner = project ? std::string("the project") : std::string("this node");

    if (!knob) {
        error = "No parameter was given";
    } else if (!holder || (knob->getHolder() != holder)) {
        error = knob->getName() + " is not a parameter of " + owner;
    } else if (!rfvGroupOf(holder)) {
        error = "There is no variables group on " + owner;
    } else if (!knob->isUserKnob() || isPluginControl(knob)) {
        error = knob->getName() + " is not a user parameter: only a parameter made by a script or the parameter manager can be a variable";
    } else if (!isVariableKnobType(knob)) {
        error = knob->getName() + ": a " + knob->typeName() + " parameter holds no value, so it cannot be a variable";
    } else {
        const std::string& name = knob->getName();
        KnobGroup* parent = dynamic_cast<KnobGroup*>(knob->getParentKnob().get());
        if (parent && parent->isRFVGroup()) {
            error = name + " is already a variable";
        } else if (!isValidVariableName(name)) {
            error = name + ": a variable name must be a valid Python identifier";
        } else if (holder->getOtherKnobByName(name, knob.get())) {
            error = name + ": " + owner + " already has another parameter with that name";
        } else if (project && isAppAttributeName(project->getApp(), name)) {
            error = name + " is an attribute of the app object, so it cannot be a project variable";
        }
    }
    if (reason) {
        *reason = error;
    }

    return error.empty();
}

bool
addAsVariable(KnobHolder* holder,
              const KnobIPtr& knob,
              std::string* error)
{
    if (!canAddAsVariable(holder, knob, error)) {
        return false;
    }
    KnobGroupPtr group = rfvGroupOf(holder);
    KnobPagePtr formerPage = std::dynamic_pointer_cast<KnobPage>(knob->getParentKnob());
    if (!group->addKnob(knob)) {
        if (error) {
            *error = "The variables group refused " + knob->getName();
        }

        return false;
    }
    if (formerPage && formerPage->isUserKnob() && !formerPage->isDeclaredByPlugin() && formerPage->getChildren().empty()) {
        holder->deleteKnob(formerPage.get(), true);
    }
    holder->recreateUserKnobs(true);
    if (dynamic_cast<Project*>(holder)) {
        AppInstancePtr app = holder->getApp();
        if (app) {
            app->refreshProjectParamsInPython();
        }
    }

    return true;
}

bool
isNodeVariable(const KnobIPtr& knob)
{
    if (!knob || !dynamic_cast<EffectInstance*>(knob->getHolder())) {
        return false;
    }
    KnobGroup* parent = dynamic_cast<KnobGroup*>(knob->getParentKnob().get());

    return parent && parent->isRFVGroup() && !isPluginControl(knob);
}

void
refreshNodeVariableGroups(const Project& project)
{
    NodesList nodes;
    project.getNodes_recursive(nodes, false);
    for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        EffectInstancePtr effect = (*it)->getEffectInstance();
        ReverseFlowVariableSet* setNode = dynamic_cast<ReverseFlowVariableSet*>(effect.get());
        if (setNode && setNode->refreshVariableGroups()) {
            setNode->recreateUserKnobs(true);
        }
    }
}

namespace {

    void
    refreshHolderExpressionListeners(const KnobHolder& holder,
                                     const std::vector<VariableDeclaration>& declared)
    {
        const KnobsVec& knobs = holder.getKnobs();
        for (KnobsVec::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
            KnobHelper* helper = dynamic_cast<KnobHelper*>(it->get());
            if (!helper) {
                continue;
            }
            const RenderContextReadSet reads = helper->getRFVReadSet();
            if (reads.empty()) {
                continue;
            }
            bool readsDeclared = reads.readsAll();
            for (std::size_t i = 0; !readsDeclared && (i < declared.size()); ++i) {
                readsDeclared = reads.contains(RenderContextName::plain(declared[i].name));
            }
            if (readsDeclared) {
                helper->refreshRFVListeners();
            }
        }
    }

} // namespace

void
refreshExpressionListeners(const Project& project)
{
    const std::vector<VariableDeclaration> declared = getDeclaredVariables(&project);
    if (declared.empty()) {
        return;
    }
    refreshHolderExpressionListeners(project, declared);
    NodesList nodes;
    project.getNodes_recursive(nodes, false);
    for (NodesList::const_iterator it = nodes.begin(); it != nodes.end(); ++it) {
        EffectInstancePtr effect = (*it)->getEffectInstance();
        if (effect) {
            refreshHolderExpressionListeners(*effect, declared);
        }
    }
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
            error = name + " is an attribute of the app object, so it cannot be a project variable";
        }
    }
    if (reason) {
        *reason = error;
    }

    return error.empty();
}

namespace {

    // Creates the user knob serialization describes in the project's RFV group and declares it as
    // app.<name>. Expressions and links are not part of the copy.
    KnobIPtr
    addProjectVariable(const ProjectPtr& project,
                       const Serialization::KnobSerialization& serialization,
                       std::string* error)
    {
        KnobGroupPtr group = project->getRFVGroup();
        KnobIPtr copy = createUserKnobFromSerialization(project.get(), serialization);

        if (!copy || !group || !group->addKnob(copy)) {
            if (copy) {
                project->deleteKnob(copy.get(), false);
            }
            if (error) {
                *error = "The project variables group refused " + serialization.name;
            }

            return KnobIPtr();
        }
        project->recreateUserKnobs(true);
        AppInstancePtr app = project->getApp();
        if (app) {
            app->refreshProjectParamsInPython();
        }

        return copy;
    }

} // namespace

KnobIPtr
promoteToProject(const KnobIPtr& knob,
                 std::string* error)
{
    if (!canPromoteToProject(knob, error)) {
        return KnobIPtr();
    }
    ProjectPtr project = knob->getHolder()->getApp()->getProject();

    // A user knob's serialization carries its definition, values and animation.
    Serialization::KnobSerialization serialization;
    knob->toSerialization(&serialization);

    return addProjectVariable(project, serialization, error);
}

PromoteToProjectCommand::PromoteToProjectCommand(const KnobIPtr& variable)
    : UndoCommand("Add to project variables")
    , _variable(variable)
    , _project()
    , _name(variable ? variable->getName() : std::string())
    , _copy()
    , _removed()
    , _error()
{
    AppInstancePtr app = (variable && variable->getHolder()) ? variable->getHolder()->getApp() : AppInstancePtr();
    if (app) {
        _project = app->getProject();
    }
}

PromoteToProjectCommand::~PromoteToProjectCommand()
{
}

void
PromoteToProjectCommand::redo()
{
    _error.clear();
    if (!_removed) {
        _copy = promoteToProject(_variable.lock(), &_error);

        return;
    }
    ProjectPtr project = _project.lock();
    if (!project) {
        _error = "The project of " + _name + " is gone";

        return;
    }
    if (project->getKnobByName(_name)) {
        _error = "The project already has a parameter named " + _name;

        return;
    }
    _copy = addProjectVariable(project, *_removed, &_error);
}

void
PromoteToProjectCommand::undo()
{
    _error.clear();
    ProjectPtr project = _project.lock();
    KnobIPtr copy = project ? findProjectVariable(*project) : KnobIPtr();
    if (!copy) {
        _error = "The project variable " + _name + " is gone";

        return;
    }
    _removed = std::make_shared<Serialization::KnobSerialization>();
    copy->toSerialization(_removed.get());
    project->deleteKnob(copy.get(), true);
    _copy.reset();
    copy.reset();
    project->recreateUserKnobs(true);
    AppInstancePtr app = project->getApp();
    if (app) {
        app->refreshProjectParamsInPython();
    }
}

KnobIPtr
PromoteToProjectCommand::findProjectVariable(const Project& project) const
{
    KnobIPtr copy = lockUndoKnob(_copy);
    if (copy && (copy->getHolder() == &project)) {
        return copy;
    }
    // Editing the variable in the project's dialog replaces the knob with one of the same name.
    KnobGroupPtr group = project.getRFVGroup();
    const KnobsVec children = group ? group->getChildren() : KnobsVec();
    for (KnobsVec::const_iterator it = children.begin(); it != children.end(); ++it) {
        if ((*it)->getName() == _name) {
            return *it;
        }
    }

    return KnobIPtr();
}

KnobIPtr
PromoteToProjectCommand::getProjectVariable() const
{
    return _copy.lock();
}

KnobIPtr
findDeclaredVariable(const KnobHolder* holder,
                     const std::string& name)
{
    if (!holder) {
        return KnobIPtr();
    }
    const std::vector<VariableDeclaration> declared = getDeclaredVariables(holder);
    for (std::size_t i = 0; i < declared.size(); ++i) {
        if (declared[i].name == name) {
            return declared[i].knob;
        }
    }

    return KnobIPtr();
}

namespace {

    template <typename T, typename V>
    RenderContextValue
    knobValue(Knob<T>* knob,
              double time,
              ViewIdx view)
    {
        const int dimensions = knob->getDimension();
        if (dimensions == 1) {
            return RenderContextValue((V)knob->getValueAtTime(time, 0, ViewSpec(view)));
        }
        std::vector<V> values(dimensions);
        for (int i = 0; i < dimensions; ++i) {
            values[i] = (V)knob->getValueAtTime(time, i, ViewSpec(view));
        }

        return RenderContextValue(values);
    }
} // anonymous namespace

bool
getVariableValue(const KnobIPtr& knob,
                 double time,
                 ViewIdx view,
                 RenderContextValue* value)
{
    if (!knob) {
        return false;
    }
    if (KnobBoolBase* isBool = dynamic_cast<KnobBoolBase*>(knob.get())) {
        *value = RenderContextValue(isBool->getValueAtTime(time, 0, ViewSpec(view)));

        return true;
    }
    if (KnobIntBase* isInt = dynamic_cast<KnobIntBase*>(knob.get())) {
        *value = knobValue<int, std::int64_t>(isInt, time, view);

        return true;
    }
    if (KnobDoubleBase* isDouble = dynamic_cast<KnobDoubleBase*>(knob.get())) {
        *value = knobValue<double, double>(isDouble, time, view);

        return true;
    }
    if (KnobStringBase* isString = dynamic_cast<KnobStringBase*>(knob.get())) {
        *value = RenderContextValue(isString->getValueAtTime(time, 0, ViewSpec(view)));

        return true;
    }

    return false;
}

RenderContextPtr
overlayDeclaredVariables(const KnobHolder* holder,
                         double time,
                         ViewIdx view,
                         const RenderContextPtr& base)
{
    RenderContext::Builder builder(base);
    const std::vector<VariableDeclaration> declared = getDeclaredVariables(holder);
    for (std::size_t i = 0; i < declared.size(); ++i) {
        RenderContextValue value;
        if (getVariableValue(declared[i].knob, time, view, &value)) {
            builder.set(RenderContextName::plain(declared[i].name), value);
        }
    }

    return builder.build();
}

} // namespace RFV

NATRON_NAMESPACE_EXIT
