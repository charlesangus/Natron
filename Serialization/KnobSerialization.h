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

#ifndef Serialization_KnobSerialization_h
#define Serialization_KnobSerialization_h

#include "Global/Macros.h"

#include <list>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "Serialization/CurveSerialization.h"
#include "Serialization/SerializationFwd.h"

// The knob, value and master class versions the XML writer stamps on what it writes.

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief The static value of one knob dimension: the T of the Engine Knob<T> holding it.
 **/
typedef std::variant<bool, int, double, std::string> KnobValueSerialization;

/**
 * @brief Where one knob dimension is linked to: a knob of another node (or of a track of
 * another node). masterDimension is -1 when the link exists but is not meant to be saved.
 **/
struct KnobMasterSerialization {
    int masterDimension;
    std::string masterNodeName;
    std::string masterNodeNameFull;
    std::string masterTrackName;
    std::string masterKnobName;

    KnobMasterSerialization()
        : masterDimension(-1)
        , masterNodeName()
        , masterNodeNameFull()
        , masterTrackName()
        , masterKnobName()
    {
    }

    bool operator==(const KnobMasterSerialization&) const = default;
};

/**
 * @brief The saved state of one knob dimension.
 **/
struct KnobDimensionSerialization {
    bool enabled;

    // Present when the dimension is animated.
    std::optional<CurveSerialization> curve;

    // Present for knob types that hold a value of their own (not pages, groups, separators,
    // buttons or parametric knobs). defaultValue holds the same alternative as value.
    std::optional<KnobValueSerialization> value;
    std::optional<KnobValueSerialization> defaultValue;

    // Present when the dimension is linked to another knob.
    std::optional<KnobMasterSerialization> master;

    std::string expression;
    bool expressionHasReturnVariable;

    KnobDimensionSerialization()
        : enabled(true)
        , curve()
        , value()
        , defaultValue()
        , master()
        , expression()
        , expressionHasReturnVariable(false)
    {
    }

    bool operator==(const KnobDimensionSerialization&) const = default;
};

/**
 * @brief The menu entries of a choice knob and their help strings.
 **/
struct KnobChoiceExtraSerialization {
    std::vector<std::string> entries;
    std::vector<std::string> helpStrings;
};

/**
 * @brief The range of an Int, Double or Color user knob.
 **/
struct KnobValueRangeSerialization {
    double minimum;
    double maximum;
    double displayMinimum;
    double displayMaximum;

    KnobValueRangeSerialization()
        : minimum(0.)
        , maximum(0.)
        , displayMinimum(0.)
        , displayMaximum(0.)
    {
    }
};

/**
 * @brief Whether an input or output file user knob holds an image sequence.
 **/
struct KnobFileExtraSerialization {
    bool useSequences;

    KnobFileExtraSerialization()
        : useSequences(false)
    {
    }
};

/**
 * @brief Whether a path user knob holds a list of paths.
 **/
struct KnobPathExtraSerialization {
    bool multiPath;

    KnobPathExtraSerialization()
        : multiPath(false)
    {
    }
};

/**
 * @brief Whether a layer-select user knob shows per-channel buttons.
 **/
struct KnobLayerSelectExtraSerialization {
    bool withChannelButtons;

    KnobLayerSelectExtraSerialization()
        : withChannelButtons(false)
    {
    }
};

/**
 * @brief How a String user knob displays its text.
 **/
struct KnobTextExtraSerialization {
    bool isLabel;
    bool isMultiLine;
    bool usesRichText;

    KnobTextExtraSerialization()
        : isLabel(false)
        , isMultiLine(false)
        , usesRichText(false)
    {
    }
};

/**
 * @brief The type-specific data of a knob. A choice knob always carries its entries; the
 * other alternatives are only filled for user knobs, the only ones whose definition is saved.
 **/
typedef std::variant<std::monostate,
                     KnobChoiceExtraSerialization,
                     KnobValueRangeSerialization,
                     KnobFileExtraSerialization,
                     KnobPathExtraSerialization,
                     KnobLayerSelectExtraSerialization,
                     KnobTextExtraSerialization>
    KnobExtraSerialization;

/**
 * @brief Plain-data equivalent of everything a project file saves for one knob.
 **/
struct KnobSerialization {
    // The knob's script name, as saved. Engine turns it into a script-friendly name when it is
    // given to a knob, see scriptFriendlyKnobName().
    std::string name;
    std::string typeName;
    int dimension;
    bool secret;

    // When set, the master of the first dimension is a knob this knob is an alias of.
    bool masterIsAlias;

