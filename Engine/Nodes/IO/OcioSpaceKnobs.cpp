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

#include "OcioSpaceKnobs.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/IO/IoColorSpace.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"

NATRON_NAMESPACE_ENTER

namespace {
const char* const kKnobOCIOConfigFile = "ocioConfigFile";
const char* const kKnobOCIOWorkingSpace = "ocioWorkingSpace";
const char* const kKnobOCIOContext = "Context";
} // anonymous namespace

OcioSpaceKnobs::OcioSpaceKnobs()
    : _holder(nullptr)
    , _automaticSpace()
    , _choiceMade()
    , _configFile()
    , _workingSpace()
    , _space()
    , _menu()
    , _spaceSet()
    , _context()
    , _hidden(false)
{
}

void
OcioSpaceKnobs::create(KnobHolder* holder,
                       const KnobPagePtr& page,
                       const Description& description,
                       const AutomaticSpace& automaticSpace,
                       const ChoiceMade& choiceMade)
{
    _holder = holder;
    _automaticSpace = automaticSpace;
    _choiceMade = choiceMade;

    KnobStringPtr configFile = AppManager::createKnob<KnobString>(holder, tr("OCIO Config File"));
    configFile->setName(kKnobOCIOConfigFile);
    configFile->setAnimationEnabled(false);
    // The project pushes its config to every node after creation and after a load.
    configFile->setIsPersistent(false);
    configFile->setSecret(true);
    page->addKnob(configFile);
    _configFile = configFile;

    KnobStringPtr workingSpace = AppManager::createKnob<KnobString>(holder, tr("Working Colorspace"));
    workingSpace->setName(kKnobOCIOWorkingSpace);
    workingSpace->setAnimationEnabled(false);
    workingSpace->setIsPersistent(false);
    workingSpace->setSecret(true);
    page->addKnob(workingSpace);
    _workingSpace = workingSpace;

    KnobStringPtr space = AppManager::createKnob<KnobString>(holder, description.label);
    space->setName(description.prefix);
    space->setHintToolTip(description.spaceHint);
    space->setAnimationEnabled(false);
    space->setSecret(true);
    page->addKnob(space);
    _space = space;

    KnobChoicePtr menu = AppManager::createKnob<KnobChoice>(holder, description.label);
    menu->setName(description.prefix + "Index");
    menu->setHintToolTip(description.menuHint);
    menu->setAnimationEnabled(false);
    menu->setCascading(true);
    menu->setNotifiesOnReselect(true);
    menu->setIsPersistent(false);
    // Only the space string it sets decides the render.
    menu->setEvaluateOnChange(false);
    page->addKnob(menu);
    _menu = menu;

    KnobBoolPtr spaceSet = AppManager::createKnob<KnobBool>(holder, description.setLabel);
    spaceSet->setName(description.prefix + "Set");
    spaceSet->setDefaultValue(false);
    spaceSet->setAnimationEnabled(false);
    spaceSet->setEvaluateOnChange(false);
    spaceSet->setSecret(true);
    page->addKnob(spaceSet);
    _spaceSet = spaceSet;

    const QString contextHint = tr("OCIO Contexts allow you to apply specific LUTs or grades to different shots.\n"
                                   "Here you can specify the context name (key) and its corresponding value.\n"
                                   "Full details of how to set up contexts and add them to your config can be found in the OpenColorIO documentation:\n"
                                   "http://opencolorio.org/userguide/contexts.html");
    KnobGroupPtr context = AppManager::createKnob<KnobGroup>(holder, tr("OCIO Context"));
    context->setName(kKnobOCIOContext);
    context->setHintToolTip(contextHint);
    context->setDefaultValue(false);
    page->addKnob(context);
    _context = context;
    for (int i = 0; i < 4; ++i) {
        const std::string n = std::to_string(i + 1);
        KnobStringPtr key = AppManager::createKnob<KnobString>(holder, "key" + n);
        key->setName("key" + n);
        key->setHintToolTip(contextHint);
        key->setAddNewLine(false);
        context->addKnob(key);
        _contextKeys[i] = key;

        KnobStringPtr value = AppManager::createKnob<KnobString>(holder, "value" + n);
        value->setName("value" + n);
        value->setHintToolTip(contextHint);
        context->addKnob(value);
        _contextValues[i] = value;
    }

    refreshMenu();
}

ProjectPtr
OcioSpaceKnobs::project() const
{
    AppInstancePtr app = _holder ? _holder->getApp() : AppInstancePtr();

    return app ? app->getProject() : ProjectPtr();
}

std::string
OcioSpaceKnobs::space() const
{
    KnobStringPtr knob = _space.lock();

    return knob ? knob->getValue() : std::string();
}

std::string
OcioSpaceKnobs::workingSpace() const
{
    KnobStringPtr knob = _workingSpace.lock();
    const std::string pushed = knob ? knob->getValue() : std::string();

    if (!pushed.empty()) {
        return pushed;
    }
    ProjectPtr p = project();

    return p ? p->getWorkingColorSpace() : std::string();
}

