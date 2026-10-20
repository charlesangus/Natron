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

#include "ModifyMetadata.h"

#include <string>
#include <vector>

#include "Engine/ChoiceOption.h"
#include "Engine/KnobTypes.h"

NATRON_NAMESPACE_ENTER

ModifyMetadata::ModifyMetadata(NodePtr node)
    : NativeEffectBase(node)
    , _slots()
{
}

void
ModifyMetadata::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    for (int i = 0; i < kModifyMetadataSlotCount; ++i) {
        Slot& slot = _slots[i];
        const std::string suffix = std::to_string(i + 1);

        KnobGroupPtr group = createKnob<KnobGroup>(tr("Operation %1").arg(i + 1));
        group->setName("slot" + suffix);
        group->setDefaultValue(i == 0);
        page->addKnob(group);

        KnobChoicePtr operation = createKnob<KnobChoice>(tr("Operation"));
        operation->setName("operation" + suffix);
        operation->setAnimationEnabled(false);
        {
            std::vector<ChoiceOption> choices;
            choices.push_back(ChoiceOption("none", tr("None").toStdString(), tr("Does nothing.").toStdString()));
            choices.push_back(ChoiceOption("set", tr("Set").toStdString(), tr("Sets the key to a value, replacing any existing one.").toStdString()));
            choices.push_back(ChoiceOption("remove", tr("Remove").toStdString(), tr("Removes the key.").toStdString()));
            choices.push_back(ChoiceOption("rename", tr("Rename").toStdString(), tr("Moves the value of the key to another key.").toStdString()));
            operation->populateChoices(choices);
        }
        operation->setDefaultValue((int)eOperationNone);
        group->addKnob(operation);
        slot.operation = operation;

        KnobStringPtr key = createKnob<KnobString>(tr("Key"));
        key->setName("key" + suffix);
        key->setAnimationEnabled(false);
        key->setHintToolTip(tr("The metadata key, with its prefix, for example exr/owner."));
        group->addKnob(key);
        slot.key = key;

        KnobStringPtr renameTo = createKnob<KnobString>(tr("New Key"));
        renameTo->setName("renameTo" + suffix);
        renameTo->setAnimationEnabled(false);
        renameTo->setHintToolTip(tr("The key the value is moved to. An existing value under this key is replaced."));
        group->addKnob(renameTo);
        slot.renameTo = renameTo;

        KnobChoicePtr type = createKnob<KnobChoice>(tr("Type"));
        type->setName("type" + suffix);
        type->setAnimationEnabled(false);
        {
            std::vector<ChoiceOption> choices;
            choices.push_back(ChoiceOption("int", tr("Integer").toStdString(), ""));
            choices.push_back(ChoiceOption("double", tr("Float").toStdString(), ""));
            choices.push_back(ChoiceOption("string", tr("String").toStdString(), ""));
            type->populateChoices(choices);
        }
        type->setDefaultValue((int)eValueTypeInt);
        group->addKnob(type);
        slot.type = type;

        KnobIntPtr intValue = createKnob<KnobInt>(tr("Value"));
        intValue->setName("intValue" + suffix);
        group->addKnob(intValue);
        slot.intValue = intValue;

        KnobDoublePtr doubleValue = createKnob<KnobDouble>(tr("Value"));
        doubleValue->setName("doubleValue" + suffix);
        group->addKnob(doubleValue);
        slot.doubleValue = doubleValue;

        KnobStringPtr stringValue = createKnob<KnobString>(tr("Value"));
        stringValue->setName("stringValue" + suffix);
        group->addKnob(stringValue);
        slot.stringValue = stringValue;

        refreshSlotVisibility(slot);
    }
}

