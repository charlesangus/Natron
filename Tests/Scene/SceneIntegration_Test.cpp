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

#include <limits>
#include <list>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "SceneTestEffect.h"

#include "Engine/AppInstance.h"
#include "Engine/CLArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDCamera.h"
#include "Engine/Nodes/Scene/USDEditTransform.h"
#include "Engine/Nodes/Scene/USDLight.h"
#include "Engine/Nodes/Scene/USDMaterial.h"
#include "Engine/Nodes/Scene/USDMerge.h"
#include "Engine/Nodes/Scene/USDPrune.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDWrite.h"
#include "Engine/Nodes/Scene/USDXformPrim.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

// The headless end-to-end path a viewer takes: USDRead, the scene action through a Dot, the
// stage a query borrows from the SceneQueryPool, and what the stage holds. If USD's libraries or plug-ins
// fail to load at run time, it fails here rather than in the GUI.
class SceneIntegrationTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        SceneQueryPool::instance().releaseIdle();

        BaseTest::TearDown();
    }

    template <typename KNOB>
    static KNOB* getKnob(const NodePtr& node,
                         const char* name)
    {
        return dynamic_cast<KNOB*>(node->getKnobByName(name).get());
    }

    NodePtr createReader(const std::string& path,
                         double frameOffset = 0.)
    {
        NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
        if (!reader) {
            return reader;
        }
        KnobFile* file = getKnob<KnobFile>(reader, kUSDReadParamFile);
        KnobDouble* offset = getKnob<KnobDouble>(reader, kUSDReadParamFrameOffset);
        EXPECT_TRUE(file && offset);
        if (file && offset) {
            file->setValue(path);
            offset->setValue(frameOffset);
        }

        return reader;
    }

    // A USDWrite writing input's scene to path over [firstFrame, lastFrame].
    NodePtr createWriter(const NodePtr& input,
                         const QString& path,
                         USDWrite::ExportModeEnum exportMode,
                         USDWrite::PathModeEnum pathMode,
                         int firstFrame,
                         int lastFrame)
    {
        NodePtr writer = createNode(QString::fromUtf8(PLUGINID_NATRON_USDWRITE));
        if (!writer) {
            return writer;
        }
        EXPECT_TRUE(writer->connectInput(input, 0));
        KnobOutputFile* file = getKnob<KnobOutputFile>(writer, kUSDWriteParamFile);
        KnobChoice* exportModeKnob = getKnob<KnobChoice>(writer, kUSDWriteParamExportMode);
        KnobChoice* pathModeKnob = getKnob<KnobChoice>(writer, kUSDWriteParamPathMode);
        KnobChoice* frameRange = getKnob<KnobChoice>(writer, kUSDWriteParamFrameRange);
        KnobInt* first = getKnob<KnobInt>(writer, kUSDWriteParamFirstFrame);
        KnobInt* last = getKnob<KnobInt>(writer, kUSDWriteParamLastFrame);
        EXPECT_TRUE(file && exportModeKnob && pathModeKnob && frameRange && first && last);
        if (file && exportModeKnob && pathModeKnob && frameRange && first && last) {
            file->setValue(path.toStdString());
            exportModeKnob->setValue((int)exportMode);
            pathModeKnob->setValue((int)pathMode);
            frameRange->setValue((int)USDWrite::eFrameRangeManual);
            first->setValue(firstFrame);
            last->setValue(lastFrame);
        }

        return writer;
    }

    static USDWrite* getWriter(const NodePtr& node)
    {
        return dynamic_cast<USDWrite*>(node->getEffectInstance().get());
    }

    // What Render does with the writer: one render over its frame range knobs.
    void render(const NodePtr& writer)
    {
        std::list<AppInstance::RenderWork> works;
        works.push_back(AppInstance::RenderWork(getWriter(writer), std::numeric_limits<int>::min(), std::numeric_limits<int>::max(),
                                                std::numeric_limits<int>::min(), false));
        getApp()->startWritersRendering(eRenderModeBlocking, works);
    }

    static PXR_NS::UsdStageRefPtr composeOutput(const NodePtr& node)
    {
        ScenePayload payload;
        std::string error;
        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk,
                  node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &payload, &error))
            << error;

        return SceneOps::compose(payload);
    }

    // The composed prim tree, flattened: what two stages that compose the same scene share.
    static std::string exportStage(const PXR_NS::UsdStageRefPtr& stage)
    {
        std::string text;
        EXPECT_TRUE(stage && stage->ExportToString(&text, false));

        return text;
    }
};

namespace {
const char* const kReferenceScene = NATRON_TESTS_FIXTURES_DIR "/scene-reference.usda";

QString
cleanAbsolute(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}
} // anonymous namespace

