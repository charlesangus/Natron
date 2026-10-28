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

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/ScenePayloadImpl.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDReference.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kFixture = NATRON_TESTS_FIXTURES_DIR "/scene-reference.usda";

PXR_NS::SdfLayerRefPtr
getTopLayer(const ScenePayload& payload)
{
    return payload.getImpl() ? payload.getImpl()->top.layer : PXR_NS::SdfLayerRefPtr();
}

template <typename KNOB>
std::shared_ptr<KNOB>
getKnob(const NodePtr& node,
        const std::string& name)
{
    return std::dynamic_pointer_cast<KNOB>(node->getKnobByName(name));
}

std::string
getTypeName(const PXR_NS::UsdStageRefPtr& stage,
            const std::string& path)
{
    const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(PXR_NS::SdfPath(path));

    return prim ? prim.GetTypeName().GetString() : std::string("<no prim>");
}
} // anonymous namespace

class Reference3DTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        SceneQueryPool::instance().releaseIdle();

        BaseTest::TearDown();
    }

    static EffectInstance::RenderRoIRetCode evaluate(const NodePtr& node,
                                                     ScenePayload* payload)
    {
        std::string error;

        return node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), payload, &error);
    }

    NodePtr createReader()
    {
        NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
        EXPECT_TRUE(bool(reader));
        if (reader) {
            KnobFilePtr file = getKnob<KnobFile>(reader, kUSDReadParamFile);
            EXPECT_TRUE(bool(file));
            if (file) {
                file->setValue(std::string(kFixture));
            }
        }

        return reader;
    }

    // A USDReference at primPath, fed by scene when there is one.
    NodePtr createReference(const NodePtr& scene,
                            const std::string& primPath)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREFERENCE));
        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        KnobStringPtr path = getKnob<KnobString>(node, kSceneParamPrimPath);
        EXPECT_TRUE(bool(path));
        if (path) {
            path->setValue(primPath);
        }
        if (scene) {
            EXPECT_TRUE(node->connectInput(scene, 0));
        }

        return node;
    }

    static void referenceFile(const NodePtr& node)
    {
        KnobFilePtr file = getKnob<KnobFile>(node, kUSDReferenceParamFile);
        ASSERT_TRUE(bool(file));
        file->setValue(std::string(kFixture));
    }
};

TEST_F(Reference3DTest, AFileReferenceDefinesThePrimAndItsAncestors)
{
    NodePtr node = createReference(NodePtr(), "/Set/Ref");
    ASSERT_TRUE(bool(node));
    referenceFile(node);

    ScenePayload payload;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, &payload));
    PXR_NS::SdfLayerRefPtr layer = getTopLayer(payload);
    ASSERT_TRUE(bool(layer));
    PXR_NS::SdfPrimSpecHandle spec = layer->GetPrimAtPath(PXR_NS::SdfPath("/Set/Ref"));
    ASSERT_TRUE(bool(spec));
    EXPECT_EQ(PXR_NS::SdfSpecifierDef, spec->GetSpecifier());
    EXPECT_TRUE(spec->HasReferences());
    EXPECT_FALSE(spec->HasPayloads());
    EXPECT_FALSE(spec->GetInstanceable());
    PXR_NS::SdfPrimSpecHandle set = layer->GetPrimAtPath(PXR_NS::SdfPath("/Set"));
    ASSERT_TRUE(bool(set));
    EXPECT_EQ(PXR_NS::SdfSpecifierDef, set->GetSpecifier());
    EXPECT_EQ(std::string("Xform"), set->GetTypeName().GetString());

    // No source prim: the file's default prim, /World, composes at /Set/Ref.
    PXR_NS::UsdStageRefPtr stage = SceneOps::compose(payload);
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ(std::string("Xform"), getTypeName(stage, "/Set/Ref"));
    EXPECT_EQ(std::string("Cube"), getTypeName(stage, "/Set/Ref/Box"));
    EXPECT_EQ(std::string("Sphere"), getTypeName(stage, "/Set/Ref/Ball"));
}

TEST_F(Reference3DTest, APayloadOfASourcePrimIsInstanceable)
{
    NodePtr node = createReference(NodePtr(), "/Ref");
    ASSERT_TRUE(bool(node));
    referenceFile(node);
    getKnob<KnobChoice>(node, kUSDReferenceParamMode)->setValueFromID("payload", 0);
    getKnob<KnobString>(node, kUSDReferenceParamSourcePrim)->setValue(std::string("/World/Ball"));
    getKnob<KnobBool>(node, kUSDReferenceParamInstanceable)->setValue(true);

    ScenePayload payload;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, &payload));
    PXR_NS::SdfLayerRefPtr layer = getTopLayer(payload);
    ASSERT_TRUE(bool(layer));
    PXR_NS::SdfPrimSpecHandle spec = layer->GetPrimAtPath(PXR_NS::SdfPath("/Ref"));
    ASSERT_TRUE(bool(spec));
    EXPECT_FALSE(spec->HasReferences());
    ASSERT_TRUE(spec->HasPayloads());
    EXPECT_TRUE(spec->GetInstanceable());

    PXR_NS::UsdStageRefPtr stage = SceneOps::compose(payload);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(PXR_NS::SdfPath("/Ref"));
    ASSERT_TRUE(bool(prim));
    EXPECT_EQ(std::string("Sphere"), prim.GetTypeName().GetString());
    EXPECT_TRUE(prim.IsInstance());
}