std::vector<std::pair<std::string, std::string>>
OcioSpaceKnobs::contextVariables(double time) const
{
    std::vector<std::pair<std::string, std::string>> variables;

    for (int i = 0; i < 4; ++i) {
        KnobStringPtr key = _contextKeys[i].lock();
        KnobStringPtr value = _contextValues[i].lock();
        if (!key || !value) {
            continue;
        }
        const std::string name = key->getValueAtTime(time);
        if (!name.empty()) {
            variables.push_back(std::make_pair(name, value->getValueAtTime(time)));
        }
    }

    return variables;
}

void
OcioSpaceKnobs::refreshMenu()
{
    KnobChoicePtr menu = _menu.lock();
    KnobStringPtr space = _space.lock();
    ProjectPtr p = project();
    ProjectColorManagementPtr colorManagement = p ? p->getColorManagement() : ProjectColorManagementPtr();

    if (!menu || !space || !colorManagement) {
        return;
    }
    const std::vector<ChoiceOption> options = Project::colorSpaceOptions(*colorManagement);
    menu->populateChoices(options);

    // An empty space converts nothing, which the menu shows as the working space.
    const std::string value = space->getValue();
    const std::string shown = IoColorSpace::nameInConfig(colorManagement->getConfig(), value.empty() ? workingSpace() : value);
    int index = -1;
    for (std::size_t i = 0; i < options.size(); ++i) {
        if (!shown.empty() && options[i].id == shown) {
            index = (int)i;
            break;
        }
    }
    if (index >= 0 && menu->getValue() != index) {
        menu->setValue(index);
    }
    // A name the config lacks cannot be shown by the menu, so the string itself is shown.
    space->setSecret(_hidden || value.empty() || index >= 0);
}

void
OcioSpaceKnobs::setHidden(bool hidden)
{
    _hidden = hidden;
    if (KnobChoicePtr menu = _menu.lock()) {
        menu->setSecret(hidden);
    }
    if (KnobGroupPtr context = _context.lock()) {
        context->setSecret(hidden);
    }
    for (int i = 0; i < 4; ++i) {
        for (const KnobStringWPtr& weak : { _contextKeys[i], _contextValues[i] }) {
            if (KnobStringPtr knob = weak.lock()) {
                knob->setSecret(hidden);
            }
        }
    }
    refreshMenu();
}

void
OcioSpaceKnobs::sourceChanged()
{
    KnobStringPtr space = _space.lock();
    KnobBoolPtr spaceSet = _spaceSet.lock();

    if (!space || !spaceSet || spaceSet->getValue() || !_automaticSpace) {
        return;
    }
    AppInstancePtr app = _holder ? _holder->getApp() : AppInstancePtr();
    const bool restoring = app && (app->isCreatingNodeTree() || (app->getProject() && app->getProject()->isLoadingProject()));
    if (restoring) {
        return;
    }
    const std::optional<std::string> automatic = _automaticSpace();
    if (automatic && !automatic->empty() && *automatic != space->getValue()) {
        space->setValue(*automatic);
    }
}

void
OcioSpaceKnobs::reset()
{
    KnobStringPtr space = _space.lock();
    KnobBoolPtr spaceSet = _spaceSet.lock();

    if (!space || !spaceSet) {
        return;
    }
    spaceSet->setValue(false);
    const std::optional<std::string> automatic = _automaticSpace ? _automaticSpace() : std::optional<std::string>();
    if (automatic) {
        if (automatic->empty()) {
            space->setValue(std::string());
        } else if (*automatic != space->getValue()) {
            space->setValue(*automatic);
        }
    }
    // The space may already hold the default, so nothing else re-selects it in the menu.
    refreshMenu();
}

bool
OcioSpaceKnobs::knobChanged(KnobI* k,
                            ValueChangedReasonEnum reason)
{
    KnobStringPtr configFile = _configFile.lock();
    KnobStringPtr workingSpace = _workingSpace.lock();
    KnobStringPtr space = _space.lock();
    KnobChoicePtr menu = _menu.lock();
    KnobBoolPtr spaceSet = _spaceSet.lock();

    if (!configFile || !workingSpace || !space || !menu || !spaceSet || reason == eValueChangedReasonTimeChanged) {
        return false;
    }
    const bool byUser = reason == eValueChangedReasonUserEdited;

    if (k == configFile.get() || k == workingSpace.get()) {
        refreshMenu();

        return true;
    }
    if ((k == menu.get() || k == space.get()) && reason == eValueChangedReasonRestoreDefault) {
        reset();
        if (_choiceMade) {
            _choiceMade();
        }

        return true;
    }
    if (k == menu.get()) {
        const int index = menu->getValue();
        if (byUser && index >= 0 && index < menu->getNumEntries()) {
            spaceSet->setValue(true);
            space->setValue(menu->getEntry(index).id);
            if (_choiceMade) {
                _choiceMade();
            }
        }

        return true;
    }
    if (k == space.get()) {
        if (byUser) {
            spaceSet->setValue(true);
        }
        refreshMenu();
        if (byUser && _choiceMade) {
            _choiceMade();
        }

        return true;
    }

    return false;
}

NATRON_NAMESPACE_EXIT
