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

#include <pxr/usd/usdGeom/cube.h>

#include "Global/Macros.h"

#include <cmath>
#include <limits>
#include <list>
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
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/ScenePayloadImpl.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDSetAttribute.h"
#include "Engine/Nodes/Scene/USDWrite.h"
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
} // anonymous namespace

class SetAttribute3DTest
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

    // A USDSetAttribute over the fixture setting name, of type, on the prims pattern matches.
    NodePtr createSetAttribute(const std::string& pattern,
                               const std::string& name,
                               const std::string& type,
                               bool primvar)
    {
        NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
        EXPECT_TRUE(bool(reader));
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDSETATTRIBUTE));
        EXPECT_TRUE(bool(node));
        if (!reader || !node) {
            return NodePtr();
        }
        getKnob<KnobFile>(reader, kUSDReadParamFile)->setValue(std::string(kFixture));
        EXPECT_TRUE(node->connectInput(reader, 0));
        getKnob<KnobString>(node, kUSDSetAttributeParamPrimPattern)->setValue(pattern);
        getKnob<KnobString>(node, kUSDSetAttributeParamAttributeName)->setValue(name);
        getKnob<KnobChoice>(node, kUSDSetAttributeParamValueType)->setValueFromID(type, 0);
        getKnob<KnobBool>(node, kUSDSetAttributeParamPrimvar)->setValue(primvar);

        return node;
    }

    static std::string getBuiltType(const NodePtr& node)
    {
        USDSetAttribute* effect = dynamic_cast<USDSetAttribute*>(node->getEffectInstance().get());

        return effect ? effect->getBuiltType() : std::string("<not a USDSetAttribute>");
    }
};

TEST_F(SetAttribute3DTest, TheValueKnobFollowsTheType)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDSETATTRIBUTE));
    ASSERT_TRUE(bool(node));
    EXPECT_EQ(std::string("float"), getBuiltType(node));
    KnobDoublePtr scalar = getKnob<KnobDouble>(node, kUSDSetAttributeParamValue);
    ASSERT_TRUE(bool(scalar));
    EXPECT_EQ(1, scalar->getDimension());
    EXPECT_EQ(std::string("Value"), scalar->getLabel());

    KnobChoicePtr type = getKnob<KnobChoice>(node, kUSDSetAttributeParamValueType);
    ASSERT_TRUE(bool(type));
    type->setValueFromID("color3f", 0);
    EXPECT_EQ(std::string("color3f"), getBuiltType(node));
    KnobColorPtr color = getKnob<KnobColor>(node, kUSDSetAttributeParamValue);
    ASSERT_TRUE(bool(color));
    EXPECT_EQ(3, color->getDimension());

    type->setValueFromID("token", 0);
    EXPECT_TRUE(bool(getKnob<KnobString>(node, kUSDSetAttributeParamValue)));
    type->setValueFromID("asset", 0);
    EXPECT_TRUE(bool(getKnob<KnobFile>(node, kUSDSetAttributeParamValue)));
    type->setValueFromID("int", 0);
    EXPECT_TRUE(bool(getKnob<KnobInt>(node, kUSDSetAttributeParamValue)));
}

