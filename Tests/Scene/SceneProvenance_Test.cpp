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

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"

#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDCamera.h"
#include "Engine/Nodes/Scene/USDEditTransform.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDSetAttribute.h"
#include "Engine/Nodes/Scene/USDXformPrim.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

using SceneOps::SceneOpinion;
using SceneOps::ScenePrimArc;
using SceneOps::ScenePrimComposition;

namespace {
void
setString(const NodePtr& node,
          const char* name,
          const std::string& value)
{
    KnobString* knob = dynamic_cast<KnobString*>(node->getKnobByName(name).get());

    ASSERT_TRUE(knob != NULL) << name;
    knob->setValue(value);
}

std::string
getString(const NodePtr& node,
          const char* name)
{
    KnobString* knob = dynamic_cast<KnobString*>(node->getKnobByName(name).get());

    EXPECT_TRUE(knob != NULL) << name;

    return knob ? knob->getValue() : std::string();
}

PXR_NS::UsdAttribute
attributeAt(const PXR_NS::UsdStageRefPtr& stage,
            const std::string& primPath,
            const std::string& name)
{
    const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(PXR_NS::SdfPath(primPath));

    EXPECT_TRUE(bool(prim)) << primPath;

    return prim ? prim.GetAttribute(PXR_NS::TfToken(name)) : PXR_NS::UsdAttribute();
}
} // anonymous namespace

class SceneProvenanceTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        SceneQueryPool::instance().releaseIdle();

        BaseTest::TearDown();
    }

    static PXR_NS::UsdStageRefPtr compose(const NodePtr& node)
    {
        ScenePayload payload;
        std::string error;

        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk,
                  node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &payload, &error))
            << error;

        return SceneOps::compose(payload);
    }
};

TEST_F(SceneProvenanceTest, EachOpinionResolvesToTheNodeThatAuthoredIt)
{
    NodePtr camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
    NodePtr transform = createNode(QString::fromUtf8(PLUGINID_NATRON_USDEDITTRANSFORM));
    ASSERT_TRUE(camera && transform);
    const std::string cameraPath = getString(camera, kSceneParamPrimPath);
    ASSERT_FALSE(cameraPath.empty());
    setString(transform, kUSDEditTransformParamPrimPattern, cameraPath);
    KnobDouble* translate = dynamic_cast<KnobDouble*>(transform->getKnobByName(kSceneParamTranslate).get());
    ASSERT_TRUE(translate != NULL);
    translate->setValue(5., ViewSpec::all(), 1);
    ASSERT_TRUE(transform->connectInput(camera, 0));

    PXR_NS::UsdStageRefPtr stage = compose(transform);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdTimeCode time(1.);

    std::string identifier;
    const PXR_NS::UsdAttribute focalLength = attributeAt(stage, cameraPath, "focalLength");
    ASSERT_TRUE(bool(focalLength));
    EXPECT_EQ(camera, SceneOps::authoringNodeOf(focalLength, time, &identifier));
    EXPECT_FALSE(identifier.empty());

    const std::string opName = "xformOp:transform:" + transform->getScriptName_mt_safe();
    const PXR_NS::UsdAttribute op = attributeAt(stage, cameraPath, opName);
    ASSERT_TRUE(bool(op)) << opName;
    EXPECT_EQ(transform, SceneOps::authoringNodeOf(op, time, &identifier));

    // The edit's xformOpOrder is stronger than the camera's.
    const PXR_NS::UsdAttribute order = attributeAt(stage, cameraPath, "xformOpOrder");
    ASSERT_TRUE(bool(order));
    EXPECT_EQ(transform, SceneOps::authoringNodeOf(order, time, &identifier));

    // An attribute nobody authored falls back to its schema: no node, no layer.
    const PXR_NS::UsdAttribute exposure = attributeAt(stage, cameraPath, "exposure");
    if (exposure && !exposure.HasAuthoredValue()) {
        EXPECT_FALSE(bool(SceneOps::authoringNodeOf(exposure, time, &identifier)));
        EXPECT_TRUE(identifier.empty());
    }
}

