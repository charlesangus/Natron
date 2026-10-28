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
#include <QTemporaryDir>

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
#include "Engine/Nodes/Scene/USDEditPrim.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Project.h"
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

bool
companionOn(const NodePtr& node,
            const std::string& name)
{
    KnobBoolPtr companion = getKnob<KnobBool>(node, name + "Authored");

    return companion && companion->getValue();
}
} // anonymous namespace

class EditPrim3DTest
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

    // A USDEditPrim in mode (edit or define) showing schema's knobs, fed by input when there is one.
    NodePtr createEditPrim(const NodePtr& input,
                           const std::string& mode,
                           const std::string& schema)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDEDITPRIM));
        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        KnobChoicePtr modeKnob = getKnob<KnobChoice>(node, kUSDEditPrimParamMode);
        KnobChoicePtr schemaKnob = getKnob<KnobChoice>(node, kUSDEditPrimParamSchema);
        EXPECT_TRUE(bool(modeKnob));
        EXPECT_TRUE(bool(schemaKnob));
        if (modeKnob) {
            modeKnob->setValueFromID(mode, 0);
        }
        if (schemaKnob) {
            schemaKnob->setValueFromID(schema, 0);
        }
        if (input) {
            EXPECT_TRUE(node->connectInput(input, 0));
        }

        return node;
    }

    static std::string getBuiltSchema(const NodePtr& node)
    {
        USDEditPrim* effect = dynamic_cast<USDEditPrim*>(node->getEffectInstance().get());

        return effect ? effect->getBuiltSchema() : std::string("<not a USDEditPrim>");
    }
};

TEST_F(EditPrim3DTest, PickingASchemaBuildsItsKnobs)
{
    NodePtr node = createEditPrim(NodePtr(), "edit", "Camera");
    ASSERT_TRUE(bool(node));
    EXPECT_EQ(std::string("Camera"), getBuiltSchema(node));

    KnobDoublePtr focalLength = getKnob<KnobDouble>(node, "focalLength");
    ASSERT_TRUE(bool(focalLength));
    EXPECT_EQ(std::string("Focal Length"), focalLength->getLabel());
    EXPECT_DOUBLE_EQ(50., focalLength->getValue());
    EXPECT_FALSE(companionOn(node, "focalLength"));
    EXPECT_TRUE(bool(getKnob<KnobChoice>(node, "projection")));
    KnobDoublePtr clippingRange = getKnob<KnobDouble>(node, "clippingRange");
    ASSERT_TRUE(bool(clippingRange));
    EXPECT_EQ(2, clippingRange->getDimension());

    // Another schema replaces the knobs.
    KnobChoicePtr schema = getKnob<KnobChoice>(node, kUSDEditPrimParamSchema);
    ASSERT_TRUE(bool(schema));
    schema->setValueFromID("SphereLight", 0);
    EXPECT_EQ(std::string("SphereLight"), getBuiltSchema(node));
    EXPECT_FALSE(bool(node->getKnobByName("focalLength")));
    EXPECT_FALSE(bool(node->getKnobByName("focalLengthAuthored")));
    EXPECT_TRUE(bool(getKnob<KnobColor>(node, "inputs_color")));

    schema->setValueFromID(kUSDEditPrimSchemaNone, 0);
    EXPECT_EQ(std::string(), getBuiltSchema(node));
    EXPECT_FALSE(bool(node->getKnobByName("inputs_color")));
}

TEST_F(EditPrim3DTest, EditAuthorsOnlyTheTouchedAttribute)
{
    NodePtr reader = createReader();
    ASSERT_TRUE(bool(reader));
    NodePtr node = createEditPrim(reader, "edit", "Camera");
    ASSERT_TRUE(bool(node));
    getKnob<KnobString>(node, kUSDEditPrimParamPrimPattern)->setValue(std::string("/World/Cam"));
    KnobDoublePtr focalLength = getKnob<KnobDouble>(node, "focalLength");
    ASSERT_TRUE(bool(focalLength));
    focalLength->setValue(80., ViewSpec::all(), 0, eValueChangedReasonUserEdited, 0);
    EXPECT_TRUE(companionOn(node, "focalLength"));

    ScenePayload payload;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, &payload));
    PXR_NS::SdfLayerRefPtr layer = getTopLayer(payload);
    ASSERT_TRUE(bool(layer));
    PXR_NS::SdfPrimSpecHandle spec = layer->GetPrimAtPath(PXR_NS::SdfPath("/World/Cam"));
    ASSERT_TRUE(bool(spec));
    EXPECT_EQ(PXR_NS::SdfSpecifierOver, spec->GetSpecifier());
    EXPECT_TRUE(spec->GetTypeName().IsEmpty());
    EXPECT_EQ(1u, spec->GetProperties().size());
    EXPECT_TRUE(bool(layer->GetAttributeAtPath(PXR_NS::SdfPath("/World/Cam.focalLength"))));
    EXPECT_FALSE(spec->HasInfo(PXR_NS::UsdTokens->apiSchemas));

    PXR_NS::UsdStageRefPtr stage = SceneOps::compose(payload);
    ASSERT_TRUE(bool(stage));
    PXR_NS::UsdGeomCamera camera(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Cam")));
    ASSERT_TRUE(bool(camera));
    float value = 0.f;
    ASSERT_TRUE(camera.GetFocalLengthAttr().Get(&value));
    EXPECT_FLOAT_EQ(80.f, value);
}

