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
#include <charconv>
#include <cstdint>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/RFV/RFVRead.h"
#include "Engine/Nodes/RFV/RFVSet.h"
#include "Engine/OpenGLViewerI.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/RenderContext.h"
#include "Engine/UndoKnobRef.h"
#include "Engine/ViewerInstance.h"

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

void
refreshDeclarationSnapshot(KnobHolder* holder)
{
    if (!holder) {
        return;
    }
    std::shared_ptr<std::vector<VariableDeclaration>> declarations = std::make_shared<std::vector<VariableDeclaration>>(getDeclaredVariables(holder));
    const RFVDeclarationsConstPtr previous = holder->getRFVDeclarationSnapshot();
    holder->setRFVDeclarationSnapshot(declarations);

    bool changed = !previous || (previous->size() != declarations->size());
    for (std::size_t i = 0; !changed && (i < declarations->size()); ++i) {
        changed = ((*previous)[i].name != (*declarations)[i].name) || ((*previous)[i].knob != (*declarations)[i].knob);
    }
    if (changed) {
        // What a Set overrides and what a project variable's readers read follow the declarations.
        invalidateRenderContextReads();
    }
}

RFVDeclarationsConstPtr
getDeclarationSnapshot(const KnobHolder* holder)
{
    static const RFVDeclarationsConstPtr none = std::make_shared<const std::vector<VariableDeclaration>>();
    RFVDeclarationsConstPtr declarations = holder ? holder->getRFVDeclarationSnapshot() : RFVDeclarationsConstPtr();

    return declarations ? declarations : none;
}

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
        RFVSet* setNode = dynamic_cast<RFVSet*>(effect.get());
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
    const RFVDeclarationsConstPtr declared = getDeclarationSnapshot(holder);
    for (const VariableDeclaration& declaration : *declared) {
        if (declaration.name == name) {
            return declaration.knob;
        }
    }

    return KnobIPtr();
}

void
appendProjectVariableReads(const Project& project,
                           RenderContextReadSet* reads)
{
    if (reads->readsAll()) {
        return;
    }
    std::set<std::string> expanded;
    std::vector<std::string> pending;
    for (const RenderContextName& name : reads->names()) {
        if (name.ns.empty()) {
            pending.push_back(name.name);
        }
    }
    while (!pending.empty()) {
        const std::string name = pending.back();
        pending.pop_back();
        if (!expanded.insert(name).second) {
            continue;
        }
        const KnobIPtr variable = findDeclaredVariable(&project, name);
        const KnobHelper* helper = dynamic_cast<const KnobHelper*>(variable.get());
        if (!helper) {
            continue;
        }
        const RenderContextReadSet variableReads = helper->getRFVReadSet();
        if (variableReads.readsAll()) {
            reads->setReadsAll();

            return;
        }
        for (const RenderContextName& read : variableReads.names()) {
            reads->add(read);
            if (read.ns.empty()) {
                pending.push_back(read.name);
            }
        }
    }
}

