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

#include <cmath>
#include <string>

#include <gtest/gtest.h>

#include <QFile>
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
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDCamera.h"
#include "Engine/Nodes/Scene/USDPrim.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
KnobDouble*
getDoubleKnob(const NodePtr& node,
              const char* name)
{
    return dynamic_cast<KnobDouble*>(node->getKnobByName(name).get());
}

KnobChoice*
getChoiceKnob(const NodePtr& node,
              const char* name)
{
    return dynamic_cast<KnobChoice*>(node->getKnobByName(name).get());
}

// The prim path a node authors by default: "/" and its script name.
PXR_NS::SdfPath
getNodePath(const NodePtr& node)
{
    return PXR_NS::SdfPath("/" + node->getScriptName_mt_safe());
}

float
getFloat(const PXR_NS::UsdAttribute& attribute,
         PXR_NS::UsdTimeCode time = PXR_NS::UsdTimeCode::Default())
{
    float value = -1.f;

    EXPECT_TRUE(attribute.Get(&value, time)) << attribute.GetPath().GetString();

    return value;
}
} // anonymous namespace

class USDCameraTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        SceneQueryPool::instance().releaseIdle();

        BaseTest::TearDown();
    }

    static EffectInstance::RenderRoIRetCode evaluate(const NodePtr& node,
                                                     ScenePayload* payload,
                                                     std::string* error)
    {
        return node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), payload, error);
    }

    // The node's composed stage, or null after a failed ASSERT.
    static PXR_NS::UsdStageRefPtr composeNode(const NodePtr& node)
    {
        ScenePayload payload;
        std::string error;

        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, &payload, &error)) << error;

        return SceneOps::compose(payload);
    }
};

TEST_F(USDCameraTest, DefaultsAuthorAFiftyMillimetreCameraInTenthsOfASceneUnit)
{
    NodePtr camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
    ASSERT_TRUE(bool(camera));
    EXPECT_TRUE(camera->getEffectInstance()->producesSceneData());

    PXR_NS::UsdStageRefPtr stage = composeNode(camera);
    ASSERT_TRUE(bool(stage));
    PXR_NS::UsdGeomCamera usdCamera(stage->GetPrimAtPath(getNodePath(camera)));
    ASSERT_TRUE(bool(usdCamera));
    EXPECT_EQ(PXR_NS::TfToken("Camera"), usdCamera.GetPrim().GetTypeName());

    // No input: the stage is the default centimetre scene, where a millimetre is 1 / 10 of a
    // scene unit and USD's camera unit (a tenth of a scene unit) is that millimetre.
    EXPECT_FLOAT_EQ(50.f, getFloat(usdCamera.GetFocalLengthAttr()));
    EXPECT_FLOAT_EQ(36.f, getFloat(usdCamera.GetHorizontalApertureAttr()));
    EXPECT_FLOAT_EQ(24.f, getFloat(usdCamera.GetVerticalApertureAttr()));
    EXPECT_FLOAT_EQ(0.f, getFloat(usdCamera.GetHorizontalApertureOffsetAttr()));
    EXPECT_FLOAT_EQ(0.f, getFloat(usdCamera.GetVerticalApertureOffsetAttr()));
    PXR_NS::GfVec2f clipping;
    ASSERT_TRUE(usdCamera.GetClippingRangeAttr().Get(&clipping));
    EXPECT_FLOAT_EQ(0.1f, clipping[0]);
    EXPECT_FLOAT_EQ(10000.f, clipping[1]);
    PXR_NS::TfToken projection;
    ASSERT_TRUE(usdCamera.GetProjectionAttr().Get(&projection));
    EXPECT_EQ(PXR_NS::UsdGeomTokens->perspective, projection);
    // Not animated, so nothing is sampled.
    EXPECT_EQ((std::size_t)0, usdCamera.GetFocalLengthAttr().GetNumTimeSamples());
}

