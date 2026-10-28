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
#include <cmath>
#include <filesystem>
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
#include "Engine/Nodes/Scene/USDXformPrim.h"
#include "Engine/ScenePayload.h"
#include "Engine/TimeLine.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
KnobDouble*
getDoubleKnob(const NodePtr& node,
              const char* name)
{
    return dynamic_cast<KnobDouble*>(node->getKnobByName(name).get());
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

// /Track/Cam: an animated camera under a translated, turned and scaled Xform, keyed at frames 1
// and 10 (focal length 0.35 then 0.5); /Witness: a still camera. Meters, Y up, 24 timecodes per
// second, as the project's defaults.
const char* const kTrackedCamera = NATRON_TESTS_FIXTURES_DIR "/scene-camera-tracked.usda";

std::string
readFile(const std::string& path)
{
    QFile f(QString::fromStdString(path));

    EXPECT_TRUE(f.open(QIODevice::ReadOnly)) << path;

    return f.readAll().toStdString();
}

void
writeFile(const std::string& path,
          const std::string& content)
{
    {
        QFile f(QString::fromStdString(path));

        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(content.c_str(), (qint64)content.size());
    }
    // Each write a second later than the last: USD reloads a layer only when its file's
    // modification time moved, and two writes can land in one clock tick.
    static int writes = 0;
    ++writes;
    std::error_code ec;
    std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(writes), ec);
    ASSERT_FALSE(ec) << ec.message();
}

std::string
replaceOnce(std::string text,
            const std::string& from,
            const std::string& to)
{
    const std::size_t at = text.find(from);

    EXPECT_NE(std::string::npos, at) << from;
    if (at != std::string::npos) {
        text.replace(at, from.size(), to);
    }

    return text;
}

void
expectSameMatrix(const PXR_NS::GfMatrix4d& expected,
                 const PXR_NS::GfMatrix4d& actual,
                 const char* what)
{
    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            EXPECT_NEAR(expected[row][column], actual[row][column], 1e-9) << what << " [" << row << "][" << column << "]";
        }
    }
}

