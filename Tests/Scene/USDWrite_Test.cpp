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
#include <pxr/usd/usdGeom/basisCurves.h>
#include <pxr/usd/usdSkel/animation.h>

#include "Global/Macros.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "SceneTestEffect.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Scene/LiveTextureStore.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneProjectionCameras.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDAssignMaterial.h"
#include "Engine/Nodes/Scene/USDCamera.h"
#include "Engine/Nodes/Scene/USDCard.h"
#include "Engine/Nodes/Scene/USDMaterial.h"
#include "Engine/Nodes/Scene/USDMerge.h"
#include "Engine/Nodes/Scene/USDProject3D.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDWrite.h"
#include "Engine/Nodes/Scene/USDXformPrim.h"
#include "Engine/Project.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

#include <OpenImageIO/imageio.h>

NATRON_NAMESPACE_USING

namespace {
const char* const kReferenceScene = NATRON_TESTS_FIXTURES_DIR "/scene-reference.usda";

// An animated scene at 48 time codes per second, against the project's 24 frames per second.
const char* const kFastScene = "#usda 1.0\n"
                               "(\n"
                               "    defaultPrim = \"World\"\n"
                               "    timeCodesPerSecond = 48\n"
                               "    upAxis = \"Y\"\n"
                               ")\n"
                               "def Xform \"World\"\n{\n"
                               "    def Sphere \"Ball\"\n    {\n"
                               "        double3 xformOp:translate.timeSamples = {\n"
                               "            0: (0, 0, 0),\n"
                               "            96: (0, 4, 0),\n"
                               "        }\n"
                               "        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                               "    }\n}\n";

// A Z-up centimetre file: a mesh from (0, 0, 0) to (100, 200, 300) and a camera, under an Xform.
const char* const kZUpCentimetreScene = "#usda 1.0\n"
                                        "(\n"
                                        "    defaultPrim = \"World\"\n"
                                        "    metersPerUnit = 0.01\n"
                                        "    upAxis = \"Z\"\n"
                                        ")\n"
                                        "def Xform \"World\"\n{\n"
                                        "    double3 xformOp:translate = (0, 0, 50)\n"
                                        "    uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                                        "    def Mesh \"Box\"\n    {\n"
                                        "        point3f[] points = [(0, 0, 0), (100, 200, 300)]\n"
                                        "        int[] faceVertexCounts = [2]\n"
                                        "        int[] faceVertexIndices = [0, 1]\n"
                                        "    }\n"
                                        "    def Camera \"Cam\"\n    {\n"
                                        "        float2 clippingRange = (1, 100000)\n"
                                        "    }\n}\n";

PXR_NS::GfRange3d
worldBounds(const PXR_NS::UsdStageRefPtr& stage,
            const char* path)
{
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

QString
cleanAbsolute(const QString& path)
{
    return QDir::cleanPath(QFileInfo(path).absoluteFilePath());
}

QStringList
fileNames(const QString& dir)
{
    return QDir(dir).entryList(QDir::Files | QDir::Hidden, QDir::Name);
}

std::map<QString, QByteArray>
fileContents(const QString& dir)
{
    std::map<QString, QByteArray> contents;

    for (const QString& name : fileNames(dir)) {
        QFile f(QDir(dir).filePath(name));
        EXPECT_TRUE(f.open(QIODevice::ReadOnly)) << name.toStdString();
        contents[name] = f.readAll();
    }

    return contents;
}

void
writeTextFile(const QString& path,
              const char* content)
{
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate)) << path.toStdString();
    f.write(content);
}
} // anonymous namespace

// The graph of a read scene and a card each placed under a prim of their own, merged and written:
//
//   USDRead ----------------> USDXformPrim /Transform1 --\
//                                                      USDMerge -> USDWrite
//   USDMaterial -> Card ----> USDXformPrim /Transform2 --/
//
// Each USDXformPrim references the layer stack of its input from an anonymous layer, and the card
// so references its material's.
class USDWriteTest
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

    static void setString(const NodePtr& node,
                          const char* name,
                          const char* value)
    {
        KnobString* knob = getKnob<KnobString>(node, name);
        ASSERT_TRUE(knob != NULL) << name;
        knob->setValue(std::string(value));
    }

    // The example graph reading path with frameOffset; returns its USDMerge.
    NodePtr createGraph(const std::string& path,
                        double frameOffset)
    {
        NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
        NodePtr transform1 = createNode(QString::fromUtf8(PLUGINID_NATRON_USDXFORMPRIM));
        NodePtr material = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMATERIAL));
        NodePtr card = createNode(QString::fromUtf8(kTestPluginIDSceneGeometry));
        NodePtr transform2 = createNode(QString::fromUtf8(PLUGINID_NATRON_USDXFORMPRIM));
        NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
        EXPECT_TRUE(reader && transform1 && material && card && transform2 && merge);
        if (!(reader && transform1 && material && card && transform2 && merge)) {
            return NodePtr();
        }

        KnobFile* file = getKnob<KnobFile>(reader, kUSDReadParamFile);
        KnobDouble* offset = getKnob<KnobDouble>(reader, kUSDReadParamFrameOffset);
        KnobDouble* translate1 = getKnob<KnobDouble>(transform1, kSceneParamTranslate);
        KnobDouble* translate2 = getKnob<KnobDouble>(transform2, kSceneParamTranslate);
        KnobColor* diffuse = getKnob<KnobColor>(material, kUSDMaterialParamDiffuseColor);
        EXPECT_TRUE(file && offset && translate1 && translate2 && diffuse);
        if (!(file && offset && translate1 && translate2 && diffuse)) {
            return NodePtr();
        }
        file->setValue(path);
        offset->setValue(frameOffset);
        setString(transform1, kSceneParamPrimPath, "/Transform1");
        translate1->setValue(10., ViewSpec::all(), 0);
        setString(transform2, kSceneParamPrimPath, "/Transform2");
        translate2->setValue(-10., ViewSpec::all(), 0);
        setString(card, kSceneParamPrimPath, "/Card");
        setString(material, kSceneParamPrimPath, "/Red");
        diffuse->setValue(1., ViewSpec::all(), 0);
        diffuse->setValue(0., ViewSpec::all(), 1);
        diffuse->setValue(0., ViewSpec::all(), 2);

        EXPECT_TRUE(transform1->connectInput(reader, 0));
        EXPECT_TRUE(card->connectInput(material, 0));
        EXPECT_TRUE(transform2->connectInput(card, 0));
        EXPECT_TRUE(merge->connectInput(transform1, 0));
        EXPECT_TRUE(merge->connectInput(transform2, 1));

        return merge;
    }

    NodePtr createWriter(const NodePtr& input,
                         const QString& path,
                         USDWrite::ExportModeEnum exportMode,
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
            pathModeKnob->setValue((int)USDWrite::ePathModeRelative);
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

    // A write runs on a render thread, and the live textures it exports are not rendered on the
    // main thread, which the tests run on.
    static StatusEnum writeOffMainThread(const NodePtr& node,
                                         int firstFrame,
                                         int lastFrame)
    {
        StatusEnum status = eStatusFailed;
        std::thread renderThread([&]() { status = getWriter(node)->writeFrameRange(firstFrame, lastFrame, ViewIdx(0)); });
        renderThread.join();

        return status;
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

    static std::string exportStage(const PXR_NS::UsdStageRefPtr& stage)
    {
        std::string text;
        EXPECT_TRUE(stage && stage->ExportToString(&text, false));

        return text;
    }

    // The composed prim tree path's USDRead composes.
    std::string readBack(const QString& path)
    {
        NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
        KnobFile* file = reader ? getKnob<KnobFile>(reader, kUSDReadParamFile) : NULL;
        EXPECT_TRUE(file != NULL);
        if (!file) {
            return std::string();
        }
        file->setValue(path.toStdString());

        return exportStage(composeOutput(reader));
    }
};

// Written in either mode and read back, the scene composes the prim tree it had, with the read
// file's time offset and a time code rate other than the project's carried through the
// references.
TEST_F(USDWriteTest, GraftedScenesRoundTripInBothModes)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString fastPath = tmp.path() + QString::fromUtf8("/fast.usda");
    writeTextFile(fastPath, kFastScene);

    struct Case {
        std::string path;
        double frameOffset;
        const char* name;
    };
    const Case cases[] = {
        { kReferenceScene, 3., "reference" },
        { fastPath.toStdString(), 2., "fast" },
    };
    for (const Case& c : cases) {
        NodePtr merge = createGraph(c.path, c.frameOffset);
        ASSERT_TRUE(bool(merge));
        const std::string original = exportStage(composeOutput(merge));
        ASSERT_FALSE(original.empty());
        ASSERT_NE(std::string::npos, original.find("Transform1")) << original;
        ASSERT_NE(std::string::npos, original.find("Red")) << original;

        const USDWrite::ExportModeEnum modes[] = { USDWrite::eExportModeLayered, USDWrite::eExportModeFlattened };
        for (USDWrite::ExportModeEnum mode : modes) {
            const QString path = tmp.path() + QString::fromUtf8("/%1/%2").arg(QString::fromUtf8(c.name)).arg(QString::fromUtf8(mode == USDWrite::eExportModeLayered ? "layered.usda" : "flattened.usdc"));
            NodePtr writer = createWriter(merge, path, mode, -10, 100);
            ASSERT_TRUE(writer && getWriter(writer));
            ASSERT_EQ(eStatusOK, getWriter(writer)->writeFrameRange(-10, 100, ViewIdx(0)));
            ASSERT_TRUE(QFile::exists(path));

            EXPECT_EQ(original, readBack(path)) << c.name << " mode " << (int)mode;

            // A fresh stage on the written file composes the same prims, bound as they were.
            PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(path.toStdString());
            ASSERT_TRUE(static_cast<bool>(stage));
            EXPECT_EQ(PXR_NS::TfToken("Sphere"), stage->GetPrimAtPath(PXR_NS::SdfPath("/Transform1/World/Ball")).GetTypeName());
            EXPECT_EQ(PXR_NS::TfToken("Cube"), stage->GetPrimAtPath(PXR_NS::SdfPath("/Transform2/Card")).GetTypeName());
            const PXR_NS::UsdShadeMaterial bound = PXR_NS::UsdShadeMaterialBindingAPI(stage->GetPrimAtPath(PXR_NS::SdfPath("/Transform2/Card"))).ComputeBoundMaterial();
            ASSERT_TRUE(static_cast<bool>(bound));
            EXPECT_EQ(PXR_NS::SdfPath("/Transform2/Card/Red"), bound.GetPath());
        }
    }
}

// Layered mode writes the layer each reference names beside the written file and points the
// reference at it, relative to the written file: no written layer names an anonymous layer, and
// the read file stays a sublayer, referred to rather than copied.
TEST_F(USDWriteTest, LayeredWritesReferencedLayersAsSidecarsAndKeepsTheReadFile)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    NodePtr merge = createGraph(kReferenceScene, 0.);
    ASSERT_TRUE(bool(merge));
    const QString outDir = tmp.path() + QString::fromUtf8("/out");
    const QString path = outDir + QString::fromUtf8("/scene.usda");
    NodePtr writer = createWriter(merge, path, USDWrite::eExportModeLayered, 1, 24);
    ASSERT_TRUE(writer && getWriter(writer));
    ASSERT_EQ(eStatusOK, getWriter(writer)->writeFrameRange(1, 24, ViewIdx(0)));

    // The two inputs of the USDXformPrims, and the card's material.
    const QStringList expected = QStringList() << QString::fromUtf8("scene.usda") << QString::fromUtf8("scene_layer1.usda")
                                               << QString::fromUtf8("scene_layer2.usda") << QString::fromUtf8("scene_layer3.usda");
    ASSERT_EQ(expected, fileNames(outDir));

    const QString fixture = cleanAbsolute(QString::fromUtf8(kReferenceScene));
    int fixtureSublayers = 0;
    for (const QString& name : expected) {
        PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::FindOrOpen(QDir(outDir).filePath(name).toStdString());
        ASSERT_TRUE(static_cast<bool>(layer)) << name.toStdString();
        std::string text;
        ASSERT_TRUE(layer->ExportToString(&text));
        EXPECT_EQ(std::string::npos, text.find("anon:")) << name.toStdString() << "\n"
                                                         << text;
        // Referred to, not copied.
        EXPECT_FALSE(static_cast<bool>(layer->GetPrimAtPath(PXR_NS::SdfPath("/World/Box")))) << name.toStdString();
        EXPECT_FALSE(static_cast<bool>(layer->GetPrimAtPath(PXR_NS::SdfPath("/Transform1/World/Box")))) << name.toStdString();
        for (const std::string& subLayer : layer->GetSubLayerPaths()) {
            const QString sub = QString::fromStdString(subLayer);
            EXPECT_TRUE(QDir::isRelativePath(sub)) << subLayer;
            if (QDir::cleanPath(QDir(outDir).absoluteFilePath(sub)) == fixture) {
                ++fixtureSublayers;
            }
        }
    }
    EXPECT_EQ(1, fixtureSublayers);

    // The written file references its sidecars by relative path.
    PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::FindOrOpen(path.toStdString());
    ASSERT_TRUE(static_cast<bool>(layer));
    const char* const grafted[] = { "/Transform1/World", "/Transform2/Card" };
    for (const char* graftedPath : grafted) {
        PXR_NS::SdfPrimSpecHandle spec = layer->GetPrimAtPath(PXR_NS::SdfPath(graftedPath));
        ASSERT_TRUE(static_cast<bool>(spec)) << graftedPath;
        EXPECT_EQ(PXR_NS::SdfSpecifierDef, spec->GetSpecifier());
        PXR_NS::SdfReferenceListOp referenceList;
        ASSERT_TRUE(layer->HasField(spec->GetPath(), PXR_NS::SdfFieldKeys->References, &referenceList)) << graftedPath;
        const std::vector<PXR_NS::SdfReference> references = referenceList.GetAppliedItems();
        ASSERT_EQ((std::size_t)1, references.size()) << graftedPath;
        const QString asset = QString::fromStdString(references[0].GetAssetPath());
        EXPECT_TRUE(asset.startsWith(QString::fromUtf8("./scene_layer"))) << references[0].GetAssetPath();
    }

    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(layer);
    ASSERT_TRUE(static_cast<bool>(stage));
    EXPECT_EQ(PXR_NS::TfToken("Cube"), stage->GetPrimAtPath(PXR_NS::SdfPath("/Transform1/World/Box")).GetTypeName());
    EXPECT_EQ(PXR_NS::TfToken("Material"), stage->GetPrimAtPath(PXR_NS::SdfPath("/Transform2/Card/Red")).GetTypeName());
    const PXR_NS::GfVec3d boxAt = PXR_NS::UsdGeomXformable(stage->GetPrimAtPath(PXR_NS::SdfPath("/Transform1/World/Box")))
                                      .ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode(1.))
                                      .ExtractTranslation();
    EXPECT_NEAR(10., boxAt[0], 1e-9);
}