TEST_F(SceneIntegrationTest, ReferenceSceneThroughTheActionComposesWithItsBounds)
{
    NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
    NodePtr dot = createNode(QString::fromUtf8(PLUGINID_NATRON_DOT));
    ASSERT_TRUE(reader && dot);
    KnobFile* file = dynamic_cast<KnobFile*>(reader->getKnobByName(kUSDReadParamFile).get());
    ASSERT_TRUE(file != NULL);
    file->setValue(NATRON_TESTS_FIXTURES_DIR "/scene-reference.usda");
    ASSERT_TRUE(dot->connectInput(reader, 0));
    ASSERT_TRUE(dot->getEffectInstance()->producesSceneData());

    ScenePayload payload;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              dot->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &payload, &error))
        << error;

    SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(payload, &error);
    ASSERT_TRUE(lease.isValid()) << error;
    PXR_NS::UsdStageRefPtr stage = lease.getStage();
    ASSERT_TRUE(bool(stage));

    const char* const prims[] = { "/World", "/World/Box", "/World/Ball", "/World/Cam" };
    for (const char* path : prims) {
        EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath(path)).IsValid()) << path;
    }
    EXPECT_EQ(PXR_NS::TfToken("Cube"), stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Box")).GetTypeName());
    EXPECT_EQ(PXR_NS::TfToken("Camera"), stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Cam")).GetTypeName());
    EXPECT_EQ(PXR_NS::UsdGeomTokens->y, PXR_NS::UsdGeomGetStageUpAxis(stage));

    // The box spans [-1, 1] on every axis; the ball (radius 0.5) sits at x = 3 and rises from
    // y = 0 at timecode 1 to y = 2 at timecode 24.
    const PXR_NS::TfTokenVector purposes = { PXR_NS::UsdGeomTokens->default_ };
    {
        PXR_NS::UsdGeomBBoxCache bboxes(PXR_NS::UsdTimeCode(1.), purposes);
        const PXR_NS::GfRange3d range = bboxes.ComputeWorldBound(stage->GetPseudoRoot()).ComputeAlignedRange();
        EXPECT_NEAR(-1., range.GetMin()[0], 1e-6);
        EXPECT_NEAR(-1., range.GetMin()[1], 1e-6);
        EXPECT_NEAR(3.5, range.GetMax()[0], 1e-6);
        EXPECT_NEAR(1., range.GetMax()[1], 1e-6);
    }
    {
        PXR_NS::UsdGeomBBoxCache bboxes(PXR_NS::UsdTimeCode(24.), purposes);
        const PXR_NS::GfRange3d range = bboxes.ComputeWorldBound(stage->GetPseudoRoot()).ComputeAlignedRange();
        EXPECT_NEAR(2.5, range.GetMax()[1], 1e-6);
    }
}

TEST_F(SceneIntegrationTest, CreatedPrimsGroupUnderTheRigNextToTheReadScene)
{
    NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
    NodePtr rig = createNode(QString::fromUtf8(PLUGINID_NATRON_USDXFORMPRIM));
    NodePtr camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
    NodePtr light = createNode(QString::fromUtf8(PLUGINID_NATRON_USDLIGHT));
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
    ASSERT_TRUE(reader && rig && camera && light && merge);
    KnobFile* file = dynamic_cast<KnobFile*>(reader->getKnobByName(kUSDReadParamFile).get());
    ASSERT_TRUE(file != NULL);
    file->setValue(NATRON_TESTS_FIXTURES_DIR "/scene-reference.usda");

    // A rig xform under the read scene's root, holding the camera and a light.
    const auto setPrimPath = [](const NodePtr& node, const char* path) {
        KnobString* primPath = dynamic_cast<KnobString*>(node->getKnobByName(kSceneParamPrimPath).get());
        ASSERT_TRUE(primPath != NULL);
        primPath->setValue(std::string(path));
    };
    setPrimPath(rig, "/World/Rig");
    setPrimPath(camera, "/Shot");
    setPrimPath(light, "/Key");
    KnobDouble* rigTranslate = dynamic_cast<KnobDouble*>(rig->getKnobByName(kSceneParamTranslate).get());
    KnobDouble* cameraTranslate = dynamic_cast<KnobDouble*>(camera->getKnobByName(kSceneParamTranslate).get());
    ASSERT_TRUE(rigTranslate && cameraTranslate);
    rigTranslate->setValue(100., ViewSpec::all(), 0);
    cameraTranslate->setValue(5., ViewSpec::all(), 2);

    ASSERT_TRUE(rig->connectInput(camera, 0));
    ASSERT_TRUE(rig->connectInput(light, 1));
    ASSERT_TRUE(merge->connectInput(reader, 0));
    ASSERT_TRUE(merge->connectInput(rig, 1));

    const auto evaluate = [](const NodePtr& node, ScenePayload* payload, std::string* error) {
        return node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), payload, error);
    };
    ScenePayload readPayload, rigPayload, payload;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(reader, &readPayload, &error)) << error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(rig, &rigPayload, &error)) << error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(merge, &payload, &error)) << error;
    // The rig's layers stack over the read scene's, which share none of them.
    EXPECT_EQ(readPayload.getNumLayers() + rigPayload.getNumLayers(), payload.getNumLayers());
    // The read scene's metadata passes through the rig's layers.
    EXPECT_EQ(SceneMetadata::eUpAxisY, payload.getMetadata().upAxis);
    EXPECT_EQ(std::string("World"), payload.getMetadata().defaultPrim);

    SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(payload, &error);
    ASSERT_TRUE(lease.isValid()) << error;
    PXR_NS::UsdStageRefPtr stage = lease.getStage();
    ASSERT_TRUE(bool(stage));
    const char* const readPrims[] = { "/World", "/World/Box", "/World/Ball", "/World/Cam" };
    for (const char* path : readPrims) {
        EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath(path)).IsValid()) << path;
    }
    EXPECT_EQ(PXR_NS::TfToken("Cube"), stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Box")).GetTypeName());
    EXPECT_EQ(PXR_NS::TfToken("Xform"), stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Rig")).GetTypeName());
    EXPECT_EQ(PXR_NS::TfToken("Camera"), stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Rig/Shot")).GetTypeName());
    EXPECT_EQ(PXR_NS::TfToken("DistantLight"), stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Rig/Key")).GetTypeName());

    // The camera inherits the rig's transform.
    const PXR_NS::GfMatrix4d toWorld = PXR_NS::UsdGeomXformable(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Rig/Shot")))
                                           .ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode(1.));
    EXPECT_NEAR(100., toWorld.ExtractTranslation()[0], 1e-9);
    EXPECT_NEAR(5., toWorld.ExtractTranslation()[2], 1e-9);
}

// A scene built from nodes, written by USDWrite and read back by USDRead, composes the same prim
// tree in both modes. The read scene is offset in time, so the sublayer offset Layered mode
// writes is checked along with everything else.
TEST_F(SceneIntegrationTest, USDWriteRoundTripsTheSceneInBothModes)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr reader = createReader(kReferenceScene, 3.);
    NodePtr rig = createNode(QString::fromUtf8(PLUGINID_NATRON_USDXFORMPRIM));
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
    NodePtr prune = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPRUNE));
    ASSERT_TRUE(reader && rig && merge && prune);
    KnobString* rigPath = getKnob<KnobString>(rig, kSceneParamPrimPath);
    KnobDouble* rigTranslate = getKnob<KnobDouble>(rig, kSceneParamTranslate);
    KnobString* prunePattern = getKnob<KnobString>(prune, kUSDPruneParamPrimPattern);
    ASSERT_TRUE(rigPath && rigTranslate && prunePattern);
    rigPath->setValue(std::string("/World/Rig"));
    rigTranslate->setValue(100., ViewSpec::all(), 0);
    prunePattern->setValue(std::string("/World/Cam"));
    ASSERT_TRUE(merge->connectInput(reader, 0));
    ASSERT_TRUE(merge->connectInput(rig, 1));
    ASSERT_TRUE(prune->connectInput(merge, 0));

    const std::string original = exportStage(composeOutput(prune));
    ASSERT_FALSE(original.empty());
    // The offset moved the ball's samples off 1 and 24.
    ASSERT_EQ(std::string::npos, original.find("    1: (3, 0, 0)")) << original;

    const USDWrite::ExportModeEnum modes[] = { USDWrite::eExportModeLayered, USDWrite::eExportModeFlattened };
    for (USDWrite::ExportModeEnum mode : modes) {
        const QString path = tmp.path() + QString::fromUtf8(mode == USDWrite::eExportModeLayered ? "/layered.usda" : "/flattened.usdc");
        NodePtr writer = createWriter(prune, path, mode, USDWrite::ePathModeRelative, -10, 40);
        ASSERT_TRUE(writer && getWriter(writer));
        ASSERT_EQ(eStatusOK, getWriter(writer)->writeFrameRange(-10, 40, ViewIdx(0)));
        ASSERT_TRUE(QFile::exists(path));

        NodePtr readBack = createReader(path.toStdString());
        ASSERT_TRUE(bool(readBack));
        EXPECT_EQ(original, exportStage(composeOutput(readBack))) << "mode " << (int)mode;
    }
}

