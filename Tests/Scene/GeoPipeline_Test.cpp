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

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usdGeom/xformable.h>

#include "Global/Macros.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <QFile>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Geo/GeoSpreadsheet.h"
#include "Engine/GeoDetail.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Geometry/GeoCopyToPoints.h"
#include "Engine/Nodes/Geometry/GeoDelete.h"
#include "Engine/Nodes/Geometry/GeoDisplace.h"
#include "Engine/Nodes/Geometry/GeoEdit.h"
#include "Engine/Nodes/Geometry/GeoGroup.h"
#include "Engine/Nodes/Geometry/GeoNormals.h"
#include "Engine/Nodes/Geometry/GeoPrimitive.h"
#include "Engine/Nodes/Geometry/GeoScatter.h"
#include "Engine/Nodes/Geometry/GeoToUSD.h"
#include "Engine/Nodes/Geometry/GeoTransform.h"
#include "Engine/Nodes/Geometry/GeoUVProject.h"
#include "Engine/Nodes/Geometry/GeoWrangle.h"
#include "Engine/Nodes/Geometry/USDToGeo.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDMerge.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDWrite.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/TimeLine.h"
#include "Engine/UndoCommand.h"
#include "Engine/UndoRecorder.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

// /World/A is a quad, /World/B a triangle translated by 10 in X, /World/C a cube.
const char* const kMeshScene = "#usda 1.0\n"
                               "(\n"
                               "    metersPerUnit = 0.01\n"
                               "    upAxis = \"Y\"\n"
                               ")\n"
                               "\n"
                               "def Xform \"World\"\n"
                               "{\n"
                               "    def Mesh \"A\"\n"
                               "    {\n"
                               "        int[] faceVertexCounts = [4]\n"
                               "        int[] faceVertexIndices = [0, 1, 2, 3]\n"
                               "        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]\n"
                               "    }\n"
                               "\n"
                               "    def Mesh \"B\"\n"
                               "    {\n"
                               "        int[] faceVertexCounts = [3]\n"
                               "        int[] faceVertexIndices = [0, 1, 2]\n"
                               "        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n"
                               "        double3 xformOp:translate = (10, 0, 0)\n"
                               "        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                               "    }\n"
                               "\n"
                               "    def Cube \"C\"\n"
                               "    {\n"
                               "    }\n"
                               "}\n";

// One triangle, merged next to the geometry the graph authors.
const char* const kOtherScene = "#usda 1.0\n"
                                "(\n"
                                "    metersPerUnit = 0.01\n"
                                "    upAxis = \"Y\"\n"
                                ")\n"
                                "\n"
                                "def Xform \"Other\"\n"
                                "{\n"
                                "    def Mesh \"Tri\"\n"
                                "    {\n"
                                "        int[] faceVertexCounts = [3]\n"
                                "        int[] faceVertexIndices = [0, 1, 2]\n"
                                "        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n"
                                "    }\n"
                                "}\n";

constexpr double kTolerance = 1e-4;

// Keeps what is recorded inside an open step.
class StepRecorder
    : public UndoRecorderI {
public:
    StepRecorder()
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

struct MeshCounts {
    std::size_t points = 0;
    std::size_t faces = 0;

    bool operator==(const MeshCounts& other) const
    {
        return points == other.points && faces == other.faces;
    }
};

// Every mesh prim under /geo, which is where GeoToUSD puts what it authors.
std::vector<PXR_NS::UsdGeomMesh>
authoredMeshes(const PXR_NS::UsdStageRefPtr& stage)
{
    std::vector<PXR_NS::UsdGeomMesh> meshes;
    const PXR_NS::SdfPath root("/geo");
    for (const PXR_NS::UsdPrim& prim : stage->Traverse()) {
        if (prim.GetPath().HasPrefix(root) && prim.IsA<PXR_NS::UsdGeomMesh>()) {
            meshes.push_back(PXR_NS::UsdGeomMesh(prim));
        }
    }

    return meshes;
}

PXR_NS::VtVec3fArray
meshPoints(const PXR_NS::UsdGeomMesh& mesh)
{
    PXR_NS::VtVec3fArray points;
    EXPECT_TRUE(mesh.GetPointsAttr().Get(&points, PXR_NS::UsdTimeCode(1.)));

    return points;
}

MeshCounts
countsOf(const PXR_NS::UsdGeomMesh& mesh)
{
    PXR_NS::VtIntArray faces;
    EXPECT_TRUE(mesh.GetFaceVertexCountsAttr().Get(&faces, PXR_NS::UsdTimeCode(1.)));
    MeshCounts counts;
    counts.points = meshPoints(mesh).size();
    counts.faces = faces.size();

    return counts;
}

} // anonymous namespace

class GeoPipelineTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        KnobDouble* metersPerUnit = dynamic_cast<KnobDouble*>(getApp()->getProject()->getKnobByName("metersPerUnit").get());
        ASSERT_TRUE(metersPerUnit != NULL);
        metersPerUnit->setValue(0.01);
    }

    virtual void TearDown() OVERRIDE
    {
        for (auto it = _nodes.rbegin(); it != _nodes.rend(); ++it) {
            if (*it) {
                (*it)->destroyNode(false, false);
            }
        }
        _nodes.clear();
        SceneQueryPool::instance().releaseIdle();

        BaseTest::TearDown();
    }

    NodePtr make(const char* pluginID)
    {
        NodePtr node = createNode(QString::fromUtf8(pluginID));

        EXPECT_TRUE(bool(node)) << pluginID;
        if (node) {
            _nodes.push_back(node);
        }

        return node;
    }

    template <typename K>
    static K* knob(const NodePtr& node,
                   const char* name)
    {
        K* k = dynamic_cast<K*>(node->getKnobByName(name).get());

        EXPECT_TRUE(k != NULL) << name;

        return k;
    }

    static void setString(const NodePtr& node,
                          const char* name,
                          const std::string& value)
    {
        KnobString* k = knob<KnobString>(node, name);
        ASSERT_TRUE(k != NULL);
        k->setValue(value);
    }

    static void setBool(const NodePtr& node,
                        const char* name,
                        bool value)
    {
        KnobBool* k = knob<KnobBool>(node, name);
        ASSERT_TRUE(k != NULL);
        k->setValue(value);
    }

    static void setInt(const NodePtr& node,
                       const char* name,
                       int value)
    {
        KnobInt* k = knob<KnobInt>(node, name);
        ASSERT_TRUE(k != NULL);
        k->setValue(value);
    }

    static void setChoice(const NodePtr& node,
                          const char* name,
                          const char* id)
    {
        KnobChoice* k = knob<KnobChoice>(node, name);
        ASSERT_TRUE(k != NULL);
        k->setValueFromID(id, 0);
    }

    static void setVector(const NodePtr& node,
                          const char* name,
                          double x,
                          double y,
                          double z)
    {
        KnobDouble* k = knob<KnobDouble>(node, name);
        ASSERT_TRUE(k != NULL);
        k->setValue(x, ViewSpec::all(), 0);
        k->setValue(y, ViewSpec::all(), 1);
        k->setValue(z, ViewSpec::all(), 2);
    }

    std::string tempPath(const std::string& name) const
    {
        return _dir.path().toStdString() + "/" + name;
    }

    std::string writeScene(const std::string& name,
                           const std::string& content) const
    {
        const std::string path = tempPath(name);
        QFile f(QString::fromStdString(path));

        EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(content.c_str(), (qint64)content.size());

        return path;
    }

    NodePtr grid(int rows,
                 int columns)
    {
        NodePtr node = make(PLUGINID_NATRON_GEOPRIMITIVE);
        if (node) {
            setChoice(node, kGeoPrimitiveParamType, "grid");
            setInt(node, kGeoPrimitiveParamGridRows, rows);
            setInt(node, kGeoPrimitiveParamGridColumns, columns);
        }

        return node;
    }

    NodePtr reader(const std::string& path)
    {
        NodePtr node = make(PLUGINID_NATRON_USDREAD);
        if (node) {
            KnobFile* file = knob<KnobFile>(node, kUSDReadParamFile);
            if (file) {
                file->setValue(path);
            }
        }

        return node;
    }

    // Connects from to input index of to, creating nothing.
    void wire(const NodePtr& from,
              const NodePtr& to,
              int index)
    {
        ASSERT_TRUE(from && to);
        ASSERT_TRUE(to->connectInput(from, index));
    }

    // A node of pluginID fed by input on its first input.
    NodePtr after(const NodePtr& input,
                  const char* pluginID)
    {
        NodePtr node = make(pluginID);
        if (node) {
            wire(input, node, 0);
        }

        return node;
    }

    GeoDetailConstPtr pull(const NodePtr& node,
                           double time = 1.)
    {
        GeoDetailConstPtr detail;
        std::string error;

        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(node, &detail, &error, time)) << error;

        return detail;
    }

    EffectInstance::RenderRoIRetCode evaluate(const NodePtr& node,
                                              GeoDetailConstPtr* detail,
                                              std::string* error,
                                              double time = 1.)
    {
        AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
        ParallelRenderArgsSetter frameRenderArgs(time,
                                                 ViewIdx(0),
                                                 true /*isRenderUserInteraction*/,
                                                 false /*isSequential*/,
                                                 abortInfo,
                                                 node,
                                                 0 /*textureIndex*/,
                                                 getApp()->getTimeLine().get(),
                                                 NodePtr(),
                                                 false /*isAnalysis*/,
                                                 false /*draftMode*/,
                                                 RenderStatsPtr());

        return node->getEffectInstance()->evaluateGeometry(EffectInstance::EvaluateGeometryArgs(time, ViewIdx(0)), detail, error);
    }

    // A 2 by 2 grid displaced by image on GeoDisplace's Image input, one unit per unit of luminance.
    NodePtr displacedBy(const NodePtr& image)
    {
        const NodePtr source = grid(2, 2);
        const NodePtr displace = source ? after(source, PLUGINID_NATRON_GEODISPLACE) : NodePtr();
        if (!displace) {
            return NodePtr();
        }
        setChoice(displace, kGeoDisplaceParamDisplaceBy, "image");
        knob<KnobDouble>(displace, kGeoDisplaceParamAmplitude)->setValue(1.);
        knob<KnobDouble>(displace, kGeoDisplaceParamOffset)->setValue(0.);
        EXPECT_TRUE(displace->connectInput(image, 1));

        return displace;
    }

    // Writes the scene node's output to a flattened file in the temp dir and opens it.
    PXR_NS::UsdStageRefPtr writeUsd(const NodePtr& scene,
                                    const std::string& name)
    {
        NodePtr writer = make(PLUGINID_NATRON_USDWRITE);
        if (!writer) {
            return PXR_NS::UsdStageRefPtr();
        }
        wire(scene, writer, 0);
        const std::string path = tempPath(name);
        KnobOutputFile* file = knob<KnobOutputFile>(writer, kUSDWriteParamFile);
        KnobChoice* exportMode = knob<KnobChoice>(writer, kUSDWriteParamExportMode);
        KnobChoice* pathMode = knob<KnobChoice>(writer, kUSDWriteParamPathMode);
        KnobChoice* frameRange = knob<KnobChoice>(writer, kUSDWriteParamFrameRange);
        KnobInt* first = knob<KnobInt>(writer, kUSDWriteParamFirstFrame);
        KnobInt* last = knob<KnobInt>(writer, kUSDWriteParamLastFrame);
        if (!(file && exportMode && pathMode && frameRange && first && last)) {
            return PXR_NS::UsdStageRefPtr();
        }
        file->setValue(path);
        exportMode->setValue((int)USDWrite::eExportModeFlattened);
        pathMode->setValue((int)USDWrite::ePathModeRelative);
        frameRange->setValue((int)USDWrite::eFrameRangeManual);
        first->setValue(1);
        last->setValue(1);

        USDWrite* effect = dynamic_cast<USDWrite*>(writer->getEffectInstance().get());
        EXPECT_TRUE(effect != NULL);
        if (!effect || effect->writeFrameRange(1, 1, ViewIdx(0)) != eStatusOK) {
            ADD_FAILURE() << "the write failed: " << name;

            return PXR_NS::UsdStageRefPtr();
        }
        PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(path);
        EXPECT_TRUE(static_cast<bool>(stage)) << path;

        return stage;
    }

    // The mesh GeoToUSD authored, which must be the only one under /geo.
    static PXR_NS::UsdGeomMesh onlyMesh(const PXR_NS::UsdStageRefPtr& stage)
    {
        const std::vector<PXR_NS::UsdGeomMesh> meshes = authoredMeshes(stage);
        EXPECT_EQ(1u, meshes.size());

        return meshes.empty() ? PXR_NS::UsdGeomMesh() : meshes.front();
    }

    // grid -> GeoDelete(group, type) -> GeoToUSD -> USDWrite; the exported mesh's counts.
    MeshCounts deletedAndExported(const NodePtr& source,
                                  const std::string& group,
                                  const char* groupType,
                                  const std::string& file)
    {
        NodePtr remove = after(source, PLUGINID_NATRON_GEODELETE);
        NodePtr toUSD = remove ? after(remove, PLUGINID_NATRON_GEOTOUSD) : NodePtr();
        if (!toUSD) {
            return MeshCounts();
        }
        setString(remove, kGeoDeleteParamGroup, group);
        setChoice(remove, kGeoDeleteParamGroupType, groupType);
        const PXR_NS::UsdStageRefPtr stage = writeUsd(toUSD, file);
        if (!stage) {
            return MeshCounts();
        }
        const PXR_NS::UsdGeomMesh mesh = onlyMesh(stage);

        return mesh ? countsOf(mesh) : MeshCounts();
    }

