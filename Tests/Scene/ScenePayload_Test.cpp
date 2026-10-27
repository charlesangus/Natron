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

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_USING

namespace {
PXR_NS::SdfLayerRefPtr
makeLayerDefining(const std::string& primPath)
{
    PXR_NS::SdfLayerRefPtr layer = SceneOps::createNodeLayer("ScenePayloadTest");
    PXR_NS::SdfPrimSpecHandle prim = PXR_NS::SdfCreatePrimInLayer(layer, PXR_NS::SdfPath(primPath));

    if (prim) {
        prim->SetSpecifier(PXR_NS::SdfSpecifierDef);
        prim->SetTypeName("Xform");
    }

    return layer;
}

// The #include lines of a source file, so a test can tell which headers it pulls in.
std::vector<std::string>
readIncludeLines(const std::string& path)
{
    std::vector<std::string> includes;
    std::ifstream file(path.c_str());
    std::string line;

    while (std::getline(file, line)) {
        if (line.find("#include") != std::string::npos) {
            includes.push_back(line);
        }
    }

    return includes;
}
} // anonymous namespace

TEST(ScenePayloadTest, EmptyPayloadHasNoLayersAndFallbackMetadata)
{
    ScenePayload payload;

    EXPECT_TRUE(payload.isEmpty());
    EXPECT_EQ((std::size_t)0, payload.getNumLayers());
    EXPECT_EQ((U64)0, payload.getStackHash());
    EXPECT_EQ(SceneMetadata(), payload.getMetadata());
    EXPECT_TRUE(SceneOps::getLayers(payload).empty());
}

TEST(ScenePayloadTest, AppendingALayerChangesTheStackHash)
{
    const ScenePayload empty;
    const ScenePayload one = SceneOps::appendLayer(empty, makeLayerDefining("/A"), 11);
    const ScenePayload two = SceneOps::appendLayer(one, makeLayerDefining("/B"), 22);

    EXPECT_EQ((std::size_t)1, one.getNumLayers());
    EXPECT_EQ((std::size_t)2, two.getNumLayers());
    EXPECT_NE((U64)0, one.getStackHash());
    EXPECT_NE(empty.getStackHash(), one.getStackHash());
    EXPECT_NE(one.getStackHash(), two.getStackHash());

    // The base is a value: appending to it left it as it was.
    EXPECT_EQ((std::size_t)1, one.getNumLayers());
}

TEST(ScenePayloadTest, IdenticalStacksHashIdentically)
{
    const ScenePayload first = SceneOps::appendLayer(SceneOps::appendLayer(ScenePayload(), makeLayerDefining("/A"), 11), makeLayerDefining("/B"), 22);
    const ScenePayload second = SceneOps::appendLayer(SceneOps::appendLayer(ScenePayload(), makeLayerDefining("/A"), 11), makeLayerDefining("/B"), 22);

    EXPECT_EQ(first.getStackHash(), second.getStackHash());
    // Equal content, but other layer objects.
    EXPECT_FALSE(first.isSameStack(second));
    EXPECT_TRUE(first.isSameStack(first));

    const ScenePayload swapped = SceneOps::appendLayer(SceneOps::appendLayer(ScenePayload(), makeLayerDefining("/B"), 22), makeLayerDefining("/A"), 11);
    EXPECT_NE(first.getStackHash(), swapped.getStackHash());
}

TEST(ScenePayloadTest, LayersAreListedWeakestFirst)
{
    PXR_NS::SdfLayerRefPtr weak = makeLayerDefining("/A");
    PXR_NS::SdfLayerRefPtr strong = makeLayerDefining("/B");
    const ScenePayload payload = SceneOps::appendLayer(SceneOps::appendLayer(ScenePayload(), weak, 1), strong, 2);

    const std::vector<SceneLayerEntry> layers = SceneOps::getLayers(payload);
    ASSERT_EQ((std::size_t)2, layers.size());
    EXPECT_EQ(weak, layers[0].layer);
    EXPECT_EQ((U64)1, layers[0].hash);
    EXPECT_EQ(strong, layers[1].layer);
    EXPECT_EQ((U64)2, layers[1].hash);
}