// Layered mode refers to the file USDRead reads, relative to the written file or absolute, rather
// than copying it.
TEST_F(SceneIntegrationTest, USDWriteLayeredSublayersTheReadFile)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr reader = createReader(kReferenceScene);
    ASSERT_TRUE(bool(reader));
    const QString fixture = cleanAbsolute(QString::fromUtf8(kReferenceScene));

    const USDWrite::PathModeEnum modes[] = { USDWrite::ePathModeRelative, USDWrite::ePathModeAbsolute };
    for (USDWrite::PathModeEnum mode : modes) {
        const QString path = tmp.path() + QString::fromUtf8(mode == USDWrite::ePathModeRelative ? "/out/relative.usda" : "/out/absolute.usda");
        NodePtr writer = createWriter(reader, path, USDWrite::eExportModeLayered, mode, 1, 24);
        ASSERT_TRUE(writer && getWriter(writer));
        ASSERT_EQ(eStatusOK, getWriter(writer)->writeFrameRange(1, 24, ViewIdx(0)));

        PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::FindOrOpen(path.toStdString());
        ASSERT_TRUE(static_cast<bool>(layer));
        const std::vector<std::string> subLayers = layer->GetSubLayerPaths();
        ASSERT_EQ((std::size_t)1, subLayers.size());
        const QString subLayer = QString::fromStdString(subLayers[0]);
        if (mode == USDWrite::ePathModeRelative) {
            EXPECT_TRUE(QDir::isRelativePath(subLayer)) << subLayers[0];
            EXPECT_EQ(fixture, QDir::cleanPath(QDir(tmp.path() + QString::fromUtf8("/out")).absoluteFilePath(subLayer))) << subLayers[0];
        } else {
            EXPECT_EQ(fixture, subLayer);
        }
        // Referred to, not copied.
        EXPECT_FALSE(layer->GetPrimAtPath(PXR_NS::SdfPath("/World/Box")));

        PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(layer);
        ASSERT_TRUE(static_cast<bool>(stage));
        EXPECT_EQ(PXR_NS::TfToken("Cube"), stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Box")).GetTypeName());
    }
}

namespace {
// The file the first UsdUVTexture on stage reads, as authored.
std::string
textureFileOf(const PXR_NS::UsdStageRefPtr& stage)
{
    for (const PXR_NS::UsdPrim& prim : stage->Traverse()) {
        const PXR_NS::UsdShadeShader shader(prim);
        PXR_NS::TfToken id;
        PXR_NS::SdfAssetPath file;
        if (shader && shader.GetShaderId(&id) && (id == PXR_NS::TfToken("UsdUVTexture")) && shader.GetInput(PXR_NS::TfToken("file")).Get(&file)) {
            return file.GetAssetPath();
        }
    }

    return std::string();
}
} // anonymous namespace

