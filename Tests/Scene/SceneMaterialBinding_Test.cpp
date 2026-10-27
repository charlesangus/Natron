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

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"

NATRON_NAMESPACE_USING

namespace {
// /Asset/geo/{a,b}, and the shared materials M, N and D.
const char* const kAsset = "def Xform \"Asset\"\n{\n"
                           "    def Xform \"geo\"\n    {\n"
                           "        def Mesh \"a\"\n        {\n        }\n"
                           "        def Mesh \"b\"\n        {\n        }\n"
                           "    }\n}\n"
                           "def Scope \"materials\"\n{\n"
                           "    def Material \"M\"\n    {\n    }\n"
                           "    def Material \"N\"\n    {\n    }\n"
                           "    def Material \"D\"\n    {\n    }\n}\n";

// kAsset with /Asset/geo/a bound directly to /materials/D.
const char* const kAssetWithDirectBinding = "over \"Asset\"\n{\n    over \"geo\"\n    {\n"
                                            "        over \"a\" (\n            prepend apiSchemas = [\"MaterialBindingAPI\"]\n        )\n        {\n"
                                            "            rel material:binding = </materials/D>\n        }\n    }\n}\n";

PXR_NS::SdfLayerRefPtr
makeLayer(const std::string& body)
{
    PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::CreateAnonymous("SceneMaterialBindingTest");

    EXPECT_TRUE(layer->ImportFromString("#usda 1.0\n" + body));

    return layer;
}

// A stage sublayering layers, strongest first.
PXR_NS::UsdStageRefPtr
compose(const std::vector<PXR_NS::SdfLayerRefPtr>& layers)
{
    PXR_NS::SdfLayerRefPtr root = PXR_NS::SdfLayer::CreateAnonymous("SceneMaterialBindingTest-root");
    std::vector<std::string> paths;

    for (const PXR_NS::SdfLayerRefPtr& layer : layers) {
        paths.push_back(layer->GetIdentifier());
    }
    root->SetSubLayerPaths(paths);

    return PXR_NS::UsdStage::Open(root, PXR_NS::UsdStage::LoadAll);
}

// Authors binding on root into a layer of its own, over the stage weaker composes, the way a node
// layer is authored: reading the root's existing bindings from weaker.
PXR_NS::SdfLayerRefPtr
authorBinding(const std::vector<PXR_NS::SdfLayerRefPtr>& weaker,
              const std::string& root,
              SceneNodeAuthoring::SceneCollectionBinding assignment)
{
    const PXR_NS::UsdStageRefPtr upstream = compose(weaker);
    assignment.existingBindings = SceneNodeAuthoring::getCollectionBindingNames(upstream->GetPrimAtPath(PXR_NS::SdfPath(root)));

    PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::CreateAnonymous("SceneMaterialBindingTest-binding");
    const PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(layer, PXR_NS::SdfLayerHandle(), PXR_NS::UsdStage::LoadNone);
    std::string error;
    EXPECT_TRUE(SceneNodeAuthoring::authorCollectionBinding(stage->OverridePrim(PXR_NS::SdfPath(root)), assignment, &error)) << error;

    return layer;
}

SceneNodeAuthoring::SceneCollectionBinding
binding(const std::vector<std::string>& includes,
        const std::string& materialPath,
        const std::string& name,
        const PXR_NS::TfToken& strength = PXR_NS::UsdShadeTokens->strongerThanDescendants,
        const PXR_NS::TfToken& purpose = PXR_NS::UsdShadeTokens->allPurpose)
{
    SceneNodeAuthoring::SceneCollectionBinding result;

    for (const std::string& path : includes) {
        result.includes.push_back(PXR_NS::SdfPath(path));
    }
    result.materialPath = materialPath;
    result.bindingName = name;
    result.strength = strength;
    result.purpose = purpose;

    return result;
}

std::string
boundMaterial(const PXR_NS::UsdStageRefPtr& stage,
              const std::string& path,
              const PXR_NS::TfToken& purpose = PXR_NS::UsdShadeTokens->allPurpose)
{
    const PXR_NS::UsdShadeMaterial material = PXR_NS::UsdShadeMaterialBindingAPI(stage->GetPrimAtPath(PXR_NS::SdfPath(path))).ComputeBoundMaterial(purpose);

    return material ? material.GetPath().GetString() : std::string();
}
} // anonymous namespace