TEST_F(EditPrim3DTest, AnInstancedApiSchemaIsAppliedWithItsInstanceName)
{
    NodePtr reader = createReader();
    ASSERT_TRUE(bool(reader));
    NodePtr node = createEditPrim(reader, "edit", "CollectionAPI");
    ASSERT_TRUE(bool(node));
    getKnob<KnobString>(node, kUSDEditPrimParamPrimPattern)->setValue(std::string("/World"));
    KnobBoolPtr includeRoot = getKnob<KnobBool>(node, "collection_includeRoot");
    ASSERT_TRUE(bool(includeRoot));
    includeRoot->setValue(true, ViewSpec::all(), 0, eValueChangedReasonUserEdited, 0);

    // A multiple-apply schema needs an instance name.
    ScenePayload failed;
    EXPECT_NE(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, &failed));

    getKnob<KnobString>(node, kUSDEditPrimParamInstanceName)->setValue(std::string("lights"));
    ScenePayload payload;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, &payload));
    PXR_NS::SdfLayerRefPtr layer = getTopLayer(payload);
    ASSERT_TRUE(bool(layer));
    PXR_NS::SdfPrimSpecHandle spec = layer->GetPrimAtPath(PXR_NS::SdfPath("/World"));
    ASSERT_TRUE(bool(spec));
    EXPECT_TRUE(spec->HasInfo(PXR_NS::UsdTokens->apiSchemas));
    EXPECT_EQ(1u, spec->GetProperties().size());
    EXPECT_TRUE(bool(layer->GetAttributeAtPath(PXR_NS::SdfPath("/World.collection:lights:includeRoot"))));

    PXR_NS::UsdStageRefPtr stage = SceneOps::compose(payload);
    ASSERT_TRUE(bool(stage));
    PXR_NS::UsdPrim world = stage->GetPrimAtPath(PXR_NS::SdfPath("/World"));
    ASSERT_TRUE(bool(world));
    EXPECT_TRUE(world.HasAPI(PXR_NS::TfToken("CollectionAPI"), PXR_NS::TfToken("lights")));
    bool value = false;
    ASSERT_TRUE(world.GetAttribute(PXR_NS::TfToken("collection:lights:includeRoot")).Get(&value));
    EXPECT_TRUE(value);
}

TEST_F(EditPrim3DTest, DefineCreatesTheTypedPrimAndItsMissingAncestors)
{
    NodePtr node = createEditPrim(NodePtr(), "define", "SphereLight");
    ASSERT_TRUE(bool(node));
    getKnob<KnobString>(node, kSceneParamPrimPath)->setValue(std::string("/rig/key"));
    KnobDoublePtr intensity = getKnob<KnobDouble>(node, "inputs_intensity");
    ASSERT_TRUE(bool(intensity));
    intensity->setValue(5., ViewSpec::all(), 0, eValueChangedReasonUserEdited, 0);

    ScenePayload payload;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, &payload));
    PXR_NS::SdfLayerRefPtr layer = getTopLayer(payload);
    ASSERT_TRUE(bool(layer));
    PXR_NS::SdfPrimSpecHandle rig = layer->GetPrimAtPath(PXR_NS::SdfPath("/rig"));
    ASSERT_TRUE(bool(rig));
    EXPECT_EQ(PXR_NS::SdfSpecifierDef, rig->GetSpecifier());
    EXPECT_EQ(PXR_NS::TfToken("Xform"), rig->GetTypeName());
    PXR_NS::SdfPrimSpecHandle key = layer->GetPrimAtPath(PXR_NS::SdfPath("/rig/key"));
    ASSERT_TRUE(bool(key));
    EXPECT_EQ(PXR_NS::SdfSpecifierDef, key->GetSpecifier());
    EXPECT_EQ(PXR_NS::TfToken("SphereLight"), key->GetTypeName());
    EXPECT_EQ(1u, key->GetProperties().size());
    EXPECT_TRUE(bool(layer->GetAttributeAtPath(PXR_NS::SdfPath("/rig/key.inputs:intensity"))));

    PXR_NS::UsdStageRefPtr stage = SceneOps::compose(payload);
    ASSERT_TRUE(bool(stage));
    PXR_NS::UsdLuxSphereLight light(stage->GetPrimAtPath(PXR_NS::SdfPath("/rig/key")));
    ASSERT_TRUE(bool(light));
    float value = 0.f;
    ASSERT_TRUE(light.GetIntensityAttr().Get(&value));
    EXPECT_FLOAT_EQ(5.f, value);
}

