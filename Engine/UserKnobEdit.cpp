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

#include "UserKnobEdit.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <map>
#include <stdexcept>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/EffectInstance.h"
#include "Engine/GraphEditRecorder.h"
#include "Engine/Knob.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobFactory.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobLinkGroup.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeGroup.h"
#include "Engine/PythonUndoCommands.h"
#include "Engine/RFV.h"
#include "Engine/UndoCommand.h"
#include "Serialization/Yaml/KnobYaml.h"

NATRON_NAMESPACE_ENTER

NATRON_NAMESPACE_ANONYMOUS_ENTER

NodePtr
nodeOfHolder(KnobHolder* holder)
{
    EffectInstance* effect = dynamic_cast<EffectInstance*>(holder);

    return effect ? effect->getNode() : NodePtr();
}

// The types and dimensions UserParamHolder's create*Param functions make.
bool
isCreatableUserKnobType(const std::string& typeName,
                        int dimension)
{
    if ((typeName == KnobInt::typeNameStatic()) || (typeName == KnobDouble::typeNameStatic())) {
        return (dimension >= 1) && (dimension <= 3);
    }
    if (typeName == KnobColor::typeNameStatic()) {
        return (dimension == 3) || (dimension == 4);
    }
    if (typeName == KnobParametric::typeNameStatic()) {
        return dimension >= 1;
    }
    const std::string singleDimensionTypes[] = {
        KnobBool::typeNameStatic(),
        KnobChoice::typeNameStatic(),
        KnobString::typeNameStatic(),
        KnobFile::typeNameStatic(),
        KnobOutputFile::typeNameStatic(),
        KnobPath::typeNameStatic(),
        KnobChannelSet::typeNameStatic(),
        KnobLayerSelect::typeNameStatic(),
        KnobChannelSelect::typeNameStatic(),
        KnobButton::typeNameStatic(),
        KnobSeparator::typeNameStatic(),
        KnobPage::typeNameStatic(),
        KnobGroup::typeNameStatic(),
    };
    for (const std::string& name : singleDimensionTypes) {
        if (typeName == name) {
            return dimension == 1;
        }
    }

    return false;
}

