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

#ifndef Serialization_BoostXml_KnobXml_h
#define Serialization_BoostXml_KnobXml_h

#include "Global/Macros.h"

#include <list>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "Serialization/BoostXml/CurveXml.h"
#include "Serialization/KnobSerialization.h"

#if !defined(Q_MOC_RUN) && !defined(SBK_RUN)
GCC_DIAG_UNUSED_LOCAL_TYPEDEFS_OFF
// clang-format off
GCC_DIAG_OFF(unused-parameter)
GCC_DIAG_OFF(sign-compare)
// /opt/local/include/boost/serialization/smart_cast.hpp:254:25: warning: unused parameter 'u' [-Wunused-parameter]
#include <boost/archive/xml_iarchive.hpp>
#include <boost/archive/xml_oarchive.hpp>
#include <boost/serialization/list.hpp>
#include <boost/serialization/map.hpp>
#include <boost/serialization/nvp.hpp>
#include <boost/serialization/split_free.hpp>
#include <boost/serialization/string.hpp>
#include <boost/serialization/utility.hpp>
#include <boost/serialization/vector.hpp>
#include <boost/serialization/version.hpp>
GCC_DIAG_UNUSED_LOCAL_TYPEDEFS_ON
GCC_DIAG_ON(unused-parameter)
GCC_DIAG_ON(sign-compare)
// clang-format on
#endif

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief The archived form of one knob dimension. The value and expression-result elements a
 * dimension holds depend on the knob's type, which a nested object cannot see on its own, so
 * the knob adapter archives this view of a KnobDimensionSerialization in the position where
 * the format has always had its own class (with its own class_id and version). Saving reads
 * source; loading writes destination and, for the selected entry an old choice knob kept in
 * each dimension, choiceOptionID.
 **/
struct KnobDimensionXml {
    const KnobDimensionSerialization* source;
    KnobDimensionSerialization* destination;
    KnobTypeTraits traits;
    std::optional<std::string>* choiceOptionID;

    explicit KnobDimensionXml(const KnobDimensionSerialization* sourceDimension)
        : source(sourceDimension)
        , destination(nullptr)
        , traits()
        , choiceOptionID(nullptr)
    {
    }

    KnobDimensionXml(KnobDimensionSerialization* destinationDimension,
                     const KnobTypeTraits& knobTraits,
                     std::optional<std::string>* knobChoiceOptionID)
        : source(nullptr)
        , destination(destinationDimension)
        , traits(knobTraits)
        , choiceOptionID(knobChoiceOptionID)
    {
    }
};

} // namespace Serialization
NATRON_NAMESPACE_EXIT

// Non-intrusive adapters reproducing the NVP sequence, nesting and class versions of the old
// intrusive KnobSerialization/ValueSerialization/MasterSerialization/GroupKnobSerialization, including every branch
// that reads files written by older versions.
namespace boost {
namespace serialization {

    template <class Archive>
    void
    save(Archive& ar,
         const NATRON_NAMESPACE::Serialization::KnobMasterSerialization& master,
         const unsigned int /*version*/)
    {
        ar& ::boost::serialization::make_nvp("MasterDimension", master.masterDimension);
        ar& ::boost::serialization::make_nvp("MasterNodeName", master.masterNodeName);
        ar& ::boost::serialization::make_nvp("MasterNodeNameFull", master.masterNodeNameFull);
        ar& ::boost::serialization::make_nvp("MasterKnobName", master.masterKnobName);
        ar& ::boost::serialization::make_nvp("MasterTrackName", master.masterTrackName);
    }