TEST_F(SceneProvenanceTest, AReauthoredLayerResolvesToTheSameNode)
{
    NodePtr camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
    ASSERT_TRUE(bool(camera));
    const std::string cameraPath = getString(camera, kSceneParamPrimPath);
    KnobDouble* focal = dynamic_cast<KnobDouble*>(camera->getKnobByName(kUSDCameraParamFocalLength).get());
    ASSERT_TRUE(focal != NULL);

    ASSERT_TRUE(bool(compose(camera)));
    focal->setValue(focal->getValue() + 10.);
    PXR_NS::UsdStageRefPtr stage = compose(camera);
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ(camera, SceneOps::authoringNodeOf(attributeAt(stage, cameraPath, "focalLength"), PXR_NS::UsdTimeCode(1.), NULL));
}

TEST_F(SceneProvenanceTest, GraftedOpinionsKeepTheirAuthors)
{
    NodePtr world = createNode(QString::fromUtf8(PLUGINID_NATRON_USDXFORMPRIM));
    NodePtr a = createNode(QString::fromUtf8(PLUGINID_NATRON_USDXFORMPRIM));
    ASSERT_TRUE(world && a);
    setString(world, kSceneParamPrimPath, "/World");
    setString(a, kSceneParamPrimPath, "/A");
    dynamic_cast<KnobDouble*>(world->getKnobByName(kSceneParamTranslate).get())->setValue(100., ViewSpec::all(), 0);
    dynamic_cast<KnobDouble*>(a->getKnobByName(kSceneParamTranslate).get())->setValue(3., ViewSpec::all(), 1);
    ASSERT_TRUE(world->connectInput(a, 0));

    PXR_NS::UsdStageRefPtr stage = compose(world);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdTimeCode time(1.);

    // A's opinion reaches /World/A through the graft's reference, from A's own layer.
    const PXR_NS::UsdAttribute aOp = attributeAt(stage, "/World/A", "xformOp:transform");
    ASSERT_TRUE(bool(aOp));
    EXPECT_EQ(a, SceneOps::authoringNodeOf(aOp, time, NULL));

    // /World's own transform is in the re-parenting node's layers.
    const PXR_NS::UsdAttribute worldOp = attributeAt(stage, "/World", "xformOp:transform");
    ASSERT_TRUE(bool(worldOp));
    EXPECT_EQ(world, SceneOps::authoringNodeOf(worldOp, time, NULL));
}

TEST_F(SceneProvenanceTest, AFileReadInReportsItsPath)
{
    NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
    ASSERT_TRUE(bool(reader));
    KnobFile* file = dynamic_cast<KnobFile*>(reader->getKnobByName(kUSDReadParamFile).get());
    ASSERT_TRUE(file != NULL);
    file->setValue(NATRON_TESTS_FIXTURES_DIR "/scene-reference.usda");

    PXR_NS::UsdStageRefPtr stage = compose(reader);
    ASSERT_TRUE(bool(stage));
    std::string identifier;
    const PXR_NS::UsdAttribute size = attributeAt(stage, "/World/Box", "size");
    ASSERT_TRUE(bool(size));
    EXPECT_FALSE(bool(SceneOps::authoringNodeOf(size, PXR_NS::UsdTimeCode(1.), &identifier)));
    const std::string suffix = "scene-reference.usda";
    ASSERT_GE(identifier.size(), suffix.size()) << identifier;
    EXPECT_EQ(suffix, identifier.substr(identifier.size() - suffix.size()));
}

namespace {
const ScenePrimArc*
findArc(const ScenePrimComposition& composition,
        ScenePrimArc::Type type)
{
    for (const ScenePrimArc& arc : composition.arcs) {
        if (arc.type == type) {
            return &arc;
        }
    }

    return nullptr;
}
} // anonymous namespace