void
rehashExtraContextReaders(const Project& project,
                          const std::string& name,
                          bool renderAgain)
{
    NodesList nodes;
    project.getNodes_recursive(nodes, false);
    for (const NodePtr& node : nodes) {
        if (!node || !node->isNodeCreated()) {
            continue;
        }
        const RenderContextReadSet reads = node->getExtraContextReads();
        if (reads.empty()) {
            continue;
        }
        if (name.empty() || reads.readsAll() || reads.contains(RenderContextName::plain(name))) {
            node->markHashDirty();
            EffectInstancePtr effect = renderAgain ? node->getEffectInstance() : EffectInstancePtr();
            if (effect) {
                effect->incrHashAndEvaluate(true, true);
            }
        }
    }
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

bool
isTimeVarying(const KnobIPtr& knob)
{
    if (!knob) {
        return false;
    }
    // Keyframes or an expression.
    if (knob->hasAnimation()) {
        return true;
    }
    // A linked knob takes its master's value.
    bool linked = false;
    for (int i = 0, n = knob->getDimension(); i < n; ++i) {
        const std::pair<int, KnobIPtr> master = knob->getMaster(i);
        if (master.second) {
            if (isTimeVarying(master.second)) {
                return true;
            }
            linked = true;
        }
    }
    if (linked) {
        return false;
    }
    bool hasExpression = false;
    bool readsOutsideKnob = false;
    knob->getContentHashDependencies(&hasExpression, &readsOutsideKnob);

    return readsOutsideKnob;
}

RenderContextPtr
overlayDeclaredVariables(const KnobHolder* holder,
                         double time,
                         ViewIdx view,
                         const RenderContextPtr& base)
{
    RenderContext::Builder builder(base);
    const RFVDeclarationsConstPtr declared = getDeclarationSnapshot(holder);
    for (const VariableDeclaration& declaration : *declared) {
        if (isTimeVarying(declaration.knob)) {
            const KnobHelper* helper = dynamic_cast<const KnobHelper*>(declaration.knob.get());
            const RenderContextPtr incoming = base ? base : RenderContext::empty();
            RenderContextReadSetConstPtr reads;
            if (helper) {
                helper->getMemoizedValueContext(&reads, 0);
            }
            const RenderContextPtr captured = reads ? incoming->restrictTo(*reads) : incoming;
            builder.set(RenderContextName::plain(declaration.name), RenderContextBinding::create(declaration.knob, declaration.name, captured));
            continue;
        }
        RenderContextValue value;
        if (getVariableValue(declaration.knob, time, view, &value)) {
            builder.set(RenderContextName::plain(declaration.name), value);
        }
    }

    return builder.build();
}

NodePtr
getActiveViewer(const AppInstancePtr& app)
{
    return app ? app->getActiveViewerNode() : NodePtr();
}

namespace {

    // The inputs a render of output pulls: a viewer's A input, and its B input while it composites
    // the two; every input of any other node.
    std::vector<int>
    displayedInputs(const NodePtr& output)
    {
        std::vector<int> inputs;
        ViewerInstance* viewer = output->isEffectViewer();
        if (!viewer) {
            for (int i = 0, n = output->getNInputs(); i < n; ++i) {
                inputs.push_back(i);
            }

            return inputs;
        }
        int a = -1;
        int b = -1;
        viewer->getActiveInputs(a, b);
        if (a >= 0) {
            inputs.push_back(a);
        }
        OpenGLViewerI* ui = viewer->getUiContext();
        if ((b >= 0) && (b != a) && ui && (ui->getCompositingOperator() != eViewerCompositingOperatorNone)) {
            inputs.push_back(b);
        }

        return inputs;
    }

    struct PathEdge {
        NodePtr consumer;
        int inputNb;
    };

    // The edges of the first path found from output's displayed inputs up to target, nearest to
    // output first. Iterative and visiting each node once, so deep and wide graphs stay linear.
    bool
    findPathUpTo(const NodePtr& output,
                 const NodePtr& target,
                 std::vector<PathEdge>* path)
    {
        if (output == target) {
            return true;
        }
        struct Step {
            NodePtr node;
            std::vector<int> inputs;
            std::size_t next;
        };
        std::vector<Step> stack;
        std::set<const Node*> visited;
        stack.push_back(Step { output, displayedInputs(output), 0 });
        visited.insert(output.get());
        while (!stack.empty()) {
            Step& top = stack.back();
            if (top.next >= top.inputs.size()) {
                stack.pop_back();
                continue;
            }
            const int inputNb = top.inputs[top.next++];
            NodePtr input = top.node->getInput(inputNb);
            if (!input || !visited.insert(input.get()).second) {
                continue;
            }
            if (input == target) {
                for (const Step& step : stack) {
                    path->push_back(PathEdge { step.node, step.inputs[step.next - 1] });
                }

                return true;
            }
            std::vector<int> inputs;
            for (int i = 0, n = input->getNInputs(); i < n; ++i) {
                inputs.push_back(i);
            }
            stack.push_back(Step { input, std::move(inputs), 0 });
        }

        return false;
    }

    // The time consumer, acting under context at time, reads input inputNb at: the time it passes
    // through as an identity of that input, otherwise the first frame getFramesNeeded() lists for
    // it, otherwise time. A time node between a reader and the output thus moves the reader's time.
    double
    inputTimeOf(const EffectInstancePtr& consumer,
                int inputNb,
                double time,
                ViewIdx view,
                const RenderContextPtr& context)
    {
        ActionContextScope acting(true, context);
        const U64 hash = consumer->getRenderHash();
        double identityTime = time;
        ViewIdx identityView = view;
        int identityInput = -1;
        if (consumer->isIdentity_public(false, hash, time, RenderScale::identity, RectI(0, 0, 1, 1), view, &identityTime, &identityView, &identityInput) && (identityInput == inputNb)) {
            return identityTime;
        }
        const FramesNeededMap needed = consumer->getFramesNeeded_public(hash, time, view, 0);
        const FramesNeededMap::const_iterator input = needed.find(inputNb);
        if (input == needed.end()) {
            return time;
        }
        FrameRangesMap::const_iterator ranges = input->second.find(view);
        if (ranges == input->second.end()) {
            ranges = input->second.begin();
        }
        if ((ranges == input->second.end()) || ranges->second.empty()) {
            return time;
        }

        return ranges->second.front().min;
    }

    std::string
    displayName(const RenderContextName& name)
    {
        return name.ns.empty() ? name.name : (name.ns + "." + name.name);
    }

    // The variable knob's value at time and view under context, as rfv reads it there.
    void
    evaluateVariable(const RenderContextName& name,
                     const KnobIPtr& knob,
                     const RenderContextPtr& context,
                     double time,
                     ViewIdx view,
                     RenderContext::Builder* builder,
                     ResolvedContext* resolved)
    {
        try {
            ActionContextScope acting(true, context);
            RenderContextValue value;
            if (getVariableValue(knob, time, view, &value)) {
                builder->set(name, value);
            }
        } catch (const std::exception& e) {
            resolved->errors.push_back(std::make_pair(displayName(name), std::string(e.what())));
        }
    }

} // namespace

ResolvedContext
resolveContextBelow(const NodePtr& output,
                    const NodePtr& target,
                    double time,
                    ViewIdx view)
{
    ResolvedContext resolved;
    std::vector<PathEdge> path;
    // A render's root starts from the empty context.
    RenderContextPtr pathContext;
    if (output && target && findPathUpTo(output, target, &path)) {
        resolved.outputName = output->getScriptName_mt_safe();
        for (const PathEdge& edge : path) {
            EffectInstancePtr effect = edge.consumer->getEffectInstance();
            if (!effect) {
                continue;
            }
            std::vector<RenderContextPtr> contexts;
            double inputTime = time;
            try {
                inputTime = inputTimeOf(effect, edge.inputNb, time, view, pathContext);
                if (effect->getInputContexts(edge.inputNb, time, view, pathContext, &contexts) && !contexts.empty()) {
                    pathContext = contexts.front();
                }
            } catch (const std::exception& e) {
                resolved.errors.push_back(std::make_pair(edge.consumer->getScriptName_mt_safe(), std::string(e.what())));
            }
            time = inputTime;
        }
    }
    resolved.time = time;

    RenderContext::Builder builder;
    AppInstancePtr app = target ? target->getApp() : (output ? output->getApp() : AppInstancePtr());
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    if (project) {
        const RFVDeclarationsConstPtr declared = getDeclarationSnapshot(project.get());
        for (const VariableDeclaration& declaration : *declared) {
            const RenderContextName name = RenderContextName::plain(declaration.name);
            if (pathContext && pathContext->find(name)) {
                continue;
            }
            evaluateVariable(name, declaration.knob, pathContext, time, view, &builder, &resolved);
        }
    }
    if (pathContext) {
        for (const RenderContext::Entry& entry : pathContext->entries()) {
            const RenderContextBinding* binding = std::get_if<RenderContextBinding>(&entry.second);
            if (!binding) {
                builder.set(entry.first, entry.second);
                continue;
            }
            // As rfv reads a binding: a knob deleted since, or off its node, is unset.
            const KnobIPtr knob = binding->knob.lock();
            const KnobHolder* holder = knob ? knob->getHolder() : NULL;
            const EffectInstance* effect = dynamic_cast<const EffectInstance*>(holder);
            if (!holder || (effect && !effect->getNode())) {
                continue;
            }
            evaluateVariable(entry.first, knob, binding->captured, time, view, &builder, &resolved);
        }
    }
    resolved.context = builder.build();

    return resolved;
} // resolveContextBelow

ResolvedContext
resolveContextAt(const NodePtr& target,
                 double time,
                 ViewIdx view)
{
    return resolveContextBelow(getActiveViewer(target ? target->getApp() : AppInstancePtr()), target, time, view);
}

namespace {

    std::string
    formatDouble(double value)
    {
        char buffer[64];
        const std::to_chars_result result = std::to_chars(buffer, buffer + sizeof(buffer), value);
        std::string text(buffer, result.ptr);
        // Python writes an integral float with a fractional part.
        if (text.find_first_of(".eni") == std::string::npos) {
            text += ".0";
        }

        return text;
    }

    std::string
    formatString(const std::string& value)
    {
        // Python's choice of quotes: single unless only double ones avoid escaping.
        const char quote = ((value.find('\'') != std::string::npos) && (value.find('"') == std::string::npos)) ? '"' : '\'';
        std::string text(1, quote);
        for (const char c : value) {
            if ((c == quote) || (c == '\\')) {
                text += '\\';
                text += c;
            } else if (c == '\n') {
                text += "\\n";
            } else if (c == '\r') {
                text += "\\r";
            } else if (c == '\t') {
                text += "\\t";
            } else {
                text += c;
            }
        }
        text += quote;

        return text;
    }

    template <typename T, typename Format>
    std::string
    formatList(const std::vector<T>& values,
               Format format)
    {
        std::string text = "[";
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i > 0) {
                text += ", ";
            }
            text += format(values[i]);
        }

        return text + "]";
    }

} // namespace

std::string
formatValue(const RenderContextValue& value)
{
    if (const bool* b = std::get_if<bool>(&value)) {
        return *b ? "True" : "False";
    }
    if (const std::int64_t* i = std::get_if<std::int64_t>(&value)) {
        return std::to_string(*i);
    }
    if (const double* d = std::get_if<double>(&value)) {
        return formatDouble(*d);
    }
    if (const std::string* s = std::get_if<std::string>(&value)) {
        return formatString(*s);
    }
    if (const std::vector<double>* ds = std::get_if<std::vector<double>>(&value)) {
        return formatList(*ds, formatDouble);
    }
    if (const std::vector<std::int64_t>* is = std::get_if<std::vector<std::int64_t>>(&value)) {
        return formatList(*is, [](std::int64_t i) { return std::to_string(i); });
    }
    if (const std::vector<std::string>* ss = std::get_if<std::vector<std::string>>(&value)) {
        return formatList(*ss, formatString);
    }

    return "<binding>";
}

void
scheduleReaderRefresh(const AppInstancePtr& app)
{
    if (app) {
        RFVRead::scheduleRefreshAll(app.get());
    }
}

} // namespace RFV

NATRON_NAMESPACE_EXIT