TEST_F(EditPrim3DTest, DefineLeavesExistingAncestorsAsTheyAre)
{
    NodePtr reader = createReader();
    ASSERT_TRUE(bool(reader));
    NodePtr node = createEditPrim(reader, "define", "SphereLight");
    ASSERT_TRUE(bool(node));
    getKnob<KnobString>(node, kSceneParamPrimPath)->setValue(std::string("/World/Fill"));

    ScenePayload payload;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, &payload));
    PXR_NS::SdfLayerRefPtr layer = getTopLayer(payload);
    ASSERT_TRUE(bool(layer));
    PXR_NS::SdfPrimSpecHandle world = layer->GetPrimAtPath(PXR_NS::SdfPath("/World"));
    ASSERT_TRUE(bool(world));
    EXPECT_EQ(PXR_NS::SdfSpecifierOver, world->GetSpecifier());
    EXPECT_TRUE(world->GetTypeName().IsEmpty());
    PXR_NS::SdfPrimSpecHandle fill = layer->GetPrimAtPath(PXR_NS::SdfPath("/World/Fill"));
    ASSERT_TRUE(bool(fill));
    EXPECT_EQ(0u, fill->GetProperties().size());

    PXR_NS::UsdStageRefPtr stage = SceneOps::compose(payload);
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ(PXR_NS::TfToken("Xform"), stage->GetPrimAtPath(PXR_NS::SdfPath("/World")).GetTypeName());
    EXPECT_EQ(PXR_NS::TfToken("SphereLight"), stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Fill")).GetTypeName());
}

TEST_F(EditPrim3DTest, DefiningAnAbstractSchemaFails)
{
    NodePtr node = createEditPrim(NodePtr(), "define", "Imageable");
    ASSERT_TRUE(bool(node));
    getKnob<KnobString>(node, kSceneParamPrimPath)->setValue(std::string("/thing"));

    ScenePayload payload;
    EXPECT_NE(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, &payload));
}

TEST_F(EditPrim3DTest, SchemaKnobsSurviveSaveAndLoad)
{
    NodePtr node = createEditPrim(NodePtr(), "edit", "Camera");
    ASSERT_TRUE(bool(node));
    const std::string name = node->getScriptName();
    KnobDoublePtr focalLength = getKnob<KnobDouble>(node, "focalLength");
    KnobChoicePtr projection = getKnob<KnobChoice>(node, "projection");
    ASSERT_TRUE(bool(focalLength));
    ASSERT_TRUE(bool(projection));
    focalLength->setValue(80., ViewSpec::all(), 0, eValueChangedReasonUserEdited, 0);
    // A changed value with its toggle turned off stays off: loading a choice knob goes through
    // knobChanged().
    projection->setValueFromID("orthographic", 0);
    projection->setValue(projection->getValue(), ViewSpec::all(), 0, eValueChangedReasonUserEdited, 0);
    ASSERT_TRUE(companionOn(node, "projection"));
    getKnob<KnobBool>(node, "projectionAuthored")->setValue(false);

    ProjectPtr project = getApp()->getProject();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("edit-prim.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));

    node.reset();
    focalLength.reset();
    projection.reset();
    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr loaded = project->getNodeByName(name);
    ASSERT_TRUE(bool(loaded));
    EXPECT_EQ(std::string("Camera"), getBuiltSchema(loaded));
    KnobDoublePtr loadedFocalLength = getKnob<KnobDouble>(loaded, "focalLength");
    ASSERT_TRUE(bool(loadedFocalLength));
    EXPECT_DOUBLE_EQ(80., loadedFocalLength->getValue());
    EXPECT_TRUE(companionOn(loaded, "focalLength"));
    KnobChoicePtr loadedProjection = getKnob<KnobChoice>(loaded, "projection");
    ASSERT_TRUE(bool(loadedProjection));
    EXPECT_EQ(std::string("orthographic"), loadedProjection->getActiveEntry().id);
    EXPECT_FALSE(companionOn(loaded, "projection"));
}
