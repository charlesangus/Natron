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

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/ScenePayloadImpl.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDCamera.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Project.h"
#include "Engine/ScenePayload.h"
#include "Engine/UndoCommand.h"
#include "Engine/UndoRecorder.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
// The upAxis the layer authors on its pseudo-root, empty if none.
PXR_NS::TfToken
getLayerUpAxis(const PXR_NS::SdfLayerRefPtr& layer)
{
    PXR_NS::TfToken upAxis;

    if (layer) {
        layer->HasField(PXR_NS::SdfPath::AbsoluteRootPath(), PXR_NS::UsdGeomTokens->upAxis, &upAxis);
    }

    return upAxis;
}

// The metersPerUnit the layer authors on its pseudo-root, 0 if none.
double
getLayerMetersPerUnit(const PXR_NS::SdfLayerRefPtr& layer)
{
    double metersPerUnit = 0.;

    if (layer) {
        layer->HasField(PXR_NS::SdfPath::AbsoluteRootPath(), PXR_NS::UsdGeomTokens->metersPerUnit, &metersPerUnit);
    }

    return metersPerUnit;
}

// The layer the payload's last node authored.
PXR_NS::SdfLayerRefPtr
getTopLayer(const ScenePayload& payload)
{
    return payload.getImpl() ? payload.getImpl()->top.layer : PXR_NS::SdfLayerRefPtr();
}

void
writeFile(const std::string& path,
          const std::string& content)
{
    QFile f(QString::fromStdString(path));

    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(content.c_str(), (qint64)content.size());
}

// A layer defining one Xform, with the given stage metadata.
std::string
sceneWithUnits(const std::string& upAxis,
               double metersPerUnit)
{
    return "#usda 1.0\n(\n    upAxis = \"" + upAxis + "\"\n    metersPerUnit = " + std::to_string(metersPerUnit) + "\n)\n\ndef Xform \"World\"\n{\n}\n";
}

// A Z-up centimetre file: an Xform root holding a mesh from (0, 0, 0) to (100, 200, 300).
const char* const kZUpCentimetres = "#usda 1.0\n"
                                    "(\n"
                                    "    upAxis = \"Z\"\n"
                                    "    metersPerUnit = 0.01\n"
                                    ")\n"
                                    "def Xform \"World\"\n"
                                    "{\n"
                                    "    def Mesh \"Box\"\n"
                                    "    {\n"
                                    "        point3f[] points = [(0, 0, 0), (100, 200, 300)]\n"
                                    "        int[] faceVertexCounts = [2]\n"
                                    "        int[] faceVertexIndices = [0, 1]\n"
                                    "    }\n"
                                    "}\n";

const char* const kUpAxisOp = "xformOp:rotateX:upAxisCorrection";
const char* const kUnitsOp = "xformOp:scale:unitsCorrection";

// Whether the payload's top layer authors the attribute on /World.
bool
topLayerHasWorldAttribute(const ScenePayload& payload,
                          const std::string& name)
{
    const PXR_NS::SdfLayerRefPtr layer = getTopLayer(payload);

    return layer && bool(layer->GetAttributeAtPath(PXR_NS::SdfPath("/World." + name)));
}

PXR_NS::GfRange3d
getWorldBounds(const ScenePayload& payload,
               const std::string& path)
{
    PXR_NS::UsdStageRefPtr stage = SceneOps::compose(payload);

    if (!stage) {
        return PXR_NS::GfRange3d();
    }
    PXR_NS::UsdGeomBBoxCache cache(PXR_NS::UsdTimeCode::Default(), { PXR_NS::UsdGeomTokens->default_ });

    return cache.ComputeWorldBound(stage->GetPrimAtPath(PXR_NS::SdfPath(path))).ComputeAlignedRange();
}

void
expectNear(const PXR_NS::GfVec3d& expected,
           const PXR_NS::GfVec3d& actual)
{
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(expected[i], actual[i], 1e-6) << "component " << i;
    }
}

// The node's persistent message, empty if it has none.
std::string
getMessage(const NodePtr& node)
{
    if (!node->hasPersistentMessage()) {
        return std::string();
    }
    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);

    return message.toStdString();
}

bool
contains(const std::string& text,
         const char* part)
{
    return text.find(part) != std::string::npos;
}

