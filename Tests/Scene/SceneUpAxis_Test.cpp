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

    void setProjectMaterialsScope(const std::string& path)
    {
        KnobString* knob = dynamic_cast<KnobString*>(getApp()->getProject()->getKnobByName("materialsScope").get());

        ASSERT_TRUE(knob != NULL);
        knob->setValue(path);
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
    EXPECT_TRUE(message.contains(QString::fromUtf8("are Y"))) << message.toStdString();
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

TEST_F(SceneUpAxisTest, TheMaterialsScopeDefaultsToMaterials)
{
    KnobString* knob = dynamic_cast<KnobString*>(getApp()->getProject()->getKnobByName("materialsScope").get());
    ASSERT_TRUE(knob != NULL);
    EXPECT_EQ("/materials", knob->getValue());
    EXPECT_EQ("materials", getApp()->getProject()->getProjectMaterialsScope());
}

TEST_F(SceneUpAxisTest, OnlyASingleAbsolutePrimNameIsAMaterialsScope)
{
    EXPECT_EQ("looks", Project::validateMaterialsScope("/looks"));
    EXPECT_EQ("_shared2", Project::validateMaterialsScope("/_shared2"));
    EXPECT_EQ("", Project::validateMaterialsScope(""));
    EXPECT_EQ("", Project::validateMaterialsScope("/"));
    EXPECT_EQ("", Project::validateMaterialsScope("materials"));
    EXPECT_EQ("", Project::validateMaterialsScope("/a/b"));
    EXPECT_EQ("", Project::validateMaterialsScope("/materials/"));
    EXPECT_EQ("", Project::validateMaterialsScope("/2materials"));
    EXPECT_EQ("", Project::validateMaterialsScope("/my materials"));

    setProjectMaterialsScope("/a/b");
    EXPECT_EQ("", getApp()->getProject()->getProjectMaterialsScope());
    setProjectMaterialsScope("materials");
    EXPECT_EQ("", getApp()->getProject()->getProjectMaterialsScope());
    setProjectMaterialsScope("");
    EXPECT_EQ("", getApp()->getProject()->getProjectMaterialsScope());
    setProjectMaterialsScope("/looks");
    EXPECT_EQ("looks", getApp()->getProject()->getProjectMaterialsScope());
}

TEST_F(SceneUpAxisTest, ChangingTheMaterialsScopeReauthorsASceneLayer)
{
    NodePtr camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
    ASSERT_TRUE(bool(camera));

    ScenePayload before;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(camera, &before, &error)) << error;
    const PXR_NS::SdfLayerRefPtr beforeLayer = getTopLayer(before);
    ASSERT_TRUE(bool(beforeLayer));

    // Unchanged settings keep the memoized layer.
    ScenePayload again;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(camera, &again, &error)) << error;
    EXPECT_EQ(beforeLayer, getTopLayer(again));

    setProjectMaterialsScope("/looks");
    ScenePayload after;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(camera, &after, &error)) << error;
    const PXR_NS::SdfLayerRefPtr afterLayer = getTopLayer(after);
    ASSERT_TRUE(bool(afterLayer));
    EXPECT_NE(beforeLayer, afterLayer);

    // An invalid scope turns the feature off, which keys differently again.
    setProjectMaterialsScope("/a/b");
    ScenePayload off;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(camera, &off, &error)) << error;
    EXPECT_NE(afterLayer, getTopLayer(off));
}
