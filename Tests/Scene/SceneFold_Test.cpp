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

#include <chrono>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <pxr/imaging/hd/sceneIndexPrimView.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/usdImaging/usdImaging/sceneIndices.h>

#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_USING

namespace {
PXR_NS::SdfLayerRefPtr
makeLayer(const std::string& body)
{
    PXR_NS::SdfLayerRefPtr layer = SceneOps::createNodeLayer("SceneFoldTest");

    EXPECT_TRUE(layer->ImportFromString("#usda 1.0\n" + body));

    return layer;
}

ScenePayload
stackOf(const PXR_NS::SdfLayerRefPtr& layer)
{
    return SceneOps::appendLayer(ScenePayload(), layer, 1);
}

// usda wants a newline after a non-empty prim's opening brace and between properties.
const char* const kScene = "def Xform \"World\" {\n"
                           "    def Xform \"Heavy\" {\n"
                           "        def Cube \"A\" {\n"
                           "            double size = 1\n"
                           "        }\n"
                           "        def Cube \"B\" {\n"
                           "            double size = 1\n"
                           "            double3 xformOp:translate = (3, 0, 0)\n"
                           "            uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                           "        }\n"
                           "    }\n"
                           "    def Cube \"Light\" {\n"
                           "        double size = 2\n"
                           "    }\n"
                           "}\n";

struct Imaged {
    // Prim types under each path the scene index has, by path.
    std::vector<std::pair<PXR_NS::SdfPath, PXR_NS::TfToken>> prims;

    std::size_t countUnder(const PXR_NS::SdfPath& root,
                           const PXR_NS::TfToken& type) const
    {
        std::size_t n = 0;
        for (const std::pair<PXR_NS::SdfPath, PXR_NS::TfToken>& prim : prims) {
            if (prim.first.HasPrefix(root) && (prim.second == type)) {
                ++n;
            }
        }

        return n;
    }
};

// What Hydra is handed for the stage: the draw mode scene index is in the chain, as in the engine.
Imaged
image(const PXR_NS::UsdStageRefPtr& stage)
{
    PXR_NS::UsdImagingCreateSceneIndicesInfo info;
    info.stage = stage;
    const PXR_NS::UsdImagingSceneIndices indices = PXR_NS::UsdImagingCreateSceneIndices(info);
    Imaged imaged;

    EXPECT_TRUE(bool(indices.finalSceneIndex));
    if (!indices.finalSceneIndex) {
        return imaged;
    }
    for (const PXR_NS::SdfPath& path : PXR_NS::HdSceneIndexPrimView(indices.finalSceneIndex)) {
        imaged.prims.push_back(std::make_pair(path, indices.finalSceneIndex->GetPrim(path).primType));
    }

    return imaged;
}
} // anonymous namespace

TEST(SceneFoldTest, OnlyAFoldableStageFolds)
{
    SceneStage plain;
    ASSERT_TRUE(plain.isValid());
    EXPECT_FALSE(plain.isFoldable());
    EXPECT_FALSE(plain.setFoldedPrims(std::set<PXR_NS::SdfPath> { PXR_NS::SdfPath("/World") }));
    EXPECT_FALSE(bool(plain.getStage()->GetSessionLayer()));

    SceneStage foldable(true);
    ASSERT_TRUE(foldable.isValid());
    EXPECT_TRUE(foldable.isFoldable());
    EXPECT_TRUE(bool(foldable.getStage()->GetSessionLayer()));
}

TEST(SceneFoldTest, AFoldedSubtreeIsImagedAsItsBounds)
{
    PXR_NS::SdfLayerRefPtr layer = makeLayer(kScene);
    const ScenePayload payload = stackOf(layer);
    const PXR_NS::SdfPath heavy("/World/Heavy");
    const PXR_NS::SdfPath light("/World/Light");

    SceneStage stage(true);
    ASSERT_TRUE(stage.isValid());
    stage.sync(payload);

    const Imaged unfolded = image(stage.getStage());
    EXPECT_EQ(2u, unfolded.countUnder(heavy, PXR_NS::HdPrimTypeTokens->cube));
    EXPECT_EQ(0u, unfolded.countUnder(heavy, PXR_NS::HdPrimTypeTokens->basisCurves));

    // The fold is kept across a sync that replaces the stack.
    EXPECT_TRUE(stage.setFoldedPrims(std::set<PXR_NS::SdfPath> { heavy }));
    EXPECT_FALSE(stage.setFoldedPrims(std::set<PXR_NS::SdfPath> { heavy }));
    stage.sync(stackOf(makeLayer(kScene)));

    const PXR_NS::UsdPrim heavyPrim = stage.getStage()->GetPrimAtPath(heavy);
    ASSERT_TRUE(bool(heavyPrim));
    PXR_NS::TfToken drawMode;
    bool apply = false;
    ASSERT_TRUE(heavyPrim.GetAttribute(PXR_NS::UsdGeomTokens->modelDrawMode).Get(&drawMode));
    ASSERT_TRUE(heavyPrim.GetAttribute(PXR_NS::UsdGeomTokens->modelApplyDrawMode).Get(&apply));
    EXPECT_EQ(PXR_NS::UsdGeomTokens->bounds, drawMode);
    EXPECT_TRUE(apply);
    // The over goes in the session layer, not in the scene's layers.
    EXPECT_FALSE(bool(layer->GetPrimAtPath(heavy)) && layer->GetPrimAtPath(heavy)->HasInfo(PXR_NS::UsdTokens->apiSchemas));

    const Imaged folded = image(stage.getStage());
    EXPECT_EQ(0u, folded.countUnder(heavy, PXR_NS::HdPrimTypeTokens->cube));
    EXPECT_GE(folded.countUnder(heavy, PXR_NS::HdPrimTypeTokens->basisCurves), 1u);
    EXPECT_EQ(1u, folded.countUnder(light, PXR_NS::HdPrimTypeTokens->cube));

    // A render stage of the same scene does not see the fold.
    SceneStage render;
    render.sync(payload);
    EXPECT_EQ(2u, image(render.getStage()).countUnder(heavy, PXR_NS::HdPrimTypeTokens->cube));
    EXPECT_EQ(2u, image(SceneOps::compose(payload)).countUnder(heavy, PXR_NS::HdPrimTypeTokens->cube));

    // Unfolding takes the over away, empty ancestors included.
    EXPECT_TRUE(stage.setFoldedPrims(std::set<PXR_NS::SdfPath>()));
    EXPECT_FALSE(bool(stage.getStage()->GetSessionLayer()->GetPrimAtPath(PXR_NS::SdfPath("/World"))));
    EXPECT_EQ(2u, image(stage.getStage()).countUnder(heavy, PXR_NS::HdPrimTypeTokens->cube));
}

