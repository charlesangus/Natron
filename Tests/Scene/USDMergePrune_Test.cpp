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

#include <pxr/usd/usd/primRange.h>

#include "Global/Macros.h"

#include <algorithm>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/StageCache.h"
#include "Engine/Nodes/Scene/USDMerge.h"
#include "Engine/Nodes/Scene/USDPrim.h"
#include "Engine/Nodes/Scene/USDPrune.h"
#include "Engine/Nodes/Scene/USDTransform.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

NATRON_NAMESPACE_USING

namespace {
KnobDouble*
getDoubleKnob(const NodePtr& node,
              const char* name)
{
    return dynamic_cast<KnobDouble*>(node->getKnobByName(name).get());
}

void
setString(const NodePtr& node,
          const char* name,
          const std::string& value)
{
    KnobString* knob = dynamic_cast<KnobString*>(node->getKnobByName(name).get());

    ASSERT_TRUE(knob != NULL) << name;
    knob->setValue(value);
}

PXR_NS::GfVec3d
worldTranslation(const PXR_NS::UsdStageRefPtr& stage,
                 const char* path)
{
    const PXR_NS::UsdGeomXformable xformable(stage->GetPrimAtPath(PXR_NS::SdfPath(path)));

    EXPECT_TRUE(bool(xformable)) << path;

    return xformable.ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode(1.)).ExtractTranslation();
}

// The layers of payload, weakest first.
std::vector<PXR_NS::SdfLayer*>
layersOf(const ScenePayload& payload)
{
    std::vector<PXR_NS::SdfLayer*> layers;

    for (const SceneLayerEntry& entry : SceneOps::getLayers(payload)) {
        layers.push_back(entry.layer.operator->());
    }

    return layers;
}
} // anonymous namespace

class USDMergePruneTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        StageCache::instance().clear();

        BaseTest::TearDown();
    }

    static ScenePayload evaluate(const NodePtr& node)
    {
        ScenePayload payload;
        std::string error;

        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk,
                  node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &payload, &error))
            << error;

        return payload;
    }

    // A USDPrim authoring path, moved by translate, over input (if any).
    NodePtr prim(const char* path,
                 const PXR_NS::GfVec3d& translate,
                 const NodePtr& input = NodePtr())
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPRIM));

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        setString(node, kSceneParamPrimPath, path);
        for (int d = 0; d < 3; ++d) {
            getDoubleKnob(node, kSceneParamTranslate)->setValue(translate[d], ViewSpec::all(), d);
        }
        if (input) {
            EXPECT_TRUE(node->connectInput(input, 0));
        }

        return node;
    }

    NodePtr merge(const std::vector<NodePtr>& inputs)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));

        EXPECT_TRUE(bool(node));
        for (std::size_t i = 0; node && i < inputs.size(); ++i) {
            if (inputs[i]) {
                EXPECT_TRUE(node->connectInput(inputs[i], (int)i)) << i;
            }
        }

        return node;
    }

    NodePtr prune(const NodePtr& input,
                  const char* pattern,
                  const char* modeID)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPRUNE));

        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        setString(node, kUSDPruneParamPrimPattern, pattern);
        KnobChoice* mode = dynamic_cast<KnobChoice*>(node->getKnobByName(kUSDPruneParamMode).get());
        EXPECT_TRUE(mode != NULL);
        if (mode) {
            mode->setValueFromID(modeID, 0);
        }
        EXPECT_TRUE(node->connectInput(input, 0));

        return node;
    }
};