private:
    QTemporaryDir _dir;
    std::vector<NodePtr> _nodes;
};

// Group, delete, displace, normals, wrangle and edit nodes, bridged into USD, merged with a read
// scene and written; the file holds what the geometry chain made, and reading it back gives the
// same geometry.
TEST_F(GeoPipelineTest, FullChainExportsTheGeometryAndReadsBackAsTheSameDetail)
{
    const NodePtr source = grid(9, 9);
    const NodePtr transform = after(source, PLUGINID_NATRON_GEOTRANSFORM);
    ASSERT_TRUE(source && transform);
    setVector(transform, kGeoTransformParamTranslate, 0.5, 0.25, -0.75);

    const GeoDetailConstPtr moved = pull(transform);
    ASSERT_TRUE(moved != nullptr);
    ASSERT_GT(moved->pointCount(), GeoIndex(0));
    const std::array<double, 3> corner = moved->pointPosition(moved->pointMap().offsetFromIndex(0));

    const NodePtr group = after(transform, PLUGINID_NATRON_GEOGROUP);
    ASSERT_TRUE(group != nullptr);
    setString(group, kGeoGroupParamGroupName, "cut");
    setChoice(group, kGeoGroupParamGroupType, "points");
    setBool(group, kGeoGroupParamUseBounding, true);
    setVector(group, kGeoGroupParamCentre, corner[0], corner[1], corner[2]);
    setVector(group, kGeoGroupParamSize, 1e-3, 1e-3, 1e-3);
    setBool(group, kGeoGroupParamExpand, true);
    setInt(group, kGeoGroupParamExpandSteps, 1);
    setBool(group, kGeoGroupParamPromote, true);
    setChoice(group, kGeoGroupParamPromoteTo, "primitives");

    const NodePtr remove = after(group, PLUGINID_NATRON_GEODELETE);
    ASSERT_TRUE(remove != nullptr);
    setString(remove, kGeoDeleteParamGroup, "cut");
    setChoice(remove, kGeoDeleteParamGroupType, "primitives");

    // The displace node has no constant mode: an amplitude of 0 leaves the offset as a constant push along the normal.
    const NodePtr displace = after(remove, PLUGINID_NATRON_GEODISPLACE);
    ASSERT_TRUE(displace != nullptr);
    knob<KnobDouble>(displace, kGeoDisplaceParamAmplitude)->setValue(0.);
    knob<KnobDouble>(displace, kGeoDisplaceParamOffset)->setValue(0.05);

    const NodePtr normals = after(displace, PLUGINID_NATRON_GEONORMALS);
    const NodePtr wrangle = normals ? after(normals, PLUGINID_NATRON_GEOWRANGLE) : NodePtr();
    ASSERT_TRUE(wrangle != nullptr);
    setString(wrangle, kGeoWrangleParamSnippet, "@P.y += 0.1;");

    const GeoDetailConstPtr beforeEdit = pull(wrangle);
    ASSERT_TRUE(beforeEdit != nullptr);
    ASSERT_GT(beforeEdit->pointCount(), GeoIndex(5));
    const GeoDetailConstPtr grown = pull(source);
    ASSERT_TRUE(grown != nullptr);
    EXPECT_EQ(grown->primitiveCount() - 3, beforeEdit->primitiveCount());

    const GeoIndex editedPoint = 5;
    const std::array<double, 3> offset = { 0.25, 0.5, -0.75 };
    GeoEditData edits;
    edits.pointCount = beforeEdit->pointCount();
    edits.primitiveCount = beforeEdit->primitiveCount();
    edits.pointOffsets[editedPoint] = offset;
    const NodePtr edit = after(wrangle, PLUGINID_NATRON_GEOEDIT);
    ASSERT_TRUE(edit != nullptr);
    setString(edit, kGeoEditParamEdits, edits.encode());

    const GeoDetailConstPtr bridged = pull(edit);
    ASSERT_TRUE(bridged != nullptr);
    EXPECT_EQ(beforeEdit->pointCount(), bridged->pointCount());
    const std::array<double, 3> unedited = beforeEdit->pointPosition(beforeEdit->pointMap().offsetFromIndex(editedPoint));
    const std::array<double, 3> edited = bridged->pointPosition(bridged->pointMap().offsetFromIndex(editedPoint));
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(unedited[i] + offset[i], edited[i], kTolerance) << i;
    }

    const NodePtr toUSD = after(edit, PLUGINID_NATRON_GEOTOUSD);
    const NodePtr other = reader(writeScene("other.usda", kOtherScene));
    const NodePtr merge = make(PLUGINID_NATRON_USDMERGE);
    ASSERT_TRUE(toUSD && other && merge);
    wire(toUSD, merge, 0);
    wire(other, merge, 1);
    const PXR_NS::UsdStageRefPtr stage = writeUsd(merge, "chain.usdc");
    ASSERT_TRUE(static_cast<bool>(stage));

    EXPECT_TRUE(stage->GetPrimAtPath(PXR_NS::SdfPath("/Other/Tri")).IsValid());
    const PXR_NS::UsdGeomMesh mesh = onlyMesh(stage);
    ASSERT_TRUE(static_cast<bool>(mesh));
    const PXR_NS::VtVec3fArray points = meshPoints(mesh);
    ASSERT_EQ(std::size_t(bridged->pointCount()), points.size());
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(edited[i], points[editedPoint][i], kTolerance) << i;
    }
    EXPECT_EQ(std::size_t(bridged->primitiveCount()), countsOf(mesh).faces);
    EXPECT_TRUE(mesh.GetNormalsAttr().HasAuthoredValue());
    PXR_NS::VtVec3fArray normalValues;
    EXPECT_TRUE(mesh.GetNormalsAttr().Get(&normalValues, PXR_NS::UsdTimeCode(1.)));
    EXPECT_FALSE(normalValues.empty());

    const NodePtr back = after(reader(tempPath("chain.usdc")), PLUGINID_NATRON_USDTOGEO);
    ASSERT_TRUE(back != nullptr);
    setString(back, kUSDToGeoParamPrimPattern, mesh.GetPath().GetString());
    setBool(back, kUSDToGeoParamUnpack, true);
    const GeoDetailConstPtr readBack = pull(back);
    ASSERT_TRUE(readBack != nullptr);

    GeoDetailBuilder compact(*bridged);
    compact.defragment();
    const GeoDetail expected = compact.finish();
    ASSERT_EQ(expected.pointCount(), readBack->pointCount());
    ASSERT_EQ(expected.primitiveCount(), readBack->primitiveCount());
    for (GeoIndex p = 0; p < expected.pointCount(); ++p) {
        const std::array<double, 3> want = expected.pointPosition(expected.pointMap().offsetFromIndex(p));
        const std::array<double, 3> got = readBack->pointPosition(readBack->pointMap().offsetFromIndex(p));
        for (int i = 0; i < 3; ++i) {
            EXPECT_NEAR(want[i], got[i], kTolerance) << "point " << p << " component " << i;
        }
    }
}

