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

#include "Knob.h"

#include <algorithm> // min
#include <cassert>
#include <list>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <QDateTime>
#include <QString>

#include <ofxImageEffect.h>

#include <SequenceParsing.h>

#include "Serialization/KnobSerialization.h"
#include "Serialization/Yaml/KnobYaml.h"

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Curve.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobLinkGroup.h"
#include "Engine/KnobShuffleMap.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/StringAnimationManager.h"
#include "Engine/TrackMarker.h"
#include "Engine/TrackerContext.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

NATRON_NAMESPACE_ANONYMOUS_ENTER

/*
 * Loading applies a saved knob to a live one through the clone functions, which copy from
 * another knob without triggering the evaluation and instanceChanged notifications a
 * setValue() on a live knob would. So the saved data is first loaded into a knob that belongs
 * to no holder, then cloned from.
 */
KnobIPtr
createDetachedKnob(const std::string& typeName,
                   int dimension)
{
    KnobIPtr ret;

    if (typeName == KnobInt::typeNameStatic()) {
        ret = std::make_shared<KnobInt>((KnobHolder*)NULL, std::string(), dimension, false);
    } else if (typeName == KnobBool::typeNameStatic()) {
        ret = std::make_shared<KnobBool>((KnobHolder*)NULL, std::string(), dimension, false);
    } else if (typeName == KnobDouble::typeNameStatic()) {
        ret = std::make_shared<KnobDouble>((KnobHolder*)NULL, std::string(), dimension, false);
    } else if (typeName == KnobChoice::typeNameStatic()) {
        ret = std::make_shared<KnobChoice>((KnobHolder*)NULL, std::string(), dimension, false);
    } else if (typeName == KnobString::typeNameStatic()) {
        ret = std::make_shared<KnobString>((KnobHolder*)NULL, std::string(), dimension, false);
    } else if (typeName == KnobParametric::typeNameStatic()) {
        ret = std::make_shared<KnobParametric>((KnobHolder*)NULL, std::string(), dimension, false);
    } else if (typeName == KnobColor::typeNameStatic()) {
        ret = std::make_shared<KnobColor>((KnobHolder*)NULL, std::string(), dimension, false);
    } else if (typeName == KnobPath::typeNameStatic()) {
        ret = std::make_shared<KnobPath>((KnobHolder*)NULL, std::string(), dimension, false);
    } else if (typeName == KnobLayers::typeNameStatic()) {
        ret = std::make_shared<KnobLayers>((KnobHolder*)NULL, std::string(), dimension, false);
    } else if (typeName == KnobChannelSet::typeNameStatic()) {
        ret = std::make_shared<KnobChannelSet>((KnobHolder*)NULL, std::string(), dimension, false);
    } else if (typeName == KnobLayerSelect::typeNameStatic()) {
        ret = std::make_shared<KnobLayerSelect>((KnobHolder*)NULL, std::string(), dimension, false);
    } else if (typeName == KnobChannelSelect::typeNameStatic()) {
        ret = std::make_shared<KnobChannelSelect>((KnobHolder*)NULL, std::string(), dimension, false);
    } else if (typeName == KnobShuffleMap::typeNameStatic()) {
        ret = std::make_shared<KnobShuffleMap>((KnobHolder*)NULL, std::string(), dimension, false);
    } else if (typeName == KnobFile::typeNameStatic()) {
        ret = std::make_shared<KnobFile>((KnobHolder*)NULL, std::string(), dimension, false);
    } else if (typeName == KnobOutputFile::typeNameStatic()) {
        ret = std::make_shared<KnobOutputFile>((KnobHolder*)NULL, std::string(), dimension, false);
    } else if (typeName == KnobButton::typeNameStatic()) {
        ret = std::make_shared<KnobButton>((KnobHolder*)NULL, std::string(), dimension, false);
    } else if (typeName == KnobSeparator::typeNameStatic()) {
        ret = std::make_shared<KnobSeparator>((KnobHolder*)NULL, std::string(), dimension, false);
    } else if (typeName == KnobGroup::typeNameStatic()) {
        ret = std::make_shared<KnobGroup>((KnobHolder*)NULL, std::string(), dimension, false);
    } else if (typeName == KnobPage::typeNameStatic()) {
        ret = std::make_shared<KnobPage>((KnobHolder*)NULL, std::string(), dimension, false);
    }

    if (ret) {
        ret->populate();
    }

    return ret;
}

template <typename T>
const T*
getSerializedValue(const std::optional<Serialization::KnobValueSerialization>& value)
{
    if (!value) {
        return NULL;
    }

    return std::get_if<T>(&(*value));
}

template <typename T>
void
loadDetachedKnobValue(Knob<T>* knob,
                      const Serialization::KnobDimensionSerialization& dimensionData,
                      int dimension,
                      ViewSpec view)
{
    const T* value = getSerializedValue<T>(dimensionData.value);
    if (value) {
        knob->setValue(*value, view, dimension);
    }
    const T* defaultValue = getSerializedValue<T>(dimensionData.defaultValue);
    if (defaultValue) {
        knob->setDefaultValueWithoutApplying(*defaultValue, dimension);
    }
}