// A file read above nodes' layers keeps its strength in the written scene, a relative texture path
// is anchored where the node resolved it and written relative to the written file's folder, and a
// USDRead of the written file sees the next write.
TEST_F(SceneIntegrationTest, USDWriteLayeredKeepsTheStackOrderAndAnchorsRelativePaths)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString overPath = tmp.path() + QString::fromUtf8("/over.usda");
    {
        QFile f(overPath);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("#usda 1.0\n"
                "over \"World\"\n{\n"
                "    over \"Box\"\n    {\n"
                "        double3 xformOp:translate = (0, 7, 0)\n"
                "        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                "    }\n}\n");
    }

    // Nodes author the box at x = 5 holding a card with a textured material; the file read above
    // them moves the box to y = 7.
    NodePtr box = createNode(QString::fromUtf8(PLUGINID_NATRON_USDXFORMPRIM));
    NodePtr card = createNode(QString::fromUtf8(kTestPluginIDSceneGeometry));
    NodePtr material = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMATERIAL));
    NodePtr reader = createReader(overPath.toStdString());
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
    ASSERT_TRUE(box && card && material && reader && merge);
    KnobString* boxPath = getKnob<KnobString>(box, kSceneParamPrimPath);
    KnobDouble* translate = getKnob<KnobDouble>(box, kSceneParamTranslate);
    KnobFile* texture = getKnob<KnobFile>(material, kUSDMaterialParamDiffuseTexture);
    ASSERT_TRUE(boxPath && translate && texture);
    boxPath->setValue(std::string("/World/Box"));
    translate->setValue(5., ViewSpec::all(), 0);
    texture->setValue(std::string("textures/wood.png"));
    ASSERT_TRUE(card->connectInput(material, 0));
    ASSERT_TRUE(box->connectInput(card, 0));
    ASSERT_TRUE(merge->connectInput(box, 0));
    ASSERT_TRUE(merge->connectInput(reader, 1));

    const QString outDir = tmp.path() + QString::fromUtf8("/out");
    const QString path = outDir + QString::fromUtf8("/scene.usda");
    NodePtr writer = createWriter(merge, path, USDWrite::eExportModeLayered, USDWrite::ePathModeRelative, 1, 1);
    ASSERT_TRUE(writer && getWriter(writer));
    ASSERT_EQ(eStatusOK, getWriter(writer)->writeFrameRange(1, 1, ViewIdx(0)));

    // The read file is the written scene's strongest sublayer, referred to, not copied.
    PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::FindOrOpen(path.toStdString());
    ASSERT_TRUE(static_cast<bool>(layer));
    const std::vector<std::string> subLayers = layer->GetSubLayerPaths();
    ASSERT_FALSE(subLayers.empty());
    EXPECT_EQ(cleanAbsolute(overPath), QDir::cleanPath(QDir(outDir).absoluteFilePath(QString::fromStdString(subLayers[0])))) << subLayers[0];

    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(layer);
    ASSERT_TRUE(static_cast<bool>(stage));
    const PXR_NS::UsdGeomXformable written(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Box")));
    ASSERT_TRUE(bool(written));
    EXPECT_EQ(PXR_NS::GfVec3d(0., 7., 0.), written.ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode(1.)).ExtractTranslation());

    // The texture path the node resolved from the working directory, relative to the written file.
    const QString anchored = QDir::cleanPath(QDir::current().absoluteFilePath(QString::fromUtf8("textures/wood.png")));
    const std::string file = textureFileOf(stage);
    ASSERT_FALSE(file.empty());
    EXPECT_TRUE(QDir::isRelativePath(QString::fromStdString(file))) << file;
    EXPECT_EQ(anchored, QDir::cleanPath(QDir(outDir).absoluteFilePath(QString::fromStdString(file)))) << file;

    // A USDRead of the written file sees the next write.
    NodePtr reread = createReader(path.toStdString());
    ASSERT_TRUE(bool(reread));
    PXR_NS::UsdStageRefPtr before = composeOutput(reread);
    ASSERT_TRUE(static_cast<bool>(before));
    EXPECT_EQ(file, textureFileOf(before));
    before.Reset();
    texture->setValue(std::string("textures/oak.png"));
    ASSERT_EQ(eStatusOK, getWriter(writer)->writeFrameRange(1, 1, ViewIdx(0)));
    PXR_NS::UsdStageRefPtr after = composeOutput(reread);
    ASSERT_TRUE(static_cast<bool>(after));
    const std::string oak = textureFileOf(after);
    EXPECT_EQ(QDir::cleanPath(QDir::current().absoluteFilePath(QString::fromUtf8("textures/oak.png"))),
              QDir::cleanPath(QDir(outDir).absoluteFilePath(QString::fromStdString(oak))))
        << oak;
}

namespace {
void
writeTextFile(const QString& path,
              const char* content)
{
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate)) << path.toStdString();
    f.write(content);
}

// The names in dir, sorted.
QStringList
fileNames(const QString& dir)
{
    return QDir(dir).entryList(QDir::Files | QDir::Hidden, QDir::Name);
}
} // anonymous namespace