// Packed prims read from a file, moved and bridged with the file as the scene input move the
// prims where they were, without authoring any others.
TEST_F(GeoPipelineTest, ModifyRoundTripMovesTheSourcePrimsInPlace)
{
    const NodePtr scene = reader(writeScene("meshes.usda", kMeshScene));
    const NodePtr packed = after(scene, PLUGINID_NATRON_USDTOGEO);
    ASSERT_TRUE(scene && packed);
    setString(packed, kUSDToGeoParamPrimPattern, "/World/A /World/B");
    const NodePtr transform = after(packed, PLUGINID_NATRON_GEOTRANSFORM);
    const NodePtr toUSD = transform ? after(transform, PLUGINID_NATRON_GEOTOUSD) : NodePtr();
    ASSERT_TRUE(toUSD != nullptr);
    setVector(transform, kGeoTransformParamTranslate, 0., 5., 0.);
    wire(scene, toUSD, kGeoToUSDInputScene);

    const PXR_NS::UsdStageRefPtr stage = writeUsd(toUSD, "modified.usdc");
    ASSERT_TRUE(static_cast<bool>(stage));

    std::set<std::string> paths;
    for (const PXR_NS::UsdPrim& prim : stage->Traverse()) {
        paths.insert(prim.GetPath().GetString());
    }
    EXPECT_EQ((std::set<std::string> { "/World", "/World/A", "/World/B", "/World/C" }), paths);

    const struct {
        const char* path;
        std::array<double, 3> translation;
    } expected[] = {
        { "/World/A", { 0., 5., 0. } },
        { "/World/B", { 10., 5., 0. } },
        { "/World/C", { 0., 0., 0. } },
    };
    for (const auto& e : expected) {
        const PXR_NS::UsdGeomXformable xformable(stage->GetPrimAtPath(PXR_NS::SdfPath(e.path)));
        ASSERT_TRUE(static_cast<bool>(xformable)) << e.path;
        const PXR_NS::GfVec3d t = xformable.ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode(1.)).ExtractTranslation();
        for (int i = 0; i < 3; ++i) {
            EXPECT_NEAR(e.translation[i], t[i], kTolerance) << e.path << " component " << i;
        }
    }
}