void
loadDetachedKnob(const Serialization::KnobSerialization& serialization,
                 const KnobIPtr& knob)
{
    knob->setName(serialization.name);
    knob->setSecret(serialization.secret);

    KnobIntBase* isInt = dynamic_cast<KnobIntBase*>(knob.get());
    KnobBoolBase* isBool = dynamic_cast<KnobBoolBase*>(knob.get());
    KnobDoubleBase* isDouble = dynamic_cast<KnobDoubleBase*>(knob.get());
    KnobChoice* isChoice = dynamic_cast<KnobChoice*>(knob.get());
    KnobStringBase* isString = dynamic_cast<KnobStringBase*>(knob.get());
    KnobFile* isFile = dynamic_cast<KnobFile*>(knob.get());
    KnobParametric* isParametric = dynamic_cast<KnobParametric*>(knob.get());
    KnobPage* isPage = dynamic_cast<KnobPage*>(knob.get());
    KnobGroup* isGroup = dynamic_cast<KnobGroup*>(knob.get());
    KnobSeparator* isSeparator = dynamic_cast<KnobSeparator*>(knob.get());
    KnobButton* isButton = dynamic_cast<KnobButton*>(knob.get());
    AnimatingKnobStringHelper* isStringAnimated = dynamic_cast<AnimatingKnobStringHelper*>(knob.get());
    KnobLayerSelect* isLayerSelect = dynamic_cast<KnobLayerSelect*>(knob.get());

    // Input image files no longer hold keyframes: the Reader's file parameter (whose script name
    // is kOfxImageEffectFileParamName, see kOfxImageEffectContextReader) turns the frame of its
    // last saved keyframe back into a sequence pattern instead.
    const bool isReaderFileParam = isFile && (isFile->getName() == kOfxImageEffectFileParamName);

    const int savedDimensionCount = (int)serialization.dimensions.size();
    const bool oneDimensionForAll = (savedDimensionCount == 1) && (knob->getDimension() > 1);
    const int dimensionCount = oneDimensionForAll ? knob->getDimension() : std::min(knob->getDimension(), savedDimensionCount);
    for (int i = 0; i < dimensionCount; ++i) {
        const Serialization::KnobDimensionSerialization& dimensionData = serialization.dimensions[oneDimensionForAll ? 0 : i];

        knob->setEnabled(i, dimensionData.enabled);

        bool convertOldFileKeyframesToPattern = false;
        if (dimensionData.curve) {
            Curve curve;
            curve.fromSerialization(*dimensionData.curve);
            convertOldFileKeyframesToPattern = isReaderFileParam;
            if (!convertOldFileKeyframesToPattern) {
                CurvePtr knobCurve = knob->getCurve(ViewIdx(0), i);
                if (knobCurve) {
                    knobCurve->clone(curve);
                }
            }
        }

        if (isInt && !isChoice) {
            loadDetachedKnobValue<int>(isInt, dimensionData, i, ViewSpec::all());
        } else if (isBool && !isGroup && !isPage && !isSeparator && !isButton) {
            loadDetachedKnobValue<bool>(isBool, dimensionData, i, ViewSpec::all());
        } else if (isDouble && !isParametric) {
            loadDetachedKnobValue<double>(isDouble, dimensionData, i, ViewSpec::all());
        } else if (isChoice) {
            loadDetachedKnobValue<int>(isChoice, dimensionData, i, ViewIdx(0));
        } else if (isString && !isFile) {
            loadDetachedKnobValue<std::string>(isString, dimensionData, i, ViewIdx(0));
        } else if (isFile) {
            const std::string* savedValue = getSerializedValue<std::string>(dimensionData.value);
            if (savedValue) {
                std::string value = *savedValue;
                if (convertOldFileKeyframesToPattern) {
                    SequenceParsing::FileNameContent content(value);
                    content.generatePatternWithFrameNumberAtIndex(content.getPotentialFrameNumbersCount() - 1,
                                                                  content.getLeadingZeroes() + 1,
                                                                  &value);
                }
                isFile->setValue(value, ViewIdx(0), i);
            }
            const std::string* defaultValue = getSerializedValue<std::string>(dimensionData.defaultValue);
            if (defaultValue) {
                isFile->setDefaultValueWithoutApplying(*defaultValue, i);
            }
        }
    }

    if (isParametric) {
        if (serialization.parametricCurves) {
            std::list<Curve> curves;
            for (std::list<Serialization::CurveSerialization>::const_iterator it = serialization.parametricCurves->begin(); it != serialization.parametricCurves->end(); ++it) {
                Curve curve;
                curve.fromSerialization(*it);
                curves.push_back(curve);
            }
            isParametric->loadParametricCurves(curves);
        }
    } else if (isStringAnimated) {
        if (serialization.stringAnimation && !isReaderFileParam) {
            isStringAnimated->loadAnimation(*serialization.stringAnimation);
        }
    }

    if (serialization.isUserKnob && isLayerSelect) {
        const Serialization::KnobLayerSelectExtraSerialization* layerSelectData = std::get_if<Serialization::KnobLayerSelectExtraSerialization>(&serialization.extraData);
        isLayerSelect->setWithChannelButtons(layerSelectData ? layerSelectData->withChannelButtons : false);
    }
} // loadDetachedKnob