TEST(SceneFoldTest, AFoldUnloadsPayloadsBelowIt)
{
    PXR_NS::SdfLayerRefPtr asset = makeLayer("def Xform \"Asset\" {\n    def Cube \"Geo\" {}\n}\n");
    PXR_NS::SdfLayerRefPtr layer = makeLayer("def Xform \"World\" {\n"
                                             "    def Xform \"Outer\" {\n"
                                             "        def Xform \"P\" ( payload = @"
                                             + asset->GetIdentifier() + "@</Asset> ) {}\n"
                                                                        "        def Xform \"Q\" ( payload = @"
                                             + asset->GetIdentifier() + "@</Asset> ) {}\n"
                                                                        "    }\n"
                                                                        "}\n");
    const PXR_NS::SdfPath outer("/World/Outer");
    const PXR_NS::SdfPath p("/World/Outer/P");
    const PXR_NS::SdfPath q("/World/Outer/Q");

    SceneStage stage(true);
    ASSERT_TRUE(stage.isValid());
    // Folded before the first sync: the payload is never loaded.
    EXPECT_TRUE(stage.setFoldedPrims(std::set<PXR_NS::SdfPath> { outer, q }));
    stage.sync(stackOf(layer));
    const PXR_NS::UsdStageRefPtr& usd = stage.getStage();
    ASSERT_TRUE(bool(usd->GetPrimAtPath(p)));
    EXPECT_FALSE(usd->GetPrimAtPath(p).IsLoaded());
    EXPECT_FALSE(bool(usd->GetPrimAtPath(p.AppendChild(PXR_NS::TfToken("Geo")))));

    // Unfolding the outer prim loads P, but Q stays folded and unloaded.
    EXPECT_TRUE(stage.setFoldedPrims(std::set<PXR_NS::SdfPath> { q }));
    EXPECT_TRUE(usd->GetPrimAtPath(p).IsLoaded());
    EXPECT_TRUE(bool(usd->GetPrimAtPath(p.AppendChild(PXR_NS::TfToken("Geo")))));
    EXPECT_FALSE(usd->GetPrimAtPath(q).IsLoaded());

    EXPECT_TRUE(stage.setFoldedPrims(std::set<PXR_NS::SdfPath>()));
    EXPECT_TRUE(usd->GetPrimAtPath(q).IsLoaded());
}

TEST(SceneFoldTest, FoldingTheTopOfALargeSceneLightensImaging)
{
    const int kCubes = 20000;
    std::ostringstream body;
    body << "def Xform \"World\" {\n";
    for (int i = 0; i < kCubes; ++i) {
        body << "    def Cube \"C" << i << "\" {\n        double size = 0.5\n    }\n";
    }
    body << "}\n";
    PXR_NS::SdfLayerRefPtr layer = makeLayer(body.str());
    const PXR_NS::SdfPath world("/World");

    SceneStage stage(true);
    stage.sync(stackOf(layer));
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    const Imaged full = image(stage.getStage());
    const double fullMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    EXPECT_EQ((std::size_t)kCubes, full.countUnder(world, PXR_NS::HdPrimTypeTokens->cube));

    stage.setFoldedPrims(std::set<PXR_NS::SdfPath> { world });
    start = std::chrono::steady_clock::now();
    const Imaged folded = image(stage.getStage());
    const double foldedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    EXPECT_EQ(0u, folded.countUnder(world, PXR_NS::HdPrimTypeTokens->cube));
    EXPECT_LT(folded.prims.size() * 10, full.prims.size());
    std::cout << "[ SceneFold ] " << kCubes << " cubes: scene index walk " << fullMs << " ms unfolded, " << foldedMs
              << " ms folded; " << full.prims.size() << " vs " << folded.prims.size() << " Hydra prims" << std::endl;
}