TEST_F(GeoPipelineTest, ScatteredPackedCopiesExportAsOnePointInstancer)
{
    const int count = 50;
    const NodePtr surface = grid(4, 4);
    const NodePtr card = make(PLUGINID_NATRON_GEOPRIMITIVE);
    const NodePtr scatter = after(surface, PLUGINID_NATRON_GEOSCATTER);
    const NodePtr copy = make(PLUGINID_NATRON_GEOCOPYTOPOINTS);
    ASSERT_TRUE(surface && card && scatter && copy);
    setInt(scatter, kGeoScatterParamCount, count);
    setBool(copy, kGeoCopyToPointsParamPackAndInstance, true);
    wire(card, copy, 0);
    wire(scatter, copy, 1);
    const NodePtr toUSD = after(copy, PLUGINID_NATRON_GEOTOUSD);
    ASSERT_TRUE(toUSD != nullptr);

    const PXR_NS::UsdStageRefPtr stage = writeUsd(toUSD, "scatter.usdc");
    ASSERT_TRUE(static_cast<bool>(stage));

    std::vector<PXR_NS::UsdGeomPointInstancer> instancers;
    for (const PXR_NS::UsdPrim& prim : stage->Traverse()) {
        if (prim.IsA<PXR_NS::UsdGeomPointInstancer>()) {
            instancers.push_back(PXR_NS::UsdGeomPointInstancer(prim));
        }
    }
    ASSERT_EQ(1u, instancers.size());
    PXR_NS::VtIntArray indices;
    ASSERT_TRUE(instancers.front().GetProtoIndicesAttr().Get(&indices, PXR_NS::UsdTimeCode(1.)));
    EXPECT_EQ(std::size_t(count), indices.size());
}