// The sidecar names follow the scene: writing it again writes the same files, byte for byte, and
// removes sidecars an earlier write left that this one does not produce. A flattened write to the
// same name removes them all, with a warning.
TEST_F(USDWriteTest, SidecarsAreStableAndStaleOnesAreRemoved)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    NodePtr merge = createGraph(kReferenceScene, 0.);
    ASSERT_TRUE(bool(merge));
    const QString outDir = tmp.path() + QString::fromUtf8("/out");
    ASSERT_TRUE(QDir().mkpath(outDir));
    writeTextFile(outDir + QString::fromUtf8("/notes.txt"), "keep\n");
    writeTextFile(outDir + QString::fromUtf8("/scene_layer7.usda"), "#usda 1.0\n");
    const QString path = outDir + QString::fromUtf8("/scene.usda");

    NodePtr writer = createWriter(merge, path, USDWrite::eExportModeLayered, 1, 24);
    ASSERT_TRUE(writer && getWriter(writer));
    ASSERT_EQ(eStatusOK, getWriter(writer)->writeFrameRange(1, 24, ViewIdx(0)));
    const QStringList expected = QStringList() << QString::fromUtf8("notes.txt") << QString::fromUtf8("scene.usda") << QString::fromUtf8("scene_layer1.usda")
                                               << QString::fromUtf8("scene_layer2.usda") << QString::fromUtf8("scene_layer3.usda");
    ASSERT_EQ(expected, fileNames(outDir));
    EXPECT_FALSE(writer->hasPersistentMessage());
    const std::map<QString, QByteArray> first = fileContents(outDir);

    ASSERT_EQ(eStatusOK, getWriter(writer)->writeFrameRange(1, 24, ViewIdx(0)));
    EXPECT_EQ(expected, fileNames(outDir));
    EXPECT_TRUE(first == fileContents(outDir));

    NodePtr again = createWriter(merge, path, USDWrite::eExportModeLayered, 1, 24);
    ASSERT_TRUE(again && getWriter(again));
    ASSERT_EQ(eStatusOK, getWriter(again)->writeFrameRange(1, 24, ViewIdx(0)));
    EXPECT_TRUE(first == fileContents(outDir));

    NodePtr flattened = createWriter(merge, path, USDWrite::eExportModeFlattened, 1, 24);
    ASSERT_TRUE(flattened && getWriter(flattened));
    ASSERT_EQ(eStatusOK, getWriter(flattened)->writeFrameRange(1, 24, ViewIdx(0)));
    EXPECT_EQ(QStringList() << QString::fromUtf8("notes.txt") << QString::fromUtf8("scene.usda"), fileNames(outDir));
    ASSERT_TRUE(flattened->hasPersistentMessage());
    QString message;
    int type = 0;
    flattened->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("scene_layer3.usda"))) << message.toStdString();

    // The flattened file stands on its own.
    PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::FindOrOpen(path.toStdString());
    ASSERT_TRUE(static_cast<bool>(layer));
    EXPECT_TRUE(layer->GetSubLayerPaths().empty());
    EXPECT_TRUE(static_cast<bool>(layer->GetPrimAtPath(PXR_NS::SdfPath("/Transform1/World/Box"))));
    EXPECT_TRUE(static_cast<bool>(layer->GetPrimAtPath(PXR_NS::SdfPath("/Transform2/Card/Red"))));
}

// A shared material assigned and then moved with the asset under a USDXformPrim writes, in either
// mode, one material in the Materials Scope that the moved prims stay bound to, with no layer
// naming an anonymous one and, layered, every reference relative to the written file.
TEST_F(USDWriteTest, ASharedMaterialAssignmentRoundTripsInBothModes)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
    NodePtr material = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMATERIAL));
    NodePtr assign = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));
    NodePtr transform = createNode(QString::fromUtf8(PLUGINID_NATRON_USDXFORMPRIM));
    ASSERT_TRUE(reader && material && assign && transform);
    KnobFile* file = getKnob<KnobFile>(reader, kUSDReadParamFile);
    ASSERT_TRUE(file != NULL);
    file->setValue(std::string(kReferenceScene));
    setString(material, kSceneParamPrimPath, "/Material1");
    setString(assign, kUSDAssignMaterialParamPrimPattern, "/World/Box /World/Ball");
    setString(transform, kSceneParamPrimPath, "/T1");
    ASSERT_TRUE(assign->connectInput(reader, kUSDAssignMaterialInputScene));
    ASSERT_TRUE(assign->connectInput(material, kUSDAssignMaterialInputMaterial));
    ASSERT_TRUE(transform->connectInput(assign, 0));

    const char* const boundPrims[] = { "/T1/World/Box", "/T1/World/Ball" };
    const PXR_NS::UsdStageRefPtr original = composeOutput(transform);
    ASSERT_TRUE(static_cast<bool>(original));
    for (const char* prim : boundPrims) {
        const PXR_NS::UsdShadeMaterial bound = PXR_NS::UsdShadeMaterialBindingAPI(original->GetPrimAtPath(PXR_NS::SdfPath(prim))).ComputeBoundMaterial();
        ASSERT_TRUE(static_cast<bool>(bound)) << prim;
        ASSERT_EQ(PXR_NS::SdfPath("/materials/Material1"), bound.GetPath()) << prim;
    }

    const USDWrite::ExportModeEnum modes[] = { USDWrite::eExportModeLayered, USDWrite::eExportModeFlattened };
    for (USDWrite::ExportModeEnum mode : modes) {
        const bool layered = (mode == USDWrite::eExportModeLayered);
        const QString outDir = tmp.path() + QString::fromUtf8(layered ? "/layered" : "/flattened");
        const QString path = outDir + QString::fromUtf8(layered ? "/scene.usda" : "/scene.usdc");
        NodePtr writer = createWriter(transform, path, mode, 1, 24);
        ASSERT_TRUE(writer && getWriter(writer));
        ASSERT_EQ(eStatusOK, getWriter(writer)->writeFrameRange(1, 24, ViewIdx(0)));
        ASSERT_TRUE(QFile::exists(path));

        EXPECT_EQ(exportStage(original), readBack(path)) << "mode " << (int)mode;

        PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(path.toStdString());
        ASSERT_TRUE(static_cast<bool>(stage));
        // Exactly one copy of the material, in the scope, and none carried along under /T1.
        int materials = 0;
        for (const PXR_NS::UsdPrim& prim : stage->Traverse()) {
            if (prim.IsA<PXR_NS::UsdShadeMaterial>()) {
                ++materials;
                EXPECT_EQ(PXR_NS::SdfPath("/materials/Material1"), prim.GetPath());
            }
        }
        EXPECT_EQ(1, materials) << "mode " << (int)mode;
        for (const char* prim : boundPrims) {
            const PXR_NS::UsdShadeMaterial bound = PXR_NS::UsdShadeMaterialBindingAPI(stage->GetPrimAtPath(PXR_NS::SdfPath(prim))).ComputeBoundMaterial();
            ASSERT_TRUE(static_cast<bool>(bound)) << prim << " mode " << (int)mode;
            EXPECT_EQ(PXR_NS::SdfPath("/materials/Material1"), bound.GetPath()) << prim << " mode " << (int)mode;
        }

        for (const QString& name : fileNames(outDir)) {
            PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::FindOrOpen(QDir(outDir).filePath(name).toStdString());
            ASSERT_TRUE(static_cast<bool>(layer)) << name.toStdString();
            std::string text;
            ASSERT_TRUE(layer->ExportToString(&text));
            EXPECT_EQ(std::string::npos, text.find("anon:")) << name.toStdString() << "\n"
                                                             << text;
            if (!layered) {
                continue;
            }
            for (const PXR_NS::SdfPrimSpecHandle& root : layer->GetRootPrims()) {
                PXR_NS::SdfReferenceListOp referenceList;
                if (!layer->HasField(root->GetPath(), PXR_NS::SdfFieldKeys->References, &referenceList)) {
                    continue;
                }
                for (const PXR_NS::SdfReference& reference : referenceList.GetAppliedItems()) {
                    if (!reference.GetAssetPath().empty()) {
                        EXPECT_TRUE(QDir::isRelativePath(QString::fromStdString(reference.GetAssetPath()))) << reference.GetAssetPath();
                    }
                }
            }
        }
    }
}

// A read corrected to the project's metrics writes as it composes in both modes: flattened bakes
// the corrective ops in, layered keeps them over the file, which it still sublayers by relative
// path, and both carry the project's metrics. An uncorrected read keeps the file's.
TEST_F(USDWriteTest, CorrectedReadsRoundTripInBothModes)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString filePath = tmp.path() + QString::fromUtf8("/zcm.usda");
    writeTextFile(filePath, kZUpCentimetreScene);

    for (bool corrected : { true, false }) {
        NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
        ASSERT_TRUE(bool(reader));
        KnobFile* file = getKnob<KnobFile>(reader, kUSDReadParamFile);
        KnobBool* matchUpAxis = getKnob<KnobBool>(reader, kUSDReadParamMatchUpAxis);
        KnobBool* matchUnits = getKnob<KnobBool>(reader, kUSDReadParamMatchUnits);
        ASSERT_TRUE(file && matchUpAxis && matchUnits);
        file->setValue(filePath.toStdString());
        matchUpAxis->setValue(corrected);
        matchUnits->setValue(corrected);

        const std::string original = exportStage(composeOutput(reader));
        ASSERT_FALSE(original.empty());
        const PXR_NS::TfToken upAxis = corrected ? PXR_NS::UsdGeomTokens->y : PXR_NS::UsdGeomTokens->z;
        const double metersPerUnit = corrected ? 1. : 0.01;

        const USDWrite::ExportModeEnum modes[] = { USDWrite::eExportModeLayered, USDWrite::eExportModeFlattened };
        for (USDWrite::ExportModeEnum mode : modes) {
            const QString outDir = tmp.path() + QString::fromUtf8(corrected ? "/corrected" : "/uncorrected");
            const QString path = outDir + QString::fromUtf8(mode == USDWrite::eExportModeLayered ? "/layered.usda" : "/flattened.usda");
            NodePtr writer = createWriter(reader, path, mode, 1, 24);
            ASSERT_TRUE(writer && getWriter(writer));
            ASSERT_EQ(eStatusOK, getWriter(writer)->writeFrameRange(1, 24, ViewIdx(0)));
            ASSERT_TRUE(QFile::exists(path));

            EXPECT_EQ(original, readBack(path)) << "corrected " << corrected << " mode " << (int)mode;

            PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(path.toStdString());
            ASSERT_TRUE(static_cast<bool>(stage));
            EXPECT_EQ(upAxis, PXR_NS::UsdGeomGetStageUpAxis(stage));
            EXPECT_DOUBLE_EQ(metersPerUnit, PXR_NS::UsdGeomGetStageMetersPerUnit(stage));
            const PXR_NS::GfRange3d bounds = worldBounds(stage, "/World/Box");
            const PXR_NS::UsdGeomCamera camera(stage->GetPrimAtPath(PXR_NS::SdfPath("/World/Cam")));
            ASSERT_TRUE(static_cast<bool>(camera));
            PXR_NS::GfVec2f clippingRange;
            ASSERT_TRUE(camera.GetClippingRangeAttr().Get(&clippingRange));
            if (corrected) {
                // Scaled to metres, then Z-up turned Y-up: (x, y, z) becomes (x, z, -y).
                expectNear(PXR_NS::GfVec3d(0, 0.5, -2), bounds.GetMin());
                expectNear(PXR_NS::GfVec3d(1, 3.5, 0), bounds.GetMax());
                EXPECT_FLOAT_EQ(0.01f, clippingRange[0]);
                EXPECT_FLOAT_EQ(1000.f, clippingRange[1]);
            } else {
                expectNear(PXR_NS::GfVec3d(0, 0, 50), bounds.GetMin());
                expectNear(PXR_NS::GfVec3d(100, 200, 350), bounds.GetMax());
                EXPECT_FLOAT_EQ(1.f, clippingRange[0]);
            }

            PXR_NS::SdfLayerRefPtr written = PXR_NS::SdfLayer::FindOrOpen(path.toStdString());
            ASSERT_TRUE(static_cast<bool>(written));
            const PXR_NS::SdfAttributeSpecHandle rotate = written->GetAttributeAtPath(PXR_NS::SdfPath("/World.xformOp:rotateX:upAxisCorrection"));
            EXPECT_EQ(corrected, static_cast<bool>(rotate));
            if (mode == USDWrite::eExportModeLayered) {
                // The corrective opinions over the file, which is referred to, not copied.
                const std::vector<std::string> subLayers = written->GetSubLayerPaths();
                ASSERT_EQ((std::size_t)1, subLayers.size());
                EXPECT_TRUE(QDir::isRelativePath(QString::fromStdString(subLayers[0]))) << subLayers[0];
                EXPECT_EQ(cleanAbsolute(filePath), QDir::cleanPath(QDir(outDir).absoluteFilePath(QString::fromStdString(subLayers[0]))));
                EXPECT_FALSE(static_cast<bool>(written->GetAttributeAtPath(PXR_NS::SdfPath("/World/Box.points"))));
            } else {
                EXPECT_TRUE(written->GetSubLayerPaths().empty());
                EXPECT_TRUE(static_cast<bool>(written->GetAttributeAtPath(PXR_NS::SdfPath("/World/Box.points"))));
            }
        }
    }
}

