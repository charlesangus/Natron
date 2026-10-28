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

#include "SchemaKnobMapper.h"

#include <algorithm>
#include <cctype>
#include <set>

#include <QString>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"

NATRON_NAMESPACE_ENTER

namespace SchemaKnobMapper {
namespace {
    const char* const kInstancePlaceholder = "__INSTANCE_NAME__";

    bool
    toSchemaKind(PXR_NS::UsdSchemaKind kind,
                 SchemaKindEnum* result)
    {
        switch (kind) {
        case PXR_NS::UsdSchemaKind::ConcreteTyped:
            *result = eSchemaKindConcreteTyped;

            return true;
        case PXR_NS::UsdSchemaKind::AbstractTyped:
            *result = eSchemaKindAbstractTyped;

            return true;
        case PXR_NS::UsdSchemaKind::SingleApplyAPI:
            *result = eSchemaKindSingleApplyAPI;

            return true;
        case PXR_NS::UsdSchemaKind::MultipleApplyAPI:
            *result = eSchemaKindMultipleApplyAPI;

            return true;
        default:
            return false;
        }
    }

    std::vector<SchemaEntry>
    buildSchemaList()
    {
        std::vector<SchemaEntry> result;
        std::set<PXR_NS::TfType> types;
        const PXR_NS::UsdSchemaRegistry& registry = PXR_NS::UsdSchemaRegistry::GetInstance();

        PXR_NS::TfType::Find<PXR_NS::UsdSchemaBase>().GetAllDerivedTypes(&types);
        for (const PXR_NS::TfType& type : types) {
            const PXR_NS::UsdSchemaRegistry::SchemaInfo* info = registry.FindSchemaInfo(type);
            SchemaEntry entry;
            if (!info || info->identifier.IsEmpty() || !toSchemaKind(info->kind, &entry.kind)) {
                continue;
            }
            entry.identifier = info->identifier.GetString();
            const PXR_NS::PlugPluginPtr plugin = PXR_NS::PlugRegistry::GetInstance().GetPluginForType(type);
            entry.group = plugin ? plugin->GetName() : std::string("other");
            result.push_back(entry);
        }
        std::sort(result.begin(), result.end(), [](const SchemaEntry& a, const SchemaEntry& b) {
            return (a.group != b.group) ? (a.group < b.group) : (a.identifier < b.identifier);
        });

        return result;
    }

    const PXR_NS::UsdPrimDefinition*
    findPrimDefinition(const SchemaEntry& entry)
    {
        const PXR_NS::UsdSchemaRegistry& registry = PXR_NS::UsdSchemaRegistry::GetInstance();
        const PXR_NS::TfToken identifier(entry.identifier);

        switch (entry.kind) {
        case eSchemaKindConcreteTyped:
            return registry.FindConcretePrimDefinition(identifier);
        case eSchemaKindAbstractTyped:
            return registry.FindAbstractPrimDefinition(identifier);
        case eSchemaKindSingleApplyAPI:
        case eSchemaKindMultipleApplyAPI:
            return registry.FindAppliedAPIPrimDefinition(identifier);
        }

        return nullptr;
    }

    template <typename VEC>
    void
    appendVector(const PXR_NS::VtValue& value,
                 std::vector<double>* numbers)
    {
        if (!value.IsHolding<VEC>()) {
            return;
        }
        const VEC& v = value.UncheckedGet<VEC>();
        for (std::size_t i = 0; i < VEC::dimension; ++i) {
            numbers->push_back((double)v[i]);
        }
    }