bool
isScriptIdentifier(const std::string& name)
{
    if (name.empty() || ((name[0] >= '0') && (name[0] <= '9'))) {
        return false;
    }
    for (char c : name) {
        const bool ok = ((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z')) || ((c >= '0') && (c <= '9')) || (c == '_');
        if (!ok) {
            return false;
        }
    }

    return true;
}

// A page's own list also holds the knobs inside its groups.
std::vector<KnobIPtr>
directChildren(const KnobIPtr& parent)
{
    std::vector<KnobIPtr> children;
    if (KnobGroup* isGroup = dynamic_cast<KnobGroup*>(parent.get())) {
        children = isGroup->getChildren();
    } else if (KnobPage* isPage = dynamic_cast<KnobPage*>(parent.get())) {
        children = isPage->getChildren();
    }
    std::vector<KnobIPtr> ret;
    for (std::size_t i = 0; i < children.size(); ++i) {
        if (children[i] && (children[i]->getParentKnob() == parent)) {
            ret.push_back(children[i]);
        }
    }

    return ret;
}

int
indexInParent(const KnobIPtr& knob)
{
    const KnobIPtr parent = knob->getParentKnob();
    if (!parent) {
        return -1;
    }
    const std::vector<KnobIPtr> children = directChildren(parent);
    for (std::size_t i = 0; i < children.size(); ++i) {
        if (children[i] == knob) {
            return (int)i;
        }
    }

    return -1;
}

bool
isAncestorOrSelf(const KnobIPtr& ancestor,
                 KnobIPtr knob)
{
    while (knob) {
        if (knob == ancestor) {
            return true;
        }
        knob = knob->getParentKnob();
    }

    return false;
}

KnobPagePtr
pageOf(KnobIPtr knob)
{
    while (knob) {
        KnobPagePtr isPage = std::dynamic_pointer_cast<KnobPage>(knob);
        if (isPage) {
            return isPage;
        }
        knob = knob->getParentKnob();
    }

    return KnobPagePtr();
}

KnobIPtr
findUserContainer(KnobHolder* holder,
                  const std::string& name)
{
    KnobIPtr knob = holder->getKnobByName(name);
    if (!knob || !knob->isUserKnob()) {
        return KnobIPtr();
    }
    if (!dynamic_cast<KnobPage*>(knob.get()) && !dynamic_cast<KnobGroup*>(knob.get())) {
        return KnobIPtr();
    }

    return knob;
}

bool
insertInContainer(const KnobIPtr& parent,
                  const KnobIPtr& knob,
                  int index)
{
    KnobPage* parentPage = dynamic_cast<KnobPage*>(parent.get());
    if (parentPage && (knob->getParentKnob() != parent)) {
        // A page's list can still hold a knob that was in one of its groups, and
        // KnobPage::insertKnob() leaves a knob it already lists unparented where it is.
        parentPage->removeKnob(knob.get());
    }
    const std::vector<KnobIPtr> siblings = directChildren(parent);
    if ((index < 0) || (index > (int)siblings.size())) {
        index = (int)siblings.size();
    }
    // The container's list holds knobs that are not its direct children, so the index is
    // translated to a position in that list.
    int position = INT_MAX;
    if (index < (int)siblings.size()) {
        std::vector<KnobIPtr> all;
        if (KnobGroup* isGroup = dynamic_cast<KnobGroup*>(parent.get())) {
            all = isGroup->getChildren();
        } else if (KnobPage* isPage = dynamic_cast<KnobPage*>(parent.get())) {
            all = isPage->getChildren();
        }
        std::vector<KnobIPtr>::const_iterator found = std::find(all.begin(), all.end(), siblings[index]);
        position = (int)std::distance(all.cbegin(), found);
    }
    if (KnobGroup* isGroup = dynamic_cast<KnobGroup*>(parent.get())) {
        return isGroup->insertKnob(position, knob);
    }
    if (parentPage) {
        parentPage->insertKnob(position, knob);

        return knob->getParentKnob() == parent;
    }

    return false;
}

// The checks KnobPage::insertKnob() and KnobGroup::insertKnob() make, so that a move can be
// refused before the knob leaves its current parent.
bool
acceptsChild(const KnobIPtr& parent,
             const KnobIPtr& knob)
{
    if (!parent || (!parent->isUserKnob() && knob->isUserKnob())) {
        return false;
    }
    if (KnobGroup* isGroup = dynamic_cast<KnobGroup*>(parent.get())) {
        return !isGroup->isRFVGroup() || RFV::isAdmittedInRFVGroup(*isGroup, knob);
    }

    return dynamic_cast<KnobPage*>(parent.get()) != nullptr;
}

KnobIPtr
destinationOf(KnobHolder* holder,
              const std::string& parentName)
{
    return parentName.empty() ? KnobIPtr(holder->getUserPageKnob()) : findUserContainer(holder, parentName);
}

// An index of -1 in the knob's own parent keeps it where it is.
bool
isPlacedAt(KnobHolder* holder,
           const KnobIPtr& knob,
           const std::string& parentName,
           int index)
{
    const KnobIPtr parent = destinationOf(holder, parentName);
    if (!parent || (knob->getParentKnob() != parent)) {
        return false;
    }
    const int current = indexInParent(knob);
    const int last = (int)directChildren(parent).size() - 1;

    return (index < 0) || (index == current) || ((index > last) && (current == last));
}

// An empty parentName is the holder's User page, created when missing.
bool
placeKnob(KnobHolder* holder,
          const KnobIPtr& knob,
          const std::string& parentName,
          int index)
{
    if (isPlacedAt(holder, knob, parentName, index)) {
        return true;
    }
    KnobIPtr parent = parentName.empty() ? KnobIPtr(holder->getOrCreateUserPageKnob()) : findUserContainer(holder, parentName);
    if (!acceptsChild(parent, knob)) {
        return false;
    }
    const KnobIPtr oldParent = knob->getParentKnob();
    const int oldIndex = indexInParent(knob);
    knob->resetParent();
    if (insertInContainer(parent, knob, index)) {
        return true;
    }
    if (oldParent) {
        insertInContainer(oldParent, knob, oldIndex);
    }

    return false;
}

// An expression is validated by running it, and fails on a parameter not yet declared to Python:
// declare before setting expressions that name a new or renamed parameter, or that run on one.
void
declareToPython(KnobHolder* holder)
{
    NodePtr node = nodeOfHolder(holder);
    if (node) {
        node->declarePythonFields();
    }
}

void
refreshHolder(KnobHolder* holder)
{
    holder->recreateUserKnobs(true);
    // recreateUserKnobs() only declares the knobs to Python when a settings panel exists.
    declareToPython(holder);
}

Serialization::KnobValueSerialization
toStorage(const Serialization::KnobValueSerialization& value,
          Serialization::KnobStorageTypeEnum storage,
          bool roundToInteger)
{
    double number = 0.;
    std::string text;
    if (const bool* b = std::get_if<bool>(&value)) {
        number = *b ? 1. : 0.;
        text = *b ? "1" : "0";
    } else if (const int* i = std::get_if<int>(&value)) {
        number = *i;
        text = std::to_string(*i);
    } else if (const double* d = std::get_if<double>(&value)) {
        number = *d;
        text = std::to_string(*d);
    } else if (const std::string* s = std::get_if<std::string>(&value)) {
        text = *s;
    }
    switch (storage) {
    case Serialization::eKnobStorageTypeBool:
        return Serialization::KnobValueSerialization(std::in_place_type<bool>, number != 0.);
    case Serialization::eKnobStorageTypeInt: {
        const double rounded = std::round(std::max((double)INT_MIN, std::min((double)INT_MAX, number)));

        return Serialization::KnobValueSerialization(std::in_place_type<int>, (int)rounded);
    }
    case Serialization::eKnobStorageTypeDouble:
        return Serialization::KnobValueSerialization(std::in_place_type<double>, roundToInteger ? std::round(number) : number);
    case Serialization::eKnobStorageTypeString:
        return Serialization::KnobValueSerialization(std::in_place_type<std::string>, text);
    case Serialization::eKnobStorageTypeNone:
        break;
    }

    return value;
}

double
clampToInt(double value)
{
    return std::max((double)INT_MIN, std::min((double)INT_MAX, value));
}

Serialization::KnobSerialization
serializationFromSpec(const UserKnobSpec& spec)
{
    Serialization::KnobSerialization serialization;
    serialization.name = spec.name;
    serialization.typeName = spec.typeName;
    serialization.dimension = spec.dimension;
    serialization.secret = spec.secret;
    serialization.isUserKnob = true;
    serialization.label = spec.label;
    serialization.tooltip = spec.tooltip;
    serialization.triggerNewLine = spec.newLine;
    serialization.evaluatesOnChange = spec.evaluatesOnChange;
    serialization.animationEnabled = spec.animates;
    serialization.isPersistent = true;
    serialization.extraData = spec.extra;
    if (spec.useHostOverlayHandle) {
        serialization.useHostOverlayHandle = true;
    }
    serialization.dimensions.resize(spec.dimension);
    for (int i = 0; i < spec.dimension; ++i) {
        serialization.dimensions[i].enabled = true;
    }

    const Serialization::KnobTypeTraits traits = Serialization::getKnobTypeTraits(spec.typeName);
    if (traits.isChoice) {
        const Serialization::KnobChoiceExtraSerialization* choice = std::get_if<Serialization::KnobChoiceExtraSerialization>(&spec.extra);
        int index = 0;
        if (choice && !spec.defaults.empty()) {
            const std::string* id = std::get_if<std::string>(&spec.defaults[0]);
            std::vector<std::string>::const_iterator found = id ? std::find(choice->entries.begin(), choice->entries.end(), *id) : choice->entries.end();
            if (found != choice->entries.end()) {
                index = (int)std::distance(choice->entries.begin(), found);
                serialization.choiceOptionID = *id;
            }
        }
        serialization.dimensions[0].value = Serialization::KnobValueSerialization(std::in_place_type<int>, index);
        serialization.dimensions[0].defaultValue = serialization.dimensions[0].value;
    } else if (traits.hasValue) {
        for (int i = 0; i < spec.dimension && i < (int)spec.defaults.size(); ++i) {
            serialization.dimensions[i].defaultValue = toStorage(spec.defaults[i], traits.storageType, false);
            serialization.dimensions[i].value = serialization.dimensions[i].defaultValue;
        }
    }
    if (traits.isInt) {
        Serialization::KnobValueRangeSerialization* range = std::get_if<Serialization::KnobValueRangeSerialization>(&serialization.extraData);
        if (range) {
            range->minimum = clampToInt(range->minimum);
            range->maximum = clampToInt(range->maximum);
            range->displayMinimum = clampToInt(range->displayMinimum);
            range->displayMaximum = clampToInt(range->displayMaximum);
        }
    }

    return serialization;
}

template <typename T>
void
applyRange(Knob<T>* knob,
           const Serialization::KnobValueRangeSerialization& range)
{
    std::vector<T> minimums, maximums, displayMinimums, displayMaximums;
    for (int i = 0; i < knob->getDimension(); ++i) {
        minimums.push_back(static_cast<T>(range.minimum));
        maximums.push_back(static_cast<T>(range.maximum));
        displayMinimums.push_back(static_cast<T>(range.displayMinimum));
        displayMaximums.push_back(static_cast<T>(range.displayMaximum));
    }
    knob->setMinimumsAndMaximums(minimums, maximums);
    knob->setDisplayMinimumsAndMaximums(displayMinimums, displayMaximums);
}

void
applyDefaults(const KnobIPtr& knob,
              const UserKnobSpec& spec)
{
    if (spec.defaults.empty()) {
        return;
    }
    if (KnobChoice* isChoice = dynamic_cast<KnobChoice*>(knob.get())) {
        if (const std::string* id = std::get_if<std::string>(&spec.defaults[0])) {
            isChoice->setDefaultValueFromIDWithoutApplying(*id);
        }

        return;
    }
    const int count = std::min(knob->getDimension(), (int)spec.defaults.size());
    KnobIntBase* isInt = dynamic_cast<KnobIntBase*>(knob.get());
    KnobBoolBase* isBool = dynamic_cast<KnobBoolBase*>(knob.get());
    KnobDoubleBase* isDouble = dynamic_cast<KnobDoubleBase*>(knob.get());
    KnobStringBase* isString = dynamic_cast<KnobStringBase*>(knob.get());
    for (int i = 0; i < count; ++i) {
        if (isInt) {
            isInt->setDefaultValueWithoutApplying(std::get<int>(toStorage(spec.defaults[i], Serialization::eKnobStorageTypeInt, false)), i);
        } else if (isDouble) {
            isDouble->setDefaultValueWithoutApplying(std::get<double>(toStorage(spec.defaults[i], Serialization::eKnobStorageTypeDouble, false)), i);
        } else if (isString) {
            isString->setDefaultValueWithoutApplying(std::get<std::string>(toStorage(spec.defaults[i], Serialization::eKnobStorageTypeString, false)), i);
        } else if (isBool) {
            isBool->setDefaultValueWithoutApplying(std::get<bool>(toStorage(spec.defaults[i], Serialization::eKnobStorageTypeBool, false)), i);
        }
    }
}

void
applyExtra(const KnobIPtr& knob,
           const Serialization::KnobExtraSerialization& extra)
{
    if (const Serialization::KnobChoiceExtraSerialization* choice = std::get_if<Serialization::KnobChoiceExtraSerialization>(&extra)) {
        KnobChoice* isChoice = dynamic_cast<KnobChoice*>(knob.get());
        if (isChoice) {
            std::vector<ChoiceOption> options(choice->entries.size());
            for (std::size_t i = 0; i < options.size(); ++i) {
                options[i].id = choice->entries[i];
                if (i < choice->helpStrings.size()) {
                    options[i].tooltip = choice->helpStrings[i];
                }
            }
            isChoice->populateChoices(options);
        }
    } else if (const Serialization::KnobValueRangeSerialization* range = std::get_if<Serialization::KnobValueRangeSerialization>(&extra)) {
        if (KnobIntBase* isInt = dynamic_cast<KnobIntBase*>(knob.get())) {
            Serialization::KnobValueRangeSerialization clamped = *range;
            clamped.minimum = clampToInt(clamped.minimum);
            clamped.maximum = clampToInt(clamped.maximum);
            clamped.displayMinimum = clampToInt(clamped.displayMinimum);
            clamped.displayMaximum = clampToInt(clamped.displayMaximum);
            applyRange<int>(isInt, clamped);
        } else if (KnobDoubleBase* isDouble = dynamic_cast<KnobDoubleBase*>(knob.get())) {
            applyRange<double>(isDouble, *range);
        }
    } else if (const Serialization::KnobFileExtraSerialization* file = std::get_if<Serialization::KnobFileExtraSerialization>(&extra)) {
        if (KnobFile* isFile = dynamic_cast<KnobFile*>(knob.get())) {
            isFile->setAsInputImage(file->useSequences);
        } else if (KnobOutputFile* isOutputFile = dynamic_cast<KnobOutputFile*>(knob.get())) {
            isOutputFile->setAsOutputImageFile(file->useSequences);
        }
    } else if (const Serialization::KnobPathExtraSerialization* path = std::get_if<Serialization::KnobPathExtraSerialization>(&extra)) {
        if (KnobPath* isPath = dynamic_cast<KnobPath*>(knob.get())) {
            isPath->setMultiPath(path->multiPath);
        }
    } else if (const Serialization::KnobLayerSelectExtraSerialization* layer = std::get_if<Serialization::KnobLayerSelectExtraSerialization>(&extra)) {
        if (KnobLayerSelect* isLayerSelect = dynamic_cast<KnobLayerSelect*>(knob.get())) {
            isLayerSelect->setWithChannelButtons(layer->withChannelButtons);
        } else if (KnobChannelSet* isChannelSet = dynamic_cast<KnobChannelSet*>(knob.get())) {
            isChannelSet->setWithChannelButtons(layer->withChannelButtons);
        }
    } else if (const Serialization::KnobTextExtraSerialization* text = std::get_if<Serialization::KnobTextExtraSerialization>(&extra)) {
        if (KnobString* isString = dynamic_cast<KnobString*>(knob.get())) {
            isString->setAsLabel(text->isLabel);
            isString->setAsMultiLine(!text->isLabel && text->isMultiLine);
            isString->setUsesRichText(!text->isLabel && text->isMultiLine && text->usesRichText);
        }
    } else if (const Serialization::KnobParametricExtraSerialization* parametric = std::get_if<Serialization::KnobParametricExtraSerialization>(&extra)) {
        if (KnobParametric* isParametric = dynamic_cast<KnobParametric*>(knob.get())) {
            for (int i = 0; i < isParametric->getDimension(); ++i) {
                if (i < (int)parametric->curveLabels.size()) {
                    isParametric->setDimensionName(i, parametric->curveLabels[i]);
                }
                if ((i < (int)parametric->curveColors.size()) && (parametric->curveColors[i].size() == 3)) {
                    const std::vector<double>& color = parametric->curveColors[i];
                    isParametric->setCurveColor(i, color[0], color[1], color[2]);
                }
            }
            isParametric->setParametricRange(parametric->rangeMinimum, parametric->rangeMaximum);
        }
    } else if (const Serialization::KnobButtonExtraSerialization* button = std::get_if<Serialization::KnobButtonExtraSerialization>(&extra)) {
        if (KnobButton* isButton = dynamic_cast<KnobButton*>(knob.get())) {
            isButton->setScript(button->script);
        }
    }
}

struct DimensionExpression {
    std::string expression;
    bool hasRet;
};

std::map<KnobIPtr, std::vector<DimensionExpression>>
listenerExpressions(const KnobIPtr& knob)
{
    std::map<KnobIPtr, std::vector<DimensionExpression>> ret;
    KnobI::ListenerDimsMap listeners;
    knob->getListeners(listeners);
    for (KnobI::ListenerDimsMap::const_iterator it = listeners.begin(); it != listeners.end(); ++it) {
        KnobIPtr listener = it->first.lock();
        if (!listener || (listener == knob)) {
            continue;
        }
        bool isExpression = false;
        for (std::size_t i = 0; i < it->second.size(); ++i) {
            isExpression = isExpression || it->second[i].isExpr;
        }
        if (!isExpression) {
            continue;
        }
        std::vector<DimensionExpression>& expressions = ret[listener];
        for (int i = 0; i < listener->getDimension(); ++i) {
            DimensionExpression expression = { listener->getExpression(i), listener->isExpressionUsingRetVariable(i) };
            expressions.push_back(expression);
        }
    }

    return ret;
}

void
setExpressionNoThrow(const KnobIPtr& knob,
                     int dimension,
                     const std::string& expression,
                     bool hasRet)
{
    try {
        knob->setExpression(dimension, expression, hasRet, false);
    } catch (...) {
    }
}

void
renameInListeners(KnobHolder* holder,
                  const std::map<KnobIPtr, std::vector<DimensionExpression>>& listeners,
                  const std::string& oldName,
                  const std::string& newName)
{
    for (std::map<KnobIPtr, std::vector<DimensionExpression>>::const_iterator it = listeners.begin(); it != listeners.end(); ++it) {
        for (int i = 0; i < (int)it->second.size() && i < it->first->getDimension(); ++i) {
            const DimensionExpression& expression = it->second[i];
            if (expression.expression.empty()) {
                continue;
            }
            const std::string renamed = renameUserKnobInExpression(holder, it->first, expression.expression, oldName, newName);
            setExpressionNoThrow(it->first, i, renamed, expression.hasRet);
        }
    }
}

void
countLinkGroupIds(const Serialization::GroupKnobSerialization& group,
                  std::map<std::string, int>* counts)
{
    for (std::list<Serialization::GroupKnobChildSerialization>::const_iterator it = group.children.begin(); it != group.children.end(); ++it) {
        if (it->isGroup) {
            countLinkGroupIds(it->group, counts);
            continue;
        }
        for (std::size_t d = 0; d < it->knob.dimensions.size(); ++d) {
            const std::optional<Serialization::KnobLinkGroupSerialization>& linkGroup = it->knob.dimensions[d].linkGroup;
            if (linkGroup && !linkGroup->id.empty()) {
                ++(*counts)[linkGroup->id];
            }
        }
    }
}

// A link group with one copied member loses its id: it has no partner in the paste, and membership
// of a group outside the copy is dropped. One with several keeps its id, which links the pasted
// members together.
void
linkGroupsToMasters(const std::map<std::string, int>& counts,
                    Serialization::GroupKnobSerialization* group)
{
    for (std::list<Serialization::GroupKnobChildSerialization>::iterator it = group->children.begin(); it != group->children.end(); ++it) {
        if (it->isGroup) {
            linkGroupsToMasters(counts, &it->group);
            continue;
        }
        for (std::size_t d = 0; d < it->knob.dimensions.size(); ++d) {
            Serialization::KnobDimensionSerialization& dimension = it->knob.dimensions[d];
            if (!dimension.linkGroup) {
                continue;
            }
            std::map<std::string, int>::const_iterator count = counts.find(dimension.linkGroup->id);
            if ((count != counts.end()) && (count->second > 1)) {
                continue;
            }
            dimension.linkGroup.reset();
        }
    }
}

struct PastedKnob {
    KnobIPtr knob;
    Serialization::KnobSerialization serialization;
};

KnobIPtr
pasteChild(KnobHolder* holder,
           const Serialization::GroupKnobChildSerialization& child,
           const KnobIPtr& parent,
           int index,
           std::list<PastedKnob>* pasted,
           std::map<std::string, std::string>* renamed)
{
    if (child.isGroup) {
        const Serialization::GroupKnobSerialization& groupSerialization = child.group;
        const std::string groupName = uniqueKnobName(holder, groupSerialization.name);
        KnobGroupPtr group = AppManager::createKnob<KnobGroup>(holder, groupSerialization.label, 1, false);
        group->setAsUserKnob(true);
        group->setName(groupName);
        group->setAsTab(groupSerialization.isSetAsTab);
        group->setSecret(groupSerialization.secret);
        group->setHintToolTip(groupSerialization.tooltip);
        if (groupSerialization.isRFVGroup) {
            group->setAsRFVGroup(true);
        }
        if (!insertInContainer(parent, group, index)) {
            holder->deleteKnob(group.get(), false);

            return KnobIPtr();
        }
        group->setValue(groupSerialization.isOpened);
        for (std::list<Serialization::GroupKnobChildSerialization>::const_iterator it = groupSerialization.children.begin(); it != groupSerialization.children.end(); ++it) {
            pasteChild(holder, *it, group, -1, pasted, renamed);
        }
        (*renamed)[groupSerialization.name] = groupName;

        return group;
    }

    Serialization::KnobSerialization serialization = child.knob;
    serialization.name = uniqueKnobName(holder, serialization.name);
    KnobIPtr knob = createUserKnobFromSerialization(holder, serialization);
    if (!knob) {
        return KnobIPtr();
    }
    if (!insertInContainer(parent, knob, index)) {
        holder->deleteKnob(knob.get(), false);

        return KnobIPtr();
    }
    PastedKnob entry = { knob, serialization };
    pasted->push_back(entry);
    (*renamed)[child.knob.name] = serialization.name;

    return knob;
}

void
restorePastedExpressionsAndLinks(KnobHolder* holder,
                                 const std::list<PastedKnob>& pasted,
                                 const std::map<std::string, std::string>& renamed)
{
    // Through placeholders: with a renamed to a2 and a2 to a3, renaming in place would turn a
    // into a3.
    // The prefix is lengthened until no knob name or expression text contains it, so a real knob can
    // never be mistaken for a placeholder.
    std::string placeholderPrefix = "natronPastedKnob";
    {
        std::vector<std::string> texts;
        for (std::map<std::string, std::string>::const_iterator it = renamed.begin(); it != renamed.end(); ++it) {
            texts.push_back(it->first);
            texts.push_back(it->second);
        }
        for (std::list<PastedKnob>::const_iterator it = pasted.begin(); it != pasted.end(); ++it) {
            texts.push_back(it->knob->getName());
            for (std::size_t d = 0; d < it->serialization.dimensions.size(); ++d) {
                texts.push_back(it->serialization.dimensions[d].expression);
            }
        }
        std::vector<KnobIPtr> holderKnobs = holder->getKnobs_mt_safe();
        for (std::size_t i = 0; i < holderKnobs.size(); ++i) {
            texts.push_back(holderKnobs[i]->getName());
        }
        bool clash = true;
        while (clash) {
            clash = false;
            for (std::size_t i = 0; i < texts.size() && !clash; ++i) {
                clash = texts[i].find(placeholderPrefix) != std::string::npos;
            }
            if (clash) {
                placeholderPrefix += "X";
            }
        }
    }
    std::vector<std::pair<std::string, std::string>> toPlaceholder;
    std::vector<std::pair<std::string, std::string>> fromPlaceholder;
    for (std::map<std::string, std::string>::const_iterator it = renamed.begin(); it != renamed.end(); ++it) {
        if (it->first == it->second) {
            continue;
        }
        const std::string placeholder = placeholderPrefix + std::to_string(toPlaceholder.size()) + "_";
        toPlaceholder.push_back(std::make_pair(it->first, placeholder));
        fromPlaceholder.push_back(std::make_pair(placeholder, it->second));
    }
    const auto rewrite = [&](const KnobIPtr& listener, std::string expression) {
        for (std::size_t i = 0; i < toPlaceholder.size(); ++i) {
            expression = renameUserKnobInExpression(holder, listener, expression, toPlaceholder[i].first, toPlaceholder[i].second);
        }
        for (std::size_t i = 0; i < fromPlaceholder.size(); ++i) {
            expression = renameUserKnobInExpression(holder, listener, expression, fromPlaceholder[i].first, fromPlaceholder[i].second);
        }

        return expression;
    };

    AppInstancePtr app = holder->getApp();
    bool hasLinkGroups = false;
    for (std::list<PastedKnob>::const_iterator it = pasted.begin(); it != pasted.end(); ++it) {
        const KnobIPtr& knob = it->knob;
        it->knob->storeLinkGroupsFromSerialization(it->serialization);
        for (std::size_t d = 0; d < it->serialization.dimensions.size(); ++d) {
            hasLinkGroups = hasLinkGroups || it->serialization.dimensions[d].linkGroup;
        }
        const std::vector<Serialization::KnobDimensionSerialization>& dimensions = it->serialization.dimensions;
        if (dimensions.empty()) {
            continue;
        }
        // One saved dimension stands for all of them.
        const bool folded = (dimensions.size() == 1);
        for (int d = 0; d < knob->getDimension(); ++d) {
            const Serialization::KnobDimensionSerialization& dimension = dimensions[folded ? 0 : std::min(d, (int)dimensions.size() - 1)];
            if (!folded && (d >= (int)dimensions.size())) {
                break;
            }
            if (dimension.master && !dimension.master->masterKnobName.empty() && app) {
                const Serialization::KnobMasterSerialization& master = *dimension.master;
                NodePtr node = app->getNodeByFullySpecifiedName(master.masterNodeNameFull.empty() ? master.masterNodeName : master.masterNodeNameFull);
                KnobIPtr target = node ? node->getKnobByName(master.masterKnobName) : KnobIPtr();
                if (target && (target != knob)) {
                    int targetDimension = master.masterDimension;
                    if (folded && (d < target->getDimension())) {
                        targetDimension = d;
                    }
                    if ((targetDimension >= 0) && (targetDimension < target->getDimension()) && knob->linkTo(d, target, targetDimension)) {
                        continue;
                    }
                }
            }
            if (!dimension.expression.empty()) {
                setExpressionNoThrow(knob, d, rewrite(knob, dimension.expression), dimension.expressionHasReturnVariable);
            }
        }
    }
    NodePtr node = nodeOfHolder(holder);
    if (hasLinkGroups && node) {
        KnobHelper::restoreLinkGroups(NodesList(1, node));
    }
}

// The fields applySpecInPlace() ignores (a placement the knob already has, empty defaults, an
// empty extra) take current's values, so that a spec changing nothing compares equal to current.
UserKnobSpec
effectiveSpec(KnobHolder* holder,
              const KnobIPtr& knob,
              const UserKnobSpec& spec,
              const UserKnobSpec& current)
{
    UserKnobSpec ret = spec;
    if ((spec.typeName == KnobPage::typeNameStatic()) || isPlacedAt(holder, knob, spec.parentName, spec.index)) {
        ret.parentName = current.parentName;
        ret.index = current.index;
    }
    if (isPageOrGroupType(spec.typeName) || spec.defaults.empty()) {
        ret.defaults = current.defaults;
    }
    if (isPageOrGroupType(spec.typeName) || std::holds_alternative<std::monostate>(spec.extra)) {
        ret.extra = current.extra;
    }
    if (!dynamic_cast<KnobDouble*>(knob.get())) {
        ret.useHostOverlayHandle = current.useHostOverlayHandle;
    }
    if (!dynamic_cast<KnobGroup*>(knob.get())) {
        ret.groupAsTab = current.groupAsTab;
    }
    if (!knob->canAnimate()) {
        ret.animates = current.animates;
    }

    return ret;
}

bool
applySpecInPlace(KnobHolder* holder,
                 const KnobIPtr& knob,
                 const UserKnobSpec& spec,
                 std::string* error)
{
    const std::string oldName = knob->getName();
    if (spec.name != oldName) {
        const std::map<KnobIPtr, std::vector<DimensionExpression>> listeners = listenerExpressions(knob);
        try {
            knob->setName(spec.name, true);
        } catch (const std::exception& e) {
            if (error) {
                *error = e.what();
            }

            return false;
        }
        NodePtr node = nodeOfHolder(holder);
        if (node) {
            node->removeParameterFromPython(oldName);
        }
        declareToPython(holder);
        renameInListeners(holder, listeners, oldName, spec.name);
    }

    knob->setLabel(spec.label);
    knob->setHintToolTip(spec.tooltip);
    knob->setSecret(spec.secret);
    knob->setAddNewLine(spec.newLine);
    if (knob->canAnimate()) {
        knob->setAnimationEnabled(spec.animates);
    }
    knob->setEvaluateOnChange(spec.evaluatesOnChange);
    if (KnobGroup* isGroup = dynamic_cast<KnobGroup*>(knob.get())) {
        isGroup->setAsTab(spec.groupAsTab);
    }
    if (!isPageOrGroupType(spec.typeName)) {
        // The entries before the defaults: a choice's default is one of them.
        applyExtra(knob, spec.extra);
        applyDefaults(knob, spec);
        KnobDouble* isDouble = dynamic_cast<KnobDouble*>(knob.get());
        if (isDouble && (isDouble->getHasHostOverlayHandle() != spec.useHostOverlayHandle)) {
            isDouble->setHasHostOverlayHandle(spec.useHostOverlayHandle);
        }
    }
    bool placed = true;
    if (spec.typeName != KnobPage::typeNameStatic()) {
        placed = placeKnob(holder, knob, spec.parentName, spec.index);
        if (!placed && error) {
            *error = "The parameter " + knob->getName() + " could not be moved to " + (spec.parentName.empty() ? std::string("the User page") : spec.parentName) + ".";
        }
    }
    refreshHolder(holder);

    return placed;
} // applySpecInPlace

void
reportUserKnobsChanged(KnobHolder* holder)
{
    if (GraphEditRecorder::isActive()) {
        GraphEditRecorder::onKnobListChanged(holder);
    }
}

// The knob is found by the name it has in the state undo or redo starts from when a snapshot
// undo has replaced the object since.
class UserKnobCommandBase
    : public UndoCommand {
public:
    UserKnobCommandBase(KnobHolder* holder,
                        const KnobIPtr& knob)
        : UndoCommand("Edit parameters of " + (nodeOfHolder(holder) ? nodeOfHolder(holder)->getLabel_mt_safe() : std::string()))
        , _node(nodeOfHolder(holder))
        , _knob(knob)
    {
    }

protected:
    KnobHolder* holder() const
    {
        NodePtr node = _node.lock();

        return node ? node->getEffectInstance().get() : nullptr;
    }

    KnobIPtr knobNamed(const std::string& name)
    {
        KnobHolder* h = holder();
        if (!h) {
            return KnobIPtr();
        }
        KnobIPtr knob = _knob.lock();
        if (!knob || (h->getKnobByName(knob->getName()) != knob)) {
            knob = h->getKnobByName(name);
            _knob = knob;
        }

        return knob;
    }

private:
    NodeWPtr _node;
    KnobIWPtr _knob;
};

class UserKnobSpecCommand
    : public UserKnobCommandBase {
public:
    UserKnobSpecCommand(KnobHolder* holder,
                        const KnobIPtr& knob,
                        const UserKnobSpec& before,
                        const UserKnobSpec& after)
        : UserKnobCommandBase(holder, knob)
        , _before(before)
        , _after(after)
    {
    }

    virtual void undo() OVERRIDE FINAL
    {
        apply(_after, _before);
    }

    virtual void redo() OVERRIDE FINAL
    {
        apply(_before, _after);
    }

private:
    void apply(const UserKnobSpec& from,
               const UserKnobSpec& to)
    {
        KnobHolder* h = holder();
        KnobIPtr knob = knobNamed(from.name);
        if (!h || !knob || (knob->typeName() != to.typeName) || (knob->getDimension() != to.dimension)) {
            return;
        }
        applySpecInPlace(h, knob, to, nullptr);
        reportUserKnobsChanged(h);
    }

    UserKnobSpec _before;
    UserKnobSpec _after;
};

class UserKnobMoveCommand
    : public UserKnobCommandBase {
public:
    UserKnobMoveCommand(KnobHolder* holder,
                        const KnobIPtr& knob,
                        bool up)
        : UserKnobCommandBase(holder, knob)
        , _name(knob->getName())
        , _up(up)
    {
    }

    virtual void undo() OVERRIDE FINAL
    {
        move(!_up);
    }

    virtual void redo() OVERRIDE FINAL
    {
        move(_up);
    }

private:
    void move(bool up)
    {
        KnobHolder* h = holder();
        KnobIPtr knob = knobNamed(_name);
        if (!h || !knob) {
            return;
        }
        if (up ? h->moveKnobOneStepUp(knob.get()) : h->moveKnobOneStepDown(knob.get())) {
            refreshHolder(h);
        }
    }

    std::string _name;
    bool _up;
};

NATRON_NAMESPACE_ANONYMOUS_EXIT

bool
isPageOrGroupType(const std::string& typeName)
{
    return (typeName == KnobPage::typeNameStatic()) || (typeName == KnobGroup::typeNameStatic());
}

bool
isNumericType(const std::string& typeName)
{
    return (typeName == KnobInt::typeNameStatic()) || (typeName == KnobDouble::typeNameStatic()) || (typeName == KnobColor::typeNameStatic());
}

std::string
uniqueKnobName(KnobHolder* holder,
               const std::string& wanted,
               bool numbered)
{
    const std::string base = NATRON_PYTHON_NAMESPACE::makeNameScriptFriendly(wanted);
    NodeCollection* isCollection = dynamic_cast<NodeCollection*>(holder);
    for (int no = 1;; ++no) {
        const std::string name = (no == 1 && !numbered) ? base : base + std::to_string(no);
        if (holder->getKnobByName(name) || NATRON_PYTHON_NAMESPACE::isKeyword(name)) {
            continue;
        }
        if (isCollection && isCollection->getNodeByName(name)) {
            continue;
        }

        return name;
    }
}

UserKnobSpec
specFromKnob(const KnobIPtr& knob)
{
    UserKnobSpec spec;
    if (!knob) {
        return spec;
    }
    spec.typeName = knob->typeName();
    spec.dimension = knob->getDimension();
    spec.name = knob->getName();
    spec.label = knob->getLabel();
    spec.tooltip = knob->getHintToolTip();
    spec.secret = knob->getIsSecret();
    spec.newLine = knob->isNewLineActivated();
    spec.animates = knob->isAnimationEnabled();
    spec.evaluatesOnChange = knob->getEvaluateOnChange();
    KnobIPtr parent = knob->getParentKnob();
    if (parent) {
        spec.parentName = parent->getName();
        spec.index = indexInParent(knob);
    }
    if (KnobGroup* isGroup = dynamic_cast<KnobGroup*>(knob.get())) {
        spec.groupAsTab = isGroup->isTab();
    }
    if (isPageOrGroupType(spec.typeName)) {
        return spec;
    }

    Serialization::KnobSerialization serialization;
    knob->toSerialization(&serialization);
    spec.extra = serialization.extraData;

    if (KnobChoice* isChoice = dynamic_cast<KnobChoice*>(knob.get())) {
        const std::vector<ChoiceOption> entries = isChoice->getEntries_mt_safe();
        const int index = isChoice->getDefaultValue(0);
        spec.defaults.push_back(Serialization::KnobValueSerialization(std::in_place_type<std::string>, ((index >= 0) && (index < (int)entries.size())) ? entries[index].id : std::string()));
    } else if (Serialization::getKnobTypeTraits(spec.typeName).hasValue) {
        KnobIntBase* isInt = dynamic_cast<KnobIntBase*>(knob.get());
        KnobBoolBase* isBool = dynamic_cast<KnobBoolBase*>(knob.get());
        KnobDoubleBase* isDouble = dynamic_cast<KnobDoubleBase*>(knob.get());
        KnobStringBase* isString = dynamic_cast<KnobStringBase*>(knob.get());
        for (int i = 0; i < spec.dimension; ++i) {
            if (isInt) {
                spec.defaults.push_back(Serialization::KnobValueSerialization(std::in_place_type<int>, isInt->getDefaultValue(i)));
            } else if (isDouble) {
                spec.defaults.push_back(Serialization::KnobValueSerialization(std::in_place_type<double>, isDouble->getDefaultValue(i)));
            } else if (isString) {
                spec.defaults.push_back(Serialization::KnobValueSerialization(std::in_place_type<std::string>, isString->getDefaultValue(i)));
            } else if (isBool) {
                spec.defaults.push_back(Serialization::KnobValueSerialization(std::in_place_type<bool>, isBool->getDefaultValue(i)));
            }
        }
    }
    if (KnobDouble* isDouble = dynamic_cast<KnobDouble*>(knob.get())) {
        spec.useHostOverlayHandle = isDouble->getHasHostOverlayHandle();
    }

    return spec;
} // specFromKnob

std::string
validateSpec(KnobHolder* holder,
             const KnobIPtr& knob,
             const UserKnobSpec& spec)
{
    if (!holder) {
        return "There is no node to hold the parameter.";
    }
    if (knob) {
        if (!knob->isUserKnob() || (knob->getHolder() != holder)) {
            return "Only user parameters of this node can be edited.";
        }
        if ((spec.typeName != knob->typeName()) || (spec.dimension != knob->getDimension())) {
            return "The type of a parameter is changed with changeUserKnobType().";
        }
    } else if (!isCreatableUserKnobType(spec.typeName, spec.dimension)) {
        return "A user parameter cannot be of type " + spec.typeName + " with " + std::to_string(spec.dimension) + " dimension(s).";
    }

    if (spec.name.empty()) {
        return "The script name cannot be empty.";
    }
    if (!isScriptIdentifier(spec.name)) {
        return "The script name " + spec.name + " must start with a letter or an underscore and hold only letters, digits and underscores.";
    }
    if (NATRON_PYTHON_NAMESPACE::isKeyword(spec.name)) {
        return "The script name " + spec.name + " is a Python keyword.";
    }
    KnobIPtr other = holder->getKnobByName(spec.name);
    if (other && (other != knob)) {
        return "A parameter named " + spec.name + " already exists.";
    }
    NodeCollection* isCollection = dynamic_cast<NodeCollection*>(holder);
    if (isCollection && (!knob || (spec.name != knob->getName())) && isCollection->getNodeByName(spec.name)) {
        return "A node inside this group is named " + spec.name + ".";
    }

    if (spec.typeName == KnobPage::typeNameStatic()) {
        if (!spec.parentName.empty()) {
            return "A page cannot have a parent.";
        }
    } else if (!spec.parentName.empty()) {
        KnobIPtr parent = findUserContainer(holder, spec.parentName);
        if (!parent) {
            return "There is no user page or group named " + spec.parentName + ".";
        }
        if (knob && isAncestorOrSelf(knob, parent)) {
            return "A group cannot be moved into itself.";
        }
        if (knob && !isPlacedAt(holder, knob, spec.parentName, spec.index) && !acceptsChild(parent, knob)) {
            return "The parameter " + knob->getName() + " cannot be a child of " + spec.parentName + ".";
        }
    }

    const Serialization::KnobTypeTraits traits = Serialization::getKnobTypeTraits(spec.typeName);
    if (traits.isChoice) {
        const Serialization::KnobChoiceExtraSerialization* choice = std::get_if<Serialization::KnobChoiceExtraSerialization>(&spec.extra);
        if (!spec.defaults.empty()) {
            const std::string* id = std::get_if<std::string>(&spec.defaults[0]);
            if (!id || !choice || (std::find(choice->entries.begin(), choice->entries.end(), *id) == choice->entries.end())) {
                return "The default of a choice must be one of its entries.";
            }
        }
    } else if (traits.hasValue && !spec.defaults.empty() && ((int)spec.defaults.size() != spec.dimension)) {
        return "A parameter needs one default per dimension.";
    }

    return std::string();
} // validateSpec

KnobIPtr
createUserKnob(KnobHolder* holder,
               const UserKnobSpec& spec,
               std::string* error)
{
    const std::string message = validateSpec(holder, KnobIPtr(), spec);
    if (!message.empty()) {
        if (error) {
            *error = message;
        }

        return KnobIPtr();
    }

    UserKnobsUndoScope undo(holder, "Add parameter " + spec.name);
    KnobIPtr knob;
    if (spec.typeName == KnobPage::typeNameStatic()) {
        KnobPagePtr page = AppManager::createKnob<KnobPage>(holder, spec.label, 1, false);
        page->setAsUserKnob(true);
        page->setName(spec.name);
        knob = page;
    } else if (spec.typeName == KnobGroup::typeNameStatic()) {
        KnobGroupPtr group = AppManager::createKnob<KnobGroup>(holder, spec.label, 1, false);
        group->setAsUserKnob(true);
        group->setName(spec.name);
        group->setAsTab(spec.groupAsTab);
        knob = group;
    } else {
        knob = createUserKnobFromSerialization(holder, serializationFromSpec(spec));
    }
    if (!knob) {
        if (error) {
            *error = "The parameter " + spec.name + " could not be created.";
        }

        return KnobIPtr();
    }
    if (isPageOrGroupType(spec.typeName)) {
        knob->setHintToolTip(spec.tooltip);
        knob->setSecret(spec.secret);
    }
    if ((spec.typeName != KnobPage::typeNameStatic()) && !placeKnob(holder, knob, spec.parentName, spec.index)) {
        holder->deleteKnob(knob.get(), true);
        if (error) {
            *error = "The parameter " + spec.name + " could not be added to " + spec.parentName + ".";
        }

        return KnobIPtr();
    }
    refreshHolder(holder);
    undo.markChanged();

    return knob;
} // createUserKnob

bool
applySpec(KnobHolder* holder,
          const KnobIPtr& knob,
          const UserKnobSpec& spec,
          std::string* error)
{
    const std::string message = knob ? validateSpec(holder, knob, spec) : std::string("There is no parameter to edit.");
    if (!message.empty()) {
        if (error) {
            *error = message;
        }

        return false;
    }

    const UserKnobSpec before = specFromKnob(knob);
    if (effectiveSpec(holder, knob, spec, before) == before) {
        return true;
    }
    // Creating the missing User page adds a knob, which only a snapshot undoes.
    const bool createsUserPage = (spec.typeName != KnobPage::typeNameStatic()) && spec.parentName.empty() && !holder->getUserPageKnob();
    UserKnobsUndoScope undo(holder, "Edit parameter " + spec.name, createsUserPage);
    const bool ok = applySpecInPlace(holder, knob, spec, error);
    const UserKnobSpec after = specFromKnob(knob);
    if (after == before) {
        return ok;
    }
    if (createsUserPage) {
        undo.markChanged();
    } else {
        undo.record(std::make_shared<UserKnobSpecCommand>(holder, knob, before, after));
    }

    return ok;
} // applySpec

std::list<KnobIPtr>
getUserKnobDependents(const KnobIPtr& knob)
{
    std::list<KnobIPtr> ret;
    if (!knob) {
        return ret;
    }
    const auto add = [&](const KnobIPtr& other) {
        if (other && (other != knob) && (std::find(ret.begin(), ret.end(), other) == ret.end())) {
            ret.push_back(other);
        }
    };
    KnobI::ListenerDimsMap listeners;
    knob->getListeners(listeners);
    for (KnobI::ListenerDimsMap::const_iterator it = listeners.begin(); it != listeners.end(); ++it) {
        add(it->first.lock());
    }
    const std::vector<KnobLinkedDimension> linked = knob->getLinkedKnobs();
    for (std::size_t i = 0; i < linked.size(); ++i) {
        add(linked[i].knob);
    }

    return ret;
}

bool
deleteUserKnob(KnobHolder* holder,
               const KnobIPtr& knob)
{
    if (!holder || !knob || !knob->isUserKnob() || (knob->getHolder() != holder)) {
        return false;
    }
    UserKnobsUndoScope undo(holder, "Remove parameter " + knob->getName());
    holder->deleteKnob(knob.get(), true);
    refreshHolder(holder);
    undo.markChanged();

    return true;
}

bool
moveUserKnob(KnobHolder* holder,
             const KnobIPtr& knob,
             bool up)
{
    if (!holder || !knob || !knob->isUserKnob() || (knob->getHolder() != holder)) {
        return false;
    }
    UserKnobsUndoScope undo(holder, "Move parameter " + knob->getName(), false);
    const bool moved = up ? holder->moveKnobOneStepUp(knob.get()) : holder->moveKnobOneStepDown(knob.get());
    if (moved) {
        refreshHolder(holder);
        undo.record(std::make_shared<UserKnobMoveCommand>(holder, knob, up));
    }

    return moved;
}

bool
isUserKnobTypeChangeConvertible(const std::string& fromType,
                                int fromDimension,
                                const std::string& toType,
                                int toDimension)
{
    return isNumericType(fromType) && isNumericType(toType) && isCreatableUserKnobType(fromType, fromDimension) && isCreatableUserKnobType(toType, toDimension);
}

UserKnobTypeChangeEnum
changeUserKnobType(KnobHolder* holder,
                   const KnobIPtr& knob,
                   const std::string& typeName,
                   int dimension,
                   bool allowReset,
                   KnobIPtr* newKnob,
                   std::string* error)
{
    if (newKnob) {
        *newKnob = knob;
    }
    const auto fail = [&](const std::string& message) {
        if (error) {
            *error = message;
        }

        return eUserKnobTypeChangeFailed;
    };
    if (!holder || !knob || !knob->isUserKnob() || (knob->getHolder() != holder)) {
        return fail("Only user parameters of this node can change type.");
    }
    if (isPageOrGroupType(knob->typeName()) || isPageOrGroupType(typeName) || !isCreatableUserKnobType(typeName, dimension)) {
        return fail("A user parameter cannot be changed to type " + typeName + " with " + std::to_string(dimension) + " dimension(s).");
    }
    const std::string oldType = knob->typeName();
    const int oldDimension = knob->getDimension();
    if ((oldType == typeName) && (oldDimension == dimension)) {
        return eUserKnobTypeChangeConverted;
    }
    // An alias and its master share one type: no replacement of another type can stay either.
    if (knob->getAliasMaster() || !knob->getAliasesOfThis().empty()) {
        return fail("The parameter " + knob->getName() + " is an alias or has aliases, which keep its type.");
    }
    const bool convertible = isUserKnobTypeChangeConvertible(oldType, oldDimension, typeName, dimension);
    if (!convertible && !allowReset) {
        return eUserKnobTypeChangeNeedsReset;
    }

    const std::string name = knob->getName();
    UserKnobsUndoScope undo(holder, "Change type of " + name);

    Serialization::KnobSerialization serialization;
    knob->toSerialization(&serialization);
    if ((serialization.dimensions.size() == 1) && (oldDimension > 1)) {
        serialization.dimensions.resize(oldDimension, serialization.dimensions[0]);
    }
    std::vector<Serialization::KnobDimensionSerialization> oldDimensions = serialization.dimensions;
    serialization.typeName = typeName;
    serialization.dimension = dimension;
    serialization.name = uniqueKnobName(holder, name + "_retyped");
    serialization.dimensions.clear();
    serialization.dimensions.resize(dimension);
    serialization.useHostOverlayHandle.reset();
    serialization.parametricCurves.reset();
    serialization.stringAnimation.reset();
    serialization.choiceOptionID.reset();
    const Serialization::KnobTypeTraits traits = Serialization::getKnobTypeTraits(typeName);
    if (convertible) {
        for (int i = 0; i < dimension && i < oldDimension; ++i) {
            Serialization::KnobDimensionSerialization& to = serialization.dimensions[i];
            const Serialization::KnobDimensionSerialization& from = oldDimensions[i];
            to.enabled = from.enabled;
            if (from.value) {
                to.value = toStorage(*from.value, traits.storageType, false);
            }
            if (from.defaultValue) {
                to.defaultValue = toStorage(*from.defaultValue, traits.storageType, false);
            }
            if (from.curve) {
                to.curve = from.curve;
                if (traits.isInt) {
                    for (std::size_t k = 0; k < to.curve->keyframes.size(); ++k) {
                        to.curve->keyframes[k].value = std::round(to.curve->keyframes[k].value);
                    }
                }
            }
        }
        Serialization::KnobValueRangeSerialization* range = std::get_if<Serialization::KnobValueRangeSerialization>(&serialization.extraData);
        if (range && traits.isInt) {
            range->minimum = clampToInt(range->minimum);
            range->maximum = clampToInt(range->maximum);
            range->displayMinimum = clampToInt(range->displayMinimum);
            range->displayMaximum = clampToInt(range->displayMaximum);
        }
    } else {
        serialization.extraData = Serialization::KnobExtraSerialization();
        for (int i = 0; i < dimension; ++i) {
            serialization.dimensions[i].enabled = true;
        }
    }

    std::vector<DimensionExpression> ownExpressions;
    for (int i = 0; i < oldDimension; ++i) {
        DimensionExpression expression = { knob->getExpression(i), knob->isExpressionUsingRetVariable(i) };
        ownExpressions.push_back(expression);
    }
    struct GroupMembership {
        KnobLinkGroupPtr group;
        bool linked;
        bool nodeLink;
    };
    std::vector<GroupMembership> memberships(oldDimension);
    for (int i = 0; i < oldDimension; ++i) {
        GroupMembership& membership = memberships[i];
        membership.group = knob->getLinkGroup(i);
        membership.linked = membership.group && membership.group->isMemberLinked(knob.get(), i);
        membership.nodeLink = membership.group && membership.group->isMemberNodeLink(knob.get(), i);
    }
    const std::map<KnobIPtr, std::vector<DimensionExpression>> listeners = listenerExpressions(knob);

    KnobIPtr replacement = createUserKnobFromSerialization(holder, serialization);
    if (!replacement) {
        return fail("The parameter " + name + " could not be created as " + typeName + ".");
    }
    KnobIPtr parent = knob->getParentKnob();
    const int index = indexInParent(knob);
    if (parent && !insertInContainer(parent, replacement, index)) {
        holder->deleteKnob(replacement.get(), false);

        return fail("The parameter " + name + " could not be put back in " + parent->getName() + ".");
    }

    // Joined through the old knob while it is still a member: deleting it first would dissolve a group
    // it shared with a single other knob, and linking to an unlinked member moves that member out.
    bool restoredNodeLink = false;
    if (convertible) {
        KnobHelperPtr ownState;
        for (int i = 0; i < dimension && i < oldDimension; ++i) {
            const GroupMembership& membership = memberships[i];
            if (!membership.group || (knob->getLinkGroup(i) != membership.group)) {
                continue;
            }
            if (!membership.linked && !ownState) {
                // Joining adopts the group's state, which an unlinked member does not share.
                ownState = appPTR->getKnobFactory().createKnob(replacement->typeName(), NULL, name, dimension, false);
                if (ownState) {
                    ownState->clone(replacement.get());
                }
            }
            membership.group->setMemberLinked(knob.get(), i, true);
            if (!replacement->linkTo(i, knob, i) || (replacement->getLinkGroup(i) != membership.group)) {
                continue;
            }
            membership.group->setMemberNodeLink(replacement.get(), i, membership.nodeLink);
            restoredNodeLink |= membership.nodeLink;
            if (!membership.linked) {
                membership.group->setMemberLinked(replacement.get(), i, false);
                if (ownState) {
                    replacement->clone(ownState.get(), i, i);
                }
                if (ownExpressions[i].expression.empty() && !replacement->getExpression(i).empty()) {
                    replacement->clearExpression(i, true);
                }
            }
        }
    }

    // While the old knob is still on the holder: a listener still reading it once it is detached
    // unregisters from a knob without a holder when its expression is set again.
    for (std::map<KnobIPtr, std::vector<DimensionExpression>>::const_iterator it = listeners.begin(); it != listeners.end(); ++it) {
        for (int i = 0; i < (int)it->second.size() && i < it->first->getDimension(); ++i) {
            if (!it->second[i].expression.empty()) {
                it->first->clearExpression(i, true);
            }
        }
    }
    holder->deleteKnob(knob.get(), true);
    replacement->setName(name);
    declareToPython(holder);

    if (convertible) {
        for (int i = 0; i < dimension && i < (int)ownExpressions.size(); ++i) {
            if (!ownExpressions[i].expression.empty()) {
                setExpressionNoThrow(replacement, i, ownExpressions[i].expression, ownExpressions[i].hasRet);
            }
        }
        NodePtr node = nodeOfHolder(holder);
        if (restoredNodeLink && node) {
            node->emitLinksChanged(NodesList());
        }
    }
    for (std::map<KnobIPtr, std::vector<DimensionExpression>>::const_iterator it = listeners.begin(); it != listeners.end(); ++it) {
        for (int i = 0; i < (int)it->second.size() && i < it->first->getDimension(); ++i) {
            if (!it->second[i].expression.empty()) {
                setExpressionNoThrow(it->first, i, it->second[i].expression, it->second[i].hasRet);
            }
        }
    }
    refreshHolder(holder);
    undo.markChanged();
    if (newKnob) {
        *newKnob = replacement;
    }

    return convertible ? eUserKnobTypeChangeConverted : eUserKnobTypeChangeReset;
} // changeUserKnobType

std::string
copyUserKnobs(KnobHolder* holder,
              const std::list<KnobIPtr>& knobs)
{
    std::list<Serialization::GroupKnobSerialization> pages;
    if (!holder) {
        return Serialization::encodeUserPagesYaml(pages);
    }
    const auto isCopiedWithAncestor = [&](const KnobIPtr& knob) {
        for (KnobIPtr parent = knob->getParentKnob(); parent; parent = parent->getParentKnob()) {
            if (std::find(knobs.begin(), knobs.end(), parent) != knobs.end()) {
                return true;
            }
        }

        return false;
    };
    const auto pageEntry = [&](const KnobPagePtr& page) -> Serialization::GroupKnobSerialization& {
        for (std::list<Serialization::GroupKnobSerialization>::iterator it = pages.begin(); it != pages.end(); ++it) {
            if (it->name == page->getName()) {
                return *it;
            }
        }
        pages.push_back(Serialization::GroupKnobSerialization());
        pages.back().name = page->getName();
        pages.back().label = page->getLabel();
        pages.back().secret = page->getIsSecret();
        pages.back().tooltip = page->getHintToolTip();

        return pages.back();
    };

    for (std::list<KnobIPtr>::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
        const KnobIPtr& knob = *it;
        if (!knob || !knob->isUserKnob() || (knob->getHolder() != holder) || isCopiedWithAncestor(knob)) {
            continue;
        }
        KnobPagePtr isPage = std::dynamic_pointer_cast<KnobPage>(knob);
        if (isPage) {
            Serialization::GroupKnobSerialization layout;
            userKnobLayoutToSerialization(knob, &layout);
            Serialization::GroupKnobSerialization& entry = pageEntry(isPage);
            entry.children.splice(entry.children.end(), layout.children);
            continue;
        }
        KnobPagePtr page = pageOf(knob);
        if (!page) {
            continue;
        }
        Serialization::GroupKnobChildSerialization child;
        if (dynamic_cast<KnobGroup*>(knob.get())) {
            child.isGroup = true;
            userKnobLayoutToSerialization(knob, &child.group);
        } else {
            knob->toSerialization(&child.knob);
        }
        pageEntry(page).children.push_back(child);
    }
    std::map<std::string, int> linkGroupCounts;
    for (std::list<Serialization::GroupKnobSerialization>::const_iterator it = pages.begin(); it != pages.end(); ++it) {
        countLinkGroupIds(*it, &linkGroupCounts);
    }
    for (std::list<Serialization::GroupKnobSerialization>::iterator it = pages.begin(); it != pages.end(); ++it) {
        linkGroupsToMasters(linkGroupCounts, &*it);
    }

    return Serialization::encodeUserPagesYaml(pages);
} // copyUserKnobs

std::list<KnobIPtr>
pasteUserKnobs(KnobHolder* holder,
               const std::string& text,
               const KnobIPtr& parent,
               int index,
               std::string* error)
{
    std::list<KnobIPtr> created;
    if (!holder) {
        return created;
    }
    std::list<Serialization::GroupKnobSerialization> pages;
    try {
        pages = Serialization::decodeUserPagesYaml(text);
    } catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }

        return created;
    }
    if (parent && (!parent->isUserKnob() || (parent->getHolder() != holder) || (!dynamic_cast<KnobPage*>(parent.get()) && !dynamic_cast<KnobGroup*>(parent.get())))) {
        if (error) {
            *error = "Parameters can only be pasted into a user page or group of this node.";
        }

        return created;
    }

    UserKnobsUndoScope undo(holder, "Paste parameters");
    std::list<PastedKnob> pasted;
    std::map<std::string, std::string> renamed;
    for (std::list<Serialization::GroupKnobSerialization>::const_iterator page = pages.begin(); page != pages.end(); ++page) {
        KnobIPtr container = parent;
        int insertIndex = index;
        if (!container) {
            const std::string pageName = uniqueKnobName(holder, page->name);
            KnobPagePtr newPage = AppManager::createKnob<KnobPage>(holder, page->label, 1, false);
            newPage->setAsUserKnob(true);
            newPage->setName(pageName);
            newPage->setSecret(page->secret);
            newPage->setHintToolTip(page->tooltip);
            renamed[page->name] = pageName;
            container = newPage;
            insertIndex = -1;
            created.push_back(newPage);
        }
        for (std::list<Serialization::GroupKnobChildSerialization>::const_iterator child = page->children.begin(); child != page->children.end(); ++child) {
            KnobIPtr knob = pasteChild(holder, *child, container, insertIndex, &pasted, &renamed);
            if (!knob) {
                continue;
            }
            if (parent) {
                created.push_back(knob);
                if (insertIndex >= 0) {
                    ++insertIndex;
                }
            }
        }
        if (parent) {
            index = insertIndex;
        }
    }
    declareToPython(holder);
    restorePastedExpressionsAndLinks(holder, pasted, renamed);
    refreshHolder(holder);
    if (!created.empty() || !pasted.empty()) {
        undo.markChanged();
    }

    return created;
} // pasteUserKnobs