// A file's renderSettingsPrimPath stage metadata reaches the composed stage through USDRead and
// the written file's root layer through USDWrite, in either mode.
TEST_F(USDWriteTest, RenderSettingsPrimPathRoundTripsInBothModes)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString filePath = tmp.path() + QString::fromUtf8("/render.usda");
    writeTextFile(filePath, "#usda 1.0\n"
                            "(\n"
                            "    renderSettingsPrimPath = \"/Render/Final\"\n"
                            ")\n"
                            "def Xform \"World\"\n{\n}\n"
                            "def Scope \"Render\"\n{\n"
                            "    def RenderSettings \"Final\"\n    {\n"
                            "        uniform int2 resolution = (320, 240)\n"
                            "    }\n}\n");

    NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
    ASSERT_TRUE(bool(reader));
    KnobFile* file = getKnob<KnobFile>(reader, kUSDReadParamFile);
    ASSERT_TRUE(file != NULL);
    file->setValue(filePath.toStdString());

    PXR_NS::UsdStageRefPtr composed = composeOutput(reader);
    ASSERT_TRUE(static_cast<bool>(composed));
    const PXR_NS::UsdRenderSettings active = PXR_NS::UsdRenderSettings::GetStageRenderSettings(composed);
    ASSERT_TRUE(static_cast<bool>(active));
    EXPECT_EQ(PXR_NS::SdfPath("/Render/Final"), active.GetPath());

    const USDWrite::ExportModeEnum modes[] = { USDWrite::eExportModeLayered, USDWrite::eExportModeFlattened };
    for (USDWrite::ExportModeEnum mode : modes) {
        const QString path = tmp.path() + QString::fromUtf8(mode == USDWrite::eExportModeLayered ? "/out/layered.usda" : "/out/flattened.usda");
        NodePtr writer = createWriter(reader, path, mode, 1, 1);
        ASSERT_TRUE(writer && getWriter(writer));
        ASSERT_EQ(eStatusOK, getWriter(writer)->writeFrameRange(1, 1, ViewIdx(0)));

        PXR_NS::SdfLayerRefPtr written = PXR_NS::SdfLayer::FindOrOpen(path.toStdString());
        ASSERT_TRUE(static_cast<bool>(written));
        std::string value;
        EXPECT_TRUE(written->HasField(PXR_NS::SdfPath::AbsoluteRootPath(), PXR_NS::UsdRenderTokens->renderSettingsPrimPath, &value)) << (int)mode;
        EXPECT_EQ(std::string("/Render/Final"), value) << (int)mode;

        PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(path.toStdString());
        ASSERT_TRUE(static_cast<bool>(stage));
        const PXR_NS::UsdRenderSettings reopened = PXR_NS::UsdRenderSettings::GetStageRenderSettings(stage);
        ASSERT_TRUE(static_cast<bool>(reopened)) << (int)mode;
        PXR_NS::GfVec2i resolution;
        EXPECT_TRUE(reopened.GetResolutionAttr().Get(&resolution));
        EXPECT_EQ(PXR_NS::GfVec2i(320, 240), resolution);
    }
}

namespace {
// The red of the pixel at the centre of the OpenEXR file path, or -1 when it cannot be read.
float
centreRedOf(const QString& path)
{
    std::unique_ptr<OIIO::ImageInput> in = OIIO::ImageInput::open(path.toStdString());

    if (!in) {
        return -1.f;
    }
    const OIIO::ImageSpec& spec = in->spec();
    std::vector<float> row((std::size_t)spec.width * (std::size_t)spec.nchannels);
    const bool ok = in->read_scanline(spec.height / 2, 0, OIIO::TypeDesc::FLOAT, row.data());
    in->close();

    return ok ? row[(std::size_t)(spec.width / 2) * (std::size_t)spec.nchannels] : -1.f;
}

// The pixel size of the image at path, (0, 0) when it cannot be read.
std::pair<int, int>
sizeOf(const QString& path)
{
    std::unique_ptr<OIIO::ImageInput> in = OIIO::ImageInput::open(path.toStdString());

    if (!in) {
        return std::make_pair(0, 0);
    }
    const std::pair<int, int> size(in->spec().width, in->spec().height);
    in->close();

    return size;
}
} // anonymous namespace

// A USDMaterial whose Diffuse Color input is a Constant, its red 0.1, or keyed 0.1, 0.2 and 0.3 at
// frames 1 to 3 when animated.
class USDWriteLiveTextureTest
    : public USDWriteTest {
protected:
    NodePtr createMaterial(bool animated)
    {
        NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
        NodePtr material = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMATERIAL));
        if (!constant || !material) {
            ADD_FAILURE() << "Constant or USDMaterial is not registered";

            return NodePtr();
        }
        constant->setScriptName("Constant1");
        material->setScriptName("Material1");
        KnobColor* color = getKnob<KnobColor>(constant, kConstantParamColor);
        EXPECT_TRUE(color != NULL);
        if (!color) {
            return NodePtr();
        }
        if (animated) {
            for (int frame = 1; frame <= 3; ++frame) {
                color->setValueAtTime(frame, 0.1 * frame, ViewSpec::all(), 0);
            }
        } else {
            color->setValue(0.1, ViewSpec::all(), 0);
        }
        EXPECT_TRUE(material->connectInput(constant, kUSDMaterialInputDiffuseColor));

        return material;
    }

    // The file attribute of Material1's diffuse texture.
    static PXR_NS::UsdAttribute diffuseFileOf(const PXR_NS::UsdStageRefPtr& stage)
    {
        const PXR_NS::UsdPrim texture = stage ? stage->GetPrimAtPath(PXR_NS::SdfPath("/Material1/diffuseColorTexture")) : PXR_NS::UsdPrim();

        EXPECT_TRUE(texture.IsValid());

        return texture ? texture.GetAttribute(PXR_NS::TfToken("inputs:file")) : PXR_NS::UsdAttribute();
    }
};

TEST_F(USDWriteLiveTextureTest, AnAnimatedLiveTextureIsExportedOneFilePerFrame)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    NodePtr material = createMaterial(true);
    ASSERT_TRUE(bool(material));
    const QString outDir = tmp.path() + QString::fromUtf8("/out");
    const QString path = outDir + QString::fromUtf8("/scene.usda");
    NodePtr writer = createWriter(material, path, USDWrite::eExportModeLayered, 1, 3);
    ASSERT_TRUE(writer && getWriter(writer));
    ASSERT_EQ(eStatusOK, writeOffMainThread(writer, 1, 3));
    EXPECT_FALSE(writer->hasPersistentMessage());

    const QString texturesDir = outDir + QString::fromUtf8("/scene_textures");
    const QStringList expected = QStringList() << QString::fromUtf8("Constant1_rgba.0001.exr") << QString::fromUtf8("Constant1_rgba.0002.exr")
                                               << QString::fromUtf8("Constant1_rgba.0003.exr");
    EXPECT_EQ(expected, fileNames(texturesDir));

    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(path.toStdString());
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdAttribute file = diffuseFileOf(stage);
    ASSERT_TRUE(bool(file));
    std::vector<double> times;
    ASSERT_TRUE(file.GetTimeSamples(&times));
    ASSERT_EQ((std::size_t)3, times.size());
    for (int i = 0; i < 3; ++i) {
        PXR_NS::SdfAssetPath asset;
        ASSERT_TRUE(file.Get(&asset, PXR_NS::UsdTimeCode(times[i])));
        EXPECT_EQ("./scene_textures/" + expected[i].toStdString(), asset.GetAssetPath());
        const QString exported = texturesDir + QLatin1Char('/') + expected[i];
        EXPECT_EQ(cleanAbsolute(exported).toStdString(), QDir::cleanPath(QString::fromStdString(asset.GetResolvedPath())).toStdString());
        // Half floats: 0.1, 0.2 and 0.3 to within their precision.
        EXPECT_NEAR(0.1 * (i + 1), centreRedOf(exported), 1e-3) << exported.toStdString();
    }
}

TEST_F(USDWriteLiveTextureTest, AStaticLiveTextureIsExportedOnce)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    NodePtr material = createMaterial(false);
    ASSERT_TRUE(bool(material));
    const QString path = tmp.path() + QString::fromUtf8("/scene.usda");
    NodePtr writer = createWriter(material, path, USDWrite::eExportModeFlattened, 1, 3);
    ASSERT_TRUE(writer && getWriter(writer));
    ASSERT_EQ(eStatusOK, writeOffMainThread(writer, 1, 3));

    const QString texturesDir = tmp.path() + QString::fromUtf8("/scene_textures");
    EXPECT_EQ(QStringList() << QString::fromUtf8("Constant1_rgba.exr"), fileNames(texturesDir));
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(path.toStdString());
    const PXR_NS::UsdAttribute file = diffuseFileOf(stage);
    ASSERT_TRUE(bool(file));
    EXPECT_EQ((std::size_t)0, file.GetNumTimeSamples());
    PXR_NS::SdfAssetPath asset;
    ASSERT_TRUE(file.Get(&asset));
    EXPECT_EQ(std::string("./scene_textures/Constant1_rgba.exr"), asset.GetAssetPath());
    EXPECT_NEAR(0.1, centreRedOf(texturesDir + QString::fromUtf8("/Constant1_rgba.exr")), 1e-3);
}

TEST_F(USDWriteLiveTextureTest, ATextureThatCannotBeWrittenLeavesNoUsdFile)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    NodePtr material = createMaterial(false);
    ASSERT_TRUE(bool(material));
    // A file where the textures' folder goes.
    QFile blocker(tmp.path() + QString::fromUtf8("/scene_textures"));
    ASSERT_TRUE(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    const QString path = tmp.path() + QString::fromUtf8("/scene.usda");
    NodePtr writer = createWriter(material, path, USDWrite::eExportModeLayered, 1, 1);
    ASSERT_TRUE(writer && getWriter(writer));

    EXPECT_EQ(eStatusFailed, writeOffMainThread(writer, 1, 1));
    EXPECT_FALSE(QFile::exists(path));
}

TEST_F(USDWriteLiveTextureTest, AUriWhoseSourceIsGoneWarnsAsSuch)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    NodePtr material = createMaterial(false);
    ASSERT_TRUE(bool(material));
    const QString earlier = tmp.path() + QString::fromUtf8("/earlier.usda");
    NodePtr earlierWriter = createWriter(material, earlier, USDWrite::eExportModeLayered, 1, 1);
    ASSERT_TRUE(earlierWriter && getWriter(earlierWriter));
    getKnob<KnobBool>(earlierWriter, kUSDWriteParamExportLiveTextures)->setValue(false);
    ASSERT_EQ(eStatusOK, writeOffMainThread(earlierWriter, 1, 1));

    NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
    ASSERT_TRUE(bool(reader));
    getKnob<KnobFile>(reader, kUSDReadParamFile)->setValue(earlier.toStdString());
    NodePtr writer = createWriter(reader, tmp.path() + QString::fromUtf8("/later.usda"), USDWrite::eExportModeFlattened, 1, 1);
    ASSERT_TRUE(writer && getWriter(writer));
    ASSERT_EQ(eStatusOK, writeOffMainThread(writer, 1, 1));

    ASSERT_TRUE(writer->hasPersistentMessage());
    QString message;
    int type = 0;
    writer->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("no longer available"))) << message.toStdString();
    EXPECT_FALSE(message.contains(QString::fromUtf8("Export Live Textures"))) << message.toStdString();
}

TEST_F(USDWriteLiveTextureTest, ADeletedSourceIsWrittenAsTheMaterialsOwnColourWithAWarning)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    NodePtr material = createMaterial(false);
    ASSERT_TRUE(bool(material));
    NodePtr constant = material->getInput(kUSDMaterialInputDiffuseColor);
    ASSERT_TRUE(bool(constant));
    constant->destroyNode(true, false);
    constant.reset();
    ASSERT_FALSE(material->getInput(kUSDMaterialInputDiffuseColor));

    const USDWrite::ExportModeEnum modes[] = { USDWrite::eExportModeLayered, USDWrite::eExportModeFlattened };
    const bool exports[] = { true, false };
    for (USDWrite::ExportModeEnum mode : modes) {
        for (bool exportLiveTextures : exports) {
            const std::string name = std::string(mode == USDWrite::eExportModeLayered ? "layered" : "flattened") + (exportLiveTextures ? "_exported" : "_unexported");
            SCOPED_TRACE(name);
            const QString dir = tmp.path() + QLatin1Char('/') + QString::fromStdString(name);
            const QString path = dir + QString::fromUtf8("/scene.usda");
            NodePtr writer = createWriter(material, path, mode, 1, 1);
            ASSERT_TRUE(writer && getWriter(writer));
            KnobBool* exportKnob = getKnob<KnobBool>(writer, kUSDWriteParamExportLiveTextures);
            ASSERT_TRUE(exportKnob != NULL);
            exportKnob->setValue(exportLiveTextures);
            ASSERT_EQ(eStatusOK, writeOffMainThread(writer, 1, 1));

            ASSERT_TRUE(writer->hasPersistentMessage());
            QString message;
            int type = 0;
            writer->getPersistentMessage(&message, &type, false);
            EXPECT_EQ((int)eMessageTypeWarning, type);
            EXPECT_TRUE(message.contains(QString::fromStdString(material->getFullyQualifiedName()))) << message.toStdString();
            EXPECT_TRUE(message.contains(QString::fromUtf8("own colour"))) << message.toStdString();
            EXPECT_FALSE(message.contains(QString::fromUtf8("Export Live Textures"))) << message.toStdString();
            EXPECT_FALSE(QFileInfo(dir + QString::fromUtf8("/scene_textures")).exists());

            PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(path.toStdString());
            ASSERT_TRUE(bool(stage));
            EXPECT_FALSE(stage->GetPrimAtPath(PXR_NS::SdfPath("/Material1/diffuseColorTexture")).IsValid());
            const PXR_NS::UsdShadeShader surface(stage->GetPrimAtPath(PXR_NS::SdfPath("/Material1/PreviewSurface")));
            ASSERT_TRUE(bool(surface));
            const PXR_NS::UsdShadeInput diffuse = surface.GetInput(PXR_NS::TfToken("diffuseColor"));
            ASSERT_TRUE(bool(diffuse));
            EXPECT_FALSE(diffuse.HasConnectedSource());
            PXR_NS::GfVec3f colour;
            ASSERT_TRUE(diffuse.Get(&colour));
            EXPECT_NEAR(0.18f, colour[0], 1e-6f);
            const PXR_NS::UsdShadeInput opacity = surface.GetInput(PXR_NS::TfToken("opacity"));
            EXPECT_FALSE(opacity && opacity.HasConnectedSource());
        }
    }
}