// Keeps what is recorded, the way the window's history would within one step.
class CollectingRecorder
    : public UndoRecorderI {
public:
    CollectingRecorder()
        : UndoRecorderI()
        , depth(0)
        , serial(0)
        , commands()
    {
    }

    virtual void openStep(const std::string& /*text*/) OVERRIDE FINAL
    {
        if (depth++ == 0) {
            ++serial;
        }
    }

    virtual void closeStep() OVERRIDE FINAL
    {
        if (depth > 0) {
            --depth;
        }
    }

    virtual bool isCapturing() const OVERRIDE FINAL
    {
        return depth > 0;
    }

    virtual std::uint64_t getCaptureSerial() const OVERRIDE FINAL
    {
        return serial;
    }

    virtual void record(const UndoCommandPtr& command,
                        const UndoTouched& /*touched*/) OVERRIDE FINAL
    {
        commands.push_back(command);
    }

    int depth;
    std::uint64_t serial;
    std::vector<UndoCommandPtr> commands;
};
} // anonymous namespace

class SceneUpAxisTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        SceneQueryPool::instance().releaseIdle();

        BaseTest::TearDown();
    }

    void setProjectUpAxisZ(bool z)
    {
        KnobChoice* upAxis = dynamic_cast<KnobChoice*>(getApp()->getProject()->getKnobByName("upAxis").get());

        ASSERT_TRUE(upAxis != NULL);
        upAxis->setValue(z ? 1 : 0);
        ASSERT_EQ(z, getApp()->getProject()->isProjectUpAxisZ());
    }

    void setProjectMetersPerUnit(double metersPerUnit)
    {
        KnobDouble* knob = dynamic_cast<KnobDouble*>(getApp()->getProject()->getKnobByName("metersPerUnit").get());

        ASSERT_TRUE(knob != NULL);
        knob->setValue(metersPerUnit);
        ASSERT_EQ(metersPerUnit, getApp()->getProject()->getProjectMetersPerUnit());
    }

    static EffectInstance::RenderRoIRetCode evaluate(const NodePtr& node,
                                                     ScenePayload* payload,
                                                     std::string* error)
    {
        return node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), payload, error);
    }

    NodePtr createReader(const std::string& path)
    {
        NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));

        if (!reader) {
            return reader;
        }
        KnobFile* file = dynamic_cast<KnobFile*>(reader->getKnobByName(kUSDReadParamFile).get());
        if (!file) {
            return NodePtr();
        }
        file->setValue(path);

        return reader;
    }

    static void setToggle(const NodePtr& reader,
                          const char* name,
                          bool on)
    {
        KnobBool* toggle = dynamic_cast<KnobBool*>(reader->getKnobByName(name).get());

        ASSERT_TRUE(toggle != NULL);
        toggle->setValue(on);
    }

    // The focal length a USDCamera with the given focal length in mm authors.
    float authoredFocalLength(const NodePtr& camera)
    {
        ScenePayload payload;
        std::string error;
        float value = -1.f;

        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(camera, &payload, &error)) << error;
        PXR_NS::UsdStageRefPtr stage = SceneOps::compose(payload);
        EXPECT_TRUE(bool(stage));
        if (!stage) {
            return value;
        }
        PXR_NS::UsdGeomCamera usdCamera(stage->GetPrimAtPath(PXR_NS::SdfPath("/" + camera->getScriptName_mt_safe())));
        EXPECT_TRUE(bool(usdCamera));
        if (usdCamera) {
            EXPECT_TRUE(usdCamera.GetFocalLengthAttr().Get(&value));
        }

        return value;
    }
};

TEST_F(SceneUpAxisTest, TheProjectDefaultsToYUpMeters)
{
    EXPECT_FALSE(getApp()->getProject()->isProjectUpAxisZ());
    EXPECT_EQ(1., getApp()->getProject()->getProjectMetersPerUnit());
}