TEST_F(USDCameraTest, LensKnobsAreConvertedToTheInputScenesUnits)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString path = dir.filePath(QString::fromUtf8("metres.usda"));
    {
        QFile f(path);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("#usda 1.0\n(\n    metersPerUnit = 1\n)\n\ndef Xform \"World\"\n{\n}\n");
    }
    NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
    NodePtr camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
    ASSERT_TRUE(reader && camera);
    KnobFile* file = dynamic_cast<KnobFile*>(reader->getKnobByName(kUSDReadParamFile).get());
    ASSERT_TRUE(file != NULL);
    file->setValue(path.toStdString());
    ASSERT_TRUE(camera->connectInput(reader, 0));

    PXR_NS::UsdStageRefPtr stage = composeNode(camera);
    ASSERT_TRUE(bool(stage));
    EXPECT_DOUBLE_EQ(1., PXR_NS::UsdGeomGetStageMetersPerUnit(stage));
    EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/World")).IsValid());
    PXR_NS::UsdGeomCamera usdCamera(stage->GetPrimAtPath(getNodePath(camera)));
    ASSERT_TRUE(bool(usdCamera));
    // In a metre scene a tenth of a scene unit is 100 mm.
    EXPECT_FLOAT_EQ(0.5f, getFloat(usdCamera.GetFocalLengthAttr()));
    EXPECT_FLOAT_EQ(0.36f, getFloat(usdCamera.GetHorizontalApertureAttr()));
    EXPECT_FLOAT_EQ(0.24f, getFloat(usdCamera.GetVerticalApertureAttr()));
    // Clipping is in scene units already.
    PXR_NS::GfVec2f clipping;
    ASSERT_TRUE(usdCamera.GetClippingRangeAttr().Get(&clipping));
    EXPECT_FLOAT_EQ(0.1f, clipping[0]);
}

TEST_F(USDCameraTest, AnimatedKnobsAuthorATimeSampleAtEveryProjectFrame)
{
    NodePtr camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
    ASSERT_TRUE(bool(camera));
    KnobDouble* focalLength = getDoubleKnob(camera, kUSDCameraParamFocalLength);
    ASSERT_TRUE(focalLength != NULL);
    double first = 0., last = 0.;
    getApp()->getFrameRange(&first, &last);
    ASSERT_LE(first, 1.);
    ASSERT_GE(last, 11.);
    focalLength->setValueAtTime(1., 35., ViewSpec::all(), 0);
    focalLength->setValueAtTime(11., 85., ViewSpec::all(), 0);

    PXR_NS::UsdStageRefPtr stage = composeNode(camera);
    ASSERT_TRUE(bool(stage));
    PXR_NS::UsdGeomCamera usdCamera(stage->GetPrimAtPath(getNodePath(camera)));
    ASSERT_TRUE(bool(usdCamera));
    const PXR_NS::UsdAttribute attribute = usdCamera.GetFocalLengthAttr();
    EXPECT_EQ((std::size_t)(std::floor(last) - std::ceil(first) + 1.), attribute.GetNumTimeSamples());
    // With no input the layer runs at the project rate, so timecodes are frames.
    EXPECT_FLOAT_EQ(35.f, getFloat(attribute, PXR_NS::UsdTimeCode(1.)));
    EXPECT_FLOAT_EQ((float)focalLength->getValueAtTime(6.), getFloat(attribute, PXR_NS::UsdTimeCode(6.)));
    EXPECT_FLOAT_EQ(85.f, getFloat(attribute, PXR_NS::UsdTimeCode(11.)));
    // A knob left alone is still a single default value.
    EXPECT_EQ((std::size_t)0, usdCamera.GetHorizontalApertureAttr().GetNumTimeSamples());
}