void
restoreChoiceFromDetachedKnob(KnobChoice* knob,
                              const KnobIPtr& serializedKnob,
                              const Serialization::KnobSerialization& serialization)
{
    KnobChoice* choiceSerialized = dynamic_cast<KnobChoice*>(serializedKnob.get());
    if (!choiceSerialized || !serialization.choiceOptionID) {
        return;
    }
    const std::string& optionID = *serialization.choiceOptionID;
    int id = knob->choiceRestorationId(choiceSerialized, optionID);
    knob->choiceRestoration(choiceSerialized, optionID, id);
}

void
fillMasterSerialization(const std::pair<int, KnobIPtr>& master,
                        Serialization::KnobMasterSerialization* masterData)
{
    masterData->masterDimension = master.first;
    NamedKnobHolder* holder = dynamic_cast<NamedKnobHolder*>(master.second->getHolder());
    assert(holder);

    TrackMarker* isMarker = dynamic_cast<TrackMarker*>(holder);
    if (isMarker) {
        masterData->masterTrackName = isMarker->getScriptName_mt_safe();
        masterData->masterNodeNameFull = isMarker->getContext()->getNode()->getFullyQualifiedName();
        masterData->masterNodeName = isMarker->getContext()->getNode()->getScriptName_mt_safe();
    } else {
        masterData->masterNodeNameFull = holder ? holder->getFullyQualifiedName() : "";
        masterData->masterNodeName = holder ? holder->getScriptName_mt_safe() : "";
    }
    masterData->masterKnobName = master.second->getName();
}

/*
 * The id a link group is saved under: the lowest "node.knob.dimension" or "node/track.knob.dimension"
 * among its members on active nodes and their tracks. Every member saved at the same time gets the
 * same id, and two groups never share one, since a knob dimension is in one group at most and active
 * node names, and the script names of a tracker's tracks, are unique.
 */
std::string
linkGroupSerializationId(const KnobLinkGroup& group)
{
    std::string id;
    std::vector<KnobLinkGroup::Member> members = group.getMembers();

    for (std::vector<KnobLinkGroup::Member>::const_iterator it = members.begin(); it != members.end(); ++it) {
        KnobIPtr knob = it->knob.lock();
        KnobHolder* holder = knob ? knob->getHolder() : 0;
        NodePtr node;
        std::string track;
        if (EffectInstance* effect = dynamic_cast<EffectInstance*>(holder)) {
            node = effect->getNode();
        } else if (TrackMarker* marker = dynamic_cast<TrackMarker*>(holder)) {
            // The tracker saves its tracks under its own lock, so the track is not looked up in it here.
            TrackerContextPtr context = marker->getContext();
            if (context) {
                node = context->getNode();
                track = marker->getScriptName_mt_safe();
            }
        }
        if (!node || !node->isActivated()) {
            continue;
        }
        // '/' is never part of a script name, so a track's id cannot be taken for a node's.
        const std::string owner = track.empty() ? node->getFullyQualifiedName() : node->getFullyQualifiedName() + '/' + track;
        const std::string name = owner + '.' + knob->getName() + '.' + std::to_string(it->dimension);
        if (id.empty() || (name < id)) {
            id = name;
        }
    }

    return id;
}

bool
canFoldDimensions(const KnobI* knob)
{
    if (knob->getDimension() < 2) {
        return false;
    }
    if (dynamic_cast<const KnobChoice*>(knob) || dynamic_cast<const KnobParametric*>(knob)) {
        return false;
    }

    return dynamic_cast<const KnobIntBase*>(knob) || dynamic_cast<const KnobDoubleBase*>(knob);
}

// The start state KnobGuiValue::createWidget gives a knob no GUI, user or project has set the
// fold state of yet, when every dimension holds the same value and curve.
bool
areDimensionsFoldedByDefault(const KnobI* knob)
{
    if (dynamic_cast<const KnobColor*>(knob)) {
        return true;
    }
    const KnobDouble* isDouble = dynamic_cast<const KnobDouble*>(knob);

    return isDouble && isDouble->isAutoFoldDimensionsEnabled();
}

bool
areDimensionsFolded(const KnobI* knob)
{
    if (knob->hasDimensionsFoldState()) {
        return !knob->getDimensionsUnfoldedInGui();
    }

    return areDimensionsFoldedByDefault(knob);
}

// Loading gives the one saved dimension to every dimension, so it may only stand for them when
// that loses nothing: defaults included, since loading clones those too, and with no link, link
// group or expression, which are restored per dimension.
bool
canSaveOneDimensionForAll(const KnobI* knob,
                          const Serialization::KnobSerialization& serialization,
                          bool folded)
{
    if (!folded || !canFoldDimensions(knob) || serialization.masterIsAlias || ((int)serialization.dimensions.size() != knob->getDimension())) {
        return false;
    }
    const Serialization::KnobDimensionSerialization& first = serialization.dimensions.front();
    if (first.master || first.linkGroup || !first.expression.empty()) {
        return false;
    }
    for (std::size_t i = 1; i < serialization.dimensions.size(); ++i) {
        if (!(serialization.dimensions[i] == first)) {
            return false;
        }
    }

    return true;
}