TEST_F(SceneProvenanceTest, ACompositionReportsEveryArcAndLayer)
{
    const PXR_NS::SdfLayerRefPtr referenced = PXR_NS::SdfLayer::CreateAnonymous("referenced.usda");
    ASSERT_TRUE(referenced->ImportFromString("#usda 1.0\ndef Xform \"Ref\" { }\n"));
    const PXR_NS::SdfLayerRefPtr payloaded = PXR_NS::SdfLayer::CreateAnonymous("payloaded.usda");
    ASSERT_TRUE(payloaded->ImportFromString("#usda 1.0\ndef Xform \"Pay\" { }\n"));
    const PXR_NS::SdfLayerRefPtr root = PXR_NS::SdfLayer::CreateAnonymous("root.usda");
    const std::string text = "#usda 1.0\n"
                             "class Xform \"_cls\" { }\n"
                             "def Xform \"Prim\" (\n"
                             "    inherits = </_cls>\n"
                             "    references = @"
        + referenced->GetIdentifier() + "@</Ref>\n"
                                        "    payload = @"
        + payloaded->GetIdentifier() + "@</Pay>\n"
                                       "    variants = { string look = \"red\" }\n"
                                       "    prepend variantSets = \"look\"\n"
                                       ")\n"
                                       "{\n"
                                       "    variantSet \"look\" = {\n"
                                       "        \"red\" { }\n"
                                       "    }\n"
                                       "}\n";
    ASSERT_TRUE(root->ImportFromString(text));
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(root);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::SdfPath primPath("/Prim");

    ScenePrimComposition composition = SceneOps::getPrimComposition(stage->GetPrimAtPath(primPath));
    EXPECT_FALSE(composition.isPrototype);
    ASSERT_TRUE(findArc(composition, ScenePrimArc::eRoot) != nullptr);
    EXPECT_EQ(root->GetIdentifier(), findArc(composition, ScenePrimArc::eRoot)->targetLayer);

    const ScenePrimArc* reference = findArc(composition, ScenePrimArc::eReference);
    ASSERT_TRUE(reference != nullptr);
    EXPECT_EQ("/Ref", reference->targetPath);
    EXPECT_EQ(referenced->GetIdentifier(), reference->targetLayer);
    EXPECT_EQ(root->GetIdentifier(), reference->introducingLayer);
    EXPECT_TRUE(reference->loaded);
    EXPECT_FALSE(reference->ancestral);

    const ScenePrimArc* payload = findArc(composition, ScenePrimArc::ePayload);
    ASSERT_TRUE(payload != nullptr);
    EXPECT_EQ("/Pay", payload->targetPath);
    EXPECT_EQ(payloaded->GetIdentifier(), payload->targetLayer);
    EXPECT_TRUE(payload->loaded);

    const ScenePrimArc* variant = findArc(composition, ScenePrimArc::eVariant);
    ASSERT_TRUE(variant != nullptr);
    EXPECT_EQ("/Prim{look=red}", variant->targetPath);

    const ScenePrimArc* inherit = findArc(composition, ScenePrimArc::eInherit);
    ASSERT_TRUE(inherit != nullptr);
    EXPECT_EQ("/_cls", inherit->targetPath);
    EXPECT_EQ(root->GetIdentifier(), inherit->introducingLayer);

    // An unloaded payload keeps its authored arc, with no target in the prim index.
    stage->Unload(primPath);
    composition = SceneOps::getPrimComposition(stage->GetPrimAtPath(primPath));
    payload = findArc(composition, ScenePrimArc::ePayload);
    ASSERT_TRUE(payload != nullptr);
    EXPECT_FALSE(payload->loaded);
    EXPECT_EQ("/Pay", payload->targetPath);
    EXPECT_EQ(payloaded->GetIdentifier(), payload->targetLayer);
    EXPECT_EQ(root->GetIdentifier(), payload->introducingLayer);
    EXPECT_TRUE(findArc(composition, ScenePrimArc::eReference) != nullptr);
}