// The camera at path of the file, read directly.
PXR_NS::UsdGeomCamera
getFileCamera(const PXR_NS::UsdStageRefPtr& stage,
              const char* path)
{
    return PXR_NS::UsdGeomCamera(stage->GetPrimAtPath(PXR_NS::SdfPath(path)));
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

    // A USDCamera reading its camera from the file at path.
    NodePtr createFileCamera(const std::string& path)
    {
        NodePtr camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));

        if (!camera) {
            return camera;
        }
        KnobFile* file = dynamic_cast<KnobFile*>(camera->getKnobByName(kUSDCameraParamFile).get());
        KnobBool* readFromFile = dynamic_cast<KnobBool*>(camera->getKnobByName(kUSDCameraParamReadFromFile).get());
        if (!file || !readFromFile) {
            return NodePtr();
        }
        file->setValue(path);
        readFromFile->setValue(true);

        return camera;
    }

    static void setToggle(const NodePtr& node,
                          const char* name,
                          bool value)
    {
        KnobBool* toggle = dynamic_cast<KnobBool*>(node->getKnobByName(name).get());

        ASSERT_TRUE(toggle != NULL) << name;
        toggle->setValue(value);
    }

    static void pressReload(const NodePtr& node)
    {
        KnobButton* reload = dynamic_cast<KnobButton*>(node->getKnobByName(kUSDCameraParamReload).get());

        ASSERT_TRUE(reload != NULL);
        reload->trigger();
    }

    static std::string getErrorMessage(const NodePtr& node)
    {
        QString message;
        int type = 0;

        node->getPersistentMessage(&message, &type, false);
        EXPECT_EQ((int)eMessageTypeError, type);

        return message.toStdString();
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

    // The project's default scene is in meters, where USD's camera unit (a tenth of a scene
    // unit) is 100 mm.
    EXPECT_FLOAT_EQ(0.5f, getFloat(usdCamera.GetFocalLengthAttr()));
    EXPECT_FLOAT_EQ(0.36f, getFloat(usdCamera.GetHorizontalApertureAttr()));
    EXPECT_FLOAT_EQ(0.24f, getFloat(usdCamera.GetVerticalApertureAttr()));
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

TEST_F(USDCameraTest, TheNodeIsASourceThatComposesOnlyItsOwnPrim)
{
    NodePtr camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
    ASSERT_TRUE(bool(camera));
    EXPECT_EQ(0, camera->getNInputs());
    EXPECT_EQ(0, camera->getEffectInstance()->getNInputs());

    PXR_NS::UsdStageRefPtr stage = composeNode(camera);
    ASSERT_TRUE(bool(stage));
    EXPECT_TRUE(stage->GetPrimAtPath(getNodePath(camera)).IsValid());
    EXPECT_EQ((std::size_t)1, stage->GetPseudoRoot().GetAllChildrenNames().size());
    EXPECT_TRUE(stage->GetPrimAtPath(getNodePath(camera)).GetAllChildrenNames().empty());
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
    // The layer runs at the project rate, so timecodes are frames.
    EXPECT_FLOAT_EQ(0.35f, getFloat(attribute, PXR_NS::UsdTimeCode(1.)));
    EXPECT_FLOAT_EQ((float)(focalLength->getValueAtTime(6.) * 0.01), getFloat(attribute, PXR_NS::UsdTimeCode(6.)));
    EXPECT_FLOAT_EQ(0.85f, getFloat(attribute, PXR_NS::UsdTimeCode(11.)));
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
    EXPECT_FLOAT_EQ(0.25f, gfCamera.GetFocalLength());
    EXPECT_FLOAT_EQ(0.36f, gfCamera.GetHorizontalAperture());
    EXPECT_FLOAT_EQ(0.24f, gfCamera.GetVerticalAperture());
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
    NodePtr prim = createNode(QString::fromUtf8(PLUGINID_NATRON_USDXFORMPRIM));
    NodePtr otherPrim = createNode(QString::fromUtf8(PLUGINID_NATRON_USDXFORMPRIM));
    ASSERT_TRUE(prim && otherPrim);
    EXPECT_NE(getNodePath(prim), getNodePath(otherPrim));
    EXPECT_EQ(getNodePath(prim).GetString(), dynamic_cast<KnobString*>(prim->getKnobByName(kSceneParamPrimPath).get())->getValue());
}

TEST_F(USDCameraTest, AFileCameraFollowsTheFilesWorldTransformAndLens)
{
    NodePtr camera = createFileCamera(kTrackedCamera);
    ASSERT_TRUE(bool(camera));
    double first = 0., last = 0.;
    getApp()->getFrameRange(&first, &last);
    ASSERT_LE(first, 1.);
    ASSERT_GE(last, 10.);

    // The menu lists the file's cameras, the first selected.
    KnobChoice* cameraPrim = dynamic_cast<KnobChoice*>(camera->getKnobByName(kUSDCameraParamCameraPrim).get());
    ASSERT_TRUE(cameraPrim != NULL);
    EXPECT_EQ(2, cameraPrim->getNumEntries());
    EXPECT_EQ(std::string("/Track/Cam"), cameraPrim->getActiveEntry().id);

    PXR_NS::UsdStageRefPtr stage = composeNode(camera);
    ASSERT_TRUE(bool(stage));
    EXPECT_FALSE(camera->hasPersistentMessage());
    PXR_NS::UsdGeomCamera usdCamera(stage->GetPrimAtPath(getNodePath(camera)));
    ASSERT_TRUE(bool(usdCamera));
    // Only the camera: its ancestors are flattened into one op.
    EXPECT_EQ((std::size_t)1, stage->GetPseudoRoot().GetAllChildrenNames().size());
    bool resetsXformStack = false;
    const std::vector<PXR_NS::UsdGeomXformOp> ops = usdCamera.GetOrderedXformOps(&resetsXformStack);
    ASSERT_EQ((std::size_t)1, ops.size());
    EXPECT_EQ(PXR_NS::UsdGeomXformOp::TypeTransform, ops[0].GetOpType());

    PXR_NS::UsdStageRefPtr fileStage = PXR_NS::UsdStage::Open(kTrackedCamera);
    ASSERT_TRUE(bool(fileStage));
    PXR_NS::UsdGeomCamera fileCamera = getFileCamera(fileStage, "/Track/Cam");
    ASSERT_TRUE(bool(fileCamera));
    // The project and the file both run at 24 frames per second, so frame f is timecode f in both.
    for (double frame : { 1., 10. }) {
        const PXR_NS::UsdTimeCode time(frame);
        expectSameMatrix(fileCamera.ComputeLocalToWorldTransform(time), usdCamera.ComputeLocalToWorldTransform(time), "world transform");
        EXPECT_FLOAT_EQ(getFloat(fileCamera.GetFocalLengthAttr(), time), getFloat(usdCamera.GetFocalLengthAttr(), time)) << frame;
    }
    EXPECT_FLOAT_EQ(0.35f, getFloat(usdCamera.GetFocalLengthAttr(), PXR_NS::UsdTimeCode(1.)));
    EXPECT_FLOAT_EQ(0.5f, getFloat(usdCamera.GetFocalLengthAttr(), PXR_NS::UsdTimeCode(10.)));
    // Still values stay single defaults.
    EXPECT_EQ((std::size_t)0, usdCamera.GetHorizontalApertureAttr().GetNumTimeSamples());
    EXPECT_FLOAT_EQ(0.36f, getFloat(usdCamera.GetHorizontalApertureAttr()));
    PXR_NS::GfVec2f clipping;
    ASSERT_TRUE(usdCamera.GetClippingRangeAttr().Get(&clipping));
    EXPECT_FLOAT_EQ(0.1f, clipping[0]);
    EXPECT_FLOAT_EQ(1000.f, clipping[1]);
}

TEST_F(USDCameraTest, TheFrameOffsetAndTheCameraPrimChooseWhatIsRead)
{
    NodePtr camera = createFileCamera(kTrackedCamera);
    ASSERT_TRUE(bool(camera));
    PXR_NS::UsdStageRefPtr fileStage = PXR_NS::UsdStage::Open(kTrackedCamera);
    ASSERT_TRUE(bool(fileStage));

    // Frame 1 reads the file at 1 + 9.
    KnobDouble* frameOffset = getDoubleKnob(camera, kUSDCameraParamFrameOffset);
    ASSERT_TRUE(frameOffset != NULL);
    frameOffset->setValue(9.);
    {
        PXR_NS::UsdStageRefPtr stage = composeNode(camera);
        ASSERT_TRUE(bool(stage));
        PXR_NS::UsdGeomCamera usdCamera(stage->GetPrimAtPath(getNodePath(camera)));
        ASSERT_TRUE(bool(usdCamera));
        expectSameMatrix(getFileCamera(fileStage, "/Track/Cam").ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode(10.)),
                         usdCamera.ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode(1.)), "offset world transform");
        EXPECT_FLOAT_EQ(0.5f, getFloat(usdCamera.GetFocalLengthAttr(), PXR_NS::UsdTimeCode(1.)));
    }

    KnobChoice* cameraPrim = dynamic_cast<KnobChoice*>(camera->getKnobByName(kUSDCameraParamCameraPrim).get());
    ASSERT_TRUE(cameraPrim != NULL);
    cameraPrim->setValueFromID("/Witness", 0);
    PXR_NS::UsdStageRefPtr stage = composeNode(camera);
    ASSERT_TRUE(bool(stage));
    PXR_NS::UsdGeomCamera usdCamera(stage->GetPrimAtPath(getNodePath(camera)));
    ASSERT_TRUE(bool(usdCamera));
    EXPECT_FLOAT_EQ(0.85f, getFloat(usdCamera.GetFocalLengthAttr()));
    const PXR_NS::GfVec3d position = usdCamera.ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode(1.)).ExtractTranslation();
    EXPECT_NEAR(0., position[0], 1e-9);
    EXPECT_NEAR(1., position[1], 1e-9);
    EXPECT_NEAR(20., position[2], 1e-9);
}