TEST_F(GeoPipelineTest, DeletingEdgesExportsTheSameMeshAsDeletingTheirPoints)
{
    const NodePtr source = grid(6, 6);
    ASSERT_TRUE(source != nullptr);
    const GeoDetailConstPtr input = pull(source);
    ASSERT_TRUE(input != nullptr);
    ASSERT_GT(input->primitiveCount(), GeoIndex(0));
    const GeoVertexRange first = input->topology().primitiveVertices(0);
    ASSERT_GE(first.count, GeoOffset(2));
    const GeoOffset a = input->topology().vertexPoint(first.start);
    const GeoOffset b = input->topology().vertexPoint(first.start + 1);

    const MeshCounts byEdge = deletedAndExported(source, "p" + std::to_string(a) + "-" + std::to_string(b), "edges", "edges.usdc");
    const MeshCounts byPoints = deletedAndExported(source, std::to_string(a) + " " + std::to_string(b), "points", "points.usdc");

    EXPECT_GT(byEdge.faces, 0u);
    EXPECT_LT(byEdge.faces, std::size_t(input->primitiveCount()));
    EXPECT_EQ(byPoints, byEdge);
}

// A disabled node on the Image input renders as its pass-through input, as it would feeding any
// other node; one with nothing to pass through leaves the input as if disconnected.
TEST_F(GeoPipelineTest, ADisabledImageSourcePassesItsInputThrough)
{
    const NodePtr constant = make(PLUGINID_NATRON_CONSTANT);
    ASSERT_TRUE(constant != nullptr);
    KnobColor* color = knob<KnobColor>(constant, kConstantParamColor);
    ASSERT_TRUE(color != NULL);
    color->setValues(0.5, 0.5, 0.5, 1., ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
    const NodePtr blur = after(constant, PLUGINID_NATRON_BLUR);
    ASSERT_TRUE(blur != nullptr);
    blur->setNodeDisabled(true);

    const NodePtr direct = displacedBy(constant);
    const NodePtr throughBlur = displacedBy(blur);
    ASSERT_TRUE(direct != nullptr);
    ASSERT_TRUE(throughBlur != nullptr);
    const GeoDetailConstPtr expected = pull(direct);
    const GeoDetailConstPtr got = pull(throughBlur);
    const GeoDetailConstPtr flat = pull(direct->getInput(0));
    ASSERT_TRUE(expected != nullptr);
    ASSERT_TRUE(got != nullptr);
    ASSERT_TRUE(flat != nullptr);
    ASSERT_EQ(expected->pointCount(), got->pointCount());
    ASSERT_EQ(flat->pointCount(), got->pointCount());
    double moved = 0.;
    for (GeoIndex i = 0; i < got->pointCount(); ++i) {
        const std::array<double, 3> e = expected->pointPosition(expected->pointMap().offsetFromIndex(i));
        const std::array<double, 3> g = got->pointPosition(got->pointMap().offsetFromIndex(i));
        const std::array<double, 3> f = flat->pointPosition(flat->pointMap().offsetFromIndex(i));
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR(e[c], g[c], kTolerance) << i << " " << c;
            moved = std::max(moved, std::abs(g[c] - f[c]));
        }
    }
    EXPECT_NEAR(0.5, moved, 1e-3);

    const NodePtr unfed = make(PLUGINID_NATRON_BLUR);
    ASSERT_TRUE(unfed != nullptr);
    unfed->setNodeDisabled(true);
    const NodePtr disconnected = displacedBy(unfed);
    ASSERT_TRUE(disconnected != nullptr);
    GeoDetailConstPtr detail;
    std::string error;
    EXPECT_NE(EffectInstance::eRenderRoIRetCodeOk, evaluate(disconnected, &detail, &error));
    EXPECT_NE(std::string::npos, error.find("connect the Image input")) << error;
}

