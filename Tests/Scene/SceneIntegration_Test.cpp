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

#include "Engine/AppInstance.h"
#include "Engine/CLArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/StageCache.h"
#include "Engine/Nodes/Scene/USDCamera.h"
#include "Engine/Nodes/Scene/USDLight.h"
#include "Engine/Nodes/Scene/USDMaterial.h"
#include "Engine/Nodes/Scene/USDMerge.h"
#include "Engine/Nodes/Scene/USDPrim.h"
#include "Engine/Nodes/Scene/USDPrune.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDTransform.h"
#include "Engine/Nodes/Scene/USDWrite.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

// The headless end-to-end path a viewer takes: USDRead, the scene action through a Dot, the
// composed stage from the StageCache, and what the stage holds. If USD's libraries or plug-ins
// fail to load at run time, it fails here rather than in the GUI.
class SceneIntegrationTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        StageCache::instance().clear();

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
        getApp()->startWritersRendering(true, works);
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

    PXR_NS::UsdStageRefPtr stage = StageCache::instance().getStage(payload);
    ASSERT_TRUE(bool(stage));
    // The cache hands every consumer of the stack the same stage.
    EXPECT_EQ(stage, StageCache::instance().getStage(payload));

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

TEST_F(SceneIntegrationTest, CreatedPrimsLayerOverTheReadScene)
{
    NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
    NodePtr rig = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPRIM));
    NodePtr camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
    NodePtr light = createNode(QString::fromUtf8(PLUGINID_NATRON_USDLIGHT));
    ASSERT_TRUE(reader && rig && camera && light);
    KnobFile* file = dynamic_cast<KnobFile*>(reader->getKnobByName(kUSDReadParamFile).get());
    ASSERT_TRUE(file != NULL);
    file->setValue(NATRON_TESTS_FIXTURES_DIR "/scene-reference.usda");

    // A rig xform under the read scene's root, with the camera and a light inside it.
    const auto setPrimPath = [](const NodePtr& node, const char* path) {
        KnobString* primPath = dynamic_cast<KnobString*>(node->getKnobByName(kSceneParamPrimPath).get());
        ASSERT_TRUE(primPath != NULL);
        primPath->setValue(std::string(path));
    };
    setPrimPath(rig, "/World/Rig");
    setPrimPath(camera, "/World/Rig/Shot");
    setPrimPath(light, "/World/Rig/Key");
    KnobDouble* rigTranslate = dynamic_cast<KnobDouble*>(rig->getKnobByName(kSceneParamTranslate).get());
    KnobDouble* cameraTranslate = dynamic_cast<KnobDouble*>(camera->getKnobByName(kSceneParamTranslate).get());
    ASSERT_TRUE(rigTranslate && cameraTranslate);
    rigTranslate->setValue(100., ViewSpec::all(), 0);
    cameraTranslate->setValue(5., ViewSpec::all(), 2);

    ASSERT_TRUE(rig->connectInput(reader, 0));
    ASSERT_TRUE(camera->connectInput(rig, 0));
    ASSERT_TRUE(light->connectInput(camera, 0));

    ScenePayload payload;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              light->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &payload, &error))
        << error;
    EXPECT_EQ((std::size_t)4, payload.getNumLayers());
    // The read scene's metadata passes through the created layers.
    EXPECT_EQ(SceneMetadata::eUpAxisY, payload.getMetadata().upAxis);
    EXPECT_EQ(std::string("World"), payload.getMetadata().defaultPrim);

    PXR_NS::UsdStageRefPtr stage = StageCache::instance().getStage(payload);
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
    NodePtr rig = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPRIM));
    NodePtr prune = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPRUNE));
    ASSERT_TRUE(reader && rig && prune);
    KnobString* rigPath = getKnob<KnobString>(rig, kSceneParamPrimPath);
    KnobDouble* rigTranslate = getKnob<KnobDouble>(rig, kSceneParamTranslate);
    KnobString* prunePattern = getKnob<KnobString>(prune, kUSDPruneParamPrimPattern);
    ASSERT_TRUE(rigPath && rigTranslate && prunePattern);
    rigPath->setValue(std::string("/World/Rig"));
    rigTranslate->setValue(100., ViewSpec::all(), 0);
    prunePattern->setValue(std::string("/World/Cam"));
    ASSERT_TRUE(rig->connectInput(reader, 0));
    ASSERT_TRUE(prune->connectInput(rig, 0));

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
        ASSERT_TRUE(layer);
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
        ASSERT_TRUE(stage);
        EXPECT_EQ(PXR_NS::TfToken("Cube"), stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Box")).GetTypeName());
    }
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
    ASSERT_TRUE(layer);
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

    getApp()->startWritersRendering(true, works);
    EXPECT_EQ((U64)1, getWriter(writer)->getWriteCount());
    EXPECT_TRUE(QFile::exists(overridden));
    EXPECT_FALSE(QFile::exists(original));
    PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::FindOrOpen(overridden.toStdString());
    ASSERT_TRUE(layer);
    EXPECT_EQ(1., layer->GetStartTimeCode());
    EXPECT_EQ(10., layer->GetEndTimeCode());
}

