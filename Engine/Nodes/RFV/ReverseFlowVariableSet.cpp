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

#include "ReverseFlowVariableSet.h"

#include <set>
#include <string>
#include <vector>

#include <QtCore/QDateTime>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/PythonUndoCommands.h"
#include "Engine/RFV.h"
#include "Engine/RenderContext.h"
#include "Engine/UndoRecorder.h"

NATRON_NAMESPACE_ENTER

void
ReverseFlowVariableSet::initializeKnobs()
{
    // The page and groups are user knobs: a page or group that is not refuses user-knob children,
    // and only user pages are saved with their children's layout. Being declared by the plug-in,
    // they survive undoing a change to the user knobs, and so does the drop-down inside.
    KnobPagePtr page = createKnob<KnobPage>(tr(kReverseFlowVariableSetParamPageLabel));
    page->setName(kReverseFlowVariableSetParamPage);
    page->setAsUserKnob(true);

    KnobGroupPtr variables = createKnob<KnobGroup>(tr(kReverseFlowVariableSetParamVariablesLabel));
    variables->setName(kReverseFlowVariableSetParamVariables);
    variables->setAsUserKnob(true);
    variables->setDefaultValue(true);
    variables->setAsRFVGroup(true);
    page->addKnob(variables);
    _variables = variables;

    KnobGroupPtr globalVariables = createKnob<KnobGroup>(tr(kReverseFlowVariableSetParamGlobalVariablesLabel));
    globalVariables->setName(kReverseFlowVariableSetParamGlobalVariables);
    globalVariables->setAsUserKnob(true);
    globalVariables->setDefaultValue(true);
    globalVariables->setAsRFVGroup(true);
    page->addKnob(globalVariables);
    _globalVariables = globalVariables;

    // Not a user knob, so neither saved in the page layout nor taken for a variable.
    KnobChoicePtr existing = createKnob<KnobChoice>(tr(kReverseFlowVariableSetParamExistingVariableLabel));
    existing->setName(kReverseFlowVariableSetParamExistingVariable);
    existing->setHintToolTip(tr(kReverseFlowVariableSetParamExistingVariableHint));
    existing->setAnimationEnabled(false);
    existing->setIsPersistent(false);
    existing->setEvaluateOnChange(false);
    // The selection itself is not an edit; the variable it adds is recorded instead.
    existing->setCanUndo(false);
    // Each pick adds a variable, so scrolling or arrowing through the entries must not.
    existing->setMenuOnlySelection(true);
    existing->populateChoices(std::vector<ChoiceOption>(1, ChoiceOption(kReverseFlowVariableSetExistingVariablePrompt)));
    globalVariables->addKnob(existing);
    _existingVariable = existing;
}

bool
ReverseFlowVariableSet::refreshVariableGroups()
{
    KnobGroupPtr variables = _variables.lock();
    KnobGroupPtr globalVariables = _globalVariables.lock();
    AppInstancePtr app = getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    if (!variables || !globalVariables || !project) {
        return false;
    }
    std::set<std::string> projectNames;
    const std::vector<RFV::VariableDeclaration> declared = RFV::getDeclaredVariables(project.get());
    for (std::size_t i = 0; i < declared.size(); ++i) {
        projectNames.insert(declared[i].name);
    }

    bool moved = false;
    const KnobsVec local = variables->getChildren();
    for (KnobsVec::const_iterator it = local.begin(); it != local.end(); ++it) {
        if (!RFV::isPluginControl(*it) && projectNames.count((*it)->getName())) {
            moved = globalVariables->addKnob(*it) || moved;
        }
    }
    const KnobsVec global = globalVariables->getChildren();
    for (KnobsVec::const_iterator it = global.begin(); it != global.end(); ++it) {
        if (!RFV::isPluginControl(*it) && !projectNames.count((*it)->getName())) {
            moved = variables->addKnob(*it) || moved;
        }
    }

    return moved;
}

void
ReverseFlowVariableSet::onUserKnobsChanged()
{
    refreshVariableGroups();
    // Adding, removing or renaming a variable changes what this node overrides.
    invalidateRenderContextReads();
}