TEST_F(USDCameraTest, AMissingFileFailsWithANodeError)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("missing.usda")).toStdString();
    NodePtr camera = createFileCamera(path);
    ASSERT_TRUE(bool(camera));

    ScenePayload payload;
    std::string error;
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeFailed, evaluate(camera, &payload, &error));
    EXPECT_NE(std::string::npos, error.find("does not exist")) << error;
    ASSERT_TRUE(camera->hasPersistentMessage());
    EXPECT_NE(std::string::npos, getErrorMessage(camera).find("missing.usda"));
}

TEST_F(USDCameraTest, ReloadFollowsTheRewrittenFileAndAMissingCameraErrors)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("tracked.usda")).toStdString();
    const std::string original = readFile(kTrackedCamera);
    ASSERT_FALSE(original.empty());
    writeFile(path, original);
    NodePtr camera = createFileCamera(path);
    ASSERT_TRUE(bool(camera));
    {
        PXR_NS::UsdStageRefPtr stage = composeNode(camera);
        ASSERT_TRUE(bool(stage));
        EXPECT_FLOAT_EQ(0.5f, getFloat(PXR_NS::UsdGeomCamera(stage->GetPrimAtPath(getNodePath(camera))).GetFocalLengthAttr(), PXR_NS::UsdTimeCode(10.)));
    }

    writeFile(path, replaceOnce(original, "10: 0.5,", "10: 0.7,"));
    pressReload(camera);
    {
        PXR_NS::UsdStageRefPtr stage = composeNode(camera);
        ASSERT_TRUE(bool(stage));
        EXPECT_FLOAT_EQ(0.7f, getFloat(PXR_NS::UsdGeomCamera(stage->GetPrimAtPath(getNodePath(camera))).GetFocalLengthAttr(), PXR_NS::UsdTimeCode(10.)));
    }

    // The selected camera stays selected when it is gone, so the node reports it.
    writeFile(path, replaceOnce(original, "def Camera \"Cam\"", "def Camera \"Shot\""));
    pressReload(camera);
    ScenePayload payload;
    std::string error;
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeFailed, evaluate(camera, &payload, &error));
    EXPECT_NE(std::string::npos, error.find("/Track/Cam")) << error;
    ASSERT_TRUE(camera->hasPersistentMessage());
    EXPECT_NE(std::string::npos, getErrorMessage(camera).find("/Track/Cam"));
}

