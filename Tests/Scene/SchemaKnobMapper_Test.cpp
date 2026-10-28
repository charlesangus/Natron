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

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include "Global/Macros.h"

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/Nodes/Scene/SchemaKnobMapper.h"

NATRON_NAMESPACE_USING

using namespace SchemaKnobMapper;

namespace {
const AttributeKnob*
findKnob(const SchemaDescription& description,
         const std::string& name)
{
    for (const AttributeKnob& knob : description.knobs) {
        if (knob.name == name) {
            return &knob;
        }
    }

    return nullptr;
}

bool
isUnsupported(const SchemaDescription& description,
              const std::string& name)
{
    return std::find(description.unsupported.begin(), description.unsupported.end(), name) != description.unsupported.end();
}
} // anonymous namespace

class SchemaKnobMapperTest
    : public BaseTest {
};

TEST_F(SchemaKnobMapperTest, ListsTypedAndAppliedSchemas)
{
    SchemaEntry camera;
    ASSERT_TRUE(findSchema("Camera", &camera));
    EXPECT_EQ(camera.kind, eSchemaKindConcreteTyped);
    EXPECT_EQ(camera.group, "usdGeom");

    SchemaEntry imageable;
    ASSERT_TRUE(findSchema("Imageable", &imageable));
    EXPECT_EQ(imageable.kind, eSchemaKindAbstractTyped);

    SchemaEntry light;
    ASSERT_TRUE(findSchema("LightAPI", &light));
    EXPECT_EQ(light.kind, eSchemaKindSingleApplyAPI);

    SchemaEntry collection;
    ASSERT_TRUE(findSchema("CollectionAPI", &collection));
    EXPECT_EQ(collection.kind, eSchemaKindMultipleApplyAPI);

    EXPECT_FALSE(findSchema("NoSuchSchema", &camera));
}

TEST_F(SchemaKnobMapperTest, CameraAttributesMapToKnobs)
{
    SchemaDescription description;
    std::string error;
    ASSERT_TRUE(describeSchema("Camera", &description, &error)) << error;

    const AttributeKnob* focalLength = findKnob(description, "focalLength");
    ASSERT_TRUE(focalLength != nullptr);
    EXPECT_EQ(focalLength->kind, eKnobKindDouble);
    EXPECT_EQ(focalLength->dimension, 1);
    EXPECT_EQ(focalLength->typeName, "float");
    ASSERT_EQ(focalLength->defaultNumbers.size(), 1u);
    EXPECT_DOUBLE_EQ(focalLength->defaultNumbers[0], 50.);
    EXPECT_FALSE(focalLength->uniform);
    EXPECT_FALSE(focalLength->documentation.empty());

    const AttributeKnob* clippingRange = findKnob(description, "clippingRange");
    ASSERT_TRUE(clippingRange != nullptr);
    EXPECT_EQ(clippingRange->kind, eKnobKindDouble);
    EXPECT_EQ(clippingRange->dimension, 2);
    EXPECT_EQ(clippingRange->defaultNumbers.size(), 2u);

    const AttributeKnob* projection = findKnob(description, "projection");
    ASSERT_TRUE(projection != nullptr);
    EXPECT_EQ(projection->kind, eKnobKindChoice);
    EXPECT_EQ(projection->defaultText, "perspective");
    EXPECT_NE(std::find(projection->allowedTokens.begin(), projection->allowedTokens.end(), "orthographic"), projection->allowedTokens.end());

    const AttributeKnob* purpose = findKnob(description, "purpose");
    ASSERT_TRUE(purpose != nullptr);
    EXPECT_TRUE(purpose->uniform);

    // Arrays and relationships have no knob.
    EXPECT_TRUE(isUnsupported(description, "xformOpOrder"));
    EXPECT_TRUE(isUnsupported(description, "proxyPrim"));
    EXPECT_TRUE(findKnob(description, "xformOpOrder") == nullptr);
}

TEST_F(SchemaKnobMapperTest, MeshGeometryArraysAreUnsupported)
{
    SchemaDescription description;
    std::string error;
    ASSERT_TRUE(describeSchema("Mesh", &description, &error)) << error;

    EXPECT_TRUE(isUnsupported(description, "points"));
    EXPECT_TRUE(isUnsupported(description, "faceVertexIndices"));
    const AttributeKnob* doubleSided = findKnob(description, "doubleSided");
    ASSERT_TRUE(doubleSided != nullptr);
    EXPECT_EQ(doubleSided->kind, eKnobKindBool);
}

TEST_F(SchemaKnobMapperTest, LightColourMapsToAColourKnob)
{
    SchemaDescription description;
    std::string error;
    ASSERT_TRUE(describeSchema("SphereLight", &description, &error)) << error;

    const AttributeKnob* color = findKnob(description, "inputs:color");
    ASSERT_TRUE(color != nullptr);
    EXPECT_EQ(color->kind, eKnobKindColor);
    EXPECT_EQ(color->dimension, 3);
    EXPECT_EQ(getKnobScriptName(*color), "inputs_color");
    EXPECT_EQ(getKnobLabel(getKnobScriptName(*color)), "Inputs Color");
}

TEST_F(SchemaKnobMapperTest, MultipleApplyNamesTakeTheInstance)
{
    SchemaDescription description;
    std::string error;
    ASSERT_TRUE(describeSchema("CollectionAPI", &description, &error)) << error;

    const AttributeKnob* includeRoot = findKnob(description, "collection:__INSTANCE_NAME__:includeRoot");
    ASSERT_TRUE(includeRoot != nullptr);
    EXPECT_EQ(includeRoot->kind, eKnobKindBool);
    EXPECT_TRUE(includeRoot->uniform);
    EXPECT_EQ(getAttributeName(*includeRoot, "lights"), "collection:lights:includeRoot");
    EXPECT_EQ(getKnobScriptName(*includeRoot), "collection_includeRoot");
    EXPECT_EQ(getKnobLabel("collection_includeRoot"), "Collection Include Root");

    const AttributeKnob* expansionRule = findKnob(description, "collection:__INSTANCE_NAME__:expansionRule");
    ASSERT_TRUE(expansionRule != nullptr);
    EXPECT_EQ(expansionRule->kind, eKnobKindChoice);
    EXPECT_EQ(expansionRule->allowedTokens.size(), 3u);
}

TEST_F(SchemaKnobMapperTest, UnknownSchemaIsAnError)
{
    SchemaDescription description;
    std::string error;
    EXPECT_FALSE(describeSchema("NoSuchSchema", &description, &error));
    EXPECT_FALSE(error.empty());
}