    // The fallback as knob defaults: the numbers of a numeric or bool value, the text of a token,
    // string or asset.
    void
    readFallback(const PXR_NS::VtValue& value,
                 AttributeKnob* knob)
    {
        std::vector<double>& numbers = knob->defaultNumbers;

        if (value.IsHolding<bool>()) {
            numbers.push_back(value.UncheckedGet<bool>() ? 1. : 0.);
        } else if (value.IsHolding<int>()) {
            numbers.push_back((double)value.UncheckedGet<int>());
        } else if (value.IsHolding<unsigned int>()) {
            numbers.push_back((double)value.UncheckedGet<unsigned int>());
        } else if (value.IsHolding<float>()) {
            numbers.push_back((double)value.UncheckedGet<float>());
        } else if (value.IsHolding<double>()) {
            numbers.push_back(value.UncheckedGet<double>());
        } else if (value.IsHolding<PXR_NS::GfHalf>()) {
            numbers.push_back((double)(float)value.UncheckedGet<PXR_NS::GfHalf>());
        } else if (value.IsHolding<PXR_NS::TfToken>()) {
            knob->defaultText = value.UncheckedGet<PXR_NS::TfToken>().GetString();
        } else if (value.IsHolding<std::string>()) {
            knob->defaultText = value.UncheckedGet<std::string>();
        } else if (value.IsHolding<PXR_NS::SdfAssetPath>()) {
            knob->defaultText = value.UncheckedGet<PXR_NS::SdfAssetPath>().GetAssetPath();
        } else {
            appendVector<PXR_NS::GfVec2f>(value, &numbers);
            appendVector<PXR_NS::GfVec3f>(value, &numbers);
            appendVector<PXR_NS::GfVec4f>(value, &numbers);
            appendVector<PXR_NS::GfVec2d>(value, &numbers);
            appendVector<PXR_NS::GfVec3d>(value, &numbers);
            appendVector<PXR_NS::GfVec4d>(value, &numbers);
            appendVector<PXR_NS::GfVec2h>(value, &numbers);
            appendVector<PXR_NS::GfVec3h>(value, &numbers);
            appendVector<PXR_NS::GfVec4h>(value, &numbers);
        }
        if (!numbers.empty() && ((int)numbers.size() != knob->dimension)) {
            numbers.clear();
        }
    }

    // The knob kind and dimension of an attribute of type typeName, or false when no knob maps it.
    bool
    mapType(const PXR_NS::SdfValueTypeName& typeName,
            bool hasAllowedTokens,
            KnobKindEnum* kind,
            int* dimension)
    {
        if (!typeName || typeName.IsArray()) {
            return false;
        }
        const PXR_NS::TfType type = typeName.GetType();
        const bool isColor = (typeName.GetRole() == PXR_NS::SdfValueRoleNames->Color);

        *dimension = 1;
        if (type == PXR_NS::TfType::Find<bool>()) {
            *kind = eKnobKindBool;
        } else if ((type == PXR_NS::TfType::Find<int>()) || (type == PXR_NS::TfType::Find<unsigned int>())) {
            *kind = eKnobKindInt;
        } else if ((type == PXR_NS::TfType::Find<float>()) || (type == PXR_NS::TfType::Find<double>()) || (type == PXR_NS::TfType::Find<PXR_NS::GfHalf>())) {
            *kind = eKnobKindDouble;
        } else if ((type == PXR_NS::TfType::Find<PXR_NS::GfVec2f>()) || (type == PXR_NS::TfType::Find<PXR_NS::GfVec2d>()) || (type == PXR_NS::TfType::Find<PXR_NS::GfVec2h>())) {
            *kind = eKnobKindDouble;
            *dimension = 2;
        } else if ((type == PXR_NS::TfType::Find<PXR_NS::GfVec3f>()) || (type == PXR_NS::TfType::Find<PXR_NS::GfVec3d>()) || (type == PXR_NS::TfType::Find<PXR_NS::GfVec3h>())) {
            *kind = (isColor && (type != PXR_NS::TfType::Find<PXR_NS::GfVec3h>())) ? eKnobKindColor : eKnobKindDouble;
            *dimension = 3;
        } else if ((type == PXR_NS::TfType::Find<PXR_NS::GfVec4f>()) || (type == PXR_NS::TfType::Find<PXR_NS::GfVec4d>()) || (type == PXR_NS::TfType::Find<PXR_NS::GfVec4h>())) {
            *kind = (isColor && (type != PXR_NS::TfType::Find<PXR_NS::GfVec4h>())) ? eKnobKindColor : eKnobKindDouble;
            *dimension = 4;
        } else if (type == PXR_NS::TfType::Find<PXR_NS::TfToken>()) {
            *kind = hasAllowedTokens ? eKnobKindChoice : eKnobKindString;
        } else if (type == PXR_NS::TfType::Find<std::string>()) {
            *kind = eKnobKindString;
        } else if (type == PXR_NS::TfType::Find<PXR_NS::SdfAssetPath>()) {
            *kind = eKnobKindFile;
        } else {
            return false;
        }

        return true;
    }

    template <typename VEC>
    PXR_NS::VtValue
    makeVector(const std::vector<double>& numbers)
    {
        VEC v;

        for (std::size_t i = 0; i < VEC::dimension; ++i) {
            v[i] = (typename VEC::ScalarType)((i < numbers.size()) ? numbers[i] : 0.);
        }

        return PXR_NS::VtValue(v);
    }