TEST_F(USDCameraTest, TheComposedCameraMatchesTheKnobs)
{
    NodePtr camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
    ASSERT_TRUE(bool(camera));
    KnobDouble* focalLength = getDoubleKnob(camera, kUSDCameraParamFocalLength);
    KnobDouble* nearKnob = getDoubleKnob(camera, kUSDCameraParamNear);
    KnobDouble* farKnob = getDoubleKnob(camera, kUSDCameraParamFar);
    KnobDouble* translate = getDoubleKnob(camera, kSceneParamTranslate);
    KnobDouble* rotate = getDoubleKnob(camera, kSceneParamRotate);
    ASSERT_TRUE(focalLength && nearKnob && farKnob && translate && rotate);
    focalLength->setValue(25.);
    nearKnob->setValue(1.);
    farKnob->setValue(500.);
    translate->setValue(10., ViewSpec::all(), 0);
    translate->setValue(20., ViewSpec::all(), 1);
    translate->setValue(30., ViewSpec::all(), 2);
    rotate->setValue(90., ViewSpec::all(), 1);

    PXR_NS::UsdStageRefPtr stage = composeNode(camera);
    ASSERT_TRUE(bool(stage));
    PXR_NS::UsdGeomCamera usdCamera(stage->GetPrimAtPath(getNodePath(camera)));
    ASSERT_TRUE(bool(usdCamera));

    // What a look-through reads: the GfCamera at a time.
    const PXR_NS::GfCamera gfCamera = usdCamera.GetCamera(PXR_NS::UsdTimeCode(1.));
    EXPECT_FLOAT_EQ(25.f, gfCamera.GetFocalLength());
    EXPECT_FLOAT_EQ(36.f, gfCamera.GetHorizontalAperture());
    EXPECT_FLOAT_EQ(24.f, gfCamera.GetVerticalAperture());
    EXPECT_NEAR(2. * std::atan(18. / 25.) * 180. / 3.14159265358979323846, gfCamera.GetFieldOfView(PXR_NS::GfCamera::FOVHorizontal), 1e-3);
    EXPECT_FLOAT_EQ(1.f, gfCamera.GetClippingRange().GetMin());
    EXPECT_FLOAT_EQ(500.f, gfCamera.GetClippingRange().GetMax());

    const PXR_NS::GfFrustum frustum = gfCamera.GetFrustum();
    const PXR_NS::GfVec3d position = frustum.GetPosition();
    EXPECT_NEAR(10., position[0], 1e-9);
    EXPECT_NEAR(20., position[1], 1e-9);
    EXPECT_NEAR(30., position[2], 1e-9);
    // A camera looks down -Z; 90 degrees about Y turns that to -X.
    const PXR_NS::GfVec3d view = frustum.ComputeViewDirection();
    EXPECT_NEAR(-1., view[0], 1e-6);
    EXPECT_NEAR(0., view[1], 1e-6);
    EXPECT_NEAR(0., view[2], 1e-6);
}

TEST_F(USDCameraTest, ARelativePrimPathFailsWithANodeError)
{
    NodePtr camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
    ASSERT_TRUE(bool(camera));
    KnobString* primPath = dynamic_cast<KnobString*>(camera->getKnobByName(kSceneParamPrimPath).get());
    ASSERT_TRUE(primPath != NULL);
    primPath->setValue(std::string("Camera"));

    ScenePayload payload;
    std::string error;
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeFailed, evaluate(camera, &payload, &error));
    EXPECT_NE(std::string::npos, error.find("absolute prim path")) << error;
    ASSERT_TRUE(camera->hasPersistentMessage());
    QString message;
    int type = 0;
    camera->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeError, type);

    // A valid path clears it.
    primPath->setValue(std::string("/Rig/Camera"));
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(camera, &payload, &error)) << error;
    EXPECT_FALSE(camera->hasPersistentMessage());
    PXR_NS::UsdStageRefPtr stage = SceneOps::compose(payload);
    ASSERT_TRUE(bool(stage));
    EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/Rig/Camera")).IsValid());
}