TEST_F(SceneProvenanceTest, ACompositionMapsLayersToNodesAndHandlesPrototypes)
{
    NodePtr world = createNode(QString::fromUtf8(PLUGINID_NATRON_USDXFORMPRIM));
    NodePtr a = createNode(QString::fromUtf8(PLUGINID_NATRON_USDXFORMPRIM));
    ASSERT_TRUE(world && a);
    setString(world, kSceneParamPrimPath, "/World");
    setString(a, kSceneParamPrimPath, "/A");
    ASSERT_TRUE(world->connectInput(a, 0));

    PXR_NS::UsdStageRefPtr stage = compose(world);
    ASSERT_TRUE(bool(stage));
    const ScenePrimComposition grafted = SceneOps::getPrimComposition(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/A")));
    bool targetsA = false;
    for (const ScenePrimArc& arc : grafted.arcs) {
        targetsA = targetsA || ((arc.type == ScenePrimArc::eReference) && (arc.targetOwner == a));
    }
    EXPECT_TRUE(targetsA);

    const PXR_NS::SdfLayerRefPtr root = PXR_NS::SdfLayer::CreateAnonymous("instances.usda");
    ASSERT_TRUE(root->ImportFromString("#usda 1.0\n"
                                       "def Xform \"Source\" {\n"
                                       "    def Xform \"Child\" { }\n"
                                       "}\n"
                                       "def Xform \"Instance\" (\n"
                                       "    instanceable = true\n"
                                       "    references = </Source>\n"
                                       ") { }\n"));
    PXR_NS::UsdStageRefPtr instances = PXR_NS::UsdStage::Open(root);
    ASSERT_TRUE(bool(instances));
    const PXR_NS::UsdPrim instance = instances->GetPrimAtPath(PXR_NS::SdfPath("/Instance"));
    ASSERT_TRUE(instance.IsInstance());

    const ScenePrimComposition own = SceneOps::getPrimComposition(instance);
    EXPECT_TRUE(own.prototypePath.empty());
    EXPECT_FALSE(own.arcs.empty());

    const ScenePrimComposition prototype = SceneOps::getPrimComposition(instance.GetPrototype());
    EXPECT_TRUE(prototype.isPrototype);
    EXPECT_TRUE(prototype.arcs.empty());

    const PXR_NS::UsdPrim proxy = instance.GetChild(PXR_NS::TfToken("Child"));
    ASSERT_TRUE(proxy.IsInstanceProxy());
    const ScenePrimComposition viaProxy = SceneOps::getPrimComposition(proxy);
    EXPECT_TRUE(viaProxy.isInstanceProxy);
    EXPECT_EQ(proxy.GetPrimInPrototype().GetPath().GetString(), viaProxy.prototypePath);
    EXPECT_TRUE(viaProxy.arcs.empty());
}

TEST_F(SceneProvenanceTest, OpinionsListEveryNodeStrongestFirstWithTheirValues)
{
    NodePtr camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
    NodePtr transform = createNode(QString::fromUtf8(PLUGINID_NATRON_USDEDITTRANSFORM));
    NodePtr setter = createNode(QString::fromUtf8(PLUGINID_NATRON_USDSETATTRIBUTE));
    ASSERT_TRUE(camera && transform && setter);
    const std::string cameraPath = getString(camera, kSceneParamPrimPath);
    ASSERT_FALSE(cameraPath.empty());
    setString(transform, kUSDEditTransformParamPrimPattern, cameraPath);
    setString(setter, kUSDSetAttributeParamPrimPattern, cameraPath);
    setString(setter, kUSDSetAttributeParamAttributeName, "focalLength");
    KnobDouble* value = dynamic_cast<KnobDouble*>(setter->getKnobByName(kUSDSetAttributeParamValue).get());
    ASSERT_TRUE(value != NULL);
    value->setValue(99.);
    ASSERT_TRUE(transform->connectInput(camera, 0));
    ASSERT_TRUE(setter->connectInput(transform, 0));

    PXR_NS::UsdStageRefPtr stage = compose(setter);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(PXR_NS::SdfPath(cameraPath));
    ASSERT_TRUE(bool(prim));

    // The prim is defined by the camera, then overridden by the edit and the setter, which is
    // downstream and so strongest.
    std::vector<NodePtr> primOwners;
    for (const SceneOpinion& opinion : SceneOps::getPrimOpinions(prim)) {
        if (opinion.owner) {
            primOwners.push_back(opinion.owner);
        }
    }
    ASSERT_EQ(3u, primOwners.size());
    EXPECT_EQ(setter, primOwners[0]);
    EXPECT_EQ(transform, primOwners[1]);
    EXPECT_EQ(camera, primOwners[2]);

    std::vector<SceneOpinion> focal;
    for (const SceneOpinion& opinion : SceneOps::getPropertyOpinions(prim, PXR_NS::TfToken("focalLength"), PXR_NS::UsdTimeCode(1.))) {
        if (opinion.owner) {
            focal.push_back(opinion);
        }
    }
    ASSERT_EQ(2u, focal.size());
    EXPECT_EQ(setter, focal[0].owner);
    ASSERT_TRUE(focal[0].value.IsHolding<float>());
    EXPECT_FLOAT_EQ(99.f, focal[0].value.UncheckedGet<float>());
    EXPECT_EQ(camera, focal[1].owner);
    EXPECT_FALSE(focal[1].value.IsEmpty());
    EXPECT_FALSE(focal[0].layerIdentifier.empty());
    EXPECT_DOUBLE_EQ(1., focal[0].layerScale);

    EXPECT_TRUE(SceneOps::getPropertyOpinions(prim, PXR_NS::TfToken("noSuchProperty"), PXR_NS::UsdTimeCode(1.)).empty());
    EXPECT_TRUE(SceneOps::getPrimOpinions(PXR_NS::UsdPrim()).empty());
}

TEST_F(SceneProvenanceTest, OpinionsCarryLayerOffsetsAndTimeSamples)
{
    const PXR_NS::SdfLayerRefPtr referenced = PXR_NS::SdfLayer::CreateAnonymous("referenced.usda");
    ASSERT_TRUE(referenced->ImportFromString("#usda 1.0\n"
                                             "def Xform \"Ref\" {\n"
                                             "    double animated.timeSamples = { 1: 5, 2: 6 }\n"
                                             "}\n"));
    const PXR_NS::SdfLayerRefPtr root = PXR_NS::SdfLayer::CreateAnonymous("root.usda");
    ASSERT_TRUE(root->ImportFromString("#usda 1.0\n"
                                       "def Xform \"Prim\" (\n"
                                       "    references = @"
                                       + referenced->GetIdentifier() + "@</Ref> (offset = 10; scale = 2)\n"
                                                                       ") { }\n"));
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(root);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(PXR_NS::SdfPath("/Prim"));
    ASSERT_TRUE(bool(prim));

    const std::vector<SceneOpinion> specs = SceneOps::getPrimOpinions(prim);
    ASSERT_EQ(2u, specs.size());
    EXPECT_EQ("def Xform", specs[0].primDescription);
    EXPECT_DOUBLE_EQ(0., specs[0].layerOffset);
    EXPECT_EQ(referenced->GetIdentifier(), specs[1].layerIdentifier);
    EXPECT_DOUBLE_EQ(10., specs[1].layerOffset);
    EXPECT_DOUBLE_EQ(2., specs[1].layerScale);

    const std::vector<SceneOpinion> animated = SceneOps::getPropertyOpinions(prim, PXR_NS::TfToken("animated"), PXR_NS::UsdTimeCode(12.));
    ASSERT_EQ(1u, animated.size());
    EXPECT_EQ(2u, animated[0].numTimeSamples);
    EXPECT_TRUE(animated[0].value.IsEmpty());
}