TEST_F(SceneUpAxisTest, ACameraLayerCarriesTheProjectUnitsAndSwitchingThemReauthorsIt)
{
    NodePtr camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
    ASSERT_TRUE(bool(camera));

    ScenePayload yUp;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(camera, &yUp, &error)) << error;
    const PXR_NS::SdfLayerRefPtr yLayer = getTopLayer(yUp);
    ASSERT_TRUE(bool(yLayer));
    EXPECT_EQ(PXR_NS::UsdGeomTokens->y, getLayerUpAxis(yLayer));
    EXPECT_EQ(1., getLayerMetersPerUnit(yLayer));
    EXPECT_EQ(SceneMetadata::eUpAxisY, yUp.getMetadata().upAxis);
    EXPECT_EQ(1., yUp.getMetadata().metersPerUnit);

    setProjectUpAxisZ(true);
    ScenePayload zUp;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(camera, &zUp, &error)) << error;
    const PXR_NS::SdfLayerRefPtr zLayer = getTopLayer(zUp);
    ASSERT_TRUE(bool(zLayer));
    EXPECT_NE(yLayer, zLayer);
    EXPECT_EQ(PXR_NS::UsdGeomTokens->z, getLayerUpAxis(zLayer));
    EXPECT_EQ(SceneMetadata::eUpAxisZ, zUp.getMetadata().upAxis);
    // The layer handed out before is never edited again.
    EXPECT_EQ(PXR_NS::UsdGeomTokens->y, getLayerUpAxis(yLayer));

    setProjectMetersPerUnit(0.01);
    ScenePayload centimetres;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(camera, &centimetres, &error)) << error;
    const PXR_NS::SdfLayerRefPtr centimetreLayer = getTopLayer(centimetres);
    ASSERT_TRUE(bool(centimetreLayer));
    EXPECT_NE(zLayer, centimetreLayer);
    EXPECT_EQ(0.01, getLayerMetersPerUnit(centimetreLayer));
    EXPECT_EQ(PXR_NS::UsdGeomTokens->z, getLayerUpAxis(centimetreLayer));

    PXR_NS::UsdStageRefPtr stage = SceneOps::compose(centimetres);
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ(PXR_NS::UsdGeomTokens->z, PXR_NS::UsdGeomGetStageUpAxis(stage));
    EXPECT_DOUBLE_EQ(0.01, PXR_NS::UsdGeomGetStageMetersPerUnit(stage));
}

TEST_F(SceneUpAxisTest, AReaderWarnsWhenTheFileUnitsDifferFromTheProjects)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string zPath = dir.filePath(QString::fromUtf8("zup.usda")).toStdString();
    const std::string yPath = dir.filePath(QString::fromUtf8("yup.usda")).toStdString();
    writeFile(zPath, sceneWithUnits("Z", 1.));
    writeFile(yPath, sceneWithUnits("Y", 1.));

    NodePtr zReader = createReader(zPath);
    NodePtr yReader = createReader(yPath);
    ASSERT_TRUE(zReader && yReader);

    ScenePayload payload;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(zReader, &payload, &error)) << error;
    ASSERT_TRUE(zReader->hasPersistentMessage());
    QString message;
    int type = 0;
    zReader->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("up axis is Z"))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8("project's is Y"))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8("Match Project Up Axis"))) << message.toStdString();
    // Never converted: the file's own up axis passes through.
    EXPECT_EQ(SceneMetadata::eUpAxisZ, payload.getMetadata().upAxis);

    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(yReader, &payload, &error)) << error;
    EXPECT_FALSE(yReader->hasPersistentMessage());

    // A Z-up project matches the Z-up file and clears its warning.
    setProjectUpAxisZ(true);
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(zReader, &payload, &error)) << error;
    EXPECT_FALSE(zReader->hasPersistentMessage());

    // A meters-per-unit difference alone warns too, naming both values.
    setProjectMetersPerUnit(0.01);
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(zReader, &payload, &error)) << error;
    ASSERT_TRUE(zReader->hasPersistentMessage());
    zReader->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("0.01"))) << message.toStdString();
    EXPECT_EQ(1., payload.getMetadata().metersPerUnit);
}

TEST_F(SceneUpAxisTest, CameraLensMillimetresConvertWithTheProjectMetersPerUnit)
{
    NodePtr camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
    ASSERT_TRUE(bool(camera));
    KnobDouble* focalLength = dynamic_cast<KnobDouble*>(camera->getKnobByName(kUSDCameraParamFocalLength).get());
    ASSERT_TRUE(focalLength != NULL);
    focalLength->setValue(50.);

    // USD's camera unit is a tenth of a scene unit: 100 mm in meters, 10 mm in decimetres, 1 mm in
    // centimetres.
    EXPECT_FLOAT_EQ(0.5f, authoredFocalLength(camera));
    setProjectMetersPerUnit(0.1);
    EXPECT_FLOAT_EQ(5.f, authoredFocalLength(camera));
    setProjectMetersPerUnit(0.01);
    EXPECT_FLOAT_EQ(50.f, authoredFocalLength(camera));
}

