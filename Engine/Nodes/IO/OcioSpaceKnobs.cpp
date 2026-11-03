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

ChoiceOption
OcioSpaceKnobs::projectDefaultOption(const std::string& resolved,
                                     const QString& hint)
{
    const QString label = resolved.empty() ? tr("Project default") : tr("Project default (%1)").arg(QString::fromStdString(resolved));

    return ChoiceOption(kProjectDefaultID, label.toStdString(), hint.toStdString());
}

OcioSpaceKnobs::OcioSpaceKnobs()
    : _holder(nullptr)
    , _automaticSpace()
    , _choiceMade()
    , _defaultHint()
    , _configFile()
    , _workingSpace()
    , _menu()
    , _context()
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
    _defaultHint = description.defaultHint;

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

    createMenu(holder, page, description, -1);

    const QString contextHint = tr("OCIO Contexts allow you to apply specific LUTs or grades to different shots.\n"
                                   "Here you can specify the context name (key) and its corresponding value.\n"
                                   "Full details of how to set up contexts and add them to your config can be found in the OpenColorIO documentation:\n"
                                   "http://opencolorio.org/userguide/contexts.html");
    KnobGroupPtr context = AppManager::createKnob<KnobGroup>(holder, tr("OCIO Context"));
    context->setName(kNatronOcioParamContext);
    context->setHintToolTip(contextHint);
    context->setDefaultValue(false);
    page->addKnob(context);
    _context = context;
    for (int i = 0; i < 4; ++i) {
        const std::string n = std::to_string(i + 1);
        KnobStringPtr key = AppManager::createKnob<KnobString>(holder, tr("Context Key %1").arg(i + 1));
        key->setName("contextKey" + n);
        key->setHintToolTip(contextHint);
        key->setAddNewLine(false);
        context->addKnob(key);
        _contextKeys[i] = key;

        KnobStringPtr value = AppManager::createKnob<KnobString>(holder, tr("Context Value %1").arg(i + 1));
        value->setName("contextValue" + n);
        value->setHintToolTip(contextHint);
        context->addKnob(value);
        _contextValues[i] = value;
    }

    refreshMenu();
}

void
OcioSpaceKnobs::createMenu(KnobHolder* holder,
                           const KnobPagePtr& page,
                           const Description& description,
                           int index)
{
    // Persistent and evaluating: the menu alone records the choice, so it decides the render.
    KnobChoicePtr menu = AppManager::createKnob<KnobChoice>(holder, description.label);
    menu->setName(description.name);
    menu->setHintToolTip(description.hint);
    menu->setAnimationEnabled(false);
    menu->setCascading(true);
    menu->setDefaultValue(0);
    if (index < 0) {
        page->addKnob(menu);
    } else {
        page->insertKnob(index, menu);
    }
    _menu = menu;
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
    _defaultHint = description.defaultHint;
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
    createMenu(holder, page, description, index);

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
    KnobChoicePtr menu = _menu.lock();
    const std::string chosen = menu ? menu->getActiveEntry().id : std::string();

    if (!chosen.empty() && (chosen != kProjectDefaultID)) {
        return chosen;
    }
    std::lock_guard<std::mutex> lock(_defaultMutex);

    return _defaultSpace;
}

bool
OcioSpaceKnobs::isProjectDefault() const
{
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
    if (!_automaticSpace) {
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
    refreshMenu();
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
    ProjectPtr p = project();
    ProjectColorManagementPtr colorManagement = p ? p->getColorManagement() : ProjectColorManagementPtr();

    if (!menu || !colorManagement) {
        return;
    }
    std::string resolved;
    {
        std::lock_guard<std::mutex> lock(_defaultMutex);
        resolved = _defaultSpace;
    }
    // A role or alias is shown as the colourspace it designates, as the menu lists it.
    const std::string named = resolved.empty() ? std::string() : IoColorSpace::nameInConfig(colorManagement->getConfig(), resolved);
    std::vector<ChoiceOption> options(1, projectDefaultOption(named.empty() ? resolved : named, _defaultHint));
    const std::vector<ChoiceOption> spaces = Project::colorSpaceOptions(*colorManagement);
    options.insert(options.end(), spaces.begin(), spaces.end());
    menu->populateChoices(options);
}

void
OcioSpaceKnobs::setHidden(bool hidden)
{
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
}

void
OcioSpaceKnobs::sourceChanged()
{
    AppInstancePtr app = _holder ? _holder->getApp() : AppInstancePtr();
    const bool restoring = app && (app->isCreatingNodeTree() || (app->getProject() && app->getProject()->isLoadingProject()));

    if (!restoring) {
        refreshProjectDefault();
    }
}

bool
OcioSpaceKnobs::knobChanged(KnobI* k,
                            ValueChangedReasonEnum reason)
{
    KnobStringPtr configFile = _configFile.lock();
    KnobStringPtr workingSpace = _workingSpace.lock();
    KnobChoicePtr menu = _menu.lock();

    if (!configFile || !workingSpace || !menu || reason == eValueChangedReasonTimeChanged) {
        return false;
    }
    if (k == configFile.get() || k == workingSpace.get()) {
        refreshMenu();
        // The config's file rules and the working space are what a default may stand for.
        refreshProjectDefault();

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

NATRON_NAMESPACE_EXIT