TEST_F(USDMergePruneTest, TheLastInputWinsAConflictingAttribute)
{
    NodePtr left = prim("/Thing", PXR_NS::GfVec3d(1., 0., 0.));
    NodePtr right = prim("/Thing", PXR_NS::GfVec3d(0., 2., 0.));
    NodePtr other = prim("/Other", PXR_NS::GfVec3d(0., 0., 3.));
    ASSERT_TRUE(left && right && other);

    NodePtr leftThenRight = merge({ left, right, other });
    PXR_NS::UsdStageRefPtr stage = StageCache::instance().getStage(evaluate(leftThenRight));
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ(PXR_NS::GfVec3d(0., 2., 0.), worldTranslation(stage, "/Thing"));
    // What only one input authors is there too.
    EXPECT_EQ(PXR_NS::GfVec3d(0., 0., 3.), worldTranslation(stage, "/Other"));

    NodePtr rightThenLeft = merge({ right, left });
    stage = StageCache::instance().getStage(evaluate(rightThenLeft));
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ(PXR_NS::GfVec3d(1., 0., 0.), worldTranslation(stage, "/Thing"));
}

TEST_F(USDMergePruneTest, ADiamondYieldsTheSharedSourceLayerOnce)
{
    NodePtr source = prim("/World", PXR_NS::GfVec3d(100., 0., 0.));
    ASSERT_TRUE(bool(source));
    // One branch moves /World, the other adds a child.
    NodePtr lift = createNode(QString::fromUtf8(PLUGINID_NATRON_USDTRANSFORM));
    ASSERT_TRUE(bool(lift));
    setString(lift, kUSDTransformParamPrimPattern, "/World");
    getDoubleKnob(lift, kSceneParamTranslate)->setValue(5., ViewSpec::all(), 1);
    ASSERT_TRUE(lift->connectInput(source, 0));
    NodePtr child = prim("/World/Child", PXR_NS::GfVec3d(0., 0., 1.), source);
    NodePtr node = merge({ lift, child });

    const ScenePayload sourceScene = evaluate(source);
    const ScenePayload output = evaluate(node);
    ASSERT_EQ(1u, sourceScene.getNumLayers());
    const std::vector<PXR_NS::SdfLayer*> layers = layersOf(output);
    ASSERT_EQ(3u, layers.size());
    EXPECT_EQ(1, std::count(layers.begin(), layers.end(), layersOf(sourceScene).front()));
    EXPECT_EQ(layersOf(evaluate(lift)).back(), layers[1]);
    EXPECT_EQ(layersOf(evaluate(child)).back(), layers[2]);

    // The source stays under what each branch authored over it: the move survives the union.
    PXR_NS::UsdStageRefPtr stage = StageCache::instance().getStage(output);
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ(PXR_NS::GfVec3d(100., 5., 0.), worldTranslation(stage, "/World"));
    EXPECT_EQ(PXR_NS::GfVec3d(100., 5., 1.), worldTranslation(stage, "/World/Child"));

    // The same layers give the same stack.
    EXPECT_EQ(output.getStackHash(), evaluate(merge({ lift, child })).getStackHash());
}

TEST_F(USDMergePruneTest, OneInputPassesThroughAndNoneIsEmpty)
{
    NodePtr source = prim("/Thing", PXR_NS::GfVec3d(1., 0., 0.));
    ASSERT_TRUE(bool(source));
    // Only input 2 is connected.
    NodePtr node = merge({ NodePtr(), NodePtr(), source });
    const ScenePayload input = evaluate(source);
    EXPECT_TRUE(evaluate(node).isSameStack(input));

    // The same source twice is the source.
    EXPECT_TRUE(evaluate(merge({ source, source })).isSameStack(input));

    EXPECT_TRUE(evaluate(merge({})).isEmpty());
    EXPECT_TRUE(USDMerge::mergeStacks({ ScenePayload(), ScenePayload() }).isEmpty());
}