std::string
renameUserKnobInExpression(KnobHolder* holder,
                           const KnobIPtr& listener,
                           const std::string& expression,
                           const std::string& oldName,
                           const std::string& newName)
{
    NodePtr node = nodeOfHolder(holder);
    if (!node) {
        // Without a node there is no owner name for the code to be parsed against.
        std::string ret = expression;
        for (std::size_t pos = ret.find(oldName); pos != std::string::npos; pos = ret.find(oldName, pos + newName.size())) {
            ret.replace(pos, oldName.size(), newName);
        }

        return ret;
    }
    std::set<std::string> owners;
    owners.insert(node->getScriptName_mt_safe());
    KnobHolder* listenerHolder = listener ? listener->getHolder() : nullptr;
    if (listenerHolder == holder) {
        owners.insert("thisNode");
    }
    EffectInstance* listenerEffect = dynamic_cast<EffectInstance*>(listenerHolder);
    NodePtr listenerNode = listenerEffect ? listenerEffect->getNode() : NodePtr();
    NodeCollectionPtr listenerGroup = listenerNode ? listenerNode->getGroup() : NodeCollectionPtr();
    if (listenerGroup && (dynamic_cast<NodeCollection*>(holder) == listenerGroup.get())) {
        owners.insert("thisGroup");
    }

    return renameKnobInExpressionCode(expression, oldName, newName, owners);
}

NATRON_NAMESPACE_EXIT