// The spreadsheet lists GeoUVProject's uv after a transform, on the vertices it writes it to.
TEST_F(GeoPipelineTest, TheSpreadsheetListsTheUVsGeoUVProjectWrites)
{
    const NodePtr card = make(PLUGINID_NATRON_GEOPRIMITIVE);
    const NodePtr transform = card ? after(card, PLUGINID_NATRON_GEOTRANSFORM) : NodePtr();
    const NodePtr project = transform ? after(transform, PLUGINID_NATRON_GEOUVPROJECT) : NodePtr();
    ASSERT_TRUE(project != nullptr);
    setVector(transform, kGeoTransformParamTranslate, 0.5, 0.25, -0.75);

    const GeoDetailConstPtr detail = pull(project);
    ASSERT_TRUE(detail != nullptr);
    ASSERT_TRUE(detail->findAttribute(eGeoAttribClassVertex, "uv") != nullptr);
    const GeoDetailSpreadsheetSource source(detail);
    std::set<std::string> names;
    for (const GeoSpreadsheetColumn& column : source.columns(eGeoAttribClassVertex)) {
        names.insert(column.name);
    }
    EXPECT_EQ(1u, names.count("uv[0]"));
    EXPECT_EQ(1u, names.count("uv[1]"));
    EXPECT_GT(source.rowCount(eGeoAttribClassVertex), 0);
}

// With Group/Attrib Type on Points, the spreadsheet lists the uv on the points, and none on the vertices.
TEST_F(GeoPipelineTest, TheSpreadsheetListsGeoUVProjectPointUVsOnThePoints)
{
    const NodePtr card = make(PLUGINID_NATRON_GEOPRIMITIVE);
    const NodePtr project = card ? after(card, PLUGINID_NATRON_GEOUVPROJECT) : NodePtr();
    ASSERT_TRUE(project != nullptr);
    setChoice(project, kGeoUVProjectParamGroupAttribType, "points");

    const GeoDetailConstPtr detail = pull(project);
    ASSERT_TRUE(detail != nullptr);
    EXPECT_TRUE(detail->findAttribute(eGeoAttribClassVertex, "uv") == nullptr);
    ASSERT_TRUE(detail->findAttribute(eGeoAttribClassPoint, "uv") != nullptr);
    const GeoDetailSpreadsheetSource source(detail);
    std::set<std::string> pointNames;
    for (const GeoSpreadsheetColumn& column : source.columns(eGeoAttribClassPoint)) {
        pointNames.insert(column.name);
    }
    std::set<std::string> vertexNames;
    for (const GeoSpreadsheetColumn& column : source.columns(eGeoAttribClassVertex)) {
        vertexNames.insert(column.name);
    }
    EXPECT_EQ(1u, pointNames.count("uv[0]"));
    EXPECT_EQ(1u, pointNames.count("uv[1]"));
    EXPECT_EQ(0u, vertexNames.count("uv[0]"));
    EXPECT_EQ((std::int64_t)detail->pointCount(), source.rowCount(eGeoAttribClassPoint));
}

// GeoScatter makes points with no primitives; GeoUVProject on Points still gives each one a uv,
// which GeoDisplace samples its Image input at.
TEST_F(GeoPipelineTest, GeoUVProjectWritesPointUVsOnScatteredPointsForGeoDisplace)
{
    const NodePtr source = grid(2, 2);
    const NodePtr scatter = source ? after(source, PLUGINID_NATRON_GEOSCATTER) : NodePtr();
    const NodePtr project = scatter ? after(scatter, PLUGINID_NATRON_GEOUVPROJECT) : NodePtr();
    const NodePtr displace = project ? after(project, PLUGINID_NATRON_GEODISPLACE) : NodePtr();
    const NodePtr constant = make(PLUGINID_NATRON_CONSTANT);
    ASSERT_TRUE(displace != nullptr);
    ASSERT_TRUE(constant != nullptr);
    setInt(scatter, kGeoScatterParamCount, 50);
    setBool(scatter, kGeoScatterParamTransferN, true);
    setChoice(project, kGeoUVProjectParamGroupAttribType, "points");
    wire(constant, displace, 1);

    const GeoDetailConstPtr points = pull(project);
    ASSERT_TRUE(points != nullptr);
    EXPECT_EQ(50, points->pointCount());
    EXPECT_EQ(0, points->primitiveCount());
    EXPECT_TRUE(points->findAttribute(eGeoAttribClassPoint, "uv") != nullptr);

    GeoDetailConstPtr displaced;
    std::string error;
    EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk, evaluate(displace, &displaced, &error)) << error;
}