// A layered write conforms the folder to the scene: the sidecars of a write with more layer runs
// that this one does not produce are removed, and nothing else in the folder is touched.
TEST_F(SceneIntegrationTest, USDWriteLayeredRemovesStaleSidecars)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString aPath = tmp.path() + QString::fromUtf8("/a.usda");
    const QString bPath = tmp.path() + QString::fromUtf8("/b.usda");
    writeTextFile(aPath, "#usda 1.0\n"
                         "over \"World\"\n{\n"
                         "    over \"Box\"\n    {\n"
                         "        double3 xformOp:translate = (0, 7, 0)\n"
                         "        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                         "    }\n}\n");
    writeTextFile(bPath, "#usda 1.0\n"
                         "over \"World\"\n{\n"
                         "    def Xform \"FromB\"\n    {\n    }\n}\n");

    const QString outDir = tmp.path() + QString::fromUtf8("/out");
    ASSERT_TRUE(QDir().mkpath(outDir));
    // Names close to the sidecars' that are not theirs: another extension, another output name, a
    // suffix that is not a number, and a file of another kind.
    const QStringList unrelated = QStringList() << QString::fromUtf8("notes.txt") << QString::fromUtf8("other_layer1.usda")
                                                << QString::fromUtf8("scene_layer3.usdc") << QString::fromUtf8("scene_layerX.usda");
    for (const QString& name : unrelated) {
        writeTextFile(outDir + QString::fromUtf8("/") + name, "keep\n");
    }

    // Strongest first: b.usda, other, a.usda, box. Each run of node layers below a file is a sidecar.
    NodePtr box = createNode(QString::fromUtf8(PLUGINID_NATRON_USDXFORMPRIM));
    NodePtr readerA = createReader(aPath.toStdString());
    NodePtr mergeA = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
    NodePtr other = createNode(QString::fromUtf8(PLUGINID_NATRON_USDXFORMPRIM));
    NodePtr mergeOther = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
    NodePtr readerB = createReader(bPath.toStdString());
    NodePtr mergeB = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
    ASSERT_TRUE(box && readerA && mergeA && other && mergeOther && readerB && mergeB);
    KnobString* boxPath = getKnob<KnobString>(box, kSceneParamPrimPath);
    KnobDouble* translate = getKnob<KnobDouble>(box, kSceneParamTranslate);
    KnobString* otherPath = getKnob<KnobString>(other, kSceneParamPrimPath);
    ASSERT_TRUE(boxPath && translate && otherPath);
    boxPath->setValue(std::string("/World/Box"));
    translate->setValue(5., ViewSpec::all(), 0);
    otherPath->setValue(std::string("/World/Other"));
    ASSERT_TRUE(mergeA->connectInput(box, 0));
    ASSERT_TRUE(mergeA->connectInput(readerA, 1));
    ASSERT_TRUE(mergeOther->connectInput(mergeA, 0));
    ASSERT_TRUE(mergeOther->connectInput(other, 1));
    ASSERT_TRUE(mergeB->connectInput(mergeOther, 0));
    ASSERT_TRUE(mergeB->connectInput(readerB, 1));

    const QString path = outDir + QString::fromUtf8("/scene.usda");
    NodePtr writer = createWriter(mergeB, path, USDWrite::eExportModeLayered, USDWrite::ePathModeRelative, 1, 1);
    ASSERT_TRUE(writer && getWriter(writer));
    ASSERT_EQ(eStatusOK, getWriter(writer)->writeFrameRange(1, 1, ViewIdx(0)));
    QStringList expected = unrelated;
    expected << QString::fromUtf8("scene.usda") << QString::fromUtf8("scene_layer1.usda") << QString::fromUtf8("scene_layer2.usda");
    expected.sort();
    EXPECT_EQ(expected, fileNames(outDir));

    // Without b.usda, the strongest layer is other's, which becomes the written file itself: one
    // sidecar is left, for box below a.usda.
    NodePtr shorter = createWriter(mergeOther, path, USDWrite::eExportModeLayered, USDWrite::ePathModeRelative, 1, 1);
    ASSERT_TRUE(shorter && getWriter(shorter));
    ASSERT_EQ(eStatusOK, getWriter(shorter)->writeFrameRange(1, 1, ViewIdx(0)));
    expected = unrelated;
    expected << QString::fromUtf8("scene.usda") << QString::fromUtf8("scene_layer1.usda");
    expected.sort();
    EXPECT_EQ(expected, fileNames(outDir));

    // The written file opens, every sublayer it names is there, and it composes the shorter stack.
    PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::FindOrOpen(path.toStdString());
    ASSERT_TRUE(static_cast<bool>(layer));
    const std::vector<std::string> subLayers = layer->GetSubLayerPaths();
    EXPECT_EQ(2u, subLayers.size());
    for (const std::string& subLayer : subLayers) {
        const QString subPath = QDir::cleanPath(QDir(outDir).absoluteFilePath(QString::fromStdString(subLayer)));
        EXPECT_TRUE(PXR_NS::SdfLayer::FindOrOpen(subPath.toStdString())) << subLayer;
    }
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(layer);
    ASSERT_TRUE(static_cast<bool>(stage));
    EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Other")).IsValid());
    EXPECT_FALSE(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/FromB")).IsValid());
    const PXR_NS::UsdGeomXformable written(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Box")));
    ASSERT_TRUE(bool(written));
    EXPECT_EQ(PXR_NS::GfVec3d(0., 7., 0.), written.ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode(1.)).ExtractTranslation());
}