TEST_F(USDWriteLiveTextureTest, WithoutExportTheUrisAreWrittenWithAWarning)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    NodePtr material = createMaterial(false);
    ASSERT_TRUE(bool(material));
    const QString path = tmp.path() + QString::fromUtf8("/scene.usda");
    NodePtr writer = createWriter(material, path, USDWrite::eExportModeLayered, 1, 3);
    ASSERT_TRUE(writer && getWriter(writer));
    KnobBool* exportLiveTextures = getKnob<KnobBool>(writer, kUSDWriteParamExportLiveTextures);
    ASSERT_TRUE(exportLiveTextures != NULL);
    exportLiveTextures->setValue(false);
    ASSERT_EQ(eStatusOK, writeOffMainThread(writer, 1, 3));

    EXPECT_FALSE(QFileInfo(tmp.path() + QString::fromUtf8("/scene_textures")).exists());
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(path.toStdString());
    const PXR_NS::UsdAttribute file = diffuseFileOf(stage);
    ASSERT_TRUE(bool(file));
    PXR_NS::SdfAssetPath asset;
    ASSERT_TRUE(file.Get(&asset));
    const std::string uri = asset.GetAssetPath();
    EXPECT_EQ(0u, uri.rfind(std::string(kLiveTextureScheme) + "://", 0)) << uri;
    ASSERT_TRUE(writer->hasPersistentMessage());
    QString message;
    int type = 0;
    writer->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromStdString(uri))) << message.toStdString();
}

class USDWriteProjectionTest
    : public USDWriteTest {
protected:
    // A Constant projected through a camera on +Z framing a 2:1 card at the origin exactly, onto
    // that card. The card's prim is /Card1.
    void createRig()
    {
        ProjectPtr project = getApp()->getProject();
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, 2000, 1000, "wide2to1", 1.));

        plate = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
        camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
        projector = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPROJECT3D));
        card = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
        ASSERT_TRUE(plate && camera && projector && card);
        plate->setScriptName("Plate1");
        projector->setScriptName("Project1");
        card->setScriptName("Card1");
        ASSERT_DOUBLE_EQ(2., getKnob<KnobDouble>(card, kSceneParamScale)->getValue(0));
        getKnob<KnobDouble>(camera, kUSDCameraParamFocalLength)->setValue(50.);
        getKnob<KnobDouble>(camera, kUSDCameraParamHorizontalAperture)->setValue(36.);
        getKnob<KnobDouble>(camera, kUSDCameraParamVerticalAperture)->setValue(18.);
        getKnob<KnobDouble>(camera, kSceneParamTranslate)->setValue(50. / 18., ViewSpec::all(), 2);
        ASSERT_TRUE(projector->connectInput(plate, kUSDProject3DInputImage));
        ASSERT_TRUE(projector->connectInput(camera, kUSDProject3DInputCamera));
        ASSERT_TRUE(card->connectInput(projector, 0));
    }

    static PXR_NS::UsdGeomPrimvar stOf(const PXR_NS::UsdStageRefPtr& stage)
    {
        const PXR_NS::UsdPrim prim = stage ? stage->GetPrimAtPath(PXR_NS::SdfPath("/Card1")) : PXR_NS::UsdPrim();

        EXPECT_TRUE(prim.IsValid());

        return prim ? PXR_NS::UsdGeomPrimvarsAPI(prim).GetPrimvar(PXR_NS::TfToken("st")) : PXR_NS::UsdGeomPrimvar();
    }

    static QString messageOf(const NodePtr& node)
    {
        QString message;
        int type = 0;

        node->getPersistentMessage(&message, &type, false);

        return message;
    }

    // The bounds of the card's UVs at time.
    // A quad filling the projector's frame when offset is 0.
    static PXR_NS::VtVec3fArray framePoints(float offset)
    {
        PXR_NS::VtVec3fArray points(4);
        points[0] = PXR_NS::GfVec3f(offset - 1.f, -0.5f, 0.f);
        points[1] = PXR_NS::GfVec3f(offset + 1.f, -0.5f, 0.f);
        points[2] = PXR_NS::GfVec3f(offset + 1.f, 0.5f, 0.f);
        points[3] = PXR_NS::GfVec3f(offset - 1.f, 0.5f, 0.f);

        return points;
    }

    static PXR_NS::UsdGeomMesh defineFrameQuad(const PXR_NS::UsdStageRefPtr& stage,
                                               const char* path)
    {
        PXR_NS::UsdGeomMesh mesh = PXR_NS::UsdGeomMesh::Define(stage, PXR_NS::SdfPath(path));
        PXR_NS::VtIntArray indices(4);
        for (int i = 0; i < 4; ++i) {
            indices[i] = i;
        }
        mesh.CreateFaceVertexCountsAttr().Set(PXR_NS::VtIntArray(1, 4));
        mesh.CreateFaceVertexIndicesAttr().Set(indices);

        return mesh;
    }

    // The rig's projection held at frame 1, assigned to pattern in the scene at file, in place of the card.
    NodePtr createHeldAssignOf(const QString& file,
                               const std::string& pattern)
    {
        createRig();
        NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
        NodePtr assign = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));
        if (!projector || !card || !reader || !assign) {
            ADD_FAILURE() << "a node is not registered";

            return NodePtr();
        }
        card->disconnectInput(0);
        getKnob<KnobChoice>(projector, kUSDProject3DParamProjectionSpace)->setValue((int)USDProject3D::eProjectionSpaceGeometry);
        getKnob<KnobBool>(projector, kUSDProject3DParamUseFrameHold)->setValue(true);
        getKnob<KnobDouble>(projector, kUSDProject3DParamFrameHold)->setValue(1.);
        getKnob<KnobFile>(reader, kUSDReadParamFile)->setValue(file.toStdString());
        getKnob<KnobString>(assign, kUSDAssignMaterialParamPrimPattern)->setValue(pattern);
        EXPECT_TRUE(assign->connectInput(reader, kUSDAssignMaterialInputScene));
        EXPECT_TRUE(assign->connectInput(projector, kUSDAssignMaterialInputMaterial));

        return assign;
    }

    static void expectNoProjectionData(const QString& dir,
                                       const char* what)
    {
        for (const std::pair<const QString, QByteArray>& file : fileContents(dir)) {
            for (const char* needle : { "natronProjectionFrame", "natronReferencePoints", "natronReferencePresent", kUSDProject3DFrameReaderName, kUSDProject3DReferenceReaderName, kUSDProject3DInputGeometryFrame, kUSDProject3DInputReferencePoints, kUSDProject3DAttrFrameHold }) {
                EXPECT_FALSE(file.second.contains(needle)) << what << " " << file.first.toStdString() << " holds " << needle;
            }
        }
    }

    // That st holds one set of values for all time, the frame's bottom left to its top right around the quad.
    static void expectFrameQuadUVs(const PXR_NS::UsdPrim& prim,
                                   const PXR_NS::TfToken& interpolation,
                                   const char* what)
    {
        const PXR_NS::UsdGeomPrimvar st = PXR_NS::UsdGeomPrimvarsAPI(prim).GetPrimvar(PXR_NS::TfToken("st"));
        ASSERT_TRUE(bool(st)) << what;
        EXPECT_EQ(interpolation, st.GetInterpolation()) << what;
        EXPECT_EQ((std::size_t)0, st.GetAttr().GetNumTimeSamples()) << what;
        const PXR_NS::GfVec2f expected[] = { PXR_NS::GfVec2f(0.f, 0.f), PXR_NS::GfVec2f(1.f, 0.f), PXR_NS::GfVec2f(1.f, 1.f), PXR_NS::GfVec2f(0.f, 1.f) };
        for (const double time : { 1., 3. }) {
            PXR_NS::VtVec2fArray uvs;
            ASSERT_TRUE(st.ComputeFlattened(&uvs, PXR_NS::UsdTimeCode(time))) << what;
            ASSERT_EQ((std::size_t)4, uvs.size()) << what;
            for (std::size_t i = 0; i < 4; ++i) {
                EXPECT_NEAR(expected[i][0], uvs[i][0], 1e-4f) << what << " corner " << i << " at " << time;
                EXPECT_NEAR(expected[i][1], uvs[i][1], 1e-4f) << what << " corner " << i << " at " << time;
            }
        }
    }

    PXR_NS::UsdStageRefPtr writeAndOpen(const NodePtr& input,
                                        const QString& dir,
                                        USDWrite::ExportModeEnum mode)
    {
        EXPECT_TRUE(QDir().mkpath(dir));
        const QString path = dir + QString::fromUtf8("/scene.usda");
        NodePtr writer = createWriter(input, path, mode, 1, 3);
        if (!writer || !getWriter(writer)) {
            ADD_FAILURE() << "no writer";

            return PXR_NS::UsdStageRefPtr();
        }
        EXPECT_EQ(eStatusOK, writeOffMainThread(writer, 1, 3));

        return PXR_NS::UsdStage::Open(path.toStdString());
    }

    static std::pair<PXR_NS::GfVec2f, PXR_NS::GfVec2f> boundsOf(const PXR_NS::UsdGeomPrimvar& st,
                                                                PXR_NS::UsdTimeCode time = PXR_NS::UsdTimeCode::Default())
    {
        PXR_NS::GfVec2f low(HUGE_VALF, HUGE_VALF);
        PXR_NS::GfVec2f high(-HUGE_VALF, -HUGE_VALF);
        PXR_NS::VtVec2fArray uvs;

        EXPECT_TRUE(st.ComputeFlattened(&uvs, time));
        EXPECT_FALSE(uvs.empty());
        for (const PXR_NS::GfVec2f& uv : uvs) {
            for (int c = 0; c < 2; ++c) {
                low[c] = std::min(low[c], uv[c]);
                high[c] = std::max(high[c], uv[c]);
            }
        }

        return std::make_pair(low, high);
    }

    NodePtr plate;
    NodePtr projector;
    NodePtr camera;
    NodePtr card;
};

// The bake finds the mesh through whichever purpose the assign bound the projection for.
TEST_F(USDWriteProjectionTest, AnAssignedProjectionIsBakedAsADirectOneIs)
{
    createRig();
    ASSERT_TRUE(bool(card));
    card->disconnectInput(0);
    const std::pair<USDAssignMaterial::PurposeEnum, PXR_NS::TfToken> purposes[] = {
        { USDAssignMaterial::ePurposeAll, PXR_NS::UsdShadeTokens->allPurpose },
        { USDAssignMaterial::ePurposeFull, PXR_NS::UsdShadeTokens->full },
        { USDAssignMaterial::ePurposePreview, PXR_NS::UsdShadeTokens->preview },
    };
    for (const std::pair<USDAssignMaterial::PurposeEnum, PXR_NS::TfToken>& purpose : purposes) {
        QTemporaryDir tmp;
        ASSERT_TRUE(tmp.isValid());
        NodePtr assign = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));
        ASSERT_TRUE(bool(assign));
        getKnob<KnobString>(assign, kUSDAssignMaterialParamPrimPattern)->setValue("/Card1");
        getKnob<KnobChoice>(assign, kUSDAssignMaterialParamPurpose)->setValue((int)purpose.first);
        ASSERT_TRUE(assign->connectInput(card, kUSDAssignMaterialInputScene));
        ASSERT_TRUE(assign->connectInput(projector, kUSDAssignMaterialInputMaterial));

        const QString path = tmp.path() + QString::fromUtf8("/scene.usda");
        NodePtr writer = createWriter(assign, path, USDWrite::eExportModeFlattened, 1, 3);
        ASSERT_TRUE(writer && getWriter(writer));
        ASSERT_EQ(eStatusOK, writeOffMainThread(writer, 1, 3));
        EXPECT_FALSE(writer->hasPersistentMessage()) << purpose.second.GetString() << ": " << messageOf(writer).toStdString();

        const PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(path.toStdString());
        ASSERT_TRUE(bool(stage));
        const PXR_NS::UsdGeomPrimvar st = stOf(stage);
        ASSERT_TRUE(bool(st)) << purpose.second.GetString();
        const std::pair<PXR_NS::GfVec2f, PXR_NS::GfVec2f> bounds = boundsOf(st);
        EXPECT_NEAR(0.f, bounds.first[0], 1e-4f);
        EXPECT_NEAR(0.f, bounds.first[1], 1e-4f);
        EXPECT_NEAR(1.f, bounds.second[0], 1e-4f);
        EXPECT_NEAR(1.f, bounds.second[1], 1e-4f);

        const PXR_NS::UsdShadeMaterial material = PXR_NS::UsdShadeMaterialBindingAPI(stage->GetPrimAtPath(PXR_NS::SdfPath("/Card1"))).ComputeBoundMaterial(purpose.second);
        ASSERT_TRUE(bool(material)) << purpose.second.GetString();
        const PXR_NS::UsdShadeShader surface = material.ComputeSurfaceSource();
        ASSERT_TRUE(bool(surface));
        PXR_NS::TfToken id;
        ASSERT_TRUE(surface.GetShaderId(&id));
        EXPECT_EQ(std::string("UsdPreviewSurface"), id.GetString());

        assign->disconnectInput(kUSDAssignMaterialInputMaterial);
        assign->disconnectInput(kUSDAssignMaterialInputScene);
    }
}