TEST_F(SceneUpAxisTest, MatchTogglesConvertAZUpCentimetreFileToTheProject)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("zcm.usda")).toStdString();
    writeFile(path, kZUpCentimetres);
    NodePtr reader = createReader(path);
    ASSERT_TRUE(bool(reader));

    // Both off: both aspects are named, nothing is converted.
    ScenePayload payload;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(reader, &payload, &error)) << error;
    std::string message = getMessage(reader);
    EXPECT_TRUE(contains(message, "up axis is Z")) << message;
    EXPECT_TRUE(contains(message, "meters per unit is 0.01")) << message;
    EXPECT_TRUE(contains(message, "Match Project Units")) << message;
    EXPECT_FALSE(contains(message, "fallback")) << message;
    EXPECT_FALSE(topLayerHasWorldAttribute(payload, kUpAxisOp));
    EXPECT_FALSE(topLayerHasWorldAttribute(payload, kUnitsOp));
    EXPECT_EQ(SceneMetadata::eUpAxisZ, payload.getMetadata().upAxis);
    EXPECT_DOUBLE_EQ(0.01, payload.getMetadata().metersPerUnit);
    const PXR_NS::SdfLayerRefPtr uncorrected = getTopLayer(payload);

    // The axis alone: only the units are still named, and the scene is Y-up.
    setToggle(reader, kUSDReadParamMatchUpAxis, true);
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(reader, &payload, &error)) << error;
    message = getMessage(reader);
    EXPECT_FALSE(contains(message, "up axis")) << message;
    EXPECT_TRUE(contains(message, "meters per unit is 0.01")) << message;
    EXPECT_TRUE(topLayerHasWorldAttribute(payload, kUpAxisOp));
    EXPECT_FALSE(topLayerHasWorldAttribute(payload, kUnitsOp));
    EXPECT_EQ(SceneMetadata::eUpAxisY, payload.getMetadata().upAxis);
    EXPECT_DOUBLE_EQ(0.01, payload.getMetadata().metersPerUnit);

    // Both: no warning, the project's metrics, and the box upright in metres.
    setToggle(reader, kUSDReadParamMatchUnits, true);
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(reader, &payload, &error)) << error;
    EXPECT_FALSE(reader->hasPersistentMessage()) << getMessage(reader);
    EXPECT_EQ(SceneMetadata::eUpAxisY, payload.getMetadata().upAxis);
    EXPECT_DOUBLE_EQ(1., payload.getMetadata().metersPerUnit);
    PXR_NS::UsdStageRefPtr stage = SceneOps::compose(payload);
    ASSERT_TRUE(bool(stage));
    EXPECT_EQ(PXR_NS::UsdGeomTokens->y, PXR_NS::UsdGeomGetStageUpAxis(stage));
    EXPECT_DOUBLE_EQ(1., PXR_NS::UsdGeomGetStageMetersPerUnit(stage));
    PXR_NS::GfRange3d bounds = getWorldBounds(payload, "/World/Box");
    expectNear(PXR_NS::GfVec3d(0, 0, -2), bounds.GetMin());
    expectNear(PXR_NS::GfVec3d(1, 3, 0), bounds.GetMax());

    // Switching the project's axis re-authors the correction: a Z-up project needs no rotation.
    setProjectUpAxisZ(true);
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(reader, &payload, &error)) << error;
    EXPECT_FALSE(reader->hasPersistentMessage()) << getMessage(reader);
    EXPECT_FALSE(topLayerHasWorldAttribute(payload, kUpAxisOp));
    EXPECT_TRUE(topLayerHasWorldAttribute(payload, kUnitsOp));
    EXPECT_EQ(SceneMetadata::eUpAxisZ, payload.getMetadata().upAxis);
    bounds = getWorldBounds(payload, "/World/Box");
    expectNear(PXR_NS::GfVec3d(0, 0, 0), bounds.GetMin());
    expectNear(PXR_NS::GfVec3d(1, 2, 3), bounds.GetMax());
    setProjectUpAxisZ(false);

    // Both off again: the uncorrected layer, and the warning, come back.
    setToggle(reader, kUSDReadParamMatchUpAxis, false);
    setToggle(reader, kUSDReadParamMatchUnits, false);
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(reader, &payload, &error)) << error;
    EXPECT_FALSE(topLayerHasWorldAttribute(payload, kUpAxisOp));
    EXPECT_FALSE(topLayerHasWorldAttribute(payload, kUnitsOp));
    EXPECT_EQ(SceneMetadata::eUpAxisZ, payload.getMetadata().upAxis);
    EXPECT_DOUBLE_EQ(0.01, payload.getMetadata().metersPerUnit);
    EXPECT_TRUE(contains(getMessage(reader), "up axis is Z")) << getMessage(reader);
    // The layer handed out uncorrected was never edited.
    ASSERT_TRUE(bool(uncorrected));
    EXPECT_FALSE(bool(uncorrected->GetAttributeAtPath(PXR_NS::SdfPath(std::string("/World.") + kUpAxisOp))));
}