TEST_F(USDCameraTest, TheKnobsShowTheFilesValuesUntilAGroupIsEdited)
{
    NodePtr camera = createFileCamera(kTrackedCamera);
    ASSERT_TRUE(bool(camera));
    const double frame = getApp()->getTimeLine()->currentFrame();
    PXR_NS::UsdStageRefPtr fileStage = PXR_NS::UsdStage::Open(kTrackedCamera);
    ASSERT_TRUE(bool(fileStage));
    PXR_NS::UsdGeomCamera fileCamera = getFileCamera(fileStage, "/Track/Cam");
    ASSERT_TRUE(bool(fileCamera));
    // Meters, so a camera unit is 100 mm.
    const double fileFocal = 100. * getFloat(fileCamera.GetFocalLengthAttr(), PXR_NS::UsdTimeCode(frame));

    KnobDouble* focalLength = getDoubleKnob(camera, kUSDCameraParamFocalLength);
    KnobDouble* translate = getDoubleKnob(camera, kSceneParamTranslate);
    KnobDouble* farKnob = getDoubleKnob(camera, kUSDCameraParamFar);
    ASSERT_TRUE(focalLength && translate && farKnob);
    EXPECT_NEAR(fileFocal, focalLength->getValue(), 1e-4);
    EXPECT_NEAR(1000., farKnob->getValue(), 1e-3);
    const PXR_NS::GfVec3d position = fileCamera.ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode(frame)).ExtractTranslation();
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(position[i], translate->getValue(i), 1e-6) << i;
    }
    // Shown, not edited: disabled and keying nothing.
    EXPECT_FALSE(focalLength->isEnabled(0));
    EXPECT_TRUE(focalLength->isHashExempt());
    EXPECT_FALSE(translate->isEnabled(0));

    // Edit starts from the file's value; off restores it.
    setToggle(camera, kUSDCameraParamEditLens, true);
    EXPECT_TRUE(focalLength->isEnabled(0));
    EXPECT_FALSE(focalLength->isHashExempt());
    EXPECT_NEAR(fileFocal, focalLength->getValue(), 1e-4);
    focalLength->setValue(20.);
    setToggle(camera, kUSDCameraParamEditLens, false);
    EXPECT_FALSE(focalLength->isEnabled(0));
    EXPECT_NEAR(fileFocal, focalLength->getValue(), 1e-4);

    // Off the file, every knob is the node's own again.
    setToggle(camera, kUSDCameraParamReadFromFile, false);
    EXPECT_TRUE(focalLength->isEnabled(0));
    EXPECT_FALSE(focalLength->isHashExempt());
    EXPECT_TRUE(translate->isEnabled(0));
}