TEST(SceneMaterialBindingTest, TheBindingResolvesPerPurpose)
{
    const PXR_NS::SdfLayerRefPtr asset = makeLayer(kAsset);
    const PXR_NS::SdfLayerRefPtr preview = authorBinding({ asset }, "/Asset", binding({ "/Asset/geo/a" }, "/materials/M", "lookPreview", PXR_NS::UsdShadeTokens->strongerThanDescendants, PXR_NS::UsdShadeTokens->preview));
    const PXR_NS::SdfLayerRefPtr full = authorBinding({ preview, asset }, "/Asset", binding({ "/Asset/geo/a" }, "/materials/N", "lookFull", PXR_NS::UsdShadeTokens->strongerThanDescendants, PXR_NS::UsdShadeTokens->full));
    const PXR_NS::SdfLayerRefPtr all = authorBinding({ full, preview, asset }, "/Asset", binding({ "/Asset/geo/b" }, "/materials/D", "lookAll"));
    const PXR_NS::UsdStageRefPtr stage = compose({ all, full, preview, asset });
    ASSERT_TRUE(bool(stage));

    EXPECT_EQ("/materials/M", boundMaterial(stage, "/Asset/geo/a", PXR_NS::UsdShadeTokens->preview));
    EXPECT_EQ("/materials/N", boundMaterial(stage, "/Asset/geo/a", PXR_NS::UsdShadeTokens->full));
    EXPECT_EQ("", boundMaterial(stage, "/Asset/geo/a"));
    // An all-purpose binding serves every purpose.
    EXPECT_EQ("/materials/D", boundMaterial(stage, "/Asset/geo/b"));
    EXPECT_EQ("/materials/D", boundMaterial(stage, "/Asset/geo/b", PXR_NS::UsdShadeTokens->preview));
    EXPECT_EQ("/materials/D", boundMaterial(stage, "/Asset/geo/b", PXR_NS::UsdShadeTokens->full));

    // The shape of what was authored: the collection, its expansion and the binding's strength.
    const PXR_NS::UsdPrim root = stage->GetPrimAtPath(PXR_NS::SdfPath("/Asset"));
    const PXR_NS::UsdCollectionAPI collection = PXR_NS::UsdCollectionAPI(root, PXR_NS::TfToken("lookPreview"));
    PXR_NS::TfToken expansion;
    EXPECT_TRUE(collection.GetExpansionRuleAttr().Get(&expansion));
    EXPECT_EQ(PXR_NS::UsdTokens->expandPrims, expansion);
    const PXR_NS::UsdRelationship rel = root.GetRelationship(PXR_NS::TfToken("material:binding:collection:preview:lookPreview"));
    ASSERT_TRUE(rel.IsValid());
    EXPECT_EQ(PXR_NS::UsdShadeTokens->strongerThanDescendants, PXR_NS::UsdShadeMaterialBindingAPI::GetMaterialBindingStrength(rel));
    EXPECT_TRUE(root.HasAPI<PXR_NS::UsdShadeMaterialBindingAPI>());
    // Every binding on the root, the last one authored first.
    EXPECT_EQ(std::vector<PXR_NS::TfToken>({ PXR_NS::TfToken("material:binding:collection:lookAll"),
                                             PXR_NS::TfToken("material:binding:collection:full:lookFull"),
                                             PXR_NS::TfToken("material:binding:collection:preview:lookPreview") }),
              SceneNodeAuthoring::getCollectionBindingNames(root));
}