    PXR_NS::VtValue
    makeNumeric(const PXR_NS::TfType& type,
                const std::vector<double>& numbers)
    {
        const double first = numbers.empty() ? 0. : numbers[0];

        if (type == PXR_NS::TfType::Find<float>()) {
            return PXR_NS::VtValue((float)first);
        }
        if (type == PXR_NS::TfType::Find<double>()) {
            return PXR_NS::VtValue(first);
        }
        if (type == PXR_NS::TfType::Find<PXR_NS::GfHalf>()) {
            return PXR_NS::VtValue(PXR_NS::GfHalf((float)first));
        }
        if (type == PXR_NS::TfType::Find<PXR_NS::GfVec2f>()) {
            return makeVector<PXR_NS::GfVec2f>(numbers);
        }
        if (type == PXR_NS::TfType::Find<PXR_NS::GfVec3f>()) {
            return makeVector<PXR_NS::GfVec3f>(numbers);
        }
        if (type == PXR_NS::TfType::Find<PXR_NS::GfVec4f>()) {
            return makeVector<PXR_NS::GfVec4f>(numbers);
        }
        if (type == PXR_NS::TfType::Find<PXR_NS::GfVec2d>()) {
            return makeVector<PXR_NS::GfVec2d>(numbers);
        }
        if (type == PXR_NS::TfType::Find<PXR_NS::GfVec3d>()) {
            return makeVector<PXR_NS::GfVec3d>(numbers);
        }
        if (type == PXR_NS::TfType::Find<PXR_NS::GfVec4d>()) {
            return makeVector<PXR_NS::GfVec4d>(numbers);
        }
        if (type == PXR_NS::TfType::Find<PXR_NS::GfVec2h>()) {
            return makeVector<PXR_NS::GfVec2h>(numbers);
        }
        if (type == PXR_NS::TfType::Find<PXR_NS::GfVec3h>()) {
            return makeVector<PXR_NS::GfVec3h>(numbers);
        }
        if (type == PXR_NS::TfType::Find<PXR_NS::GfVec4h>()) {
            return makeVector<PXR_NS::GfVec4h>(numbers);
        }

        return PXR_NS::VtValue();
    }

