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
#include "Engine/ContentHasher.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/IoColorSpace.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"

NATRON_NAMESPACE_ENTER

namespace {
const char* const kKnobOCIOConfigFile = "ocioConfigFile";
const char* const kKnobOCIOWorkingSpace = "ocioWorkingSpace";
const char* const kProjectDefaultID = "projectDefault";
} // anonymous namespace

const char*
OcioSpaceKnobs::projectDefaultID()
{
    return kProjectDefaultID;
}

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
    , _projectDefault(false)
    , _defaultMutex()
    , _defaultSpace()
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
    _projectDefault = description.projectDefault;

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

    createSpaceKnobs(holder, page, description, -1);

    const QString contextHint = tr("OCIO Contexts allow you to apply specific LUTs or grades to different shots.\n"
                                   "Here you can specify the context name (key) and its corresponding value.\n"
                                   "Full details of how to set up contexts and add them to your config can be found in the OpenColorIO documentation:\n"
                                   "http://opencolorio.org/userguide/contexts.html");
    KnobGroupPtr context = AppManager::createKnob<KnobGroup>(holder, tr("OCIO Context"));
    context->setName(description.contextName);
    context->setHintToolTip(contextHint);
    context->setDefaultValue(false);
    page->addKnob(context);
    _context = context;
    for (int i = 0; i < 4; ++i) {
        const std::string n = std::to_string(i + 1);
        const std::string keyName = description.contextKeyPrefix + n;
        const std::string valueName = description.contextValuePrefix + n;
        KnobStringPtr key = AppManager::createKnob<KnobString>(holder, description.contextKeyLabel.isEmpty() ? QString::fromStdString(keyName) : description.contextKeyLabel.arg(i + 1));
        key->setName(keyName);
        key->setHintToolTip(contextHint);
        key->setAddNewLine(false);
        context->addKnob(key);
        _contextKeys[i] = key;

        KnobStringPtr value = AppManager::createKnob<KnobString>(holder, description.contextValueLabel.isEmpty() ? QString::fromStdString(valueName) : description.contextValueLabel.arg(i + 1));
        value->setName(valueName);
        value->setHintToolTip(contextHint);
        context->addKnob(value);
        _contextValues[i] = value;
    }

    refreshMenu();
}

void
OcioSpaceKnobs::createSpaceKnobs(KnobHolder* holder,
                                 const KnobPagePtr& page,
                                 const Description& description,
                                 int index)
{
    const auto add = [&page, &index](const KnobIPtr& knob) {
        if (index < 0) {
            page->addKnob(knob);
        } else {
            page->insertKnob(index++, knob);
        }
    };

    if (description.projectDefault) {
        // Persistent and evaluating: the menu alone records the choice, so it decides the render.
        KnobChoicePtr menu = AppManager::createKnob<KnobChoice>(holder, description.label);
        menu->setName(description.prefix);
        menu->setHintToolTip(description.menuHint);
        menu->setAnimationEnabled(false);
        menu->setCascading(true);
        menu->setDefaultValue(0);
        add(menu);
        _menu = menu;

        return;
    }

    KnobStringPtr space = AppManager::createKnob<KnobString>(holder, description.label);
    space->setName(description.prefix);
    space->setHintToolTip(description.spaceHint);
    space->setAnimationEnabled(false);
    space->setSecret(true);
    add(space);
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
    add(menu);
    _menu = menu;

    KnobBoolPtr spaceSet = AppManager::createKnob<KnobBool>(holder, description.setLabel);
    spaceSet->setName(description.prefix + "Set");
    spaceSet->setDefaultValue(false);
    spaceSet->setAnimationEnabled(false);
    spaceSet->setEvaluateOnChange(false);
    spaceSet->setSecret(true);
    add(spaceSet);
    _spaceSet = spaceSet;
}