    template <class Archive>
    void
    load(Archive& ar,
         NATRON_NAMESPACE::Serialization::KnobMasterSerialization& master,
         const unsigned int version)
    {
        ar& ::boost::serialization::make_nvp("MasterDimension", master.masterDimension);
        ar& ::boost::serialization::make_nvp("MasterNodeName", master.masterNodeName);
        if (version >= NATRON_SERIALIZATION_KNOB_MASTER_INTRODUCE_MASTER_NODE_NAME_FULL) {
            ar& ::boost::serialization::make_nvp("MasterNodeNameFull", master.masterNodeNameFull);
        }
        ar& ::boost::serialization::make_nvp("MasterKnobName", master.masterKnobName);
        if (version >= NATRON_SERIALIZATION_KNOB_MASTER_INTRODUCE_MASTER_TRACK_NAME) {
            ar& ::boost::serialization::make_nvp("MasterTrackName", master.masterTrackName);
        }
    }

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::KnobMasterSerialization& master,
              const unsigned int version)
    {
        ::boost::serialization::split_free(ar, master, version);
    }

    template <class Archive>
    void
    save(Archive& ar,
         const NATRON_NAMESPACE::Serialization::KnobDimensionXml& item,
         const unsigned int /*version*/)
    {
        const NATRON_NAMESPACE::Serialization::KnobDimensionSerialization& dimension = *item.source;

        ar& ::boost::serialization::make_nvp("Enabled", dimension.enabled);
        bool hasAnimation = dimension.curve.has_value();
        ar& ::boost::serialization::make_nvp("HasAnimation", hasAnimation);
        if (hasAnimation) {
            ar& ::boost::serialization::make_nvp("Curve", *dimension.curve);
        }

        if (dimension.value) {
            std::visit([&ar, &dimension](const auto& value) {
                typedef typename std::decay<decltype(value)>::type ValueType;
                ValueType defaultValue = ValueType();
                if (dimension.defaultValue) {
                    const ValueType* storedDefault = std::get_if<ValueType>(&(*dimension.defaultValue));
                    if (storedDefault) {
                        defaultValue = *storedDefault;
                    }
                }
                ar& ::boost::serialization::make_nvp("Value", value);
                ar& ::boost::serialization::make_nvp("Default", defaultValue);
            },
                       *dimension.value);
        }

        bool hasMaster = dimension.master.has_value();
        ar& ::boost::serialization::make_nvp("HasMaster", hasMaster);
        if (hasMaster) {
            ar& ::boost::serialization::make_nvp("Master", *dimension.master);
        }

        ar& ::boost::serialization::make_nvp("Expression", dimension.expression);
        ar& ::boost::serialization::make_nvp("ExprHasRet", dimension.expressionHasReturnVariable);
    }

    template <typename ValueType, class Archive>
    void
    loadKnobDimensionValue(Archive& ar,
                           NATRON_NAMESPACE::Serialization::KnobDimensionXml& item,
                           const unsigned int version)
    {
        ValueType value = ValueType();
        ar& ::boost::serialization::make_nvp("Value", value);
        item.destination->value = NATRON_NAMESPACE::Serialization::KnobValueSerialization(std::in_place_type<ValueType>, value);

        if (item.traits.isChoice && (version >= NATRON_SERIALIZATION_KNOB_VALUE_INTRODUCES_CHOICE_LABEL) && (version < NATRON_SERIALIZATION_KNOB_VALUE_REMOVES_EXTRA_DATA)) {
            std::string label;
            ar& ::boost::serialization::make_nvp("Label", label);
            if (item.choiceOptionID) {
                *item.choiceOptionID = label;
            }
        }

        if (version >= NATRON_SERIALIZATION_KNOB_VALUE_INTRODUCES_DEFAULT_VALUES) {
            ValueType defaultValue = ValueType();
            ar& ::boost::serialization::make_nvp("Default", defaultValue);
            item.destination->defaultValue = NATRON_NAMESPACE::Serialization::KnobValueSerialization(std::in_place_type<ValueType>, defaultValue);
        }
    }

    template <typename ValueType, class Archive>
    void
    skipKnobDimensionExpressionResults(Archive& ar)
    {
        std::map<int, ValueType> expressionResults;
        ar& ::boost::serialization::make_nvp("ExprResults", expressionResults);
    }