void
ReverseFlowVariableSet::refreshExistingVariables()
{
    KnobChoicePtr existing = _existingVariable.lock();
    if (!existing) {
        return;
    }
    const std::vector<RFV::VariableDeclaration> variables = RFV::getExistingVariables(this);
    std::vector<ChoiceOption> entries(1, ChoiceOption(kReverseFlowVariableSetExistingVariablePrompt));
    for (std::size_t i = 0; i < variables.size(); ++i) {
        ChoiceOption entry(variables[i].name);
        KnobHolder* holder = variables[i].knob->getHolder();
        EffectInstance* effect = dynamic_cast<EffectInstance*>(holder);
        if (effect) {
            entry.tooltip = tr("Declared on %1").arg(QString::fromUtf8(effect->getNode()->getFullyQualifiedName().c_str())).toStdString();
        } else {
            entry.tooltip = tr("Declared on the project").toStdString();
        }
        entries.push_back(entry);
    }
    existing->populateChoices(entries);
    if (existing->getValue() != 0) {
        existing->setValue(0);
    }
}

bool
ReverseFlowVariableSet::knobChanged(KnobI* k,
                                    ValueChangedReasonEnum /*reason*/,
                                    ViewSpec /*view*/,
                                    double /*time*/,
                                    bool /*originatedFromMainThread*/)
{
    KnobChoicePtr existing = _existingVariable.lock();
    if (!existing || (k != existing.get())) {
        return false;
    }
    const int index = existing->getValue();
    if (index <= 0) {
        return true;
    }
    const std::string name = existing->getEntry(index).id;
    // The selection comes from the drop-down's own widget, which adding the variable rebuilds:
    // the add waits for the event loop, once that widget has returned.
    QMetaObject::invokeMethod(this, [this, name]() { addExistingVariableNow(name); }, Qt::QueuedConnection);

    return true;
}

void
ReverseFlowVariableSet::addExistingVariableNow(const std::string& name)
{
    std::string error;
    {
        UndoStepGuard step(getApp(), tr("Add existing variable %1").arg(QString::fromUtf8(name.c_str())).toStdString());
        PythonUserKnobsUndoGuard undo(this);
        RFV::addExistingVariable(this, name, &error);
    }
    if (!error.empty()) {
        appPTR->writeToErrorLog_mt_safe(QString::fromUtf8(getNode()->getScriptName_mt_safe().c_str()), QDateTime::currentDateTime(), QString::fromUtf8(error.c_str()));
    }
    // Resetting to the prompt comes back to knobChanged() with index 0.
    refreshExistingVariables();
}

void
ReverseFlowVariableSet::onSettingsPanelOpened()
{
    refreshExistingVariables();
}

void
ReverseFlowVariableSet::refreshChoiceEntries(KnobChoice* knob)
{
    // Variables come and go on the project and on other nodes while this panel stays open.
    if (knob && (knob == _existingVariable.lock().get())) {
        refreshExistingVariables();
    }
}

Route
ReverseFlowVariableSet::route(double time,
                              ViewIdx /*view*/)
{
    return Route::input(0, time);
}

bool
ReverseFlowVariableSet::getInputContexts(int inputNb,
                                         double time,
                                         ViewIdx view,
                                         const RenderContextPtr& context,
                                         std::vector<RenderContextPtr>* contexts) const
{
    if (inputNb != 0) {
        return false;
    }
    // The variables' expressions run as actions of this node under the incoming context.
    ActionContextScope acting(true, context);
    contexts->push_back(RFV::overlayDeclaredVariables(this, time, view, context));

    return true;
}

RenderContextReadSet
ReverseFlowVariableSet::getOverriddenContextVariables(int inputNb) const
{
    RenderContextReadSet overridden;
    if (inputNb != 0) {
        return overridden;
    }
    // Asked on render threads while the main thread may edit the variables: the snapshot, not
    // the groups' children.
    const RFVDeclarationsConstPtr declared = RFV::getDeclarationSnapshot(this);
    for (const RFV::VariableDeclaration& declaration : *declared) {
        overridden.add(RenderContextName::plain(declaration.name));
    }

    return overridden;
}

NATRON_NAMESPACE_EXIT