void
OcioSpaceKnobs::createSharing(const OcioSpaceKnobs& first,
                              KnobHolder* holder,
                              const KnobPagePtr& page,
                              const Description& description,
                              const AutomaticSpace& automaticSpace,
                              const ChoiceMade& choiceMade)
{
    _holder = holder;
    _automaticSpace = automaticSpace;
    _choiceMade = choiceMade;
    _projectDefault = description.projectDefault;
    _configFile = first._configFile;
    _workingSpace = first._workingSpace;
    _context = first._context;
    for (int i = 0; i < 4; ++i) {
        _contextKeys[i] = first._contextKeys[i];
        _contextValues[i] = first._contextValues[i];
    }

    int index = -1;
    if (KnobGroupPtr context = _context.lock()) {
        const std::vector<KnobIPtr> children = page->getChildren();
        for (std::size_t i = 0; i < children.size(); ++i) {
            if (children[i] == context) {
                index = (int)i;
                break;
            }
        }
    }
    createSpaceKnobs(holder, page, description, index);

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
    if (_projectDefault) {
        KnobChoicePtr menu = _menu.lock();
        const std::string chosen = menu ? menu->getActiveEntry().id : std::string();
        if (!chosen.empty() && (chosen != kProjectDefaultID)) {
            return chosen;
        }
        std::lock_guard<std::mutex> lock(_defaultMutex);

        return _defaultSpace;
    }
    KnobStringPtr knob = _space.lock();

    return knob ? knob->getValue() : std::string();
}

bool
OcioSpaceKnobs::isProjectDefault() const
{
    if (!_projectDefault) {
        return false;
    }
    KnobChoicePtr menu = _menu.lock();
    const std::string chosen = menu ? menu->getActiveEntry().id : std::string();

    return chosen.empty() || (chosen == kProjectDefaultID);
}

void
OcioSpaceKnobs::appendHash(ContentHasher& h) const
{
    if (!isProjectDefault()) {
        return;
    }
    std::lock_guard<std::mutex> lock(_defaultMutex);
    h.appendString(_defaultSpace);
}

void
OcioSpaceKnobs::refreshProjectDefault()
{
    if (!_projectDefault || !_automaticSpace) {
        return;
    }
    const std::optional<std::string> automatic = _automaticSpace();
    if (!automatic) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(_defaultMutex);
        if (*automatic == _defaultSpace) {
            return;
        }
        _defaultSpace = *automatic;
    }
    if (!isProjectDefault()) {
        return;
    }
    EffectInstance* effect = dynamic_cast<EffectInstance*>(_holder);
    NodePtr node = effect ? effect->getNode() : NodePtr();
    if (node) {
        node->markHashDirty();
    }
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

KnobGroupPtr
OcioSpaceKnobs::contextGroup() const
{
    return _context.lock();
}

void
OcioSpaceKnobs::refreshMenu()
{
    KnobChoicePtr menu = _menu.lock();
    KnobStringPtr space = _space.lock();
    ProjectPtr p = project();
    ProjectColorManagementPtr colorManagement = p ? p->getColorManagement() : ProjectColorManagementPtr();

    if (!menu || !colorManagement) {
        return;
    }
    if (_projectDefault) {
        std::vector<ChoiceOption> options(1, ChoiceOption(kProjectDefaultID, tr("Project default").toStdString(), tr("The colorspace the config's file rules give the file, else the colorspace "
                                                                                                                     "tagged in the file, else the project's default for files of its kind, "
                                                                                                                     "following them as they change.")
                                                                                                                      .toStdString()));
        const std::vector<ChoiceOption> spaces = Project::colorSpaceOptions(*colorManagement);
        options.insert(options.end(), spaces.begin(), spaces.end());
        menu->populateChoices(options);

        return;
    }
    if (!space) {
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
    if (_projectDefault) {
        AppInstancePtr app = _holder ? _holder->getApp() : AppInstancePtr();
        const bool restoring = app && (app->isCreatingNodeTree() || (app->getProject() && app->getProject()->isLoadingProject()));
        if (!restoring) {
            refreshProjectDefault();
        }

        return;
    }
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
    if (_projectDefault) {
        KnobChoicePtr menu = _menu.lock();
        if (menu && (menu->getValue() != 0)) {
            menu->setValue(0);
        }
        refreshProjectDefault();

        return;
    }
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

    if (_projectDefault) {
        if (!configFile || !workingSpace || !menu || reason == eValueChangedReasonTimeChanged) {
            return false;
        }
        if (k == configFile.get() || k == workingSpace.get()) {
            refreshMenu();
            // The config's file rules are part of the default.
            if (k == configFile.get()) {
                refreshProjectDefault();
            }

            return true;
        }
        if (k == menu.get()) {
            if ((reason == eValueChangedReasonUserEdited) || (reason == eValueChangedReasonRestoreDefault)) {
                refreshProjectDefault();
                if (_choiceMade) {
                    _choiceMade();
                }
            }

            return true;
        }

        return false;
    }
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