TEST_F(SceneUpAxisTest, AFileWithoutMetricsIsWarnedAboutWithTheFallbacksNamed)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("bare.usda")).toStdString();
    writeFile(path, "#usda 1.0\ndef Xform \"World\"\n{\n}\n");
    NodePtr reader = createReader(path);
    ASSERT_TRUE(bool(reader));

    ScenePayload payload;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(reader, &payload, &error)) << error;
    // Y matches the project; USD's 0.01 does not match its 1.
    const std::string message = getMessage(reader);
    EXPECT_FALSE(contains(message, "up axis")) << message;
    EXPECT_TRUE(contains(message, "meters per unit is 0.01 (unauthored, USD fallback)")) << message;

    setProjectUpAxisZ(true);
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(reader, &payload, &error)) << error;
    EXPECT_TRUE(contains(getMessage(reader), "up axis is Y (unauthored, USD fallback)")) << getMessage(reader);
}

TEST_F(SceneUpAxisTest, UseFileMetricsSetsTheProjectToTheFilesAsOneUndoStep)
{
    NodePtr empty = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
    ASSERT_TRUE(bool(empty));
    KnobIPtr emptyButton = empty->getKnobByName(kUSDReadParamUseFileMetrics);
    ASSERT_TRUE(bool(emptyButton));
    EXPECT_FALSE(emptyButton->isEnabled(0));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("zcm.usda")).toStdString();
    writeFile(path, kZUpCentimetres);
    NodePtr reader = createReader(path);
    ASSERT_TRUE(bool(reader));
    KnobButton* button = dynamic_cast<KnobButton*>(reader->getKnobByName(kUSDReadParamUseFileMetrics).get());
    ASSERT_TRUE(button != NULL);
    EXPECT_TRUE(button->isEnabled(0));

    std::shared_ptr<CollectingRecorder> recorder = std::make_shared<CollectingRecorder>();
    getApp()->setUndoRecorder(recorder);
    const std::uint64_t serialBefore = recorder->serial;
    button->trigger();
    EXPECT_EQ(serialBefore + 1, recorder->serial);
    EXPECT_EQ(0, recorder->depth);
    EXPECT_TRUE(getApp()->getProject()->isProjectUpAxisZ());
    EXPECT_DOUBLE_EQ(0.01, getApp()->getProject()->getProjectMetersPerUnit());

    ScenePayload payload;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(reader, &payload, &error)) << error;
    EXPECT_FALSE(reader->hasPersistentMessage()) << getMessage(reader);

    ASSERT_FALSE(recorder->commands.empty());
    {
        UndoReplayGuard replaying(getApp());
        for (std::vector<UndoCommandPtr>::reverse_iterator it = recorder->commands.rbegin(); it != recorder->commands.rend(); ++it) {
            (*it)->undo();
        }
    }
    EXPECT_FALSE(getApp()->getProject()->isProjectUpAxisZ());
    EXPECT_DOUBLE_EQ(1., getApp()->getProject()->getProjectMetersPerUnit());
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(reader, &payload, &error)) << error;
    EXPECT_TRUE(contains(getMessage(reader), "up axis is Z")) << getMessage(reader);

    recorder->commands.clear();
    getApp()->setUndoRecorder(UndoRecorderPtr());
}