    // One entry per dimension, or a single entry standing for every dimension of a
    // multi-dimensional knob whose dimensions are folded. The entry count is what the loaded
    // knob's fold state is restored from.
    std::vector<KnobDimensionSerialization> dimensions;

    // Present for parametric knobs only.
    std::optional<std::list<CurveSerialization>> parametricCurves;

    // Present for knobs whose string value can animate (String and InputFile knobs).
    std::optional<std::map<int, std::string>> stringAnimation;

    // Present for choice knobs: the ID of the selected entry, used to find it again when the
    // entries changed since the project was saved.
    std::optional<std::string> choiceOptionID;

    bool isUserKnob;
    std::string label;
    std::string tooltip;
    bool triggerNewLine;
    bool evaluatesOnChange;
    bool animationEnabled;
    bool isPersistent;
    KnobExtraSerialization extraData;

    // Present for two-dimensional Double user knobs only.
    std::optional<bool> useHostOverlayHandle;

    KnobSerialization()
        : name()
        , typeName()
        , dimension(0)
        , secret(false)
        , masterIsAlias(false)
        , dimensions()
        , parametricCurves()
        , stringAnimation()
        , choiceOptionID()
        , isUserKnob(false)
        , label()
        , tooltip()
        , triggerNewLine(false)
        , evaluatesOnChange(false)
        , animationEnabled(false)
        , isPersistent(false)
        , extraData()
        , useHostOverlayHandle()
    {
    }
};

struct GroupKnobChildSerialization;

/**
 * @brief The layout of a user page or user group knob: its definition and, in order, the user
 * knobs and nested groups it holds.
 **/
struct GroupKnobSerialization {
    std::string name;
    std::string label;
    bool secret;

    // Only meaningful for groups.
    bool isSetAsTab;
    bool isOpened;
    // Whether the group declares Reverse-Flow Variables, see KnobGroup::setAsRFVGroup().
    bool isRFVGroup;

    std::list<GroupKnobChildSerialization> children;

    GroupKnobSerialization();
};

/**
 * @brief One child of a user page or group: the nested group when isGroup is set, the regular
 * user knob otherwise.
 **/
struct GroupKnobChildSerialization {
    bool isGroup;
    KnobSerialization knob;
    GroupKnobSerialization group;

    GroupKnobChildSerialization()
        : isGroup(false)
        , knob()
        , group()
    {
    }
};

// Defined once GroupKnobChildSerialization is complete, which the children list needs.
inline GroupKnobSerialization::GroupKnobSerialization()
    : name()
    , label()
    , secret(false)
    , isSetAsTab(false)
    , isOpened(false)
    , isRFVGroup(false)
    , children()
{
}

/**
 * @brief The Knob<T> value type a knob type name stands for.
 **/
enum KnobStorageTypeEnum {
    eKnobStorageTypeNone = 0,
    eKnobStorageTypeBool,
    eKnobStorageTypeInt,
    eKnobStorageTypeDouble,
    eKnobStorageTypeString
};

/**
 * @brief What loading needs to know about a knob type from its type name alone, since the
 * archive is read without any Engine knob at hand. The names are those Engine's knob classes
 * return from typeNameStatic().
 **/
struct KnobTypeTraits {
    // One of the knob types a project file can create.
    bool isKnown;

    KnobStorageTypeEnum storageType;

    // Each dimension saves a Value and Default of storageType.
    bool hasValue;

    // The string value can animate (String and InputFile knobs).
    bool hasStringAnimation;

    bool isInt;
    bool isBool;
    bool isDouble;
    bool isColor;
    bool isChoice;
    bool isString;
    bool isInputFile;
    bool isOutputFile;
    bool isPath;
    bool isParametric;
    bool isLayerSelect;

    KnobTypeTraits()
        : isKnown(false)
        , storageType(eKnobStorageTypeNone)
        , hasValue(false)
        , hasStringAnimation(false)
        , isInt(false)
        , isBool(false)
        , isDouble(false)
        , isColor(false)
        , isChoice(false)
        , isString(false)
        , isInputFile(false)
        , isOutputFile(false)
        , isPath(false)
        , isParametric(false)
        , isLayerSelect(false)
    {
    }
};

KnobTypeTraits getKnobTypeTraits(const std::string& typeName);

/**
 * @brief The name Engine gives a knob for a saved name (spaces become underscores, other
 * characters that are not alphanumeric are dropped, a leading digit and the Python keywords
 * "from" and "lambda" get a 'p' prefix).
 * Loading code that tested a knob's name after Engine renamed it needs the same conversion.
 **/
std::string scriptFriendlyKnobName(const std::string& name);

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_KnobSerialization_h