bool
hasSavedDefaultValues(const Serialization::KnobSerialization& serialization)
{
    for (std::size_t i = 0; i < serialization.dimensions.size(); ++i) {
        if (serialization.dimensions[i].defaultValue) {
            return true;
        }
    }

    return false;
}

void
restoreDimensionsFoldState(KnobI* knob,
                           const Serialization::KnobSerialization& serialization)
{
    if ((serialization.dimension < 2) || serialization.dimensions.empty() || !canFoldDimensions(knob)) {
        return;
    }
    knob->restoreDimensionsUnfoldedInGui(serialization.dimensions.size() > 1);
}

template <class K>
std::shared_ptr<K>
createOrReuseUserKnob(KnobHolder* holder,
                      const Serialization::KnobSerialization& serialization,
                      const KnobIPtr& existingKnob)
{
    if (existingKnob) {
        return std::dynamic_pointer_cast<K>(existingKnob);
    }

    return AppManager::createKnob<K>(holder, serialization.label, serialization.dimension, false);
}

template <typename T, class K>
void
applyUserKnobRange(K* knob,
                   const Serialization::KnobSerialization& serialization)
{
    const Serialization::KnobValueRangeSerialization* range = std::get_if<Serialization::KnobValueRangeSerialization>(&serialization.extraData);
    if (!range) {
        return;
    }
    std::vector<T> minimums, maximums, displayMinimums, displayMaximums;
    for (int i = 0; i < knob->getDimension(); ++i) {
        minimums.push_back(static_cast<T>(range->minimum));
        maximums.push_back(static_cast<T>(range->maximum));
        displayMinimums.push_back(static_cast<T>(range->displayMinimum));
        displayMaximums.push_back(static_cast<T>(range->displayMaximum));
    }
    knob->setMinimumsAndMaximums(minimums, maximums);
    knob->setDisplayMinimumsAndMaximums(displayMinimums, displayMaximums);
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

bool
KnobHelper::toSerialization(Serialization::KnobSerialization* serialization)
{
    assert(serialization);
    *serialization = Serialization::KnobSerialization();

    KnobIntBase* isInt = dynamic_cast<KnobIntBase*>(this);
    KnobBoolBase* isBool = dynamic_cast<KnobBoolBase*>(this);
    KnobDoubleBase* isDoubleBase = dynamic_cast<KnobDoubleBase*>(this);
    KnobChoice* isChoice = dynamic_cast<KnobChoice*>(this);
    KnobStringBase* isStringBase = dynamic_cast<KnobStringBase*>(this);
    KnobParametric* isParametric = dynamic_cast<KnobParametric*>(this);
    KnobPage* isPage = dynamic_cast<KnobPage*>(this);
    KnobGroup* isGroup = dynamic_cast<KnobGroup*>(this);
    KnobSeparator* isSeparator = dynamic_cast<KnobSeparator*>(this);
    KnobButton* isButton = dynamic_cast<KnobButton*>(this);
    AnimatingKnobStringHelper* isStringAnimated = dynamic_cast<AnimatingKnobStringHelper*>(this);

    const int dimension = getDimension();

    serialization->name = getName();
    serialization->typeName = typeName();
    serialization->dimension = dimension;
    serialization->secret = getIsSecret();
    const KnobIPtr aliasMaster = getAliasMaster();
    serialization->masterIsAlias = bool(aliasMaster);
    serialization->dimensions.resize(dimension);

    bool hasExpressionOrLink = false;
    for (int i = 0; i < dimension; ++i) {
        Serialization::KnobDimensionSerialization& dimensionData = serialization->dimensions[i];

        dimensionData.enabled = isEnabled(i);

        if (isAnimated(i)) {
            CurvePtr curve = getCurve(ViewIdx(0), i, true);
            if (curve) {
                dimensionData.curve.emplace();
                curve->toSerialization(&(*dimensionData.curve));
            }
        }

        if (isInt && !isChoice) {
            dimensionData.value = Serialization::KnobValueSerialization(std::in_place_type<int>, isInt->getValue(i));
            dimensionData.defaultValue = Serialization::KnobValueSerialization(std::in_place_type<int>, isInt->getDefaultValue(i));
        } else if (isBool && !isPage && !isGroup && !isSeparator && !isButton) {
            dimensionData.value = Serialization::KnobValueSerialization(std::in_place_type<bool>, isBool->getValue(i));
            dimensionData.defaultValue = Serialization::KnobValueSerialization(std::in_place_type<bool>, isBool->getDefaultValue(i));
        } else if (isDoubleBase && !isParametric) {
            dimensionData.value = Serialization::KnobValueSerialization(std::in_place_type<double>, isDoubleBase->getValue(i));
            dimensionData.defaultValue = Serialization::KnobValueSerialization(std::in_place_type<double>, isDoubleBase->getDefaultValue(i));
        } else if (isChoice) {
            dimensionData.value = Serialization::KnobValueSerialization(std::in_place_type<int>, isChoice->getValue(i));
            dimensionData.defaultValue = Serialization::KnobValueSerialization(std::in_place_type<int>, isChoice->getDefaultValue(i));
        } else if (isStringBase) {
            dimensionData.value = Serialization::KnobValueSerialization(std::in_place_type<std::string>, isStringBase->getValue(i));
            dimensionData.defaultValue = Serialization::KnobValueSerialization(std::in_place_type<std::string>, isStringBase->getDefaultValue(i));
        }

        // An alias is a link group member, not a slave: its master is saved the way a slave's is. A
        // one-way link is saved only when it is one loading makes again; any other is made again by
        // whatever made it.
        std::pair<int, KnobIPtr> master = aliasMaster ? std::make_pair(i, aliasMaster) : getMaster(i);
        const bool persistenceIgnored = isMastersPersistenceIgnored();
        const bool savesMaster = master.second && !persistenceIgnored && (aliasMaster || isSavedOneWayLinkMaster(master.second));
        const bool writesMaster = aliasMaster || (isSlave(i) && (savesMaster || persistenceIgnored));
        if (writesMaster) {
            Serialization::KnobMasterSerialization masterData;
            if (savesMaster) {
                fillMasterSerialization(master, &masterData);
            }
            dimensionData.master = masterData;
        }

        // A two-way link group is saved as an id its members saved alongside share. An alias member is
        // saved through its alias master above instead, and loading joins the other members of its
        // group to the group that alias forms again.
        KnobLinkGroupPtr group = getLinkGroup(i);
        if (group && !group->isMemberAlias(this, i)) {
            const std::string id = linkGroupSerializationId(*group);
            if (!id.empty()) {
                dimensionData.linkGroup.emplace();
                dimensionData.linkGroup->id = id;
                dimensionData.linkGroup->linked = group->isMemberLinked(this, i);
                dimensionData.linkGroup->nodeLink = group->isMemberNodeLink(this, i);
                hasExpressionOrLink = true;
            }
        }

        dimensionData.expression = getExpression(i);
        dimensionData.expressionHasReturnVariable = isExpressionUsingRetVariable(i);

        if (!dimensionData.expression.empty() || (writesMaster && master.second)) {
            hasExpressionOrLink = true;
        }
    }

    const bool savesOneDimensionForAll = canSaveOneDimensionForAll(this, *serialization, areDimensionsFolded(this));
    const bool foldStateDiffersFromDefault = savesOneDimensionForAll != canSaveOneDimensionForAll(this, *serialization, areDimensionsFoldedByDefault(this));
    if (savesOneDimensionForAll) {
        serialization->dimensions.resize(1);
    }

    if (isParametric) {
        std::list<Curve> curves;
        isParametric->saveParametricCurves(&curves);
        serialization->parametricCurves.emplace();
        for (std::list<Curve>::const_iterator it = curves.begin(); it != curves.end(); ++it) {
            Serialization::CurveSerialization curveData;
            it->toSerialization(&curveData);
            serialization->parametricCurves->push_back(curveData);
        }
    } else if (isStringAnimated) {
        serialization->stringAnimation.emplace();
        isStringAnimated->getAnimation().save(&(*serialization->stringAnimation));
    }

    serialization->isUserKnob = isUserKnob();
    serialization->label = getLabel();
    serialization->tooltip = getHintToolTip();
    serialization->triggerNewLine = isNewLineActivated();
    serialization->evaluatesOnChange = getEvaluateOnChange();
    serialization->animationEnabled = isAnimationEnabled();
    serialization->isPersistent = getIsPersistent();

    if (isChoice) {
        Serialization::KnobChoiceExtraSerialization choiceData;
        std::vector<ChoiceOption> options = isChoice->getEntries_mt_safe();
        choiceData.entries.resize(options.size());
        choiceData.helpStrings.resize(options.size());
        for (std::size_t i = 0; i < options.size(); ++i) {
            choiceData.entries[i] = options[i].id;
            choiceData.helpStrings[i] = options[i].tooltip;
        }
        int index = isChoice->getValue();
        std::string optionID;
        if ((index >= 0) && (index < (int)choiceData.entries.size())) {
            optionID = choiceData.entries[index];
        }
        serialization->choiceOptionID = optionID;
        serialization->extraData = choiceData;
    }

    if (serialization->isUserKnob) {
        KnobString* isString = dynamic_cast<KnobString*>(this);
        KnobDouble* isDouble = dynamic_cast<KnobDouble*>(this);
        KnobInt* isIntKnob = dynamic_cast<KnobInt*>(this);
        KnobColor* isColor = dynamic_cast<KnobColor*>(this);
        KnobFile* isFile = dynamic_cast<KnobFile*>(this);
        KnobOutputFile* isOutputFile = dynamic_cast<KnobOutputFile*>(this);
        KnobPath* isPath = dynamic_cast<KnobPath*>(this);
        KnobLayerSelect* isLayerSelect = dynamic_cast<KnobLayerSelect*>(this);

        if (isString) {
            Serialization::KnobTextExtraSerialization textData;
            textData.isLabel = isString->isLabel();
            textData.isMultiLine = isString->isMultiLine();
            textData.usesRichText = isString->usesRichText();
            serialization->extraData = textData;
        } else if (isDouble || isIntKnob || isColor) {
            Serialization::KnobValueRangeSerialization rangeData;
            if (isDouble) {
                rangeData.minimum = isDouble->getMinimum();
                rangeData.maximum = isDouble->getMaximum();
                rangeData.displayMinimum = isDouble->getDisplayMinimum();
                rangeData.displayMaximum = isDouble->getDisplayMaximum();
            } else if (isIntKnob) {
                rangeData.minimum = isIntKnob->getMinimum();
                rangeData.maximum = isIntKnob->getMaximum();
                rangeData.displayMinimum = isIntKnob->getDisplayMinimum();
                rangeData.displayMaximum = isIntKnob->getDisplayMaximum();
            } else {
                rangeData.minimum = isColor->getMinimum();
                rangeData.maximum = isColor->getMaximum();
                rangeData.displayMinimum = isColor->getDisplayMinimum();
                rangeData.displayMaximum = isColor->getDisplayMaximum();
            }
            serialization->extraData = rangeData;
        } else if (isFile || isOutputFile) {
            Serialization::KnobFileExtraSerialization fileData;
            fileData.useSequences = isFile ? isFile->isInputImageFile() : isOutputFile->isOutputImageFile();
            serialization->extraData = fileData;
        } else if (isPath) {
            Serialization::KnobPathExtraSerialization pathData;
            pathData.multiPath = isPath->isMultiPath();
            serialization->extraData = pathData;
        } else if (isLayerSelect) {
            Serialization::KnobLayerSelectExtraSerialization layerSelectData;
            layerSelectData.withChannelButtons = isLayerSelect->getWithChannelButtons();
            serialization->extraData = layerSelectData;
        }

        if (isDouble && (dimension == 2)) {
            serialization->useHostOverlayHandle = isDouble->getHasHostOverlayHandle();
        }
    }

    // KnobChannelSet::hasModificationsForSerialization() always returns true: its default
    // differs per node, so a value left out would be read back as whatever the default is then.
    // A knob left at its defaults is not saved, which would also lose a fold state other than
    // the one a new GUI starts with.
    return hasExpressionOrLink || (getIsPersistent() && (foldStateDiffersFromDefault || hasModificationsForSerialization()));
} // KnobHelper::toSerialization

bool
KnobHelper::fromSerialization(const Serialization::KnobSerialization& savedSerialization)
{
    const Serialization::KnobSerialization serialization = completeKnobSerializationForKnob(*this, savedSerialization);

    if (typeName() != serialization.typeName) {
        return false;
    }
    KnobIPtr serializedKnob = createDetachedKnob(serialization.typeName, serialization.dimension);
    if (!serializedKnob) {
        return false;
    }
    // Derivatives are recomputed as the curves load, which a periodic curve computes across its
    // wrap-around.
    KnobParametric* isParametric = dynamic_cast<KnobParametric*>(this);
    KnobParametric* serializedParametric = dynamic_cast<KnobParametric*>(serializedKnob.get());
    if (isParametric && serializedParametric && (isParametric->getDimension() > 0)) {
        CurvePtr curve = isParametric->getParametricCurve(0);
        serializedParametric->setPeriodic(curve && curve->isCurvePeriodic());
    }
    loadDetachedKnob(serialization, serializedKnob);

    // A plug-in knob's defaults belong to the plug-in and are not saved, so the live ones stay.
    if (hasSavedDefaultValues(serialization)) {
        cloneDefaultValues(serializedKnob.get());
    }

    KnobChoice* isChoice = dynamic_cast<KnobChoice*>(this);
    if (isChoice) {
        restoreChoiceFromDetachedKnob(isChoice, serializedKnob, serialization);

        return true;
    }

    // A saved knob with fewer dimensions than this one (e.g. a parameter that became 2D) has
    // its last dimension replicated rather than only its first ones loaded, so the result
    // looks the same as before.
    const int serializedDimension = serializedKnob->getDimension();
    if ((serializedDimension > 0) && (serializedDimension < getDimension())) {
        for (int i = 0; i < serializedDimension; ++i) {
            cloneAndUpdateGui(serializedKnob.get(), i);
        }
        for (int i = serializedDimension; i < getDimension(); ++i) {
            cloneAndUpdateGui(serializedKnob.get(), i, serializedDimension - 1);
        }
    } else {
        cloneAndUpdateGui(serializedKnob.get());
    }
    setSecret(serializedKnob->getIsSecret());
    if (getDimension() == serializedDimension) {
        for (int i = 0; i < getDimension(); ++i) {
            setEnabled(i, serializedKnob->isEnabled(i));
        }
    }
    restoreDimensionsFoldState(this, serialization);

    return true;
} // KnobHelper::fromSerialization

void
KnobI::storeLinkGroupsFromSerialization(const Serialization::KnobSerialization& serialization)
{
    _storedLinkGroups.clear();
    for (std::size_t i = 0; (i < serialization.dimensions.size()) && ((int)i < getDimension()); ++i) {
        const std::optional<Serialization::KnobLinkGroupSerialization>& group = serialization.dimensions[i].linkGroup;
        if (group && !group->id.empty()) {
            StoredLinkGroup stored;
            stored.dimension = (int)i;
            stored.id = group->id;
            stored.linked = group->linked;
            stored.nodeLink = group->nodeLink;
            _storedLinkGroups.push_back(stored);
        }
    }
}

void
KnobI::storeLinksFromSerialization(const Serialization::KnobSerialization& serialization)
{
    storeLinkGroupsFromSerialization(serialization);

    if (serialization.masterIsAlias) {
        if (serialization.dimensions.empty()) {
            return;
        }
        const std::optional<Serialization::KnobMasterSerialization>& savedMaster = serialization.dimensions.front().master;
        const Serialization::KnobMasterSerialization aliasMaster = savedMaster ? *savedMaster : Serialization::KnobMasterSerialization();
        storeLink(-1, aliasMaster.masterNodeNameFull, aliasMaster.masterNodeName, aliasMaster.masterTrackName, aliasMaster.masterKnobName, -1);

        return;
    }

    for (std::size_t i = 0; i < serialization.dimensions.size(); ++i) {
        const std::optional<Serialization::KnobMasterSerialization>& master = serialization.dimensions[i].master;
        if (master && (master->masterDimension != -1)) {
            storeLink((int)i, master->masterNodeNameFull, master->masterNodeName, master->masterTrackName, master->masterKnobName, master->masterDimension);
        }
    }
}

void
KnobI::restoreExpressionsFromSerialization(const Serialization::KnobSerialization& serialization,
                                           const std::map<std::string, std::string>& oldNewScriptNamesMapping)
{
    const int dimensionCount = std::min(getDimension(), (int)serialization.dimensions.size());

    try {
        for (int i = 0; i < dimensionCount; ++i) {
            const Serialization::KnobDimensionSerialization& dimensionData = serialization.dimensions[i];
            if (dimensionData.expression.empty()) {
                continue;
            }
            QString expression = QString::fromUtf8(dimensionData.expression.c_str());
            for (std::map<std::string, std::string>::const_iterator it = oldNewScriptNamesMapping.begin();
                 it != oldNewScriptNamesMapping.end(); ++it) {
                expression.replace(QString::fromUtf8(it->first.c_str()), QString::fromUtf8(it->second.c_str()));
            }
            restoreExpression(i, expression.toStdString(), dimensionData.expressionHasReturnVariable);
        }
    } catch (const std::exception& e) {
        QString error = QString::fromUtf8("Failed to restore expression: %1").arg(QString::fromUtf8(e.what()));
        appPTR->writeToErrorLog_mt_safe(QString::fromUtf8(getName().c_str()), QDateTime::currentDateTime(), error);
    }
}

Serialization::KnobSerialization
completeKnobSerializationForKnob(const KnobI& liveKnob,
                                 const Serialization::KnobSerialization& serialization)
{
    Serialization::KnobSerialization completed = serialization;
    if (!completed.typeName.empty()) {
        return completed;
    }
    const KnobChoice* isChoice = dynamic_cast<const KnobChoice*>(&liveKnob);
    std::vector<std::string> entries;
    if (isChoice) {
        std::vector<ChoiceOption> options = isChoice->getEntries_mt_safe();
        entries.reserve(options.size());
        for (std::size_t i = 0; i < options.size(); ++i) {
            entries.push_back(options[i].id);
        }
    }
    Serialization::completeKnobSerialization(&completed, liveKnob.typeName(), liveKnob.getDimension(), isChoice ? &entries : NULL);

    return completed;
}

KnobIPtr
createDetachedKnobFromSerialization(const Serialization::KnobSerialization& serialization)
{
    KnobIPtr serializedKnob = createDetachedKnob(serialization.typeName, serialization.dimension);
    if (serializedKnob) {
        loadDetachedKnob(serialization, serializedKnob);
    }

    return serializedKnob;
}

KnobIPtr
createUserKnobFromSerialization(KnobHolder* holder,
                                const Serialization::KnobSerialization& serialization,
                                const KnobIPtr& existingKnob)
{
    KnobIPtr serializedKnob = createDetachedKnobFromSerialization(serialization);
    if (!serializedKnob) {
        return KnobIPtr();
    }

    const std::string& typeName = serialization.typeName;
    KnobIPtr knob;
    if (typeName == KnobInt::typeNameStatic()) {
        KnobIntPtr k = createOrReuseUserKnob<KnobInt>(holder, serialization, existingKnob);
        if (!k) {
            return KnobIPtr();
        }
        applyUserKnobRange<int>(k.get(), serialization);
        knob = k;
    } else if (typeName == KnobDouble::typeNameStatic()) {
        KnobDoublePtr k = createOrReuseUserKnob<KnobDouble>(holder, serialization, existingKnob);
        if (!k) {
            return KnobIPtr();
        }
        applyUserKnobRange<double>(k.get(), serialization);
        if (serialization.useHostOverlayHandle && *serialization.useHostOverlayHandle) {
            k->setHasHostOverlayHandle(true);
        }
        knob = k;
    } else if (typeName == KnobBool::typeNameStatic()) {
        knob = createOrReuseUserKnob<KnobBool>(holder, serialization, existingKnob);
    } else if (typeName == KnobChoice::typeNameStatic()) {
        KnobChoicePtr k = createOrReuseUserKnob<KnobChoice>(holder, serialization, existingKnob);
        if (!k) {
            return KnobIPtr();
        }
        const Serialization::KnobChoiceExtraSerialization* choiceData = std::get_if<Serialization::KnobChoiceExtraSerialization>(&serialization.extraData);
        if (choiceData) {
            std::vector<ChoiceOption> options(choiceData->entries.size());
            for (std::size_t i = 0; i < options.size(); ++i) {
                options[i].id = choiceData->entries[i];
                if (i < choiceData->helpStrings.size()) {
                    options[i].tooltip = choiceData->helpStrings[i];
                }
            }
            k->populateChoices(options);
        }
        knob = k;
    } else if (typeName == KnobColor::typeNameStatic()) {
        KnobColorPtr k = createOrReuseUserKnob<KnobColor>(holder, serialization, existingKnob);
        if (!k) {
            return KnobIPtr();
        }
        applyUserKnobRange<double>(k.get(), serialization);
        knob = k;
    } else if (typeName == KnobString::typeNameStatic()) {
        KnobStringPtr k = createOrReuseUserKnob<KnobString>(holder, serialization, existingKnob);
        if (!k) {
            return KnobIPtr();
        }
        const Serialization::KnobTextExtraSerialization* textData = std::get_if<Serialization::KnobTextExtraSerialization>(&serialization.extraData);
        if (textData) {
            if (textData->isLabel) {
                k->setAsLabel();
            } else if (textData->isMultiLine) {
                k->setAsMultiLine();
                if (textData->usesRichText) {
                    k->setUsesRichText(true);
                }
            }
        }
        knob = k;
    } else if (typeName == KnobFile::typeNameStatic()) {
        KnobFilePtr k = createOrReuseUserKnob<KnobFile>(holder, serialization, existingKnob);
        if (!k) {
            return KnobIPtr();
        }
        const Serialization::KnobFileExtraSerialization* fileData = std::get_if<Serialization::KnobFileExtraSerialization>(&serialization.extraData);
        if (fileData && fileData->useSequences) {
            k->setAsInputImage();
        }
        knob = k;
    } else if (typeName == KnobOutputFile::typeNameStatic()) {
        KnobOutputFilePtr k = createOrReuseUserKnob<KnobOutputFile>(holder, serialization, existingKnob);
        if (!k) {
            return KnobIPtr();
        }
        const Serialization::KnobFileExtraSerialization* fileData = std::get_if<Serialization::KnobFileExtraSerialization>(&serialization.extraData);
        if (fileData && fileData->useSequences) {
            k->setAsOutputImageFile();
        }
        knob = k;
    } else if (typeName == KnobPath::typeNameStatic()) {
        KnobPathPtr k = createOrReuseUserKnob<KnobPath>(holder, serialization, existingKnob);
        if (!k) {
            return KnobIPtr();
        }
        const Serialization::KnobPathExtraSerialization* pathData = std::get_if<Serialization::KnobPathExtraSerialization>(&serialization.extraData);
        if (pathData && pathData->multiPath) {
            k->setMultiPath(true);
        }
        knob = k;
    } else if (typeName == KnobButton::typeNameStatic()) {
        knob = createOrReuseUserKnob<KnobButton>(holder, serialization, existingKnob);
    } else if (typeName == KnobSeparator::typeNameStatic()) {
        knob = createOrReuseUserKnob<KnobSeparator>(holder, serialization, existingKnob);
    } else if (typeName == KnobParametric::typeNameStatic()) {
        knob = createOrReuseUserKnob<KnobParametric>(holder, serialization, existingKnob);
    } else if (typeName == KnobChannelSet::typeNameStatic()) {
        knob = createOrReuseUserKnob<KnobChannelSet>(holder, serialization, existingKnob);
    } else if (typeName == KnobLayerSelect::typeNameStatic()) {
        KnobLayerSelectPtr k = createOrReuseUserKnob<KnobLayerSelect>(holder, serialization, existingKnob);
        if (!k) {
            return KnobIPtr();
        }
        if (!existingKnob) {
            KnobLayerSelect* layerSelectSerialized = dynamic_cast<KnobLayerSelect*>(serializedKnob.get());
            if (layerSelectSerialized) {
                k->setWithChannelButtons(layerSelectSerialized->getWithChannelButtons());
            }
        }
        knob = k;
    } else if (typeName == KnobChannelSelect::typeNameStatic()) {
        knob = createOrReuseUserKnob<KnobChannelSelect>(holder, serialization, existingKnob);
    } else if (typeName == KnobShuffleMap::typeNameStatic()) {
        knob = createOrReuseUserKnob<KnobShuffleMap>(holder, serialization, existingKnob);
    }

    if (!knob) {
        return KnobIPtr();
    }

    knob->cloneDefaultValues(serializedKnob.get());
    KnobChoice* isChoice = dynamic_cast<KnobChoice*>(knob.get());
    if (isChoice) {
        restoreChoiceFromDetachedKnob(isChoice, serializedKnob, serialization);
    } else {
        knob->clone(serializedKnob.get());
    }
    restoreDimensionsFoldState(knob.get(), serialization);
    knob->setAsUserKnob(true);
    knob->setIsPersistent(serialization.isPersistent);
    knob->setAnimationEnabled(serialization.animationEnabled);
    knob->setEvaluateOnChange(serialization.evaluatesOnChange);
    knob->setName(serialization.name);
    knob->setHintToolTip(serialization.tooltip);
    if (!serialization.triggerNewLine) {
        knob->setAddNewLine(false);
    }

    return knob;
} // createUserKnobFromSerialization

NATRON_NAMESPACE_EXIT
