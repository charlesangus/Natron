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

#include "AuthoredKnobSet.h"

#include <string>

#include <QString>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_ENTER

namespace {
// Whether holder's knobs are being restored from a project or a saved node, whose toggles already
// hold the saved state.
bool
isRestoring(KnobHolder* holder)
{
    EffectInstance* effect = dynamic_cast<EffectInstance*>(holder);
    NodePtr node = effect ? effect->getNode() : NodePtr();

    if (node && node->isRestoringKnobs()) {
        return true;
    }
    AppInstancePtr app = holder ? holder->getApp() : AppInstancePtr();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();

    return project && project->isLoadingProject();
}
} // anonymous namespace

KnobBoolPtr
AuthoredKnobSet::add(KnobHolder* holder,
                     const KnobPagePtr& page,
                     const KnobIPtr& knob)
{
    KnobBoolPtr authored = AppManager::createKnob<KnobBool>(holder, std::string());
    const QString label = QString::fromStdString(knob->getLabel());

    authored->setName(knob->getName() + "Authored");
    authored->setHintToolTip(EffectInstance::tr("Whether %1 writes an opinion. Off, the node leaves the attribute as its input "
                                                "scene has it. Editing %1 turns this on; restoring %1 to its default turns it off.")
                                 .arg(label)
                                 .toStdString());
    authored->setAnimationEnabled(false);
    authored->setDefaultValue(false);
    authored->setAddNewLine(false);
    page->addKnob(authored);
    page->addKnob(knob);

    {
        std::lock_guard<std::mutex> l(_lock);
        Pair pair;
        pair.value = knob;
        pair.authored = authored;
        _pairs.push_back(pair);
    }

    // Setting an expression bypasses knobChanged().
    KnobSignalSlotHandlerPtr handler = knob->getSignalSlotHandler();
    if (handler) {
        const KnobIWPtr weakValue = knob;
        const KnobBoolWPtr weakAuthored = authored;
        QObject::connect(handler.get(), &KnobSignalSlotHandler::expressionChanged, handler.get(), [weakValue, weakAuthored](int dimension) {
            KnobIPtr value = weakValue.lock();
            KnobBoolPtr companion = weakAuthored.lock();
            if ( !value || !companion || value->getExpression(dimension).empty() || companion->getValue() ) {
                return;
            }
            if ( isRestoring( value->getHolder() ) ) {
                return;
            }
            companion->setValue(true); }, Qt::DirectConnection);
    }

    return authored;
}

bool
AuthoredKnobSet::onKnobChanged(KnobI* k,
                               ValueChangedReasonEnum reason)
{
    KnobBoolPtr authored = getCompanion(k);

    if (!authored) {
        return false;
    }
    if (isRestoring(k->getHolder())) {
        return true;
    }
    switch (reason) {
    case eValueChangedReasonUserEdited:
    case eValueChangedReasonNatronGuiEdited:
    case eValueChangedReasonNatronInternalEdited:
        if (!authored->getValue()) {
            authored->setValue(true);
        }
        break;
    case eValueChangedReasonRestoreDefault:
        // A reset of one dimension of several leaves the others' edits authored.
        k->computeHasModifications();
        if (!k->hasModifications() && authored->getValue()) {
            authored->setValue(false);
        }
        break;
    case eValueChangedReasonPluginEdited:
    case eValueChangedReasonTimeChanged:
    case eValueChangedReasonSlaveRefresh:
        break;
    }

    return true;
}

void
AuthoredKnobSet::forEachAuthored(const std::function<void(const KnobIPtr&)>& fn) const
{
    std::vector<Pair> pairs;
    {
        std::lock_guard<std::mutex> l(_lock);
        pairs = _pairs;
    }
    for (const Pair& pair : pairs) {
        KnobIPtr value = pair.value.lock();
        KnobBoolPtr authored = pair.authored.lock();
        if (value && authored && authored->getValue()) {
            fn(value);
        }
    }
}

KnobBoolPtr
AuthoredKnobSet::getCompanion(const KnobI* knob) const
{
    std::lock_guard<std::mutex> l(_lock);

    for (const Pair& pair : _pairs) {
        KnobIPtr value = pair.value.lock();
        if (value && (value.get() == knob)) {
            return pair.authored.lock();
        }
    }

    return KnobBoolPtr();
}

void
AuthoredKnobSet::deleteAll(KnobHolder* holder)
{
    std::vector<Pair> pairs;
    {
        std::lock_guard<std::mutex> l(_lock);
        pairs.swap(_pairs);
    }
    for (const Pair& pair : pairs) {
        KnobIPtr value = pair.value.lock();
        KnobBoolPtr authored = pair.authored.lock();
        if (value) {
            holder->deleteKnob(value.get(), false);
        }
        if (authored) {
            holder->deleteKnob(authored.get(), false);
        }
    }
}

std::size_t
AuthoredKnobSet::size() const
{
    std::lock_guard<std::mutex> l(_lock);

    return _pairs.size();
}

NATRON_NAMESPACE_EXIT