TEST(SceneMaterialBindingTest, ABindingWrittenLaterWinsWhereTheyOverlap)
{
    // Named so property name order alone would make the first one stronger.
    const PXR_NS::SdfLayerRefPtr asset = makeLayer(kAsset);
    const PXR_NS::SdfLayerRefPtr first = authorBinding({ asset }, "/Asset", binding({ "/Asset/geo" }, "/materials/M", "A"));
    const PXR_NS::SdfLayerRefPtr second = authorBinding({ first, asset }, "/Asset", binding({ "/Asset/geo/a" }, "/materials/N", "B"));
    PXR_NS::UsdStageRefPtr stage = compose({ second, first, asset });
    ASSERT_TRUE(bool(stage));

    EXPECT_EQ("/materials/N", boundMaterial(stage, "/Asset/geo/a"));
    EXPECT_EQ("/materials/M", boundMaterial(stage, "/Asset/geo/b"));

    // Written the other way round, the other one wins.
    const PXR_NS::SdfLayerRefPtr firstB = authorBinding({ asset }, "/Asset", binding({ "/Asset/geo/a" }, "/materials/N", "B"));
    const PXR_NS::SdfLayerRefPtr secondA = authorBinding({ firstB, asset }, "/Asset", binding({ "/Asset/geo" }, "/materials/M", "A"));
    stage = compose({ secondA, firstB, asset });
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ("/materials/M", boundMaterial(stage, "/Asset/geo/a"));
    EXPECT_EQ("/materials/M", boundMaterial(stage, "/Asset/geo/b"));
}

TEST(SceneMaterialBindingTest, StrongerBeatsADescendantsDirectBindingAndWeakerDoesNot)
{
    const PXR_NS::SdfLayerRefPtr asset = makeLayer(kAsset);
    const PXR_NS::SdfLayerRefPtr direct = makeLayer(kAssetWithDirectBinding);
    PXR_NS::UsdStageRefPtr stage = compose({ direct, asset });
    ASSERT_TRUE(bool(stage));
    ASSERT_EQ("/materials/D", boundMaterial(stage, "/Asset/geo/a"));

    const PXR_NS::SdfLayerRefPtr stronger = authorBinding({ direct, asset }, "/Asset", binding({ "/Asset/geo" }, "/materials/M", "Assign", PXR_NS::UsdShadeTokens->strongerThanDescendants));
    stage = compose({ stronger, direct, asset });
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ("/materials/M", boundMaterial(stage, "/Asset/geo/a"));
    EXPECT_EQ("/materials/M", boundMaterial(stage, "/Asset/geo/b"));

    const PXR_NS::SdfLayerRefPtr weaker = authorBinding({ direct, asset }, "/Asset", binding({ "/Asset/geo" }, "/materials/M", "Assign", PXR_NS::UsdShadeTokens->weakerThanDescendants));
    stage = compose({ weaker, direct, asset });
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ("/materials/D", boundMaterial(stage, "/Asset/geo/a"));
    EXPECT_EQ("/materials/M", boundMaterial(stage, "/Asset/geo/b"));
}

TEST(SceneMaterialBindingTest, TheBindingResolvesAfterAGraftRepairsItsMaterialTarget)
{
    const PXR_NS::SdfLayerRefPtr asset = makeLayer(kAsset);
    const PXR_NS::SdfLayerRefPtr assign = authorBinding({ asset }, "/Asset", binding({ "/Asset/geo/a" }, "/materials/M", "Assign"));
    // What a re-parent authors: the root referenced under /T1, and the binding's target list
    // written explicitly, as the reference drops the material outside the root.
    const PXR_NS::SdfLayerRefPtr graft = makeLayer("def Xform \"T1\"\n{\n"
                                                   "    def \"Asset\" (\n        prepend references = </Asset>\n    )\n    {\n"
                                                   "        rel material:binding:collection:Assign = [</T1/Asset.collection:Assign>, </materials/M>]\n"
                                                   "    }\n}\n");
    const PXR_NS::UsdStageRefPtr stage = compose({ graft, assign, asset });
    ASSERT_TRUE(bool(stage));

    EXPECT_EQ("/materials/M", boundMaterial(stage, "/T1/Asset/geo/a"));
    EXPECT_EQ("", boundMaterial(stage, "/T1/Asset/geo/b"));
}

TEST(SceneMaterialBindingTest, ABindingNameThatIsNotAnIdentifierFails)
{
    const PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::CreateAnonymous("SceneMaterialBindingTest-bad");
    const PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(layer, PXR_NS::SdfLayerHandle(), PXR_NS::UsdStage::LoadNone);
    std::string error;

    EXPECT_FALSE(SceneNodeAuthoring::authorCollectionBinding(stage->OverridePrim(PXR_NS::SdfPath("/Asset")), binding({ "/Asset" }, "/materials/M", "a:b"), &error));
    EXPECT_FALSE(error.empty());
}