void
ModifyMetadata::refreshSlotVisibility(const Slot& slot)
{
    KnobChoicePtr operation = slot.operation.lock();
    KnobChoicePtr type = slot.type.lock();

    if (!operation || !type) {
        return;
    }
    const int op = operation->getValue();
    const int valueType = type->getValue();
    const bool isSet = op == eOperationSet;

    KnobStringPtr key = slot.key.lock();
    KnobStringPtr renameTo = slot.renameTo.lock();
    KnobIntPtr intValue = slot.intValue.lock();
    KnobDoublePtr doubleValue = slot.doubleValue.lock();
    KnobStringPtr stringValue = slot.stringValue.lock();

    if (key) {
        key->setSecret(op == eOperationNone);
    }
    if (renameTo) {
        renameTo->setSecret(op != eOperationRename);
    }
    type->setSecret(!isSet);
    if (intValue) {
        intValue->setSecret(!isSet || valueType != eValueTypeInt);
    }
    if (doubleValue) {
        doubleValue->setSecret(!isSet || valueType != eValueTypeDouble);
    }
    if (stringValue) {
        stringValue->setSecret(!isSet || valueType != eValueTypeString);
    }
}

bool
ModifyMetadata::knobChanged(KnobI* k,
                            ValueChangedReasonEnum /*reason*/,
                            ViewSpec /*view*/,
                            double /*time*/,
                            bool /*originatedFromMainThread*/)
{
    for (const Slot& slot : _slots) {
        KnobChoicePtr operation = slot.operation.lock();
        KnobChoicePtr type = slot.type.lock();

        if ((operation && k == operation.get()) || (type && k == type.get())) {
            refreshSlotVisibility(slot);

            return true;
        }
    }

    return false;
}

void
ModifyMetadata::onKnobsLoaded()
{
    for (const Slot& slot : _slots) {
        refreshSlotVisibility(slot);
    }
}

Route
ModifyMetadata::route(double time,
                      ViewIdx /*view*/)
{
    return Route::input(0, time);
}

ImageMetadata
ModifyMetadata::deriveOutputMetadata(double time,
                                     ViewIdx view)
{
    ImageMetadata metadata = getUpstreamMetadata(time, view);

    for (const Slot& slot : _slots) {
        KnobChoicePtr operation = slot.operation.lock();
        KnobStringPtr keyKnob = slot.key.lock();

        if (!operation || !keyKnob) {
            continue;
        }
        const int op = operation->getValueAtTime(time, 0, view);
        if (op == eOperationNone) {
            continue;
        }
        const std::string key = keyKnob->getValueAtTime(time, 0, view);
        if (key.empty()) {
            continue;
        }

        switch (op) {
        case eOperationSet: {
            KnobChoicePtr type = slot.type.lock();
            const int valueType = type ? type->getValueAtTime(time, 0, view) : (int)eValueTypeInt;
            KnobIntPtr intValue = slot.intValue.lock();
            KnobDoublePtr doubleValue = slot.doubleValue.lock();
            KnobStringPtr stringValue = slot.stringValue.lock();

            if ((valueType == eValueTypeInt) && intValue) {
                metadata.setInt(key, intValue->getValueAtTime(time, 0, view));
            } else if ((valueType == eValueTypeDouble) && doubleValue) {
                metadata.setDouble(key, doubleValue->getValueAtTime(time, 0, view));
            } else if ((valueType == eValueTypeString) && stringValue) {
                metadata.setString(key, stringValue->getValueAtTime(time, 0, view));
            }
            break;
        }
        case eOperationRemove:
            metadata.remove(key);
            break;
        case eOperationRename: {
            KnobStringPtr renameTo = slot.renameTo.lock();
            const std::string newKey = renameTo ? renameTo->getValueAtTime(time, 0, view) : std::string();
            const ImageMetadata::Value* found = metadata.find(key);

            if (newKey.empty() || (newKey == key) || !found) {
                break;
            }
            const ImageMetadata::Value value = *found;
            metadata.remove(key);
            metadata.set(newKey, value);
            break;
        }
        default:
            break;
        }
    }

    return metadata;
}

NATRON_NAMESPACE_EXIT
