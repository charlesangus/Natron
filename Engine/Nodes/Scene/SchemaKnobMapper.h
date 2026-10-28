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

#ifndef Engine_Nodes_Scene_SchemaKnobMapper_h
#define Engine_Nodes_Scene_SchemaKnobMapper_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief What UsdSchemaRegistry says a schema's attributes are, as knobs a scene node can show and
 * author them from. Value types map as: bool to KnobBool; int and uint to KnobInt; float, double
 * and half to KnobDouble, and their 2, 3 and 4 vectors (of any role but colour) to KnobDouble of
 * that dimension; color3f/color4f (and the double colours) to KnobColor; a token with allowed
 * tokens to KnobChoice, any other token and string to KnobString; asset to KnobFile. Arrays,
 * matrices, quaternions and the other types, and relationships, have no knob: arrays are geometry
 * data, not settings.
 **/
namespace SchemaKnobMapper {
enum KnobKindEnum {
    eKnobKindBool = 0,
    eKnobKindInt,
    eKnobKindDouble,
    eKnobKindColor,
    eKnobKindChoice,
    eKnobKindString,
    eKnobKindFile
};

enum SchemaKindEnum {
    eSchemaKindConcreteTyped = 0,
    eSchemaKindAbstractTyped,
    eSchemaKindSingleApplyAPI,
    eSchemaKindMultipleApplyAPI
};

struct SchemaEntry {
    // The name scene description uses: the prim type name (Camera) or the API schema name
    // (CollectionAPI).
    std::string identifier;
    // The plug-in the schema comes from (usdGeom, usdLux, ...), to group a menu by.
    std::string group;
    SchemaKindEnum kind = eSchemaKindConcreteTyped;
};

struct AttributeKnob {
    // The attribute's name in the schema. A multiple-apply schema's names hold the
    // __INSTANCE_NAME__ placeholder (collection:__INSTANCE_NAME__:includeRoot): see
    // getAttributeName().
    std::string name;
    // The Sdf value type name, such as float2 or color3f.
    std::string typeName;
    KnobKindEnum kind = eKnobKindDouble;
    // 1 to 4.
    int dimension = 1;
    // The schema's fallback: one number per dimension for numeric and bool knobs, the text for
    // string, token and asset ones. Zero or empty when the schema has none.
    std::vector<double> defaultNumbers;
    std::string defaultText;
    // For eKnobKindChoice.
    std::vector<std::string> allowedTokens;
    std::string documentation;
    // A uniform attribute takes no time samples.
    bool uniform = false;
};

struct SchemaDescription {
    SchemaEntry schema;
    std::vector<AttributeKnob> knobs;
    // The attributes and relationships with no knob, by name.
    std::vector<std::string> unsupported;
};

/**
 * @brief The typed schemas, concrete and abstract, and the applied API schemas the registry knows,
 * ordered by group, then identifier.
 **/
const std::vector<SchemaEntry>& listSchemas();

// The entry of listSchemas() named identifier, or false.
bool findSchema(const std::string& identifier,
                SchemaEntry* entry);

/**
 * @brief The knobs for identifier's attributes, in the registry's property order. Returns false
 * with *errorMessage set when the registry has no such schema.
 **/
bool describeSchema(const std::string& identifier,
                    SchemaDescription* description,
                    std::string* errorMessage);

/**
 * @brief The attribute knob authors: knob.name with a multiple-apply schema's placeholder replaced
 * by instanceName, or knob.name itself.
 **/
std::string getAttributeName(const AttributeKnob& knob,
                             const std::string& instanceName);

/**
 * @brief The script name of the knob for an attribute: its name without the __INSTANCE_NAME__
 * placeholder, made a Python identifier (collection_includeRoot, inputs_intensity).
 **/
std::string getKnobScriptName(const AttributeKnob& knob);

// The knob's label: its script name split into words (inputs_intensity: "Inputs Intensity").
std::string getKnobLabel(const std::string& scriptName);

/**
 * @brief A knob for knob, named scriptName, with its default, tooltip and, for a uniform
 * attribute, animation turned off. Not added to a page.
 **/
KnobIPtr createKnob(KnobHolder* holder,
                    const AttributeKnob& knob,
                    const std::string& scriptName);

/**
 * @brief valueKnob's value at frame as a value of knob's attribute type. Empty when valueKnob is
 * not of the kind knob maps to.
 **/
PXR_NS::VtValue getValue(const AttributeKnob& knob,
                         const KnobIPtr& valueKnob,
                         double frame);

// The Sdf type of knob's attribute.
PXR_NS::SdfValueTypeName getValueTypeName(const AttributeKnob& knob);

/**
 * @brief Sets knob's typeName, kind and dimension for an attribute of the Sdf value type typeName
 * (float3, color3f, token...), a token always mapping to a string. Returns false, leaving knob as
 * it is, when typeName names no type or a type no knob maps.
 **/
bool describeValueType(const std::string& typeName,
                       AttributeKnob* knob);
} // namespace SchemaKnobMapper

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_SchemaKnobMapper_h