// A flattened write to a layered write's name removes the sidecars the flattened file does not use,
// says which with a warning, and spares look-alikes and a sidecar-named file the scene reads.
TEST_F(SceneIntegrationTest, USDWriteFlattenedRemovesStaleSidecarsWithAWarning)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString outDir = tmp.path() + QString::fromUtf8("/out");
    ASSERT_TRUE(QDir().mkpath(outDir));
    const QString aPath = tmp.path() + QString::fromUtf8("/a.usda");
    // Read by the scene, under a name the sidecar pattern matches.
    const QString bPath = outDir + QString::fromUtf8("/scene_layer9.usda");
    writeTextFile(aPath, "#usda 1.0\n"
                         "over \"World\"\n{\n"
                         "    over \"Box\"\n    {\n"
                         "        double3 xformOp:translate = (0, 7, 0)\n"
                         "        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                         "    }\n}\n");
    writeTextFile(bPath, "#usda 1.0\n"
                         "over \"World\"\n{\n"
                         "    def Xform \"FromB\"\n    {\n    }\n}\n");
    const QStringList unrelated = QStringList() << QString::fromUtf8("notes.txt") << QString::fromUtf8("other_layer1.usda")
                                                << QString::fromUtf8("scene_layer3.usdc") << QString::fromUtf8("scene_layerX.usda");
    for (const QString& name : unrelated) {
        writeTextFile(outDir + QString::fromUtf8("/") + name, "keep\n");
    }

    // Strongest first: scene_layer9.usda, other, a.usda, box: two runs of node layers, two sidecars.
    NodePtr box = createNode(QString::fromUtf8(PLUGINID_NATRON_USDXFORMPRIM));
    NodePtr readerA = createReader(aPath.toStdString());
    NodePtr mergeA = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
    NodePtr other = createNode(QString::fromUtf8(PLUGINID_NATRON_USDXFORMPRIM));
    NodePtr readerB = createReader(bPath.toStdString());
    NodePtr mergeB = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
    ASSERT_TRUE(box && readerA && mergeA && other && readerB && mergeB);
    KnobString* boxPath = getKnob<KnobString>(box, kSceneParamPrimPath);
    KnobString* otherPath = getKnob<KnobString>(other, kSceneParamPrimPath);
    ASSERT_TRUE(boxPath && otherPath);
    boxPath->setValue(std::string("/World/Box"));
    otherPath->setValue(std::string("/World/Other"));
    ASSERT_TRUE(mergeA->connectInput(box, 0));
    ASSERT_TRUE(mergeA->connectInput(readerA, 1));
    ASSERT_TRUE(mergeB->connectInput(mergeA, 0));
    ASSERT_TRUE(mergeB->connectInput(other, 1));
    ASSERT_TRUE(mergeB->connectInput(readerB, 2));

    const QString path = outDir + QString::fromUtf8("/scene.usda");
    NodePtr layered = createWriter(mergeB, path, USDWrite::eExportModeLayered, USDWrite::ePathModeRelative, 1, 1);
    ASSERT_TRUE(layered && getWriter(layered));
    ASSERT_EQ(eStatusOK, getWriter(layered)->writeFrameRange(1, 1, ViewIdx(0)));
    QStringList expected = unrelated;
    expected << QString::fromUtf8("scene.usda") << QString::fromUtf8("scene_layer1.usda") << QString::fromUtf8("scene_layer2.usda")
             << QString::fromUtf8("scene_layer9.usda");
    expected.sort();
    ASSERT_EQ(expected, fileNames(outDir));
    EXPECT_FALSE(layered->hasPersistentMessage());

    NodePtr flattened = createWriter(mergeB, path, USDWrite::eExportModeFlattened, USDWrite::ePathModeRelative, 1, 1);
    ASSERT_TRUE(flattened && getWriter(flattened));
    ASSERT_EQ(eStatusOK, getWriter(flattened)->writeFrameRange(1, 1, ViewIdx(0)));
    expected = unrelated;
    expected << QString::fromUtf8("scene.usda") << QString::fromUtf8("scene_layer9.usda");
    expected.sort();
    EXPECT_EQ(expected, fileNames(outDir));

    ASSERT_TRUE(flattened->hasPersistentMessage());
    QString message;
    int type = 0;
    flattened->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("scene_layer1.usda"))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8("scene_layer2.usda"))) << message.toStdString();
    EXPECT_FALSE(message.contains(QString::fromUtf8("scene_layer9.usda"))) << message.toStdString();

    // The flattened file stands on its own: no sublayers, and what the removed sidecars held is in it.
    PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::FindOrOpen(path.toStdString());
    ASSERT_TRUE(static_cast<bool>(layer));
    EXPECT_TRUE(layer->GetSubLayerPaths().empty());
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(layer);
    ASSERT_TRUE(static_cast<bool>(stage));
    EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Other")).IsValid());
    EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/FromB")).IsValid());
    EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Box")).IsValid());

    // A flattened write that removes nothing raises no warning.
    ASSERT_EQ(eStatusOK, getWriter(flattened)->writeFrameRange(1, 1, ViewIdx(0)));
    EXPECT_FALSE(flattened->hasPersistentMessage());
}