    template <typename KNOB>
    void
    setNumericDefaults(const std::shared_ptr<KNOB>& valueKnob,
                       const AttributeKnob& knob)
    {
        for (int i = 0; i < (int)knob.defaultNumbers.size(); ++i) {
            valueKnob->setDefaultValue(knob.defaultNumbers[i], i);
        }
    }
} // anonymous namespace

const std::vector<SchemaEntry>&
listSchemas()
{
    static const std::vector<SchemaEntry> schemas = buildSchemaList();

    return schemas;
}

bool
findSchema(const std::string& identifier,
           SchemaEntry* entry)
{
    for (const SchemaEntry& candidate : listSchemas()) {
        if (candidate.identifier == identifier) {
            *entry = candidate;

            return true;
        }
    }

    return false;
}

bool
describeSchema(const std::string& identifier,
               SchemaDescription* description,
               std::string* errorMessage)
{
    *description = SchemaDescription();
    const PXR_NS::UsdPrimDefinition* definition = findSchema(identifier, &description->schema) ? findPrimDefinition(description->schema) : nullptr;

    if (!definition) {
        *errorMessage = EffectInstance::tr("USD has no schema named %1.").arg(QString::fromStdString(identifier)).toStdString();

        return false;
    }
    for (const PXR_NS::TfToken& name : definition->GetPropertyNames()) {
        const PXR_NS::UsdPrimDefinition::Property property = definition->GetPropertyDefinition(name);
        const PXR_NS::UsdPrimDefinition::Attribute attribute(property);
        AttributeKnob knob;
        knob.name = name.GetString();
        if (!attribute) {
            description->unsupported.push_back(knob.name);
            continue;
        }
        PXR_NS::VtTokenArray allowed;
        property.GetMetadata(PXR_NS::SdfFieldKeys->AllowedTokens, &allowed);
        const PXR_NS::SdfValueTypeName typeName = attribute.GetTypeName();
        if (!mapType(typeName, !allowed.empty(), &knob.kind, &knob.dimension)) {
            description->unsupported.push_back(knob.name);
            continue;
        }
        knob.typeName = typeName.GetAsToken().GetString();
        for (const PXR_NS::TfToken& token : allowed) {
            knob.allowedTokens.push_back(token.GetString());
        }
        knob.documentation = property.GetDocumentation();
        knob.uniform = (property.GetVariability() == PXR_NS::SdfVariabilityUniform);
        PXR_NS::VtValue fallback;
        if (attribute.GetFallbackValue(&fallback)) {
            readFallback(fallback, &knob);
        }
        description->knobs.push_back(knob);
    }

    return true;
} // describeSchema

std::string
getAttributeName(const AttributeKnob& knob,
                 const std::string& instanceName)
{
    if (knob.name.find(kInstancePlaceholder) == std::string::npos) {
        return knob.name;
    }

    return PXR_NS::UsdSchemaRegistry::MakeMultipleApplyNameInstance(knob.name, instanceName).GetString();
}

std::string
getKnobScriptName(const AttributeKnob& knob)
{
    std::string name = knob.name;
    const std::string placeholder(kInstancePlaceholder);
    const std::size_t found = name.find(placeholder);

    if (found != std::string::npos) {
        std::size_t begin = found;
        std::size_t end = found + placeholder.size();
        if ((end < name.size()) && (name[end] == ':')) {
            ++end;
        } else if ((begin > 0) && (name[begin - 1] == ':')) {
            --begin;
        }
        name.erase(begin, end - begin);
    }
    std::replace(name.begin(), name.end(), ':', '_');

    return NATRON_PYTHON_NAMESPACE::makeNameScriptFriendly(name);
}

std::string
getKnobLabel(const std::string& scriptName)
{
    std::string label;
    bool wordStart = true;

    for (std::size_t i = 0; i < scriptName.size(); ++i) {
        const char c = scriptName[i];
        if ((c == '_') || (c == ':')) {
            wordStart = true;
            continue;
        }
        const bool upper = std::isupper((unsigned char)c);
        const bool previousLower = (i > 0) && std::islower((unsigned char)scriptName[i - 1]);
        if (upper && previousLower) {
            wordStart = true;
        }
        if (wordStart && !label.empty()) {
            label.push_back(' ');
        }
        label.push_back(wordStart ? (char)std::toupper((unsigned char)c) : c);
        wordStart = false;
    }

    return label;
}

KnobIPtr
createKnob(KnobHolder* holder,
           const AttributeKnob& knob,
           const std::string& scriptName)
{
    const std::string label = getKnobLabel(scriptName);
    KnobIPtr result;

    switch (knob.kind) {
    case eKnobKindBool: {
        KnobBoolPtr valueKnob = AppManager::createKnob<KnobBool>(holder, label);
        if (!knob.defaultNumbers.empty()) {
            valueKnob->setDefaultValue(knob.defaultNumbers[0] != 0.);
        }
        result = valueKnob;
        break;
    }
    case eKnobKindInt: {
        KnobIntPtr valueKnob = AppManager::createKnob<KnobInt>(holder, label);
        if (!knob.defaultNumbers.empty()) {
            valueKnob->setDefaultValue((int)knob.defaultNumbers[0]);
        }
        if (knob.typeName == "uint") {
            valueKnob->setMinimum(0);
        }
        result = valueKnob;
        break;
    }
    case eKnobKindDouble: {
        KnobDoublePtr valueKnob = AppManager::createKnob<KnobDouble>(holder, label, knob.dimension);
        setNumericDefaults(valueKnob, knob);
        result = valueKnob;
        break;
    }
    case eKnobKindColor: {
        KnobColorPtr valueKnob = AppManager::createKnob<KnobColor>(holder, label, knob.dimension);
        setNumericDefaults(valueKnob, knob);
        result = valueKnob;
        break;
    }
    case eKnobKindChoice: {
        KnobChoicePtr valueKnob = AppManager::createKnob<KnobChoice>(holder, label);
        std::vector<ChoiceOption> options;
        int defaultIndex = 0;
        for (std::size_t i = 0; i < knob.allowedTokens.size(); ++i) {
            options.push_back(ChoiceOption(knob.allowedTokens[i], knob.allowedTokens[i], std::string()));
            if (knob.allowedTokens[i] == knob.defaultText) {
                defaultIndex = (int)i;
            }
        }
        valueKnob->populateChoices(options);
        valueKnob->setDefaultValue(defaultIndex);
        valueKnob->setAnimationEnabled(false);
        result = valueKnob;
        break;
    }
    case eKnobKindString: {
        KnobStringPtr valueKnob = AppManager::createKnob<KnobString>(holder, label);
        valueKnob->setDefaultValue(knob.defaultText);
        valueKnob->setAnimationEnabled(false);
        result = valueKnob;
        break;
    }
    case eKnobKindFile: {
        KnobFilePtr valueKnob = AppManager::createKnob<KnobFile>(holder, label);
        valueKnob->setDefaultValue(knob.defaultText);
        valueKnob->setAnimationEnabled(false);
        result = valueKnob;
        break;
    }
    }
    if (!result) {
        return result;
    }
    result->setName(scriptName);
    if (knob.uniform) {
        result->setAnimationEnabled(false);
    }
    QString toolTip = QString::fromStdString(knob.documentation).trimmed();
    if (!toolTip.isEmpty()) {
        toolTip += QString::fromUtf8("\n\n");
    }
    toolTip += EffectInstance::tr("USD attribute: %1 (%2%3).")
                   .arg(QString::fromStdString(knob.name))
                   .arg(knob.uniform ? QString::fromUtf8("uniform ") : QString())
                   .arg(QString::fromStdString(knob.typeName));
    result->setHintToolTip(toolTip.toStdString());

    return result;
} // createKnob

PXR_NS::SdfValueTypeName
getValueTypeName(const AttributeKnob& knob)
{
    return PXR_NS::SdfSchema::GetInstance().FindType(knob.typeName);
}

bool
describeValueType(const std::string& typeName,
                  AttributeKnob* knob)
{
    const PXR_NS::SdfValueTypeName type = PXR_NS::SdfSchema::GetInstance().FindType(typeName);
    KnobKindEnum kind;
    int dimension;

    if (!mapType(type, false, &kind, &dimension)) {
        return false;
    }
    knob->typeName = type.GetAsToken().GetString();
    knob->kind = kind;
    knob->dimension = dimension;

    return true;
}

PXR_NS::VtValue
getValue(const AttributeKnob& knob,
         const KnobIPtr& valueKnob,
         double frame)
{
    const PXR_NS::SdfValueTypeName typeName = getValueTypeName(knob);

    if (!valueKnob || !typeName) {
        return PXR_NS::VtValue();
    }
    const PXR_NS::TfType type = typeName.GetType();
    switch (knob.kind) {
    case eKnobKindBool: {
        KnobBool* isBool = dynamic_cast<KnobBool*>(valueKnob.get());

        return isBool ? PXR_NS::VtValue((bool)isBool->getValueAtTime(frame)) : PXR_NS::VtValue();
    }
    case eKnobKindInt: {
        KnobInt* isInt = dynamic_cast<KnobInt*>(valueKnob.get());
        if (!isInt) {
            return PXR_NS::VtValue();
        }
        const int value = isInt->getValueAtTime(frame);
        if (type == PXR_NS::TfType::Find<unsigned int>()) {
            return PXR_NS::VtValue((unsigned int)std::max(0, value));
        }

        return PXR_NS::VtValue(value);
    }
    case eKnobKindDouble:
    case eKnobKindColor: {
        // KnobColor is a Knob<double> too.
        Knob<double>* isDouble = dynamic_cast<Knob<double>*>(valueKnob.get());
        if (!isDouble) {
            return PXR_NS::VtValue();
        }
        std::vector<double> numbers;
        for (int i = 0; i < isDouble->getDimension(); ++i) {
            numbers.push_back(isDouble->getValueAtTime(frame, i));
        }

        return makeNumeric(type, numbers);
    }
    case eKnobKindChoice: {
        KnobChoice* isChoice = dynamic_cast<KnobChoice*>(valueKnob.get());

        return isChoice ? PXR_NS::VtValue(PXR_NS::TfToken(isChoice->getActiveEntry().id)) : PXR_NS::VtValue();
    }
    case eKnobKindString: {
        KnobString* isString = dynamic_cast<KnobString*>(valueKnob.get());
        if (!isString) {
            return PXR_NS::VtValue();
        }
        const std::string text = isString->getValue();
        if (type == PXR_NS::TfType::Find<PXR_NS::TfToken>()) {
            return PXR_NS::VtValue(PXR_NS::TfToken(text));
        }

        return PXR_NS::VtValue(text);
    }
    case eKnobKindFile: {
        KnobFilePtr isFile = std::dynamic_pointer_cast<KnobFile>(valueKnob);

        return isFile ? PXR_NS::VtValue(PXR_NS::SdfAssetPath(SceneNodeAuthoring::getAnchoredFilePath(isFile))) : PXR_NS::VtValue();
    }
    }

    return PXR_NS::VtValue();
} // getValue
} // namespace SchemaKnobMapper

NATRON_NAMESPACE_EXIT