// Connecting the Camera input switches the projection to Camera and connecting the Image input
// switches GeoDisplace to Image; disconnecting leaves them as they are.
TEST_F(GeoPipelineTest, ConnectingTheModeInputSwitchesTheMode)
{
    const NodePtr scene = reader(writeScene("mesh.usda", kMeshScene));
    const NodePtr card = make(PLUGINID_NATRON_GEOPRIMITIVE);
    const NodePtr project = card ? after(card, PLUGINID_NATRON_GEOUVPROJECT) : NodePtr();
    const NodePtr displace = card ? after(card, PLUGINID_NATRON_GEODISPLACE) : NodePtr();
    const NodePtr constant = make(PLUGINID_NATRON_CONSTANT);
    ASSERT_TRUE(scene && project && displace && constant);
    KnobChoice* projection = knob<KnobChoice>(project, kGeoUVProjectParamProjection);
    KnobChoice* displaceBy = knob<KnobChoice>(displace, kGeoDisplaceParamDisplaceBy);
    ASSERT_TRUE(projection != NULL);
    ASSERT_TRUE(displaceBy != NULL);
    ASSERT_EQ((int)eGeoUVProjectionOrthographic, projection->getValue());
    ASSERT_EQ((int)eGeoDisplaceByNoise, displaceBy->getValue());

    wire(scene, project, 1);
    EXPECT_EQ((int)eGeoUVProjectionCamera, projection->getValue());
    wire(constant, displace, 1);
    EXPECT_EQ((int)eGeoDisplaceByImage, displaceBy->getValue());

    project->disconnectInput(1);
    displace->disconnectInput(1);
    EXPECT_EQ((int)eGeoUVProjectionCamera, projection->getValue());
    EXPECT_EQ((int)eGeoDisplaceByImage, displaceBy->getValue());

    projection->setValue((int)eGeoUVProjectionOrthographic);
    wire(scene, project, 1);
    EXPECT_EQ((int)eGeoUVProjectionCamera, projection->getValue());
}

// The switch is recorded into the step the connection is made in, so undoing it restores the mode.
TEST_F(GeoPipelineTest, TheModeSwitchOnConnectUndoes)
{
    const NodePtr card = make(PLUGINID_NATRON_GEOPRIMITIVE);
    const NodePtr displace = card ? after(card, PLUGINID_NATRON_GEODISPLACE) : NodePtr();
    const NodePtr constant = make(PLUGINID_NATRON_CONSTANT);
    ASSERT_TRUE(displace && constant);
    KnobChoice* displaceBy = knob<KnobChoice>(displace, kGeoDisplaceParamDisplaceBy);
    ASSERT_TRUE(displaceBy != NULL);

    std::shared_ptr<StepRecorder> recorder = std::make_shared<StepRecorder>();
    getApp()->setUndoRecorder(recorder);
    {
        UndoStepGuard step(getApp(), "connect");
        wire(constant, displace, 1);
    }
    EXPECT_EQ((int)eGeoDisplaceByImage, displaceBy->getValue());
    ASSERT_EQ(1u, recorder->commands.size());
    {
        UndoReplayGuard replaying(getApp());
        recorder->commands.back()->undo();
    }
    EXPECT_EQ((int)eGeoDisplaceByNoise, displaceBy->getValue());
    {
        UndoReplayGuard replaying(getApp());
        recorder->commands.back()->redo();
    }
    EXPECT_EQ((int)eGeoDisplaceByImage, displaceBy->getValue());

    displaceBy->setValue((int)eGeoDisplaceByNoise);
    displace->disconnectInput(1);
    {
        UndoReplayGuard replaying(getApp());
        wire(constant, displace, 1);
    }
    EXPECT_EQ((int)eGeoDisplaceByNoise, displaceBy->getValue());

    recorder->commands.clear();
    getApp()->setUndoRecorder(UndoRecorderPtr());
}

// A connection made while a node tree is created, as when a project loads or nodes are pasted,
// keeps the saved mode.
TEST_F(GeoPipelineTest, AConnectionMadeWhileCreatingANodeTreeKeepsTheMode)
{
    const NodePtr card = make(PLUGINID_NATRON_GEOPRIMITIVE);
    const NodePtr displace = card ? after(card, PLUGINID_NATRON_GEODISPLACE) : NodePtr();
    const NodePtr constant = make(PLUGINID_NATRON_CONSTANT);
    ASSERT_TRUE(displace && constant);
    KnobChoice* displaceBy = knob<KnobChoice>(displace, kGeoDisplaceParamDisplaceBy);
    ASSERT_TRUE(displaceBy != NULL);
    {
        CreatingNodeTreeFlag_RAII creating(getApp());
        wire(constant, displace, 1);
    }
    EXPECT_EQ((int)eGeoDisplaceByNoise, displaceBy->getValue());
}