// Exported, the projection is the plate's texture on a preview surface, read through UVs that put
// the frame's corners at (0, 0) and (1, 1); the Storm shader is left inactive.
TEST_F(USDWriteProjectionTest, AProjectionIsBakedToUVsAndAPreviewSurface)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    createRig();
    ASSERT_TRUE(bool(card));
    const QString path = tmp.path() + QString::fromUtf8("/scene.usda");
    NodePtr writer = createWriter(card, path, USDWrite::eExportModeFlattened, 1, 3);
    ASSERT_TRUE(writer && getWriter(writer));
    ASSERT_EQ(eStatusOK, writeOffMainThread(writer, 1, 3));
    // Facing the camera, the card's UVs are exact: no warning.
    EXPECT_FALSE(writer->hasPersistentMessage()) << messageOf(writer).toStdString();
    EXPECT_EQ(QStringList() << QString::fromUtf8("Plate1_rgba.exr"), fileNames(tmp.path() + QString::fromUtf8("/scene_textures")));

    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(path.toStdString());
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdGeomPrimvar st = stOf(stage);
    ASSERT_TRUE(bool(st));
    EXPECT_EQ(PXR_NS::UsdGeomTokens->faceVarying, st.GetInterpolation());
    EXPECT_EQ((std::size_t)0, st.GetAttr().GetNumTimeSamples());
    PXR_NS::VtVec2fArray uvs;
    ASSERT_TRUE(st.ComputeFlattened(&uvs));
    ASSERT_FALSE(uvs.empty());
    PXR_NS::GfVec2f low(HUGE_VALF, HUGE_VALF);
    PXR_NS::GfVec2f high(-HUGE_VALF, -HUGE_VALF);
    for (const PXR_NS::GfVec2f& uv : uvs) {
        for (int c = 0; c < 2; ++c) {
            low[c] = std::min(low[c], uv[c]);
            high[c] = std::max(high[c], uv[c]);
        }
    }
    EXPECT_NEAR(0.f, low[0], 1e-4f);
    EXPECT_NEAR(0.f, low[1], 1e-4f);
    EXPECT_NEAR(1.f, high[0], 1e-4f);
    EXPECT_NEAR(1.f, high[1], 1e-4f);

    const PXR_NS::UsdShadeMaterial material = PXR_NS::UsdShadeMaterialBindingAPI(stage->GetPrimAtPath(PXR_NS::SdfPath("/Card1"))).ComputeBoundMaterial();
    ASSERT_TRUE(bool(material));
    const PXR_NS::UsdShadeShader surface = material.ComputeSurfaceSource();
    ASSERT_TRUE(bool(surface));
    PXR_NS::TfToken id;
    ASSERT_TRUE(surface.GetShaderId(&id));
    EXPECT_EQ(std::string("UsdPreviewSurface"), id.GetString());
    PXR_NS::UsdShadeConnectableAPI source;
    PXR_NS::TfToken sourceName;
    PXR_NS::UsdShadeAttributeType sourceType;
    ASSERT_TRUE(surface.GetInput(PXR_NS::TfToken("emissiveColor")).GetConnectedSource(&source, &sourceName, &sourceType));
    EXPECT_EQ(std::string("rgb"), sourceName.GetString());
    PXR_NS::SdfAssetPath file;
    ASSERT_TRUE(PXR_NS::UsdShadeShader(source.GetPrim()).GetInput(PXR_NS::TfToken("file")).Get(&file));
    EXPECT_EQ(std::string("./scene_textures/Plate1_rgba.exr"), file.GetAssetPath());
    // Unlit and alpha-blended, as the live projection is: the plate's straight colour is the
    // emission, its alpha the opacity in the presence mode that multiplies the two.
    const PXR_NS::SdfPath texturePath = source.GetPath();
    ASSERT_TRUE(surface.GetInput(PXR_NS::TfToken("opacity")).GetConnectedSource(&source, &sourceName, &sourceType));
    EXPECT_EQ(texturePath, source.GetPath());
    EXPECT_EQ(std::string("a"), sourceName.GetString());
    EXPECT_FALSE(bool(surface.GetInput(PXR_NS::TfToken("opacityThreshold"))));
    PXR_NS::TfToken opacityMode;
    ASSERT_TRUE(surface.GetInput(PXR_NS::TfToken("opacityMode")).Get(&opacityMode));
    EXPECT_EQ(std::string("presence"), opacityMode.GetString());
    PXR_NS::GfVec3f color(1.f);
    ASSERT_TRUE(surface.GetInput(PXR_NS::TfToken("diffuseColor")).Get(&color));
    EXPECT_EQ(PXR_NS::GfVec3f(0.f), color);
    int specularWorkflow = 0;
    ASSERT_TRUE(surface.GetInput(PXR_NS::TfToken("useSpecularWorkflow")).Get(&specularWorkflow));
    EXPECT_EQ(1, specularWorkflow);
    color = PXR_NS::GfVec3f(1.f);
    ASSERT_TRUE(surface.GetInput(PXR_NS::TfToken("specularColor")).Get(&color));
    EXPECT_EQ(PXR_NS::GfVec3f(0.f), color);
    const PXR_NS::UsdPrim projection = material.GetPrim().GetChild(PXR_NS::TfToken(kUSDProject3DShaderName));
    ASSERT_TRUE(projection.IsValid());
    EXPECT_FALSE(projection.IsActive());
}

// Exported, a projection keeps no Cycles or MoonRay network: those renderers resolve the material
// to the baked preview surface Storm reads, and neither the networks nor the frame primvars they
// read are written, in World space or held in Geometry space, flattened or layered.
TEST_F(USDWriteProjectionTest, ABakedProjectionDropsItsCyclesAndMoonRayNetworks)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    createRig();
    ASSERT_TRUE(projector && card);
    KnobDouble* translate = getKnob<KnobDouble>(card, kSceneParamTranslate);
    translate->setValueAtTime(1., 0., ViewSpec::all(), 0);
    translate->setValueAtTime(3., 0.5, ViewSpec::all(), 0);
    struct Case {
        const char* dir;
        USDWrite::ExportModeEnum mode;
        bool held;
    };
    const Case cases[] = {
        { "/world-flattened", USDWrite::eExportModeFlattened, false },
        { "/world-layered", USDWrite::eExportModeLayered, false },
        { "/held-flattened", USDWrite::eExportModeFlattened, true },
        { "/held-layered", USDWrite::eExportModeLayered, true },
    };
    for (const Case& c : cases) {
        getKnob<KnobChoice>(projector, kUSDProject3DParamProjectionSpace)->setValue((int)(c.held ? USDProject3D::eProjectionSpaceGeometry : USDProject3D::eProjectionSpaceWorld));
        getKnob<KnobBool>(projector, kUSDProject3DParamUseFrameHold)->setValue(c.held);
        getKnob<KnobDouble>(projector, kUSDProject3DParamFrameHold)->setValue(1.);
        const QString dir = tmp.path() + QString::fromUtf8(c.dir);
        const PXR_NS::UsdStageRefPtr stage = writeAndOpen(card, dir, c.mode);
        ASSERT_TRUE(bool(stage)) << c.dir;

        for (const std::pair<const QString, QByteArray>& file : fileContents(dir)) {
            for (const char* needle : { "cycles:", "moonray:", "cycles_", "AttributeMap", "OpMap", "natronProjectionFrame", "natronReferencePoints", "natronReferencePresent" }) {
                EXPECT_FALSE(file.second.contains(needle)) << c.dir << " " << file.first.toStdString() << " holds " << needle;
            }
        }
        const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(PXR_NS::SdfPath("/Card1"));
        ASSERT_TRUE(bool(prim)) << c.dir;
        for (const PXR_NS::UsdGeomPrimvar& primvar : PXR_NS::UsdGeomPrimvarsAPI(prim).GetPrimvars()) {
            EXPECT_EQ(std::string::npos, primvar.GetPrimvarName().GetString().find("natronProjectionFrame")) << c.dir;
        }
        const PXR_NS::UsdShadeMaterial material = PXR_NS::UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial();
        ASSERT_TRUE(bool(material)) << c.dir;
        const PXR_NS::UsdShadeShader storm = material.ComputeSurfaceSource();
        ASSERT_TRUE(bool(storm)) << c.dir;
        PXR_NS::TfToken id;
        ASSERT_TRUE(storm.GetShaderId(&id)) << c.dir;
        EXPECT_EQ(std::string("UsdPreviewSurface"), id.GetString()) << c.dir;
        for (const char* context : { "cycles", "moonray" }) {
            EXPECT_FALSE(bool(material.GetSurfaceOutput(PXR_NS::TfToken(context)))) << c.dir << " " << context;
            const PXR_NS::UsdShadeShader surface = material.ComputeSurfaceSource(PXR_NS::TfTokenVector { PXR_NS::TfToken(context), PXR_NS::UsdShadeTokens->universalRenderContext });
            EXPECT_EQ(storm.GetPath(), surface.GetPath()) << c.dir << " " << context;
        }
        for (const PXR_NS::UsdPrim& child : material.GetPrim().GetChildren()) {
            const std::string name = child.GetName().GetString();
            EXPECT_NE(0u, name.rfind("Cycles", 0)) << c.dir << " keeps " << name;
            EXPECT_NE(0u, name.rfind("MoonRay", 0)) << c.dir << " keeps " << name;
        }
        const PXR_NS::UsdGeomPrimvar st = stOf(stage);
        ASSERT_TRUE(bool(st)) << c.dir;
        const std::pair<PXR_NS::GfVec2f, PXR_NS::GfVec2f> bounds = boundsOf(st, PXR_NS::UsdTimeCode(1.));
        EXPECT_NEAR(0.f, bounds.first[0], 1e-4f) << c.dir;
        EXPECT_NEAR(0.f, bounds.first[1], 1e-4f) << c.dir;
        EXPECT_NEAR(1.f, bounds.second[0], 1e-4f) << c.dir;
        EXPECT_NEAR(1.f, bounds.second[1], 1e-4f) << c.dir;
    }
}

// The bake strips what the projection authored, not what shares its names: a shader the user
// put under the projection's material stays.
TEST_F(USDWriteProjectionTest, ABakeKeepsAShaderTheUserPutUnderTheMaterial)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    createRig();
    ASSERT_TRUE(projector && card);
    getKnob<KnobString>(projector, kSceneParamPrimPath)->setValue("/Projection");
    const QString source = tmp.path() + QString::fromUtf8("/user.usda");
    {
        const PXR_NS::UsdStageRefPtr file = PXR_NS::UsdStage::CreateNew(source.toStdString());
        ASSERT_TRUE(bool(file));
        // The card grafts the material it is given under itself.
        PXR_NS::UsdShadeShader user = PXR_NS::UsdShadeShader::Define(file, PXR_NS::SdfPath("/Card1/Projection/CyclesUser"));
        user.CreateIdAttr(PXR_NS::VtValue(PXR_NS::TfToken("UsdPrimvarReader_float2")));
        ASSERT_TRUE(file->GetRootLayer()->Save());
    }
    NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
    ASSERT_TRUE(reader && merge);
    getKnob<KnobFile>(reader, kUSDReadParamFile)->setValue(source.toStdString());
    ASSERT_TRUE(merge->connectInput(card, 0));
    ASSERT_TRUE(merge->connectInput(reader, 1));

    const std::pair<const char*, USDWrite::ExportModeEnum> modes[] = {
        { "/flattened", USDWrite::eExportModeFlattened },
        { "/layered", USDWrite::eExportModeLayered },
    };
    for (const std::pair<const char*, USDWrite::ExportModeEnum>& mode : modes) {
        const PXR_NS::UsdStageRefPtr stage = writeAndOpen(merge, tmp.path() + QString::fromUtf8(mode.first), mode.second);
        ASSERT_TRUE(bool(stage)) << mode.first;
        const PXR_NS::UsdGeomPrimvar st = stOf(stage);
        EXPECT_TRUE(st.GetAttr() && st.HasValue()) << mode.first << ": not baked";
        const PXR_NS::UsdPrim user = stage->GetPrimAtPath(PXR_NS::SdfPath("/Card1/Projection/CyclesUser"));
        ASSERT_TRUE(user.IsValid()) << mode.first;
        PXR_NS::TfToken id;
        EXPECT_TRUE(PXR_NS::UsdShadeShader(user).GetShaderId(&id)) << mode.first;
        EXPECT_EQ(std::string("UsdPrimvarReader_float2"), id.GetString()) << mode.first;
        for (const PXR_NS::UsdPrim& child : stage->GetPrimAtPath(PXR_NS::SdfPath("/Card1/Projection")).GetChildren()) {
            EXPECT_FALSE(child.GetCustomDataByKey(PXR_NS::TfToken(kSceneProjectionNetworkMark)).IsHolding<bool>()) << mode.first << " keeps " << child.GetName().GetString();
        }
    }
}