// The frame range knobs set which time samples are written: samples outside are dropped, and the
// range's ends get the values the animation had there.
TEST_F(SceneIntegrationTest, USDWriteFrameRangeClipsTheTimeSamples)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr reader = createReader(kReferenceScene);
    ASSERT_TRUE(bool(reader));
    const QString path = tmp.path() + QString::fromUtf8("/clipped.usda");
    NodePtr writer = createWriter(reader, path, USDWrite::eExportModeFlattened, USDWrite::ePathModeRelative, 5, 10);
    ASSERT_TRUE(writer && getWriter(writer));
    render(writer);
    EXPECT_EQ((U64)1, getWriter(writer)->getWriteCount());

    PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::FindOrOpen(path.toStdString());
    ASSERT_TRUE(static_cast<bool>(layer));
    EXPECT_EQ(5., layer->GetStartTimeCode());
    EXPECT_EQ(10., layer->GetEndTimeCode());
    const PXR_NS::SdfPath translate("/World/Ball.xformOp:translate");
    const std::set<double> times = layer->ListTimeSamplesForPath(translate);
    ASSERT_EQ((std::set<double> { 5., 10. }), times);
    // The ball rises linearly from y = 0 at 1 to y = 2 at 24.
    PXR_NS::GfVec3d value;
    ASSERT_TRUE(layer->QueryTimeSample(translate, 5., &value));
    EXPECT_NEAR(3., value[0], 1e-9);
    EXPECT_NEAR(2. * 4. / 23., value[1], 1e-9);
    ASSERT_TRUE(layer->QueryTimeSample(translate, 10., &value));
    EXPECT_NEAR(2. * 9. / 23., value[1], 1e-9);
}

// `NatronRenderer -w <writer> <file> 1-10` overrides File and writes the file once, holding the
// whole range.
TEST_F(SceneIntegrationTest, USDWriteRendersOncePerCommandLineRender)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr reader = createReader(kReferenceScene);
    ASSERT_TRUE(bool(reader));
    const QString original = tmp.path() + QString::fromUtf8("/original.usda");
    const QString overridden = tmp.path() + QString::fromUtf8("/overridden.usda");
    NodePtr writer = createWriter(reader, original, USDWrite::eExportModeLayered, USDWrite::ePathModeRelative, 1, 1);
    ASSERT_TRUE(writer && getWriter(writer));

    QStringList args;
    args << QString::fromUtf8("NatronRendererTest")
         << QString::fromUtf8("-w") << QString::fromStdString(writer->getScriptName()) << overridden
         << QString::fromUtf8("usdwrite-cli-test.ntpz")
         << QString::fromUtf8("1-10");
    CLArgs cl(args, true /*forceBackground*/);
    ASSERT_FALSE(cl.getError().has_value());

    std::list<AppInstance::RenderWork> works;
    getApp()->getWritersWorkForCL(cl, works);
    ASSERT_EQ((std::size_t)1, works.size());
    EXPECT_EQ(1, works.front().firstFrame);
    EXPECT_EQ(10, works.front().lastFrame);

    getApp()->startWritersRendering(eRenderModeBlocking, works);
    EXPECT_EQ((U64)1, getWriter(writer)->getWriteCount());
    EXPECT_TRUE(QFile::exists(overridden));
    EXPECT_FALSE(QFile::exists(original));
    PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::FindOrOpen(overridden.toStdString());
    ASSERT_TRUE(static_cast<bool>(layer));
    EXPECT_EQ(1., layer->GetStartTimeCode());
    EXPECT_EQ(10., layer->GetEndTimeCode());
}