TEST_F(USDCameraTest, EditingTheLensOverridesOnlyTheLens)
{
    NodePtr camera = createFileCamera(kTrackedCamera);
    ASSERT_TRUE(bool(camera));
    setToggle(camera, kUSDCameraParamEditLens, true);
    KnobDouble* focalLength = getDoubleKnob(camera, kUSDCameraParamFocalLength);
    ASSERT_TRUE(focalLength != NULL);
    focalLength->setValue(35.);

    PXR_NS::UsdStageRefPtr stage = composeNode(camera);
    ASSERT_TRUE(bool(stage));
    PXR_NS::UsdGeomCamera usdCamera(stage->GetPrimAtPath(getNodePath(camera)));
    ASSERT_TRUE(bool(usdCamera));
    PXR_NS::UsdStageRefPtr fileStage = PXR_NS::UsdStage::Open(kTrackedCamera);
    ASSERT_TRUE(bool(fileStage));
    PXR_NS::UsdGeomCamera fileCamera = getFileCamera(fileStage, "/Track/Cam");
    ASSERT_TRUE(bool(fileCamera));

    // The file's focal length is 0.5 at frame 10; the knob's 35 mm replaces it at every frame.
    EXPECT_EQ((std::size_t)0, usdCamera.GetFocalLengthAttr().GetNumTimeSamples());
    EXPECT_FLOAT_EQ(0.35f, getFloat(usdCamera.GetFocalLengthAttr(), PXR_NS::UsdTimeCode(10.)));
    EXPECT_FLOAT_EQ(0.36f, getFloat(usdCamera.GetHorizontalApertureAttr(), PXR_NS::UsdTimeCode(10.)));
    // The clipping and the transform still come from the file.
    PXR_NS::GfVec2f clipping;
    ASSERT_TRUE(usdCamera.GetClippingRangeAttr().Get(&clipping));
    EXPECT_FLOAT_EQ(1000.f, clipping[1]);
    for (double frame : { 1., 10. }) {
        const PXR_NS::UsdTimeCode time(frame);
        expectSameMatrix(fileCamera.ComputeLocalToWorldTransform(time), usdCamera.ComputeLocalToWorldTransform(time), "world transform");
    }
}

TEST_F(USDCameraTest, EditingTheTransformMovesTheFileCameraInItsOwnSpace)
{
    NodePtr camera = createFileCamera(kTrackedCamera);
    ASSERT_TRUE(bool(camera));
    setToggle(camera, kUSDCameraParamEditTransform, true);
    KnobDouble* translate = getDoubleKnob(camera, kSceneParamTranslate);
    ASSERT_TRUE(translate != NULL);
    // The knobs start at no change, not at the file's transform they showed.
    for (int i = 0; i < 3; ++i) {
        EXPECT_DOUBLE_EQ(0., translate->getValue(i)) << i;
    }
    EXPECT_TRUE(translate->isEnabled(0));
    // One unit back along the camera's own Z.
    translate->setValue(1., ViewSpec::all(), 2);

    PXR_NS::UsdStageRefPtr stage = composeNode(camera);
    ASSERT_TRUE(bool(stage));
    PXR_NS::UsdGeomCamera usdCamera(stage->GetPrimAtPath(getNodePath(camera)));
    ASSERT_TRUE(bool(usdCamera));
    PXR_NS::UsdStageRefPtr fileStage = PXR_NS::UsdStage::Open(kTrackedCamera);
    ASSERT_TRUE(bool(fileStage));
    PXR_NS::UsdGeomCamera fileCamera = getFileCamera(fileStage, "/Track/Cam");
    ASSERT_TRUE(bool(fileCamera));
    PXR_NS::GfMatrix4d offset(1.);
    offset.SetTranslate(PXR_NS::GfVec3d(0., 0., 1.));
    for (double frame : { 1., 10. }) {
        const PXR_NS::UsdTimeCode time(frame);
        // Gf matrices multiply row vectors: the offset applies first, in the camera's space.
        expectSameMatrix(offset * fileCamera.ComputeLocalToWorldTransform(time), usdCamera.ComputeLocalToWorldTransform(time), "offset world transform");
    }
}