// Curves keep Storm's projection unbaked, read through the frame primvars; a layered export of
// them with the camera named by path keeps that and nothing of the other delegates' networks.
TEST_F(USDWriteProjectionTest, AnUnbakedProjectionWithADeferredCameraExportsStormsAlone)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString source = tmp.path() + QString::fromUtf8("/source.usda");
    {
        const PXR_NS::UsdStageRefPtr file = PXR_NS::UsdStage::CreateNew(source.toStdString());
        ASSERT_TRUE(bool(file));
        const PXR_NS::UsdGeomBasisCurves curves = PXR_NS::UsdGeomBasisCurves::Define(file, PXR_NS::SdfPath("/Curves"));
        curves.CreateCurveVertexCountsAttr().Set(PXR_NS::VtIntArray(1, 4));
        curves.CreatePointsAttr().Set(framePoints(0.f));
        ASSERT_TRUE(file->GetRootLayer()->Save());
    }
    NodePtr assign = createHeldAssignOf(source, "/Curves");
    ASSERT_TRUE(bool(assign));
    camera->setScriptName("Camera1");
    projector->disconnectInput(kUSDProject3DInputCamera);
    getKnob<KnobString>(projector, kUSDProject3DParamCameraPath)->setValue("/Camera1");
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
    ASSERT_TRUE(bool(merge));
    ASSERT_TRUE(merge->connectInput(assign, 0));
    ASSERT_TRUE(merge->connectInput(camera, 1));

    const QString dir = tmp.path() + QString::fromUtf8("/out");
    const PXR_NS::UsdStageRefPtr stage = writeAndOpen(merge, dir, USDWrite::eExportModeLayered);
    ASSERT_TRUE(bool(stage));
    for (const std::pair<const QString, QByteArray>& file : fileContents(dir)) {
        for (const char* needle : { "cycles:", "moonray:", "cycles_", "AttributeMap", "OpMap" }) {
            EXPECT_FALSE(file.second.contains(needle)) << file.first.toStdString() << " holds " << needle;
        }
    }
    const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(PXR_NS::SdfPath("/Curves"));
    ASSERT_TRUE(bool(prim));
    const PXR_NS::UsdShadeMaterial material = PXR_NS::UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial();
    ASSERT_TRUE(bool(material));
    SceneGeometryProjection projection;
    ASSERT_TRUE(SceneNodeAuthoring::readGeometryProjection(material.GetPrim(), &projection));
    const PXR_NS::UsdGeomPrimvarsAPI primvars(prim);
    const PXR_NS::UsdGeomPrimvar frame = primvars.GetPrimvar(PXR_NS::TfToken(projection.framePrimvar));
    EXPECT_TRUE(frame.GetAttr() && frame.HasValue());
    for (int column = 0; column < 3; ++column) {
        EXPECT_FALSE(bool(primvars.GetPrimvar(PXR_NS::TfToken(SceneProjectionCameras::frameColumnPrimvar(projection.framePrimvar, column))).GetAttr())) << column;
    }
    for (int part = 0; part < 4; ++part) {
        EXPECT_FALSE(bool(primvars.GetPrimvar(PXR_NS::TfToken(SceneProjectionCameras::moonRayFramePrimvar(projection.framePrimvar, part))).GetAttr())) << part;
    }
    for (const char* context : { "cycles", "moonray" }) {
        EXPECT_FALSE(bool(material.GetSurfaceOutput(PXR_NS::TfToken(context)))) << context;
    }
    for (const PXR_NS::UsdPrim& child : material.GetPrim().GetChildren()) {
        EXPECT_FALSE(child.GetCustomDataByKey(PXR_NS::TfToken(kSceneProjectionNetworkMark)).IsHolding<bool>()) << child.GetName().GetString();
    }
}

TEST_F(USDWriteProjectionTest, AProjectionBoundToNothingWarns)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    createRig();
    ASSERT_TRUE(bool(projector));
    NodePtr writer = createWriter(projector, tmp.path() + QString::fromUtf8("/scene.usda"), USDWrite::eExportModeLayered, 1, 1);
    ASSERT_TRUE(writer && getWriter(writer));
    ASSERT_EQ(eStatusOK, writeOffMainThread(writer, 1, 1));

    ASSERT_TRUE(writer->hasPersistentMessage());
    const QString message = messageOf(writer);
    EXPECT_TRUE(message.contains(QString::fromUtf8("Project1"))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8("bound to no geometry"))) << message.toStdString();
}

TEST_F(USDWriteProjectionTest, ControlsTheBakeCannotHonourWarn)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    createRig();
    ASSERT_TRUE(bool(card));
    getKnob<KnobChoice>(projector, kUSDProject3DParamProjectOn)->setValue((int)USDProject3D::eProjectOnFront);
    getKnob<KnobBool>(projector, kUSDProject3DParamClipNearFar)->setValue(true);
    getKnob<KnobChoice>(projector, kUSDProject3DParamOutside)->setValue((int)USDProject3D::eOutsideTransparent);
    NodePtr writer = createWriter(card, tmp.path() + QString::fromUtf8("/scene.usda"), USDWrite::eExportModeLayered, 1, 1);
    ASSERT_TRUE(writer && getWriter(writer));
    ASSERT_EQ(eStatusOK, writeOffMainThread(writer, 1, 1));

    ASSERT_TRUE(writer->hasPersistentMessage());
    const QString message = messageOf(writer);
    EXPECT_TRUE(message.contains(QString::fromUtf8("Project On"))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8("Clip Near Far"))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8("Outside"))) << message.toStdString();
}

// compose() resolves a camera named by path into a layer of its own, which a layered write does
// not write.
TEST_F(USDWriteProjectionTest, ALayeredWriteHoldsTheResolvedCameraOfAnUnbakedProjection)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    createRig();
    ASSERT_TRUE(bool(projector));
    camera->setScriptName("Camera1");
    projector->disconnectInput(kUSDProject3DInputCamera);
    getKnob<KnobString>(projector, kUSDProject3DParamCameraPath)->setValue("/Camera1");
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
    ASSERT_TRUE(bool(merge));
    ASSERT_TRUE(merge->connectInput(projector, 0));
    ASSERT_TRUE(merge->connectInput(camera, 1));
    const QString path = tmp.path() + QString::fromUtf8("/scene.usda");
    NodePtr writer = createWriter(merge, path, USDWrite::eExportModeLayered, 1, 1);
    ASSERT_TRUE(writer && getWriter(writer));
    ASSERT_EQ(eStatusOK, writeOffMainThread(writer, 1, 1));

    const auto matrixOf = [](const PXR_NS::UsdStageRefPtr& stage) {
        PXR_NS::GfMatrix4d matrix(0.);
        bool found = false;
        if (stage) {
            for (const PXR_NS::UsdPrim& prim : stage->Traverse()) {
                if (prim.GetAttribute(PXR_NS::TfToken(kUSDProject3DAttrCamera))) {
                    found = PXR_NS::UsdShadeShader(prim).GetInput(PXR_NS::TfToken(kUSDProject3DInputWorldToProjector)).Get(&matrix);
                }
            }
        }
        EXPECT_TRUE(found);

        return matrix;
    };
    const PXR_NS::GfMatrix4d resolved = matrixOf(composeOutput(merge));
    EXPECT_FALSE(PXR_NS::GfIsClose(resolved, PXR_NS::GfMatrix4d(1.), 1e-6));
    EXPECT_FALSE(PXR_NS::GfIsClose(resolved, PXR_NS::GfMatrix4d(0.), 1e-6));
    EXPECT_TRUE(PXR_NS::GfIsClose(resolved, matrixOf(PXR_NS::UsdStage::Open(path.toStdString())), 1e-9));
}

TEST_F(USDWriteProjectionTest, AMovingCameraBakesUVsPerFrame)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    createRig();
    ASSERT_TRUE(bool(card));
    KnobDouble* translate = getKnob<KnobDouble>(camera, kSceneParamTranslate);
    translate->setValueAtTime(1., 0., ViewSpec::all(), 0);
    translate->setValueAtTime(3., 0.5, ViewSpec::all(), 0);
    const QString path = tmp.path() + QString::fromUtf8("/scene.usda");
    NodePtr writer = createWriter(card, path, USDWrite::eExportModeLayered, 1, 3);
    ASSERT_TRUE(writer && getWriter(writer));
    ASSERT_EQ(eStatusOK, writeOffMainThread(writer, 1, 3));

    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(path.toStdString());
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdGeomPrimvar st = stOf(stage);
    ASSERT_TRUE(bool(st));
    std::vector<double> times;
    ASSERT_TRUE(st.GetAttr().GetTimeSamples(&times));
    ASSERT_EQ((std::size_t)3, times.size());
    // The camera half a unit right at the last frame: the card a quarter of the frame left.
    PXR_NS::VtVec2fArray first, last;
    ASSERT_TRUE(st.ComputeFlattened(&first, PXR_NS::UsdTimeCode(times.front())));
    ASSERT_TRUE(st.ComputeFlattened(&last, PXR_NS::UsdTimeCode(times.back())));
    ASSERT_EQ(first.size(), last.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        EXPECT_NEAR(first[i][0] - 0.25f, last[i][0], 1e-4f) << i;
        EXPECT_NEAR(first[i][1], last[i][1], 1e-4f) << i;
    }
}

// Per-vertex UVs on a tilted coarse card are not the live projection between its vertices; the export
// takes them as they are, without a warning.
TEST_F(USDWriteProjectionTest, ATiltedCoarseCardBakesWithoutAWarning)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    createRig();
    ASSERT_TRUE(bool(card));
    getKnob<KnobDouble>(card, kSceneParamRotate)->setValue(60., ViewSpec::all(), 1);
    const QString path = tmp.path() + QString::fromUtf8("/scene.usda");
    NodePtr writer = createWriter(card, path, USDWrite::eExportModeFlattened, 1, 1);
    ASSERT_TRUE(writer && getWriter(writer));
    ASSERT_EQ(eStatusOK, writeOffMainThread(writer, 1, 1));
    EXPECT_FALSE(writer->hasPersistentMessage()) << messageOf(writer).toStdString();
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(path.toStdString());
    ASSERT_TRUE(bool(stage));
    EXPECT_TRUE(bool(stOf(stage)));
}

// In Geometry space with the camera held, the bake keeps the card's UVs where the projection put them
// at the hold frame, however the card moves after it.
TEST_F(USDWriteProjectionTest, AHeldGeometrySpaceProjectionBakesUVsThatStickToTheCard)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    createRig();
    ASSERT_TRUE(projector && card);
    getKnob<KnobChoice>(projector, kUSDProject3DParamProjectionSpace)->setValue((int)USDProject3D::eProjectionSpaceGeometry);
    getKnob<KnobBool>(projector, kUSDProject3DParamUseFrameHold)->setValue(true);
    getKnob<KnobDouble>(projector, kUSDProject3DParamFrameHold)->setValue(1.);
    KnobDouble* translate = getKnob<KnobDouble>(card, kSceneParamTranslate);
    translate->setValueAtTime(1., 0., ViewSpec::all(), 0);
    translate->setValueAtTime(3., 0.5, ViewSpec::all(), 0);
    const QString path = tmp.path() + QString::fromUtf8("/scene.usda");
    NodePtr writer = createWriter(card, path, USDWrite::eExportModeFlattened, 1, 3);
    ASSERT_TRUE(writer && getWriter(writer));
    ASSERT_EQ(eStatusOK, writeOffMainThread(writer, 1, 3));

    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(path.toStdString());
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdGeomPrimvar st = stOf(stage);
    ASSERT_TRUE(bool(st));
    // The card moved a quarter of the frame right by the last frame: in World space its UVs would
    // span 0.25 to 1.25 there.
    for (const double time : { 1., 3. }) {
        const std::pair<PXR_NS::GfVec2f, PXR_NS::GfVec2f> bounds = boundsOf(st, PXR_NS::UsdTimeCode(time));
        EXPECT_NEAR(0.f, bounds.first[0], 1e-4f) << time;
        EXPECT_NEAR(0.f, bounds.first[1], 1e-4f) << time;
        EXPECT_NEAR(1.f, bounds.second[0], 1e-4f) << time;
        EXPECT_NEAR(1.f, bounds.second[1], 1e-4f) << time;
    }
}

// Each mesh bakes from its own placement, as a direct connection to it does, and the export holds
// nothing of how Storm read those placements.
TEST_F(USDWriteProjectionTest, OneProjectionOnTwoCardsApartBakesEachFromItsOwnPlacement)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    createRig();
    ASSERT_TRUE(projector && card);
    getKnob<KnobChoice>(projector, kUSDProject3DParamProjectionSpace)->setValue((int)USDProject3D::eProjectionSpaceGeometry);
    getKnob<KnobBool>(projector, kUSDProject3DParamUseFrameHold)->setValue(true);
    getKnob<KnobDouble>(projector, kUSDProject3DParamFrameHold)->setValue(1.);
    NodePtr other = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
    NodePtr assign = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));
    ASSERT_TRUE(other && merge && assign);
    other->setScriptName("Card2");
    KnobDouble* translate = getKnob<KnobDouble>(card, kSceneParamTranslate);
    translate->setValueAtTime(1., 0., ViewSpec::all(), 0);
    translate->setValueAtTime(3., 0.5, ViewSpec::all(), 0);
    KnobDouble* otherTranslate = getKnob<KnobDouble>(other, kSceneParamTranslate);
    otherTranslate->setValueAtTime(1., 0.5, ViewSpec::all(), 0);
    otherTranslate->setValueAtTime(3., 0.25, ViewSpec::all(), 0);
    ASSERT_TRUE(merge->connectInput(card, 0));
    ASSERT_TRUE(merge->connectInput(other, 1));

    const auto check = [&](const NodePtr& input, const char* name, USDWrite::ExportModeEnum mode) {
        const QString dir = tmp.path() + QLatin1Char('/') + QString::fromUtf8(name);
        ASSERT_TRUE(QDir().mkpath(dir));
        const QString path = dir + QString::fromUtf8("/scene.usda");
        NodePtr writer = createWriter(input, path, mode, 1, 3);
        ASSERT_TRUE(writer && getWriter(writer));
        ASSERT_EQ(eStatusOK, writeOffMainThread(writer, 1, 3)) << name;
        const PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(path.toStdString());
        ASSERT_TRUE(bool(stage)) << name;
        // A quarter of the frame is 0.5 world units: the second card starts that far right of the first.
        const std::pair<const char*, float> cards[] = { std::make_pair("/Card1", 0.f), std::make_pair("/Card2", 0.25f) };
        for (const std::pair<const char*, float>& expected : cards) {
            const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(PXR_NS::SdfPath(expected.first));
            ASSERT_TRUE(bool(prim)) << name << " " << expected.first;
            const PXR_NS::UsdGeomPrimvar st = PXR_NS::UsdGeomPrimvarsAPI(prim).GetPrimvar(PXR_NS::TfToken("st"));
            ASSERT_TRUE(bool(st)) << name << " " << expected.first;
            for (const double time : { 1., 3. }) {
                const std::pair<PXR_NS::GfVec2f, PXR_NS::GfVec2f> bounds = boundsOf(st, PXR_NS::UsdTimeCode(time));
                EXPECT_NEAR(expected.second, bounds.first[0], 1e-4f) << name << " " << expected.first << " at " << time;
                EXPECT_NEAR(expected.second + 1.f, bounds.second[0], 1e-4f) << name << " " << expected.first << " at " << time;
                EXPECT_NEAR(0.f, bounds.first[1], 1e-4f) << name << " " << expected.first << " at " << time;
                EXPECT_NEAR(1.f, bounds.second[1], 1e-4f) << name << " " << expected.first << " at " << time;
            }
        }
        for (const std::pair<const QString, QByteArray>& file : fileContents(dir)) {
            for (const char* needle : { "natronProjectionFrame", kUSDProject3DFrameReaderName, kUSDProject3DInputGeometryFrame, kUSDProject3DAttrFrameHold }) {
                EXPECT_FALSE(file.second.contains(needle)) << name << " " << file.first.toStdString() << " holds " << needle;
            }
        }
    };

    ASSERT_TRUE(other->connectInput(projector, 0));
    check(merge, "direct", USDWrite::eExportModeFlattened);

    card->disconnectInput(0);
    other->disconnectInput(0);
    getKnob<KnobString>(assign, kUSDAssignMaterialParamPrimPattern)->setValue("/Card1 /Card2");
    ASSERT_TRUE(assign->connectInput(merge, kUSDAssignMaterialInputScene));
    ASSERT_TRUE(assign->connectInput(projector, kUSDAssignMaterialInputMaterial));
    check(assign, "flattened", USDWrite::eExportModeFlattened);
    check(assign, "layered", USDWrite::eExportModeLayered);
}