    template <class Archive>
    void
    load(Archive& ar,
         NATRON_NAMESPACE::Serialization::KnobDimensionXml& item,
         const unsigned int version)
    {
        NATRON_NAMESPACE::Serialization::KnobDimensionSerialization& dimension = *item.destination;
        const NATRON_NAMESPACE::Serialization::KnobTypeTraits& traits = item.traits;

        ar& ::boost::serialization::make_nvp("Enabled", dimension.enabled);

        bool hasAnimation = false;
        ar& ::boost::serialization::make_nvp("HasAnimation", hasAnimation);
        if (hasAnimation) {
            dimension.curve.emplace();
            ar& ::boost::serialization::make_nvp("Curve", *dimension.curve);
        }

        if (traits.hasValue) {
            switch (traits.storageType) {
            case NATRON_NAMESPACE::Serialization::eKnobStorageTypeBool:
                loadKnobDimensionValue<bool>(ar, item, version);
                break;
            case NATRON_NAMESPACE::Serialization::eKnobStorageTypeInt:
                loadKnobDimensionValue<int>(ar, item, version);
                break;
            case NATRON_NAMESPACE::Serialization::eKnobStorageTypeDouble:
                loadKnobDimensionValue<double>(ar, item, version);
                break;
            case NATRON_NAMESPACE::Serialization::eKnobStorageTypeString:
                loadKnobDimensionValue<std::string>(ar, item, version);
                break;
            case NATRON_NAMESPACE::Serialization::eKnobStorageTypeNone:
                break;
            }
        }

        bool hasMaster = false;
        ar& ::boost::serialization::make_nvp("HasMaster", hasMaster);
        if (hasMaster) {
            dimension.master.emplace();
            ar& ::boost::serialization::make_nvp("Master", *dimension.master);
        }

        if (version >= NATRON_SERIALIZATION_KNOB_VALUE_INTRODUCES_EXPRESSIONS) {
            ar& ::boost::serialization::make_nvp("Expression", dimension.expression);
            ar& ::boost::serialization::make_nvp("ExprHasRet", dimension.expressionHasReturnVariable);
        }

        if ((version >= NATRON_SERIALIZATION_KNOB_VALUE_INTRODUCES_EXPRESSIONS_RESULTS) && (version < NATRON_SERIALIZATION_KNOB_VALUE_REMOVES_EXPRESSIONS_RESULTS)) {
            switch (traits.storageType) {
            case NATRON_NAMESPACE::Serialization::eKnobStorageTypeInt:
                skipKnobDimensionExpressionResults<int>(ar);
                break;
            case NATRON_NAMESPACE::Serialization::eKnobStorageTypeBool:
                skipKnobDimensionExpressionResults<bool>(ar);
                break;
            case NATRON_NAMESPACE::Serialization::eKnobStorageTypeDouble:
                skipKnobDimensionExpressionResults<double>(ar);
                break;
            case NATRON_NAMESPACE::Serialization::eKnobStorageTypeString:
                skipKnobDimensionExpressionResults<std::string>(ar);
                break;
            case NATRON_NAMESPACE::Serialization::eKnobStorageTypeNone:
                break;
            }
        }
    }

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::KnobDimensionXml& item,
              const unsigned int version)
    {
        ::boost::serialization::split_free(ar, item, version);
    }

    template <class Archive>
    void
    save(Archive& ar,
         const NATRON_NAMESPACE::Serialization::KnobSerialization& knob,
         const unsigned int /*version*/)
    {
        ar& ::boost::serialization::make_nvp("Name", knob.name);
        ar& ::boost::serialization::make_nvp("Type", knob.typeName);
        ar& ::boost::serialization::make_nvp("Dimension", knob.dimension);
        ar& ::boost::serialization::make_nvp("Secret", knob.secret);
        ar& ::boost::serialization::make_nvp("MasterIsAlias", knob.masterIsAlias);

        for (std::vector<NATRON_NAMESPACE::Serialization::KnobDimensionSerialization>::const_iterator it = knob.dimensions.begin(); it != knob.dimensions.end(); ++it) {
            NATRON_NAMESPACE::Serialization::KnobDimensionXml item(&(*it));
            ar& ::boost::serialization::make_nvp("item", item);
        }

        if (knob.parametricCurves) {
            ar& ::boost::serialization::make_nvp("ParametricCurves", *knob.parametricCurves);
        } else if (knob.stringAnimation) {
            ar& ::boost::serialization::make_nvp("StringsAnimation", *knob.stringAnimation);
        }
        if (knob.choiceOptionID) {
            ar& ::boost::serialization::make_nvp("ChoiceLabel", *knob.choiceOptionID);
        }

        ar& ::boost::serialization::make_nvp("UserKnob", knob.isUserKnob);
        if (knob.isUserKnob) {
            ar& ::boost::serialization::make_nvp("Label", knob.label);
            ar& ::boost::serialization::make_nvp("Help", knob.tooltip);
            ar& ::boost::serialization::make_nvp("NewLine", knob.triggerNewLine);
            ar& ::boost::serialization::make_nvp("Evaluate", knob.evaluatesOnChange);
            ar& ::boost::serialization::make_nvp("Animates", knob.animationEnabled);
            ar& ::boost::serialization::make_nvp("Persistent", knob.isPersistent);

            if (const NATRON_NAMESPACE::Serialization::KnobChoiceExtraSerialization* choiceData = std::get_if<NATRON_NAMESPACE::Serialization::KnobChoiceExtraSerialization>(&knob.extraData)) {
                ar& ::boost::serialization::make_nvp("Entries", choiceData->entries);
                ar& ::boost::serialization::make_nvp("Helps", choiceData->helpStrings);
            } else if (const NATRON_NAMESPACE::Serialization::KnobValueRangeSerialization* rangeData = std::get_if<NATRON_NAMESPACE::Serialization::KnobValueRangeSerialization>(&knob.extraData)) {
                ar& ::boost::serialization::make_nvp("Min", rangeData->minimum);
                ar& ::boost::serialization::make_nvp("Max", rangeData->maximum);
                ar& ::boost::serialization::make_nvp("DMin", rangeData->displayMinimum);
                ar& ::boost::serialization::make_nvp("DMax", rangeData->displayMaximum);
            } else if (const NATRON_NAMESPACE::Serialization::KnobFileExtraSerialization* fileData = std::get_if<NATRON_NAMESPACE::Serialization::KnobFileExtraSerialization>(&knob.extraData)) {
                ar& ::boost::serialization::make_nvp("Sequences", fileData->useSequences);
            } else if (const NATRON_NAMESPACE::Serialization::KnobPathExtraSerialization* pathData = std::get_if<NATRON_NAMESPACE::Serialization::KnobPathExtraSerialization>(&knob.extraData)) {
                ar& ::boost::serialization::make_nvp("MultiPath", pathData->multiPath);
            } else if (const NATRON_NAMESPACE::Serialization::KnobLayerSelectExtraSerialization* layerSelectData = std::get_if<NATRON_NAMESPACE::Serialization::KnobLayerSelectExtraSerialization>(&knob.extraData)) {
                ar& ::boost::serialization::make_nvp("WithChannelButtons", layerSelectData->withChannelButtons);
            } else if (const NATRON_NAMESPACE::Serialization::KnobTextExtraSerialization* textData = std::get_if<NATRON_NAMESPACE::Serialization::KnobTextExtraSerialization>(&knob.extraData)) {
                ar& ::boost::serialization::make_nvp("IsLabel", textData->isLabel);
                ar& ::boost::serialization::make_nvp("IsMultiLine", textData->isMultiLine);
                ar& ::boost::serialization::make_nvp("UseRichText", textData->usesRichText);
            }

            if (knob.useHostOverlayHandle) {
                ar& ::boost::serialization::make_nvp("HasOverlayHandle", *knob.useHostOverlayHandle);
            }
        }
    }

    template <class Archive>
    void
    loadKnobUserDefinition(Archive& ar,
                           NATRON_NAMESPACE::Serialization::KnobSerialization& knob,
                           const NATRON_NAMESPACE::Serialization::KnobTypeTraits& traits,
                           const unsigned int version)
    {
        ar& ::boost::serialization::make_nvp("Label", knob.label);
        ar& ::boost::serialization::make_nvp("Help", knob.tooltip);
        ar& ::boost::serialization::make_nvp("NewLine", knob.triggerNewLine);
        ar& ::boost::serialization::make_nvp("Evaluate", knob.evaluatesOnChange);
        ar& ::boost::serialization::make_nvp("Animates", knob.animationEnabled);
        ar& ::boost::serialization::make_nvp("Persistent", knob.isPersistent);

        if (traits.isChoice) {
            NATRON_NAMESPACE::Serialization::KnobChoiceExtraSerialization* choiceData = std::get_if<NATRON_NAMESPACE::Serialization::KnobChoiceExtraSerialization>(&knob.extraData);
            if (!choiceData) {
                choiceData = &knob.extraData.emplace<NATRON_NAMESPACE::Serialization::KnobChoiceExtraSerialization>();
            }
            ar& ::boost::serialization::make_nvp("Entries", choiceData->entries);
            if (version >= NATRON_SERIALIZATION_KNOB_INTRODUCES_CHOICE_HELP_STRINGS) {
                ar& ::boost::serialization::make_nvp("Helps", choiceData->helpStrings);
            }
        }

        if (traits.isString) {
            NATRON_NAMESPACE::Serialization::KnobTextExtraSerialization textData;
            ar& ::boost::serialization::make_nvp("IsLabel", textData.isLabel);
            ar& ::boost::serialization::make_nvp("IsMultiLine", textData.isMultiLine);
            ar& ::boost::serialization::make_nvp("UseRichText", textData.usesRichText);
            knob.extraData = textData;
        }

        if (traits.isDouble || traits.isInt || traits.isColor) {
            NATRON_NAMESPACE::Serialization::KnobValueRangeSerialization rangeData;
            ar& ::boost::serialization::make_nvp("Min", rangeData.minimum);
            ar& ::boost::serialization::make_nvp("Max", rangeData.maximum);
            if (version >= NATRON_SERIALIZATION_KNOB_INTRODUCES_DISPLAY_MIN_MAX) {
                ar& ::boost::serialization::make_nvp("DMin", rangeData.displayMinimum);
                ar& ::boost::serialization::make_nvp("DMax", rangeData.displayMaximum);
            }
            knob.extraData = rangeData;
        }

        if (traits.isInputFile || traits.isOutputFile) {
            NATRON_NAMESPACE::Serialization::KnobFileExtraSerialization fileData;
            ar& ::boost::serialization::make_nvp("Sequences", fileData.useSequences);
            knob.extraData = fileData;
        }

        if (traits.isPath) {
            NATRON_NAMESPACE::Serialization::KnobPathExtraSerialization pathData;
            ar& ::boost::serialization::make_nvp("MultiPath", pathData.multiPath);
            knob.extraData = pathData;
        }

        if (traits.isLayerSelect) {
            NATRON_NAMESPACE::Serialization::KnobLayerSelectExtraSerialization layerSelectData;
            if (version >= NATRON_SERIALIZATION_KNOB_INTRODUCES_LAYER_SELECT_CHANNEL_BUTTONS) {
                ar& ::boost::serialization::make_nvp("WithChannelButtons", layerSelectData.withChannelButtons);
            }
            knob.extraData = layerSelectData;
        }

        if (traits.isDouble && (version >= NATRON_SERIALIZATION_KNOB_INTRODUCES_NATIVE_OVERLAYS) && (knob.dimension == 2)) {
            bool useHostOverlayHandle = false;
            ar& ::boost::serialization::make_nvp("HasOverlayHandle", useHostOverlayHandle);
            knob.useHostOverlayHandle = useHostOverlayHandle;
        }

        if ((version >= NATRON_SERIALIZATION_KNOB_INTRODUCES_DEFAULT_VALUES) && (version < NATRON_SERIALIZATION_KNOB_REMOVE_DEFAULT_VALUES)) {
            // Only plain Bool knobs saved a bool default here, unlike Button, Separator, Group and
            // Page, which share the bool storage type.
            for (int i = 0; i < knob.dimension; ++i) {
                if (traits.storageType == NATRON_NAMESPACE::Serialization::eKnobStorageTypeDouble) {
                    double defaultValue;
                    ar& ::boost::serialization::make_nvp("DefaultValue", defaultValue);
                } else if (traits.storageType == NATRON_NAMESPACE::Serialization::eKnobStorageTypeInt) {
                    int defaultValue;
                    ar& ::boost::serialization::make_nvp("DefaultValue", defaultValue);
                } else if (traits.isBool) {
                    bool defaultValue;
                    ar& ::boost::serialization::make_nvp("DefaultValue", defaultValue);
                } else if (traits.storageType == NATRON_NAMESPACE::Serialization::eKnobStorageTypeString) {
                    std::string defaultValue;
                    ar& ::boost::serialization::make_nvp("DefaultValue", defaultValue);
                }
            }
        }
    }

    template <class Archive>
    void
    load(Archive& ar,
         NATRON_NAMESPACE::Serialization::KnobSerialization& knob,
         const unsigned int version)
    {
        knob = NATRON_NAMESPACE::Serialization::KnobSerialization();
        knob.version = version;

        ar& ::boost::serialization::make_nvp("Name", knob.name);
        ar& ::boost::serialization::make_nvp("Type", knob.typeName);
        ar& ::boost::serialization::make_nvp("Dimension", knob.dimension);
        if (knob.dimension > 0) {
            knob.dimensions.resize(static_cast<std::size_t>(knob.dimension));
        }

        const NATRON_NAMESPACE::Serialization::KnobTypeTraits traits = NATRON_NAMESPACE::Serialization::getKnobTypeTraits(knob.typeName);
        // The format stops reading a knob of a type no knob can be created for right here, leaving
        // the rest of its element unread.
        if (!traits.isKnown) {
            return;
        }

        ar& ::boost::serialization::make_nvp("Secret", knob.secret);

        if (version >= NATRON_SERIALIZATION_KNOB_INTRODUCES_ALIAS) {
            ar& ::boost::serialization::make_nvp("MasterIsAlias", knob.masterIsAlias);
        } else {
            knob.masterIsAlias = false;
        }

        if (traits.isChoice) {
            knob.choiceOptionID = std::string();
            knob.extraData = NATRON_NAMESPACE::Serialization::KnobChoiceExtraSerialization();
        }

        for (std::size_t i = 0; i < knob.dimensions.size(); ++i) {
            NATRON_NAMESPACE::Serialization::KnobDimensionXml item(&knob.dimensions[i], traits, &knob.choiceOptionID);
            ar& ::boost::serialization::make_nvp("item", item);
        }

        if (traits.isParametric) {
            knob.parametricCurves.emplace();
            ar& ::boost::serialization::make_nvp("ParametricCurves", *knob.parametricCurves);
        } else if (traits.hasStringAnimation) {
            knob.stringAnimation.emplace();
            ar& ::boost::serialization::make_nvp("StringsAnimation", *knob.stringAnimation);
        }

        if ((version >= NATRON_SERIALIZATION_KNOB_INTRODUCES_SLAVED_TRACKS) && (version < NATRON_SERIALIZATION_KNOB_REMOVE_SLAVED_TRACKS) && traits.isDouble && (NATRON_NAMESPACE::Serialization::scriptFriendlyKnobName(knob.name) == "center") && (knob.dimension == 2)) {
            int count = 0;
            ar& ::boost::serialization::make_nvp("SlavePtsNo", count);
            for (int i = 0; i < count; ++i) {
                std::string rotoNodeName, bezierName;
                int controlPointIndex;
                bool isFeather;
                int offsetTime;
                ar& ::boost::serialization::make_nvp("SlavePtNodeName", rotoNodeName);
                ar& ::boost::serialization::make_nvp("SlavePtBezier", bezierName);
                ar& ::boost::serialization::make_nvp("SlavePtIndex", controlPointIndex);
                ar& ::boost::serialization::make_nvp("SlavePtIsFeather", isFeather);
                if (version >= NATRON_SERIALIZATION_KNOB_INTRODUCES_SLAVED_TRACKS_OFFSET) {
                    ar& ::boost::serialization::make_nvp("OffsetTime", offsetTime);
                }
            }
        }

        if (version >= NATRON_SERIALIZATION_KNOB_INTRODUCES_USER_KNOB) {
            if (traits.isChoice) {
                ar& ::boost::serialization::make_nvp("ChoiceLabel", *knob.choiceOptionID);
            }

            ar& ::boost::serialization::make_nvp("UserKnob", knob.isUserKnob);
            if (knob.isUserKnob) {
                loadKnobUserDefinition(ar, knob, traits, version);
            }
        }
    }

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::KnobSerialization& knob,
              const unsigned int version)
    {
        ::boost::serialization::split_free(ar, knob, version);
    }

    template <class Archive>
    void
    save(Archive& ar,
         const NATRON_NAMESPACE::Serialization::GroupKnobSerialization& group,
         const unsigned int /*version*/)
    {
        ar& ::boost::serialization::make_nvp("Name", group.name);
        ar& ::boost::serialization::make_nvp("Label", group.label);
        ar& ::boost::serialization::make_nvp("Secret", group.secret);
        ar& ::boost::serialization::make_nvp("IsTab", group.isSetAsTab);
        ar& ::boost::serialization::make_nvp("IsOpened", group.isOpened);
        int nbChildren = (int)group.children.size();
        ar& ::boost::serialization::make_nvp("NbChildren", nbChildren);
        for (std::list<NATRON_NAMESPACE::Serialization::GroupKnobChildSerialization>::const_iterator it = group.children.begin(); it != group.children.end(); ++it) {
            std::string type = it->isGroup ? "Group" : "Regular";
            ar& ::boost::serialization::make_nvp("Type", type);
            if (it->isGroup) {
                ar& ::boost::serialization::make_nvp("item", it->group);
            } else {
                ar& ::boost::serialization::make_nvp("item", it->knob);
            }
        }
    }

    template <class Archive>
    void
    load(Archive& ar,
         NATRON_NAMESPACE::Serialization::GroupKnobSerialization& group,
         const unsigned int /*version*/)
    {
        group = NATRON_NAMESPACE::Serialization::GroupKnobSerialization();

        ar& ::boost::serialization::make_nvp("Name", group.name);
        ar& ::boost::serialization::make_nvp("Label", group.label);
        ar& ::boost::serialization::make_nvp("Secret", group.secret);
        ar& ::boost::serialization::make_nvp("IsTab", group.isSetAsTab);
        ar& ::boost::serialization::make_nvp("IsOpened", group.isOpened);
        int nbChildren = 0;
        ar& ::boost::serialization::make_nvp("NbChildren", nbChildren);
        for (int i = 0; i < nbChildren; ++i) {
            std::string type;
            ar& ::boost::serialization::make_nvp("Type", type);

            // Each child is read in place: an archive may record where it loaded an object.
            group.children.push_back(NATRON_NAMESPACE::Serialization::GroupKnobChildSerialization());
            NATRON_NAMESPACE::Serialization::GroupKnobChildSerialization& child = group.children.back();
            child.isGroup = (type == "Group");
            if (child.isGroup) {
                ar& ::boost::serialization::make_nvp("item", child.group);
            } else {
                ar& ::boost::serialization::make_nvp("item", child.knob);
            }
        }
    }

    template <class Archive>
    void
    serialize(Archive& ar,
              NATRON_NAMESPACE::Serialization::GroupKnobSerialization& group,
              const unsigned int version)
    {
        ::boost::serialization::split_free(ar, group, version);
    }

} // namespace serialization
} // namespace boost

BOOST_CLASS_VERSION(NATRON_NAMESPACE::Serialization::KnobSerialization, NATRON_SERIALIZATION_KNOB_VERSION)
BOOST_CLASS_VERSION(NATRON_NAMESPACE::Serialization::KnobDimensionXml, NATRON_SERIALIZATION_KNOB_VALUE_VERSION)
BOOST_CLASS_VERSION(NATRON_NAMESPACE::Serialization::KnobMasterSerialization, NATRON_SERIALIZATION_KNOB_MASTER_VERSION)

#endif // Serialization_BoostXml_KnobXml_h