TEST_F(USDCameraTest, PrimAuthorsAnXformWithItsTransformAndKind)
{
    NodePtr prim = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPRIM));
    ASSERT_TRUE(bool(prim));
    KnobDouble* translate = getDoubleKnob(prim, kSceneParamTranslate);
    KnobDouble* uniformScale = getDoubleKnob(prim, kSceneParamUniformScale);
    KnobChoice* kind = getChoiceKnob(prim, kUSDPrimParamKind);
    ASSERT_TRUE(translate && uniformScale && kind);
    translate->setValue(5., ViewSpec::all(), 1);
    uniformScale->setValue(2.);
    kind->setValueFromID("component", 0);

    PXR_NS::UsdStageRefPtr stage = composeNode(prim);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdPrim usdPrim = stage->GetPrimAtPath(getNodePath(prim));
    ASSERT_TRUE(usdPrim.IsValid());
    EXPECT_EQ(PXR_NS::TfToken("Xform"), usdPrim.GetTypeName());
    PXR_NS::TfToken kindToken;
    ASSERT_TRUE(PXR_NS::UsdModelAPI(usdPrim).GetKind(&kindToken));
    EXPECT_EQ(PXR_NS::KindTokens->component, kindToken);

    const PXR_NS::GfMatrix4d transform = PXR_NS::UsdGeomXformable(usdPrim).ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode::Default());
    EXPECT_NEAR(5., transform.ExtractTranslation()[1], 1e-9);
    EXPECT_NEAR(2., transform.TransformDir(PXR_NS::GfVec3d(1., 0., 0.)).GetLength(), 1e-6);
}

TEST_F(USDCameraTest, PrimAuthorsAScopeWithoutATransform)
{
    NodePtr prim = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPRIM));
    ASSERT_TRUE(bool(prim));
    KnobChoice* primType = getChoiceKnob(prim, kUSDPrimParamType);
    KnobDouble* translate = getDoubleKnob(prim, kSceneParamTranslate);
    ASSERT_TRUE(primType && translate);
    translate->setValue(5., ViewSpec::all(), 0);
    primType->setValueFromID("Scope", 0);
    // A Scope has no transform to edit.
    EXPECT_TRUE(translate->getIsSecret());

    PXR_NS::UsdStageRefPtr stage = composeNode(prim);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdPrim usdPrim = stage->GetPrimAtPath(getNodePath(prim));
    ASSERT_TRUE(usdPrim.IsValid());
    EXPECT_EQ(PXR_NS::TfToken("Scope"), usdPrim.GetTypeName());
    EXPECT_FALSE(usdPrim.GetAttribute(PXR_NS::UsdGeomTokens->xformOpOrder).HasAuthoredValue());
    PXR_NS::TfToken kindToken;
    EXPECT_FALSE(PXR_NS::UsdModelAPI(usdPrim).GetKind(&kindToken) && !kindToken.IsEmpty());
}

TEST_F(USDCameraTest, ThePrimPathFollowsTheNodeNameUntilEdited)
{
    NodePtr camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
    ASSERT_TRUE(bool(camera));
    KnobString* primPath = dynamic_cast<KnobString*>(camera->getKnobByName(kSceneParamPrimPath).get());
    ASSERT_TRUE(primPath != NULL);
    EXPECT_EQ("/" + camera->getScriptName_mt_safe(), primPath->getValue());
    EXPECT_EQ(0u, primPath->getValue().find("/USDCamera"));

    // A path still at its default follows a rename, and the stage follows the path.
    camera->setScriptName("Hero");
    EXPECT_EQ(std::string("/Hero"), primPath->getValue());
    EXPECT_EQ(std::string("/Hero"), primPath->getDefaultValue(0));
    PXR_NS::UsdStageRefPtr stage = composeNode(camera);
    ASSERT_TRUE(bool(stage));
    EXPECT_TRUE(bool(PXR_NS::UsdGeomCamera(stage->GetPrimAtPath(PXR_NS::SdfPath("/Hero")))));

    // An edited path stays; only the default moves.
    primPath->setValue(std::string("/Shot/Cam"));
    camera->setScriptName("Hero2");
    EXPECT_EQ(std::string("/Shot/Cam"), primPath->getValue());
    EXPECT_EQ(std::string("/Hero2"), primPath->getDefaultValue(0));

    // Each node of a kind gets its own default prim.
    NodePtr prim = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPRIM));
    NodePtr otherPrim = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPRIM));
    ASSERT_TRUE(prim && otherPrim);
    EXPECT_NE(getNodePath(prim), getNodePath(otherPrim));
    EXPECT_EQ(getNodePath(prim).GetString(), dynamic_cast<KnobString*>(prim->getKnobByName(kSceneParamPrimPath).get())->getValue());
}