TEST_F(USDWriteProjectionTest, ADeformingMeshBakesOneSetOfUVsFromItsPointsAtTheHold)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString source = tmp.path() + QString::fromUtf8("/source.usda");
    {
        const PXR_NS::UsdStageRefPtr file = PXR_NS::UsdStage::CreateNew(source.toStdString());
        ASSERT_TRUE(bool(file));
        const PXR_NS::UsdGeomMesh mesh = defineFrameQuad(file, "/Moving");
        mesh.CreatePointsAttr().Set(framePoints(0.f), PXR_NS::UsdTimeCode(1.));
        mesh.CreatePointsAttr().Set(framePoints(1.f), PXR_NS::UsdTimeCode(3.));
        ASSERT_TRUE(file->GetRootLayer()->Save());
    }
    NodePtr assign = createHeldAssignOf(source, "/Moving");
    ASSERT_TRUE(bool(assign));

    const std::pair<const char*, USDWrite::ExportModeEnum> modes[] = {
        std::make_pair("flattened", USDWrite::eExportModeFlattened),
        std::make_pair("layered", USDWrite::eExportModeLayered),
    };
    for (const std::pair<const char*, USDWrite::ExportModeEnum>& mode : modes) {
        const QString dir = tmp.path() + QLatin1Char('/') + QString::fromUtf8(mode.first);
        const PXR_NS::UsdStageRefPtr stage = writeAndOpen(assign, dir, mode.second);
        ASSERT_TRUE(bool(stage)) << mode.first;
        const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(PXR_NS::SdfPath("/Moving"));
        ASSERT_TRUE(bool(prim)) << mode.first;
        expectFrameQuadUVs(prim, PXR_NS::UsdGeomTokens->faceVarying, mode.first);
        PXR_NS::VtVec3fArray moved;
        ASSERT_TRUE(PXR_NS::UsdGeomMesh(prim).GetPointsAttr().Get(&moved, PXR_NS::UsdTimeCode(3.))) << mode.first;
        ASSERT_EQ((std::size_t)4, moved.size()) << mode.first;
        EXPECT_NEAR(0.f, moved[0][0], 1e-5f) << mode.first;
        expectNoProjectionData(dir, mode.first);
    }
}

TEST_F(USDWriteProjectionTest, ASkinnedMeshBakesOneSetOfUVsFromItsSkinnedPointsAtTheHold)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString source = tmp.path() + QString::fromUtf8("/source.usda");
    {
        const PXR_NS::UsdStageRefPtr file = PXR_NS::UsdStage::CreateNew(source.toStdString());
        ASSERT_TRUE(bool(file));
        PXR_NS::UsdSkelRoot::Define(file, PXR_NS::SdfPath("/Rig"));
        const PXR_NS::UsdSkelSkeleton skeleton = PXR_NS::UsdSkelSkeleton::Define(file, PXR_NS::SdfPath("/Rig/Skeleton"));
        const PXR_NS::VtTokenArray joints(1, PXR_NS::TfToken("Joint"));
        const PXR_NS::VtMatrix4dArray identity(1, PXR_NS::GfMatrix4d(1.));
        skeleton.CreateJointsAttr().Set(joints);
        skeleton.CreateBindTransformsAttr().Set(identity);
        skeleton.CreateRestTransformsAttr().Set(identity);
        const PXR_NS::UsdSkelAnimation animation = PXR_NS::UsdSkelAnimation::Define(file, PXR_NS::SdfPath("/Rig/Skeleton/Animation"));
        animation.CreateJointsAttr().Set(joints);
        animation.CreateRotationsAttr().Set(PXR_NS::VtQuatfArray(1, PXR_NS::GfQuatf(1.f)));
        animation.CreateScalesAttr().Set(PXR_NS::VtVec3hArray(1, PXR_NS::GfVec3h(1.f, 1.f, 1.f)));
        animation.CreateTranslationsAttr().Set(PXR_NS::VtVec3fArray(1, PXR_NS::GfVec3f(0.f)), PXR_NS::UsdTimeCode(1.));
        animation.CreateTranslationsAttr().Set(PXR_NS::VtVec3fArray(1, PXR_NS::GfVec3f(1.f, 0.f, 0.f)), PXR_NS::UsdTimeCode(3.));
        PXR_NS::UsdSkelBindingAPI::Apply(skeleton.GetPrim()).CreateAnimationSourceRel().SetTargets({ animation.GetPath() });
        const PXR_NS::UsdGeomMesh mesh = defineFrameQuad(file, "/Rig/Moving");
        mesh.CreatePointsAttr().Set(framePoints(0.f));
        const PXR_NS::UsdSkelBindingAPI binding = PXR_NS::UsdSkelBindingAPI::Apply(mesh.GetPrim());
        binding.CreateSkeletonRel().SetTargets({ skeleton.GetPath() });
        binding.CreateJointIndicesPrimvar(true, 1).Set(PXR_NS::VtIntArray(1, 0));
        binding.CreateJointWeightsPrimvar(true, 1).Set(PXR_NS::VtFloatArray(1, 1.f));
        ASSERT_TRUE(file->GetRootLayer()->Save());
    }
    NodePtr assign = createHeldAssignOf(source, "/Rig/Moving");
    ASSERT_TRUE(bool(assign));

    const QString dir = tmp.path() + QString::fromUtf8("/out");
    const PXR_NS::UsdStageRefPtr stage = writeAndOpen(assign, dir, USDWrite::eExportModeFlattened);
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(PXR_NS::SdfPath("/Rig/Moving"));
    ASSERT_TRUE(bool(prim));
    expectFrameQuadUVs(prim, PXR_NS::UsdGeomTokens->faceVarying, "the skinned mesh");
    expectNoProjectionData(dir, "the skinned mesh");
}

TEST_F(USDWriteProjectionTest, DeformingCurvesAndPointsBakeOneSetOfVertexUVs)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString source = tmp.path() + QString::fromUtf8("/source.usda");
    {
        const PXR_NS::UsdStageRefPtr file = PXR_NS::UsdStage::CreateNew(source.toStdString());
        ASSERT_TRUE(bool(file));
        const PXR_NS::UsdGeomBasisCurves curves = PXR_NS::UsdGeomBasisCurves::Define(file, PXR_NS::SdfPath("/Curves"));
        curves.CreateCurveVertexCountsAttr().Set(PXR_NS::VtIntArray(1, 4));
        curves.CreatePointsAttr().Set(framePoints(0.f), PXR_NS::UsdTimeCode(1.));
        curves.CreatePointsAttr().Set(framePoints(1.f), PXR_NS::UsdTimeCode(3.));
        const PXR_NS::UsdGeomPoints points = PXR_NS::UsdGeomPoints::Define(file, PXR_NS::SdfPath("/Points"));
        points.CreatePointsAttr().Set(framePoints(0.f), PXR_NS::UsdTimeCode(1.));
        points.CreatePointsAttr().Set(framePoints(1.f), PXR_NS::UsdTimeCode(3.));
        ASSERT_TRUE(file->GetRootLayer()->Save());
    }
    NodePtr assign = createHeldAssignOf(source, "/Curves /Points");
    ASSERT_TRUE(bool(assign));

    const QString dir = tmp.path() + QString::fromUtf8("/out");
    const PXR_NS::UsdStageRefPtr stage = writeAndOpen(assign, dir, USDWrite::eExportModeFlattened);
    ASSERT_TRUE(bool(stage));
    for (const char* name : { "/Curves", "/Points" }) {
        const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(PXR_NS::SdfPath(name));
        ASSERT_TRUE(bool(prim)) << name;
        expectFrameQuadUVs(prim, PXR_NS::UsdGeomTokens->vertex, name);
    }
    expectNoProjectionData(dir, "curves and points");
}

// A still prim holds no reference points to bake from, so it keeps the live projection as it always has.
TEST_F(USDWriteProjectionTest, StillCurvesAndPointsStayUnbaked)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString source = tmp.path() + QString::fromUtf8("/source.usda");
    {
        const PXR_NS::UsdStageRefPtr file = PXR_NS::UsdStage::CreateNew(source.toStdString());
        ASSERT_TRUE(bool(file));
        const PXR_NS::UsdGeomBasisCurves curves = PXR_NS::UsdGeomBasisCurves::Define(file, PXR_NS::SdfPath("/Curves"));
        curves.CreateCurveVertexCountsAttr().Set(PXR_NS::VtIntArray(1, 4));
        curves.CreatePointsAttr().Set(framePoints(0.f));
        const PXR_NS::UsdGeomPoints points = PXR_NS::UsdGeomPoints::Define(file, PXR_NS::SdfPath("/Points"));
        points.CreatePointsAttr().Set(framePoints(0.f));
        ASSERT_TRUE(file->GetRootLayer()->Save());
    }
    NodePtr assign = createHeldAssignOf(source, "/Curves /Points");
    ASSERT_TRUE(bool(assign));

    const QString dir = tmp.path() + QString::fromUtf8("/out");
    const PXR_NS::UsdStageRefPtr stage = writeAndOpen(assign, dir, USDWrite::eExportModeFlattened);
    ASSERT_TRUE(bool(stage));
    for (const char* name : { "/Curves", "/Points" }) {
        const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(PXR_NS::SdfPath(name));
        ASSERT_TRUE(bool(prim)) << name;
        const PXR_NS::UsdGeomPrimvar st = PXR_NS::UsdGeomPrimvarsAPI(prim).GetPrimvar(PXR_NS::TfToken("st"));
        EXPECT_FALSE(st.GetAttr() && st.HasValue()) << name;
        const PXR_NS::UsdShadeMaterial material = PXR_NS::UsdShadeMaterialBindingAPI(prim).ComputeBoundMaterial();
        ASSERT_TRUE(bool(material)) << name;
        SceneGeometryProjection projection;
        EXPECT_TRUE(SceneNodeAuthoring::readGeometryProjection(material.GetPrim(), &projection)) << name;
        const PXR_NS::UsdGeomPrimvar frame = PXR_NS::UsdGeomPrimvarsAPI(prim).GetPrimvar(PXR_NS::TfToken(projection.framePrimvar));
        EXPECT_TRUE(frame.GetAttr() && frame.HasValue()) << name;
    }
}

TEST_F(USDWriteProjectionTest, AnExportLeavesAPropertyThatOnlySharesAProjectionPrimvarsName)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const std::string name("natronReferencePointsHold1");
    const QString source = tmp.path() + QString::fromUtf8("/source.usda");
    {
        const PXR_NS::UsdStageRefPtr file = PXR_NS::UsdStage::CreateNew(source.toStdString());
        ASSERT_TRUE(bool(file));
        const PXR_NS::UsdGeomMesh mesh = defineFrameQuad(file, "/Moving");
        mesh.CreatePointsAttr().Set(framePoints(0.f), PXR_NS::UsdTimeCode(1.));
        mesh.CreatePointsAttr().Set(framePoints(1.f), PXR_NS::UsdTimeCode(3.));
        const PXR_NS::UsdPrim prim = file->DefinePrim(PXR_NS::SdfPath("/Elsewhere"), PXR_NS::TfToken("Scope"));
        PXR_NS::UsdGeomPrimvarsAPI(prim).CreatePrimvar(PXR_NS::TfToken(name), PXR_NS::SdfValueTypeNames->Point3fArray, PXR_NS::UsdGeomTokens->vertex).Set(framePoints(2.f));
        ASSERT_TRUE(file->GetRootLayer()->Save());
    }
    NodePtr assign = createHeldAssignOf(source, "/Moving");
    ASSERT_TRUE(bool(assign));

    const std::pair<const char*, USDWrite::ExportModeEnum> modes[] = {
        { "/flattened", USDWrite::eExportModeFlattened },
        { "/layered", USDWrite::eExportModeLayered },
    };
    for (const std::pair<const char*, USDWrite::ExportModeEnum>& mode : modes) {
        const PXR_NS::UsdStageRefPtr stage = writeAndOpen(assign, tmp.path() + QString::fromUtf8(mode.first), mode.second);
        ASSERT_TRUE(bool(stage)) << mode.first;
        const PXR_NS::UsdGeomPrimvar baked = PXR_NS::UsdGeomPrimvarsAPI(stage->GetPrimAtPath(PXR_NS::SdfPath("/Moving"))).GetPrimvar(PXR_NS::TfToken(name));
        EXPECT_FALSE(baked.GetAttr() && baked.HasValue()) << mode.first;
        const PXR_NS::UsdGeomPrimvar unrelated = PXR_NS::UsdGeomPrimvarsAPI(stage->GetPrimAtPath(PXR_NS::SdfPath("/Elsewhere"))).GetPrimvar(PXR_NS::TfToken(name));
        EXPECT_TRUE(unrelated.GetAttr() && unrelated.HasValue()) << mode.first;
    }
}