TEST_F(SetAttribute3DTest, APrimvarIsAConstantOneElementArray)
{
    NodePtr node = createSetAttribute("/World/Box", "displayColor", "color3f", true);
    ASSERT_TRUE(bool(node));
    KnobColorPtr color = getKnob<KnobColor>(node, kUSDSetAttributeParamValue);
    ASSERT_TRUE(bool(color));
    color->setValue(0.25, ViewSpec::all(), 0);
    color->setValue(0.5, ViewSpec::all(), 1);
    color->setValue(0.75, ViewSpec::all(), 2);

    ScenePayload payload;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, &payload));
    PXR_NS::UsdStageRefPtr stage = SceneOps::compose(payload);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdGeomPrimvar primvar = PXR_NS::UsdGeomPrimvarsAPI(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Box"))).GetPrimvar(PXR_NS::TfToken("displayColor"));
    ASSERT_TRUE(bool(primvar));
    EXPECT_EQ(PXR_NS::UsdGeomTokens->constant, primvar.GetInterpolation());
    EXPECT_EQ(PXR_NS::SdfValueTypeNames->Color3fArray, primvar.GetTypeName());
    PXR_NS::VtVec3fArray values;
    ASSERT_TRUE(primvar.Get(&values));
    ASSERT_EQ(1u, values.size());
    EXPECT_FLOAT_EQ(0.25f, values[0][0]);
    EXPECT_FLOAT_EQ(0.5f, values[0][1]);
    EXPECT_FLOAT_EQ(0.75f, values[0][2]);

    // Only the primvar is authored.
    PXR_NS::SdfLayerRefPtr layer = getTopLayer(payload);
    ASSERT_TRUE(bool(layer));
    PXR_NS::SdfPrimSpecHandle spec = layer->GetPrimAtPath(PXR_NS::SdfPath("/World/Box"));
    ASSERT_TRUE(bool(spec));
    EXPECT_EQ(PXR_NS::SdfSpecifierOver, spec->GetSpecifier());
    EXPECT_EQ(1u, spec->GetProperties().size());
}

TEST_F(SetAttribute3DTest, ASchemaAttributeKeepsItsType)
{
    // Cube's size is a double.
    NodePtr node = createSetAttribute("/World/Box", "size", "float", false);
    ASSERT_TRUE(bool(node));
    ScenePayload failed;
    EXPECT_NE(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, &failed));
    ASSERT_TRUE(node->hasPersistentMessage());

    getKnob<KnobChoice>(node, kUSDSetAttributeParamValueType)->setValueFromID("double", 0);
    KnobDoublePtr value = getKnob<KnobDouble>(node, kUSDSetAttributeParamValue);
    ASSERT_TRUE(bool(value));
    value->setValue(4.);
    ScenePayload payload;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, &payload));
    EXPECT_FALSE(node->hasPersistentMessage());
    PXR_NS::SdfAttributeSpecHandle spec = getTopLayer(payload)->GetAttributeAtPath(PXR_NS::SdfPath("/World/Box.size"));
    ASSERT_TRUE(bool(spec));
    EXPECT_FALSE(spec->IsCustom());
    PXR_NS::UsdStageRefPtr stage = SceneOps::compose(payload);
    ASSERT_TRUE(bool(stage));
    double size = 0.;
    ASSERT_TRUE(PXR_NS::UsdGeomCube(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Box"))).GetSizeAttr().Get(&size));
    EXPECT_DOUBLE_EQ(4., size);
}

TEST_F(SetAttribute3DTest, AnAnimatedValueIsTimeSampled)
{
    NodePtr node = createSetAttribute("/World/Ball", "userProperties:weight", "float", false);
    ASSERT_TRUE(bool(node));
    KnobDoublePtr value = getKnob<KnobDouble>(node, kUSDSetAttributeParamValue);
    ASSERT_TRUE(bool(value));
    double first = 0., last = 0.;
    getApp()->getFrameRange(&first, &last);
    ASSERT_LE(first, 1.);
    ASSERT_GE(last, 11.);
    value->setValueAtTime(1., 1., ViewSpec::all(), 0);
    value->setValueAtTime(11., 3., ViewSpec::all(), 0);

    ScenePayload payload;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, &payload));
    PXR_NS::UsdStageRefPtr stage = SceneOps::compose(payload);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdAttribute attribute = stage->GetAttributeAtPath(PXR_NS::SdfPath("/World/Ball.userProperties:weight"));
    ASSERT_TRUE(bool(attribute));
    EXPECT_TRUE(attribute.IsCustom());
    EXPECT_EQ((std::size_t)(std::floor(last) - std::ceil(first) + 1.), attribute.GetNumTimeSamples());
    float weight = 0.f;
    ASSERT_TRUE(attribute.Get(&weight, PXR_NS::UsdTimeCode(11.)));
    EXPECT_FLOAT_EQ(3.f, weight);
}

