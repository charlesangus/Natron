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

#include <map>
#include <string>
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

#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDMaterial.h"
#include "Engine/Nodes/Scene/USDMerge.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDWrite.h"
#include "Engine/Nodes/Scene/USDXformPrim.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

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