TEST_F(USDWriteProjectionTest, ABakedProjectionLosesItsDataWhileAnUnbakedOneKeepsItsOwn)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString source = tmp.path() + QString::fromUtf8("/source.usda");
    {
        const PXR_NS::UsdStageRefPtr file = PXR_NS::UsdStage::CreateNew(source.toStdString());
        ASSERT_TRUE(bool(file));
        const PXR_NS::UsdGeomMesh mesh = defineFrameQuad(file, "/Moving");
        mesh.CreatePointsAttr().Set(framePoints(0.f), PXR_NS::UsdTimeCode(1.));
        mesh.CreatePointsAttr().Set(framePoints(1.f), PXR_NS::UsdTimeCode(3.));
        const PXR_NS::UsdGeomBasisCurves curves = PXR_NS::UsdGeomBasisCurves::Define(file, PXR_NS::SdfPath("/Curves"));
        curves.CreateCurveVertexCountsAttr().Set(PXR_NS::VtIntArray(1, 4));
        curves.CreatePointsAttr().Set(framePoints(0.f));
        ASSERT_TRUE(file->GetRootLayer()->Save());
    }
    NodePtr first = createHeldAssignOf(source, "/Moving");
    ASSERT_TRUE(bool(first));
    NodePtr other = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPROJECT3D));
    NodePtr second = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));
    ASSERT_TRUE(other && second);
    other->setScriptName("Project2");
    ASSERT_TRUE(other->connectInput(plate, kUSDProject3DInputImage));
    ASSERT_TRUE(other->connectInput(camera, kUSDProject3DInputCamera));
    getKnob<KnobChoice>(other, kUSDProject3DParamProjectionSpace)->setValue((int)USDProject3D::eProjectionSpaceGeometry);
    getKnob<KnobBool>(other, kUSDProject3DParamUseFrameHold)->setValue(true);
    getKnob<KnobDouble>(other, kUSDProject3DParamFrameHold)->setValue(1.);
    getKnob<KnobString>(second, kUSDAssignMaterialParamPrimPattern)->setValue("/Curves");
    ASSERT_TRUE(second->connectInput(first, kUSDAssignMaterialInputScene));
    ASSERT_TRUE(second->connectInput(other, kUSDAssignMaterialInputMaterial));

    const std::pair<const char*, USDWrite::ExportModeEnum> modes[] = {
        { "/flattened", USDWrite::eExportModeFlattened },
        { "/layered", USDWrite::eExportModeLayered },
    };
    for (const std::pair<const char*, USDWrite::ExportModeEnum>& mode : modes) {
        const PXR_NS::UsdStageRefPtr stage = writeAndOpen(second, tmp.path() + QString::fromUtf8(mode.first), mode.second);
        ASSERT_TRUE(bool(stage)) << mode.first;

        const PXR_NS::UsdPrim moving = stage->GetPrimAtPath(PXR_NS::SdfPath("/Moving"));
        ASSERT_TRUE(bool(moving)) << mode.first;
        for (const PXR_NS::UsdGeomPrimvar& primvar : PXR_NS::UsdGeomPrimvarsAPI(moving).GetPrimvars()) {
            EXPECT_EQ(std::string::npos, primvar.GetPrimvarName().GetString().find("natronProjectionFrame")) << mode.first;
            EXPECT_EQ(std::string::npos, primvar.GetPrimvarName().GetString().find("natronReferencePoints")) << mode.first;
            EXPECT_EQ(std::string::npos, primvar.GetPrimvarName().GetString().find("natronReferencePresent")) << mode.first;
        }
        const PXR_NS::UsdShadeMaterial baked = PXR_NS::UsdShadeMaterialBindingAPI(moving).ComputeBoundMaterial();
        ASSERT_TRUE(bool(baked)) << mode.first;
        EXPECT_FALSE(baked.GetPrim().GetChild(PXR_NS::TfToken(kUSDProject3DFrameReaderName)).IsValid()) << mode.first;
        EXPECT_FALSE(baked.GetPrim().GetChild(PXR_NS::TfToken(kUSDProject3DReferenceReaderName)).IsValid()) << mode.first;

        const PXR_NS::UsdPrim curves = stage->GetPrimAtPath(PXR_NS::SdfPath("/Curves"));
        ASSERT_TRUE(bool(curves)) << mode.first;
        const PXR_NS::UsdShadeMaterial kept = PXR_NS::UsdShadeMaterialBindingAPI(curves).ComputeBoundMaterial();
        ASSERT_TRUE(bool(kept)) << mode.first;
        SceneGeometryProjection projection;
        ASSERT_TRUE(SceneNodeAuthoring::readGeometryProjection(kept.GetPrim(), &projection)) << mode.first;
        const PXR_NS::UsdGeomPrimvar frame = PXR_NS::UsdGeomPrimvarsAPI(curves).GetPrimvar(PXR_NS::TfToken(projection.framePrimvar));
        EXPECT_TRUE(frame.GetAttr() && frame.HasValue()) << mode.first;
        EXPECT_TRUE(kept.GetPrim().GetChild(PXR_NS::TfToken(kUSDProject3DFrameReaderName)).IsValid()) << mode.first;
    }
}

// The mesh bakes under its Full projection, while its Preview one stays a Storm shader because of
// the curves, and both read one primvar on the mesh.
TEST_F(USDWriteProjectionTest, APrimvarSharedByABakedAndAnUnbakedProjectionStays)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString source = tmp.path() + QString::fromUtf8("/source.usda");
    {
        const PXR_NS::UsdStageRefPtr file = PXR_NS::UsdStage::CreateNew(source.toStdString());
        ASSERT_TRUE(bool(file));
        const PXR_NS::UsdGeomMesh mesh = defineFrameQuad(file, "/Moving");
        mesh.CreatePointsAttr().Set(framePoints(0.f), PXR_NS::UsdTimeCode(1.));
        mesh.CreatePointsAttr().Set(framePoints(1.f), PXR_NS::UsdTimeCode(3.));
        const PXR_NS::UsdGeomBasisCurves curves = PXR_NS::UsdGeomBasisCurves::Define(file, PXR_NS::SdfPath("/Curves"));
        curves.CreateCurveVertexCountsAttr().Set(PXR_NS::VtIntArray(1, 4));
        curves.CreatePointsAttr().Set(framePoints(0.f));
        ASSERT_TRUE(file->GetRootLayer()->Save());
    }
    NodePtr first = createHeldAssignOf(source, "/Moving");
    ASSERT_TRUE(bool(first));
    getKnob<KnobChoice>(first, kUSDAssignMaterialParamPurpose)->setValue((int)USDAssignMaterial::ePurposeFull);
    NodePtr other = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPROJECT3D));
    NodePtr second = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));
    ASSERT_TRUE(other && second);
    other->setScriptName("Project2");
    ASSERT_TRUE(other->connectInput(plate, kUSDProject3DInputImage));
    ASSERT_TRUE(other->connectInput(camera, kUSDProject3DInputCamera));
    getKnob<KnobChoice>(other, kUSDProject3DParamProjectionSpace)->setValue((int)USDProject3D::eProjectionSpaceGeometry);
    getKnob<KnobBool>(other, kUSDProject3DParamUseFrameHold)->setValue(true);
    getKnob<KnobDouble>(other, kUSDProject3DParamFrameHold)->setValue(1.);
    getKnob<KnobString>(second, kUSDAssignMaterialParamPrimPattern)->setValue("/Moving /Curves");
    getKnob<KnobChoice>(second, kUSDAssignMaterialParamPurpose)->setValue((int)USDAssignMaterial::ePurposePreview);
    ASSERT_TRUE(second->connectInput(first, kUSDAssignMaterialInputScene));
    ASSERT_TRUE(second->connectInput(other, kUSDAssignMaterialInputMaterial));

    const std::pair<const char*, USDWrite::ExportModeEnum> modes[] = {
        { "/flattened", USDWrite::eExportModeFlattened },
        { "/layered", USDWrite::eExportModeLayered },
    };
    for (const std::pair<const char*, USDWrite::ExportModeEnum>& mode : modes) {
        const PXR_NS::UsdStageRefPtr stage = writeAndOpen(second, tmp.path() + QString::fromUtf8(mode.first), mode.second);
        ASSERT_TRUE(bool(stage)) << mode.first;
        const PXR_NS::UsdPrim moving = stage->GetPrimAtPath(PXR_NS::SdfPath("/Moving"));
        ASSERT_TRUE(bool(moving)) << mode.first;
        const PXR_NS::UsdGeomPrimvarsAPI primvars(moving);
        const PXR_NS::UsdGeomPrimvar frame = primvars.GetPrimvar(PXR_NS::TfToken("natronProjectionFrameHold1"));
        const PXR_NS::UsdGeomPrimvar reference = primvars.GetPrimvar(PXR_NS::TfToken("natronReferencePointsHold1"));
        EXPECT_TRUE(frame.GetAttr() && frame.HasValue()) << mode.first;
        EXPECT_TRUE(reference.GetAttr() && reference.HasValue()) << mode.first;

        const PXR_NS::UsdShadeMaterialBindingAPI binding(moving);
        const PXR_NS::UsdShadeMaterial baked = binding.ComputeBoundMaterial(PXR_NS::UsdShadeTokens->full);
        const PXR_NS::UsdShadeMaterial kept = binding.ComputeBoundMaterial(PXR_NS::UsdShadeTokens->preview);
        ASSERT_TRUE(baked && kept) << mode.first;
        EXPECT_NE(baked.GetPath(), kept.GetPath()) << mode.first;
        EXPECT_FALSE(baked.GetPrim().GetChild(PXR_NS::TfToken(kUSDProject3DFrameReaderName)).IsValid()) << mode.first;
        EXPECT_TRUE(kept.GetPrim().GetChild(PXR_NS::TfToken(kUSDProject3DFrameReaderName)).IsValid()) << mode.first;
        EXPECT_TRUE(kept.GetPrim().GetChild(PXR_NS::TfToken(kUSDProject3DReferenceReaderName)).IsValid()) << mode.first;
    }
}

// The camera's frame is the plate's format: with overscan, the baked UVs reach only the format's part
// of the exported texture, and as UVs cannot crop, the export warns that the overscan shows.
TEST_F(USDWriteProjectionTest, AnOverscanPlateBakesUVsSpanningItsFormat)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    createRig();
    ASSERT_TRUE(plate && card);
    getKnob<KnobChoice>(plate, kNativeGeneratorParamExtent)->setValue((int)ExtentKnobs::eExtentSize);
    KnobDouble* bottomLeft = getKnob<KnobDouble>(plate, kNativeGeneratorParamBottomLeft);
    KnobDouble* size = getKnob<KnobDouble>(plate, kNativeGeneratorParamSize);
    ASSERT_TRUE(bottomLeft && size);
    bottomLeft->setValue(-200., ViewSpec::all(), 0);
    bottomLeft->setValue(-100., ViewSpec::all(), 1);
    size->setValue(2400., ViewSpec::all(), 0);
    size->setValue(1200., ViewSpec::all(), 1);
    const QString path = tmp.path() + QString::fromUtf8("/scene.usda");
    NodePtr writer = createWriter(card, path, USDWrite::eExportModeFlattened, 1, 1);
    ASSERT_TRUE(writer && getWriter(writer));
    ASSERT_EQ(eStatusOK, writeOffMainThread(writer, 1, 1));
    ASSERT_TRUE(writer->hasPersistentMessage());
    const QString message = messageOf(writer);
    EXPECT_TRUE(message.contains(QString::fromUtf8("/Project1"))) << message.toStdString();
    EXPECT_TRUE(message.contains(QString::fromUtf8("format"))) << message.toStdString();

    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(path.toStdString());
    ASSERT_TRUE(bool(stage));
    const PXR_NS::UsdGeomPrimvar st = stOf(stage);
    ASSERT_TRUE(bool(st));
    const std::pair<PXR_NS::GfVec2f, PXR_NS::GfVec2f> bounds = boundsOf(st);
    EXPECT_NEAR(200.f / 2400.f, bounds.first[0], 1e-4f);
    EXPECT_NEAR(100.f / 1200.f, bounds.first[1], 1e-4f);
    EXPECT_NEAR(2200.f / 2400.f, bounds.second[0], 1e-4f);
    EXPECT_NEAR(1100.f / 1200.f, bounds.second[1], 1e-4f);
    // The exported texture holds the whole data window, overscan included.
    EXPECT_EQ(std::make_pair(2400, 1200), sizeOf(tmp.path() + QString::fromUtf8("/scene_textures/Plate1_rgba.exr")));
}

TEST_F(USDWriteTest, ExportsCarryNoNatronCustomData)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    NodePtr merge = createGraph(kReferenceScene, 0.);
    ASSERT_TRUE(bool(merge));

    const USDWrite::ExportModeEnum modes[] = { USDWrite::eExportModeLayered, USDWrite::eExportModeFlattened };
    for (USDWrite::ExportModeEnum mode : modes) {
        const QString dir = tmp.path() + QString::fromUtf8(mode == USDWrite::eExportModeLayered ? "/layered" : "/flattened");
        const QString path = dir + QString::fromUtf8("/out.usda");
        NodePtr writer = createWriter(merge, path, mode, 1, 1);
        ASSERT_TRUE(writer && getWriter(writer));
        ASSERT_EQ(eStatusOK, writeOffMainThread(writer, 1, 1));
        ASSERT_TRUE(QFile::exists(path));

        PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(path.toStdString());
        ASSERT_TRUE(static_cast<bool>(stage));
        for (const PXR_NS::UsdPrim& prim : stage->TraverseAll()) {
            for (const std::pair<const std::string, PXR_NS::VtValue>& entry : prim.GetCustomData()) {
                EXPECT_TRUE(entry.first.rfind("natron:", 0) != 0) << prim.GetPath().GetString() << " " << entry.first;
            }
        }
        for (const std::pair<const QString, QByteArray>& file : fileContents(dir)) {
            EXPECT_FALSE(file.second.contains("natron:")) << file.first.toStdString();
        }
    }
}