TEST_F(Reference3DTest, AnExistingPrimKeepsItsOwnOpinions)
{
    NodePtr reader = createReader();
    ASSERT_TRUE(bool(reader));
    NodePtr node = createReference(reader, "/World/Ball");
    ASSERT_TRUE(bool(node));
    referenceFile(node);
    getKnob<KnobString>(node, kUSDReferenceParamSourcePrim)->setValue(std::string("/World"));

    ScenePayload payload;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, &payload));
    PXR_NS::SdfLayerRefPtr layer = getTopLayer(payload);
    ASSERT_TRUE(bool(layer));
    PXR_NS::SdfPrimSpecHandle spec = layer->GetPrimAtPath(PXR_NS::SdfPath("/World/Ball"));
    ASSERT_TRUE(bool(spec));
    EXPECT_EQ(PXR_NS::SdfSpecifierOver, spec->GetSpecifier());
    PXR_NS::SdfPrimSpecHandle world = layer->GetPrimAtPath(PXR_NS::SdfPath("/World"));
    ASSERT_TRUE(bool(world));
    EXPECT_EQ(PXR_NS::SdfSpecifierOver, world->GetSpecifier());

    // A reference is weaker than the input's own layers: Ball stays a Sphere and gains children.
    PXR_NS::UsdStageRefPtr stage = SceneOps::compose(payload);
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ(std::string("Sphere"), getTypeName(stage, "/World/Ball"));
    EXPECT_EQ(std::string("Cube"), getTypeName(stage, "/World/Ball/Box"));
}

TEST_F(Reference3DTest, TheSourceInputIsReferencedWithItsWholeStack)
{
    NodePtr scene = createReader();
    NodePtr source = createReader();
    ASSERT_TRUE(bool(scene));
    ASSERT_TRUE(bool(source));
    NodePtr node = createReference(scene, "/World/Copy");
    ASSERT_TRUE(bool(node));
    getKnob<KnobChoice>(node, kUSDReferenceParamSource)->setValueFromID("input", 0);
    ASSERT_TRUE(node->connectInput(source, 1));

    ScenePayload payload;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, &payload));
    PXR_NS::SdfLayerRefPtr layer = getTopLayer(payload);
    ASSERT_TRUE(bool(layer));
    PXR_NS::SdfPrimSpecHandle spec = layer->GetPrimAtPath(PXR_NS::SdfPath("/World/Copy"));
    ASSERT_TRUE(bool(spec));
    EXPECT_EQ(PXR_NS::SdfSpecifierDef, spec->GetSpecifier());
    const PXR_NS::SdfReferenceVector references = spec->GetReferenceList().GetPrependedItems();
    ASSERT_EQ(1u, references.size());
    EXPECT_EQ(std::string("/World"), references.front().GetPrimPath().GetString());

    // The referenced layer is anonymous: the entry holds it, and the stack it sublayers.
    ASSERT_TRUE(bool(payload.getImpl()));
    std::shared_ptr<const SceneLayerDependencies> dependencies = payload.getImpl()->top.dependencies;
    ASSERT_TRUE(bool(dependencies));
    ASSERT_EQ(1u, dependencies->layers.size());
    ASSERT_TRUE(bool(dependencies->layers.front()));
    EXPECT_TRUE(dependencies->layers.front()->IsAnonymous());
    EXPECT_EQ(references.front().GetAssetPath(), dependencies->layers.front()->GetIdentifier());
    ASSERT_EQ(1u, dependencies->stacks.size());

    PXR_NS::UsdStageRefPtr stage = SceneOps::compose(payload);
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ(std::string("Cube"), getTypeName(stage, "/World/Box"));
    EXPECT_EQ(std::string("Xform"), getTypeName(stage, "/World/Copy"));
    EXPECT_EQ(std::string("Cube"), getTypeName(stage, "/World/Copy/Box"));
    EXPECT_EQ(std::string("Camera"), getTypeName(stage, "/World/Copy/Cam"));
}

TEST_F(Reference3DTest, AMissingSourcePassesTheSceneThrough)
{
    NodePtr reader = createReader();
    ASSERT_TRUE(bool(reader));
    NodePtr node = createReference(reader, "/World/Copy");
    ASSERT_TRUE(bool(node));
    getKnob<KnobChoice>(node, kUSDReferenceParamSource)->setValueFromID("input", 0);

    ScenePayload input;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(reader, &input));
    ScenePayload payload;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, &payload));
    EXPECT_TRUE(payload.isSameStack(input));
    ASSERT_TRUE(node->hasPersistentMessage());
    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeWarning, type);
}

TEST_F(Reference3DTest, ABadSourcePrimFails)
{
    NodePtr node = createReference(NodePtr(), "/Ref");
    ASSERT_TRUE(bool(node));
    referenceFile(node);
    getKnob<KnobString>(node, kUSDReferenceParamSourcePrim)->setValue(std::string("World"));

    ScenePayload payload;
    EXPECT_NE(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, &payload));
    ASSERT_TRUE(node->hasPersistentMessage());
}