TEST_F(USDMergePruneTest, PrunedPrimsAreInactiveOnTheComposedStage)
{
    NodePtr world = prim("/World", PXR_NS::GfVec3d(0.));
    NodePtr a = prim("/World/A", PXR_NS::GfVec3d(0.), world);
    NodePtr aChild = prim("/World/A/Leaf", PXR_NS::GfVec3d(0.), a);
    NodePtr b = prim("/World/B", PXR_NS::GfVec3d(0.), aChild);
    ASSERT_TRUE(bool(b));
    NodePtr node = prune(b, "/World/A", "deactivate");

    PXR_NS::UsdStageRefPtr stage = StageCache::instance().getStage(evaluate(node));
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdPrim pruned = stage->GetPrimAtPath(PXR_NS::SdfPath("/World/A"));
    ASSERT_TRUE(bool(pruned));
    EXPECT_FALSE(pruned.IsActive());
    std::vector<std::string> traversed;
    for (const PXR_NS::UsdPrim& p : stage->Traverse()) {
        traversed.push_back(p.GetPath().GetString());
    }
    EXPECT_EQ(std::vector<std::string>({ "/World", "/World/B" }), traversed);
    // The input is not edited.
    PXR_NS::UsdStageRefPtr input = StageCache::instance().getStage(evaluate(b));
    EXPECT_TRUE(input->GetPrimAtPath(PXR_NS::SdfPath("/World/A")).IsActive());

    // Wildcards name several prims.
    setString(node, kUSDPruneParamPrimPattern, "/World/*");
    stage = StageCache::instance().getStage(evaluate(node));
    ASSERT_TRUE(bool(stage));
    EXPECT_FALSE(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/A")).IsActive());
    EXPECT_FALSE(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/B")).IsActive());
    EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/World")).IsActive());
}

TEST_F(USDMergePruneTest, HiddenPrimsStayOnTheStageInvisible)
{
    NodePtr world = prim("/World", PXR_NS::GfVec3d(0.));
    NodePtr a = prim("/World/A", PXR_NS::GfVec3d(0.), world);
    NodePtr leaf = prim("/World/A/Leaf", PXR_NS::GfVec3d(0.), a);
    ASSERT_TRUE(bool(leaf));
    NodePtr node = prune(leaf, "/World/A", "hide");

    PXR_NS::UsdStageRefPtr stage = StageCache::instance().getStage(evaluate(node));
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdGeomImageable hidden(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/A")));
    ASSERT_TRUE(bool(hidden));
    EXPECT_TRUE(hidden.GetPrim().IsActive());
    EXPECT_EQ(PXR_NS::UsdGeomTokens->invisible, hidden.ComputeVisibility(PXR_NS::UsdTimeCode(1.)));
    const PXR_NS::UsdGeomImageable leafPrim(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/A/Leaf")));
    EXPECT_EQ(PXR_NS::UsdGeomTokens->invisible, leafPrim.ComputeVisibility(PXR_NS::UsdTimeCode(1.)));
    const PXR_NS::UsdGeomImageable shown(stage->GetPrimAtPath(PXR_NS::SdfPath("/World")));
    EXPECT_EQ(PXR_NS::UsdGeomTokens->inherited, shown.ComputeVisibility(PXR_NS::UsdTimeCode(1.)));
}

TEST_F(USDMergePruneTest, ZeroMatchesWarnAndPassTheSceneThrough)
{
    NodePtr world = prim("/World", PXR_NS::GfVec3d(0.));
    ASSERT_TRUE(bool(world));
    NodePtr node = prune(world, "/Nowhere", "deactivate");

    const ScenePayload input = evaluate(world);
    EXPECT_TRUE(evaluate(node).isSameStack(input));
    ASSERT_TRUE(node->hasPersistentMessage());
    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("/Nowhere"))) << message.toStdString();

    setString(node, kUSDPruneParamPrimPattern, "/World");
    EXPECT_FALSE(evaluate(node).isSameStack(input));
    EXPECT_FALSE(node->hasPersistentMessage());

    // An empty pattern passes the scene through quietly.
    setString(node, kUSDPruneParamPrimPattern, " ");
    EXPECT_TRUE(evaluate(node).isSameStack(input));
    EXPECT_FALSE(node->hasPersistentMessage());
}