// The whole scene vocabulary in one headless graph, written out:
//
//   USDRead -> USDTransform ----------------------------\
//                                                        USDMerge -> USDPrune -> USDWrite
//   USDPrim -> USDLight -> USDMaterial -----------------/
//
// Every node's opinions reach the written file: Layered mode flattens what the nodes authored
// into it and keeps the read file as its sublayer.
TEST_F(SceneIntegrationTest, SceneVocabularyGraphExportsEveryNodesOpinions)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    NodePtr reader = createReader(kReferenceScene);
    NodePtr transform = createNode(QString::fromUtf8(PLUGINID_NATRON_USDTRANSFORM));
    NodePtr props = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPRIM));
    NodePtr light = createNode(QString::fromUtf8(PLUGINID_NATRON_USDLIGHT));
    NodePtr material = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMATERIAL));
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
    NodePtr prune = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPRUNE));
    ASSERT_TRUE(reader && transform && props && light && material && merge && prune);

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
    setString(transform, kUSDTransformParamPrimPattern, "/World/Box");
    setTranslate(transform, 1, 5.);
    // A props xform at x = 7 holding a light, and a red textured material bound to it: USDMaterial
    // binds what its own input holds.
    setString(props, kSceneParamPrimPath, "/World/Props");
    setTranslate(props, 0, 7.);
    setString(light, kSceneParamPrimPath, "/World/Props/Key");
    setString(material, kSceneParamPrimPath, "/World/Looks/Red");
    setString(material, kUSDMaterialParamPrimPattern, "/World/Props");
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
    ASSERT_TRUE(light->connectInput(props, 0));
    ASSERT_TRUE(material->connectInput(light, 0));
    ASSERT_TRUE(merge->connectInput(transform, 0));
    ASSERT_TRUE(merge->connectInput(material, 1));
    ASSERT_TRUE(prune->connectInput(merge, 0));

    const QString path = tmp.path() + QString::fromUtf8("/out/scene.usda");
    NodePtr writer = createWriter(prune, path, USDWrite::eExportModeLayered, USDWrite::ePathModeRelative, 1, 24);
    ASSERT_TRUE(writer && getWriter(writer));
    render(writer);
    ASSERT_EQ((U64)1, getWriter(writer)->getWriteCount());

    // The written layer stack: the flattened node opinions over the read file.
    PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::FindOrOpen(path.toStdString());
    ASSERT_TRUE(layer);
    const std::vector<std::string> subLayers = layer->GetSubLayerPaths();
    ASSERT_EQ((std::size_t)1, subLayers.size());
    EXPECT_TRUE(QDir::isRelativePath(QString::fromStdString(subLayers[0]))) << subLayers[0];
    // USDRead: the file's prims come from the sublayer, not from copies.
    EXPECT_FALSE(layer->GetAttributeAtPath(PXR_NS::SdfPath("/World/Box.size")));
    // USDTransform: an xform op on the box.
    PXR_NS::SdfPrimSpecHandle box = layer->GetPrimAtPath(PXR_NS::SdfPath("/World/Box"));
    ASSERT_TRUE(box);
    EXPECT_TRUE(layer->GetAttributeAtPath(PXR_NS::SdfPath("/World/Box.xformOpOrder")));
    // USDMaterial: the binding on the props, and the texture relative to the written file.
    EXPECT_TRUE(layer->GetRelationshipAtPath(PXR_NS::SdfPath("/World/Props.material:binding")));
    PXR_NS::SdfAttributeSpecHandle textureFile = layer->GetAttributeAtPath(PXR_NS::SdfPath("/World/Looks/Red/diffuseColorTexture.inputs:file"));
    ASSERT_TRUE(textureFile);
    EXPECT_EQ(std::string("../textures/red.png"), textureFile->GetDefaultValue().Get<PXR_NS::SdfAssetPath>().GetAssetPath());
    // USDPrim and USDLight: their prims defined.
    PXR_NS::SdfPrimSpecHandle propsSpec = layer->GetPrimAtPath(PXR_NS::SdfPath("/World/Props"));
    PXR_NS::SdfPrimSpecHandle keySpec = layer->GetPrimAtPath(PXR_NS::SdfPath("/World/Props/Key"));
    ASSERT_TRUE(propsSpec && keySpec);
    EXPECT_EQ(PXR_NS::SdfSpecifierDef, propsSpec->GetSpecifier());
    EXPECT_EQ(PXR_NS::SdfSpecifierDef, keySpec->GetSpecifier());
    // USDPrune: the camera deactivated.
    PXR_NS::SdfPrimSpecHandle camera = layer->GetPrimAtPath(PXR_NS::SdfPath("/World/Cam"));
    ASSERT_TRUE(camera);
    EXPECT_TRUE(camera->HasActive());
    EXPECT_FALSE(camera->GetActive());

    // And composed, the file holds the scene the graph built.
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(layer);
    ASSERT_TRUE(stage);
    const PXR_NS::UsdPrim boxPrim = stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Box"));
    ASSERT_TRUE(bool(boxPrim));
    EXPECT_EQ(PXR_NS::TfToken("Cube"), boxPrim.GetTypeName());
    EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Ball")).IsValid());
    EXPECT_FALSE(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Cam")).IsActive());
    EXPECT_EQ(PXR_NS::TfToken("Xform"), stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Props")).GetTypeName());
    EXPECT_EQ(PXR_NS::TfToken("DistantLight"), stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Props/Key")).GetTypeName());
    EXPECT_EQ(PXR_NS::TfToken("Material"), stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Looks/Red")).GetTypeName());

    const PXR_NS::GfVec3d boxAt = PXR_NS::UsdGeomXformable(boxPrim).ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode(1.)).ExtractTranslation();
    EXPECT_NEAR(5., boxAt[1], 1e-9);
    const PXR_NS::GfVec3d keyAt = PXR_NS::UsdGeomXformable(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Props/Key")))
                                      .ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode(1.))
                                      .ExtractTranslation();
    EXPECT_NEAR(7., keyAt[0], 1e-9);
    const PXR_NS::UsdShadeMaterial bound = PXR_NS::UsdShadeMaterialBindingAPI(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Props"))).ComputeBoundMaterial();
    ASSERT_TRUE(bool(bound));
    EXPECT_EQ(PXR_NS::SdfPath("/World/Looks/Red"), bound.GetPath());
}