// For each field the strongest layer authoring it wins, and a layer authoring nothing leaves
// the weaker layers' values alone.
TEST(ScenePayloadTest, MetadataResolvesStrongestAuthoredFirst)
{
    const PXR_NS::SdfPath& root = PXR_NS::SdfPath::AbsoluteRootPath();

    PXR_NS::SdfLayerRefPtr weak = makeLayerDefining("/A");
    weak->SetField(root, PXR_NS::UsdGeomTokens->upAxis, PXR_NS::UsdGeomTokens->z);
    weak->SetField(root, PXR_NS::UsdGeomTokens->metersPerUnit, 1.);
    weak->SetDefaultPrim(PXR_NS::TfToken("A"));
    weak->SetTimeCodesPerSecond(30.);

    PXR_NS::SdfLayerRefPtr strong = makeLayerDefining("/B");
    strong->SetDefaultPrim(PXR_NS::TfToken("B"));

    PXR_NS::SdfLayerRefPtr silent = makeLayerDefining("/C");

    const ScenePayload payload = SceneOps::appendLayer(SceneOps::appendLayer(SceneOps::appendLayer(ScenePayload(), weak, 1), strong, 2), silent, 3);
    const SceneMetadata& metadata = payload.getMetadata();

    EXPECT_EQ(SceneMetadata::eUpAxisZ, metadata.upAxis);
    EXPECT_EQ(1., metadata.metersPerUnit);
    EXPECT_EQ(std::string("B"), metadata.defaultPrim);
    EXPECT_EQ(30., metadata.timeCodesPerSecond);
}

// Composition sublayers the stack strongest first, so a stronger opinion wins, and carries the
// resolved metadata onto the stage.
TEST(ScenePayloadTest, ComposedStageHoldsEveryLayerWithTheStrongestOpinionWinning)
{
    const PXR_NS::SdfPath primPath("/A");
    const PXR_NS::TfToken attributeName("natron:value");

    PXR_NS::SdfLayerRefPtr weak = makeLayerDefining("/A");
    PXR_NS::SdfAttributeSpecHandle weakAttribute = PXR_NS::SdfAttributeSpec::New(weak->GetPrimAtPath(primPath), attributeName, PXR_NS::SdfValueTypeNames->Int);
    ASSERT_TRUE(bool(weakAttribute));
    weakAttribute->SetDefaultValue(PXR_NS::VtValue(1));
    weak->SetField(PXR_NS::SdfPath::AbsoluteRootPath(), PXR_NS::UsdGeomTokens->upAxis, PXR_NS::UsdGeomTokens->z);

    PXR_NS::SdfLayerRefPtr strong = SceneOps::createNodeLayer("ScenePayloadTest");
    PXR_NS::SdfPrimSpecHandle over = PXR_NS::SdfCreatePrimInLayer(strong, primPath);
    ASSERT_TRUE(bool(over));
    PXR_NS::SdfAttributeSpecHandle strongAttribute = PXR_NS::SdfAttributeSpec::New(over, attributeName, PXR_NS::SdfValueTypeNames->Int);
    ASSERT_TRUE(bool(strongAttribute));
    strongAttribute->SetDefaultValue(PXR_NS::VtValue(2));
    PXR_NS::SdfPrimSpecHandle other = PXR_NS::SdfCreatePrimInLayer(strong, PXR_NS::SdfPath("/B"));
    ASSERT_TRUE(bool(other));
    other->SetSpecifier(PXR_NS::SdfSpecifierDef);

    const ScenePayload payload = SceneOps::appendLayer(SceneOps::appendLayer(ScenePayload(), weak, 1), strong, 2);
    PXR_NS::UsdStageRefPtr stage = SceneOps::compose(payload);
    ASSERT_TRUE(bool(stage));

    PXR_NS::UsdPrim prim = stage->GetPrimAtPath(primPath);
    ASSERT_TRUE(prim.IsValid());
    int value = 0;
    EXPECT_TRUE(prim.GetAttribute(attributeName).Get(&value));
    EXPECT_EQ(2, value);
    EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/B")).IsValid());

    PXR_NS::TfToken upAxis;
    EXPECT_TRUE(stage->GetMetadata(PXR_NS::UsdGeomTokens->upAxis, &upAxis));
    EXPECT_EQ(PXR_NS::UsdGeomTokens->z, upAxis);
}

// USD stays behind Engine/Nodes/Scene/: the transport type and the engine headers that carry
// it include no pxr header.
TEST(ScenePayloadTest, CoreHeadersIncludeNoPxrHeader)
{
    const char* headers[] = {
        "/ScenePayload.h",
        "/EffectInstance.h",
        "/Nodes/NativeEffectBase.h",
        "/Nodes/Scene/SceneLayerMemo.h",
    };

    for (const char* header : headers) {
        const std::string path = std::string(NATRON_TESTS_ENGINE_DIR) + header;
        const std::vector<std::string> includes = readIncludeLines(path);

        ASSERT_FALSE(includes.empty()) << path << " could not be read";
        for (std::size_t i = 0; i < includes.size(); ++i) {
            EXPECT_EQ(std::string::npos, includes[i].find("pxr")) << path << ": " << includes[i];
            EXPECT_EQ(std::string::npos, includes[i].find("UsdIncludes")) << path << ": " << includes[i];
        }
    }
}