// The whole scene vocabulary in one headless graph, written out:
//
//   USDRead -> USDEditTransform ----------------------------\
//                                                        USDMerge -> USDPrune -> USDWrite
//   USDLight ------------------\
//                          USDXformPrim ---------------/
//   USDMaterial -> geometry --/
//
// Every node's opinions reach the written file: Layered mode flattens what the nodes authored
// into it and keeps the read file as its sublayer.
TEST_F(SceneIntegrationTest, SceneVocabularyGraphExportsEveryNodesOpinions)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr reader = createReader(kReferenceScene);
    NodePtr transform = createNode(QString::fromUtf8(PLUGINID_NATRON_USDEDITTRANSFORM));
    NodePtr props = createNode(QString::fromUtf8(PLUGINID_NATRON_USDXFORMPRIM));
    NodePtr light = createNode(QString::fromUtf8(PLUGINID_NATRON_USDLIGHT));
    NodePtr material = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMATERIAL));
    NodePtr card = createNode(QString::fromUtf8(kTestPluginIDSceneGeometry));
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
    NodePtr prune = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPRUNE));
    ASSERT_TRUE(reader && transform && props && light && material && card && merge && prune);

    const auto setString = [](const NodePtr& node, const char* name, const char* value) {
        KnobString* knob = getKnob<KnobString>(node, name);
        ASSERT_TRUE(knob != NULL) << name;
        knob->setValue(std::string(value));
    };
    const auto setTranslate = [](const NodePtr& node, int dimension, double value) {
        KnobDouble* knob = getKnob<KnobDouble>(node, kSceneParamTranslate);
        ASSERT_TRUE(knob != NULL);
        knob->setValue(value, ViewSpec::all(), dimension);
    };

    // The box lifted by 5.
    setString(transform, kUSDEditTransformParamPrimPattern, "/World/Box");
    setTranslate(transform, 1, 5.);
    // A props xform at x = 7 holding a light and a card, the card carrying a red textured
    // material under its own prim.
    setString(props, kSceneParamPrimPath, "/World/Props");
    setTranslate(props, 0, 7.);
    setString(light, kSceneParamPrimPath, "/Key");
    setString(card, kSceneParamPrimPath, "/Card");
    setString(material, kSceneParamPrimPath, "/Red");
    KnobColor* diffuse = getKnob<KnobColor>(material, kUSDMaterialParamDiffuseColor);
    KnobFile* texture = getKnob<KnobFile>(material, kUSDMaterialParamDiffuseTexture);
    ASSERT_TRUE(diffuse && texture);
    diffuse->setValue(1., ViewSpec::all(), 0);
    diffuse->setValue(0., ViewSpec::all(), 1);
    diffuse->setValue(0., ViewSpec::all(), 2);
    const QString texturePath = tmp.path() + QString::fromUtf8("/textures/red.png");
    texture->setValue(texturePath.toStdString());
    // The camera deactivated.
    setString(prune, kUSDPruneParamPrimPattern, "/World/Cam");

    ASSERT_TRUE(transform->connectInput(reader, 0));
    ASSERT_TRUE(card->connectInput(material, 0));
    ASSERT_TRUE(props->connectInput(light, 0));
    ASSERT_TRUE(props->connectInput(card, 1));
    ASSERT_TRUE(merge->connectInput(transform, 0));
    ASSERT_TRUE(merge->connectInput(props, 1));
    ASSERT_TRUE(prune->connectInput(merge, 0));

    const QString path = tmp.path() + QString::fromUtf8("/out/scene.usda");
    NodePtr writer = createWriter(prune, path, USDWrite::eExportModeLayered, USDWrite::ePathModeRelative, 1, 24);
    ASSERT_TRUE(writer && getWriter(writer));
    render(writer);
    ASSERT_EQ((U64)1, getWriter(writer)->getWriteCount());

    // The written layer stack: the flattened node opinions over the read file.
    PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::FindOrOpen(path.toStdString());
    ASSERT_TRUE(static_cast<bool>(layer));
    const std::vector<std::string> subLayers = layer->GetSubLayerPaths();
    ASSERT_EQ((std::size_t)1, subLayers.size());
    EXPECT_TRUE(QDir::isRelativePath(QString::fromStdString(subLayers[0]))) << subLayers[0];
    // USDRead: the file's prims come from the sublayer, not from copies.
    EXPECT_FALSE(layer->GetAttributeAtPath(PXR_NS::SdfPath("/World/Box.size")));
    // USDEditTransform: an xform op on the box.
    PXR_NS::SdfPrimSpecHandle box = layer->GetPrimAtPath(PXR_NS::SdfPath("/World/Box"));
    ASSERT_TRUE(static_cast<bool>(box));
    EXPECT_TRUE(layer->GetAttributeAtPath(PXR_NS::SdfPath("/World/Box.xformOpOrder")));
    // USDMaterial: the binding on the card, and the texture relative to the written file.
    EXPECT_TRUE(layer->GetRelationshipAtPath(PXR_NS::SdfPath("/World/Props/Card.material:binding")));
    PXR_NS::SdfAttributeSpecHandle textureFile = layer->GetAttributeAtPath(PXR_NS::SdfPath("/World/Props/Card/Red/diffuseColorTexture.inputs:file"));
    ASSERT_TRUE(static_cast<bool>(textureFile));
    EXPECT_EQ(std::string("../textures/red.png"), textureFile->GetDefaultValue().Get<PXR_NS::SdfAssetPath>().GetAssetPath());
    // USDXformPrim and USDLight: their prims defined.
    PXR_NS::SdfPrimSpecHandle propsSpec = layer->GetPrimAtPath(PXR_NS::SdfPath("/World/Props"));
    PXR_NS::SdfPrimSpecHandle keySpec = layer->GetPrimAtPath(PXR_NS::SdfPath("/World/Props/Key"));
    ASSERT_TRUE(propsSpec && keySpec);
    EXPECT_EQ(PXR_NS::SdfSpecifierDef, propsSpec->GetSpecifier());
    EXPECT_EQ(PXR_NS::SdfSpecifierDef, keySpec->GetSpecifier());
    // USDPrune: the camera deactivated.
    PXR_NS::SdfPrimSpecHandle camera = layer->GetPrimAtPath(PXR_NS::SdfPath("/World/Cam"));
    ASSERT_TRUE(static_cast<bool>(camera));
    EXPECT_TRUE(camera->HasActive());
    EXPECT_FALSE(camera->GetActive());

    // And composed, the file holds the scene the graph built.
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(layer);
    ASSERT_TRUE(static_cast<bool>(stage));
    const PXR_NS::UsdPrim boxPrim = stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Box"));
    ASSERT_TRUE(bool(boxPrim));
    EXPECT_EQ(PXR_NS::TfToken("Cube"), boxPrim.GetTypeName());
    EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Ball")).IsValid());
    EXPECT_FALSE(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Cam")).IsActive());
    EXPECT_EQ(PXR_NS::TfToken("Xform"), stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Props")).GetTypeName());
    EXPECT_EQ(PXR_NS::TfToken("DistantLight"), stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Props/Key")).GetTypeName());
    EXPECT_EQ(PXR_NS::TfToken("Material"), stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Props/Card/Red")).GetTypeName());

    const PXR_NS::GfVec3d boxAt = PXR_NS::UsdGeomXformable(boxPrim).ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode(1.)).ExtractTranslation();
    EXPECT_NEAR(5., boxAt[1], 1e-9);
    const PXR_NS::GfVec3d keyAt = PXR_NS::UsdGeomXformable(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Props/Key")))
                                      .ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode(1.))
                                      .ExtractTranslation();
    EXPECT_NEAR(7., keyAt[0], 1e-9);
    const PXR_NS::UsdShadeMaterial bound = PXR_NS::UsdShadeMaterialBindingAPI(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Props/Card"))).ComputeBoundMaterial();
    ASSERT_TRUE(bool(bound));
    EXPECT_EQ(PXR_NS::SdfPath("/World/Props/Card/Red"), bound.GetPath());
}