TEST_F(SetAttribute3DTest, ACustomAttributeRoundTripsThroughUSDWrite)
{
    NodePtr node = createSetAttribute("/World/Box", "userProperties:weight", "float", false);
    ASSERT_TRUE(bool(node));
    KnobDoublePtr value = getKnob<KnobDouble>(node, kUSDSetAttributeParamValue);
    ASSERT_TRUE(bool(value));
    value->setValue(2.5);

    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string path = (tmp.path() + QString::fromUtf8("/set-attribute.usda")).toStdString();
    NodePtr writer = createNode(QString::fromUtf8(PLUGINID_NATRON_USDWRITE));
    ASSERT_TRUE(bool(writer));
    ASSERT_TRUE(writer->connectInput(node, 0));
    KnobOutputFilePtr file = getKnob<KnobOutputFile>(writer, kUSDWriteParamFile);
    KnobChoicePtr exportMode = getKnob<KnobChoice>(writer, kUSDWriteParamExportMode);
    KnobChoicePtr frameRange = getKnob<KnobChoice>(writer, kUSDWriteParamFrameRange);
    KnobIntPtr firstFrame = getKnob<KnobInt>(writer, kUSDWriteParamFirstFrame);
    KnobIntPtr lastFrame = getKnob<KnobInt>(writer, kUSDWriteParamLastFrame);
    ASSERT_TRUE(file && exportMode && frameRange && firstFrame && lastFrame);
    file->setValue(path);
    exportMode->setValue((int)USDWrite::eExportModeFlattened);
    frameRange->setValue((int)USDWrite::eFrameRangeManual);
    firstFrame->setValue(1);
    lastFrame->setValue(1);
    USDWrite* write = dynamic_cast<USDWrite*>(writer->getEffectInstance().get());
    ASSERT_TRUE(write != nullptr);
    std::list<AppInstance::RenderWork> works;
    works.push_back(AppInstance::RenderWork(write, std::numeric_limits<int>::min(), std::numeric_limits<int>::max(),
                                            std::numeric_limits<int>::min(), false));
    getApp()->startWritersRendering(eRenderModeBlocking, works);
    ASSERT_EQ((U64)1, write->getWriteCount());

    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(path);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdAttribute attribute = stage->GetAttributeAtPath(PXR_NS::SdfPath("/World/Box.userProperties:weight"));
    ASSERT_TRUE(bool(attribute));
    EXPECT_TRUE(attribute.IsCustom());
    EXPECT_EQ(PXR_NS::SdfValueTypeNames->Float, attribute.GetTypeName());
    float weight = 0.f;
    ASSERT_TRUE(attribute.Get(&weight));
    EXPECT_FLOAT_EQ(2.5f, weight);
}

TEST_F(SetAttribute3DTest, TheTypeAndValueSurviveSaveAndLoad)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_USDSETATTRIBUTE));
    ASSERT_TRUE(bool(node));
    const std::string name = node->getScriptName();
    getKnob<KnobChoice>(node, kUSDSetAttributeParamValueType)->setValueFromID("double3", 0);
    KnobDoublePtr value = getKnob<KnobDouble>(node, kUSDSetAttributeParamValue);
    ASSERT_TRUE(bool(value));
    ASSERT_EQ(3, value->getDimension());
    value->setValue(7., ViewSpec::all(), 2);

    ProjectPtr project = getApp()->getProject();
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dirPath = tmp.path() + QLatin1Char('/');
    const QString fileName = QString::fromUtf8("set-attribute.ntpz");
    QString savedFilePath;
    ASSERT_TRUE(project->saveProject(dirPath, fileName, &savedFilePath));

    node.reset();
    value.reset();
    project->reset(false, true);
    ASSERT_TRUE(project->loadProject(dirPath, fileName));

    NodePtr loaded = project->getNodeByName(name);
    ASSERT_TRUE(bool(loaded));
    EXPECT_EQ(std::string("double3"), getBuiltType(loaded));
    KnobDoublePtr loadedValue = getKnob<KnobDouble>(loaded, kUSDSetAttributeParamValue);
    ASSERT_TRUE(bool(loadedValue));
    ASSERT_EQ(3, loadedValue->getDimension());
    EXPECT_DOUBLE_EQ(7., loadedValue->getValue(2));
}
