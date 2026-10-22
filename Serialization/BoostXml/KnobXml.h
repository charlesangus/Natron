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
 * the format has always had its own class (with its own class_id and version).
 **/
struct KnobDimensionXml {
    const KnobDimensionSerialization* source;

    explicit KnobDimensionXml(const KnobDimensionSerialization* sourceDimension)
        : source(sourceDimension)
    {
    }
};

} // namespace Serialization
NATRON_NAMESPACE_EXIT

// Non-intrusive adapters reproducing the NVP sequence, nesting and class versions of the old
// intrusive KnobSerialization/ValueSerialization/MasterSerialization/GroupKnobSerialization.
// They write the current layout only.
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
        int savedDimensions = (int)knob.dimensions.size();
        ar& ::boost::serialization::make_nvp("SavedDimensions", savedDimensions);

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
