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

#include <pxr/usd/usdGeom/points.h>

#include "Global/Macros.h"

#include <cstdint>
#include <memory>
#include <set>
#include <span>
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
#include "Engine/Nodes/Geometry/GeoToUSD.h"
#include "Engine/Nodes/Geometry/GeoTransform.h"
#include "Engine/Nodes/Geometry/GeoUsdConvert.h"
#include "Engine/Nodes/Geometry/USDToGeo.h"
#include "Engine/Nodes/Scene/ScenePayloadImpl.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDPrune.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Project.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

// /World is translated, rotated and scaled non-uniformly, so a transform written without its
// parent's inverse lands elsewhere. /World/N is scaled
// non-uniformly too and carries normals and a vector primvar. Nothing is animated.
const char* const kScene = "#usda 1.0\n"
                           "(\n"
                           "    metersPerUnit = 0.01\n"
                           "    upAxis = \"Y\"\n"
                           ")\n"
                           "\n"
                           "def Xform \"World\"\n"
                           "{\n"
                           "    double3 xformOp:translate = (5, 0, 0)\n"
                           "    float xformOp:rotateZ = 90\n"
                           "    double3 xformOp:scale = (2, 3, 2)\n"
                           "    uniform token[] xformOpOrder = [\"xformOp:translate\", \"xformOp:rotateZ\", \"xformOp:scale\"]\n"
                           "\n"
                           "    def Mesh \"A\"\n"
                           "    {\n"
                           "        int[] faceVertexCounts = [4]\n"
                           "        int[] faceVertexIndices = [0, 1, 2, 3]\n"
                           "        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]\n"
                           "        double3 xformOp:translate = (1, 0, 0)\n"
                           "        float xformOp:rotateX = 30\n"
                           "        uniform token[] xformOpOrder = [\"xformOp:translate\", \"xformOp:rotateX\"]\n"
                           "    }\n"
                           "\n"
                           "    def Cube \"C\"\n"
                           "    {\n"
                           "        double3 xformOp:translate = (0, 2, 0)\n"
                           "        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                           "    }\n"
                           "\n"
                           "    def Mesh \"N\"\n"
                           "    {\n"
                           "        int[] faceVertexCounts = [4]\n"
                           "        int[] faceVertexIndices = [0, 1, 2, 3]\n"
                           "        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 1), (0, 1, 1)]\n"
                           "        normal3f[] normals = [(0, -0.70710677, 0.70710677), (0, -0.70710677, 0.70710677), "
                           "(0, -0.70710677, 0.70710677), (0, -0.70710677, 0.70710677)] (\n"
                           "            interpolation = \"vertex\"\n"
                           "        )\n"
                           "        vector3f[] primvars:v = [(1, 0, 0), (0, 1, 0), (0, 0, 1), (1, 1, 1)] (\n"
                           "            interpolation = \"vertex\"\n"
                           "        )\n"
                           "        float xformOp:rotateY = 20\n"
                           "        double3 xformOp:scale = (1, 3, 0.5)\n"
                           "        uniform token[] xformOpOrder = [\"xformOp:rotateY\", \"xformOp:scale\"]\n"
                           "    }\n"
                           "\n"
                           "    def Points \"P\"\n"
                           "    {\n"
                           "        point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n"
                           "        double3 xformOp:translate = (0, 0, 3)\n"
                           "        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                           "    }\n"
                           "}\n";

const double kShift = 5.;

std::set<std::string>
primPaths(const PXR_NS::UsdStageRefPtr& stage)
{
    std::set<std::string> paths;
    for (const PXR_NS::UsdPrim& prim : stage->Traverse()) {
        paths.insert(prim.GetPath().GetString());
    }

    return paths;
}

PXR_NS::GfMatrix4d
worldOf(const PXR_NS::UsdStageRefPtr& stage,
        const char* path)
{
    PXR_NS::UsdGeomXformCache cache(PXR_NS::UsdTimeCode::Default());

    return cache.GetLocalToWorldTransform(stage->GetPrimAtPath(PXR_NS::SdfPath(path)));
}

// The prim's points in world space.
std::vector<PXR_NS::GfVec3d>
worldPoints(const PXR_NS::UsdStageRefPtr& stage,
            const char* path)
{
    std::vector<PXR_NS::GfVec3d> out;
    PXR_NS::VtVec3fArray points;
    EXPECT_TRUE(PXR_NS::UsdGeomPointBased(stage->GetPrimAtPath(PXR_NS::SdfPath(path))).GetPointsAttr().Get(&points)) << path;
    const PXR_NS::GfMatrix4d world = worldOf(stage, path);
    for (const PXR_NS::GfVec3f& point : points) {
        out.push_back(world.Transform(PXR_NS::GfVec3d(point)));
    }

    return out;
}

void
expectClose(const PXR_NS::GfMatrix4d& expected,
            const PXR_NS::GfMatrix4d& actual)
{
    EXPECT_TRUE(PXR_NS::GfIsClose(expected, actual, 1e-6)) << "expected " << expected << "\nactual   " << actual;
}

PXR_NS::UsdStageRefPtr
openScene()
{
    PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::CreateAnonymous(".usda");
    EXPECT_TRUE(layer->ImportFromString(kScene));

    return PXR_NS::UsdStage::Open(layer);
}

// One quad whose `path` is @p path, marked as read from USD when @p fromUsd.
GeoDetailConstPtr
quadAt(const char* path,
       bool fromUsd)
{
    GeoDetailBuilder b;
    const GeoOffset first = b.appendPoints(4);
    b.setPointPosition(first, 0., 0., 0.);
    b.setPointPosition(first + 1, 1., 0., 0.);
    b.setPointPosition(first + 2, 1., 1., 0.);
    b.setPointPosition(first + 3, 0., 1., 0.);
    const GeoOffset corners[4] = { first, first + 1, first + 2, first + 3 };
    b.appendPrimitive(eGeoPrimitivePolygon, corners);
    GeoAttributeDesc pathDesc;
    pathDesc.name = "path";
    pathDesc.storage = eGeoStorageString;
    b.addAttribute(eGeoAttribClassPrimitive, pathDesc).setString(0, path);
    if (fromUsd) {
        GeoAttributeDesc sourceDesc;
        sourceDesc.name = kGeoUsdSourceAttr;
        sourceDesc.storage = eGeoStorageInt32;
        b.addAttribute(eGeoAttribClassPrimitive, sourceDesc).set<std::int32_t>(0, 1);
    }

    return std::make_shared<const GeoDetail>(b.finish());
}

// Converts @p detail under the default root against openScene() as the input.
std::unique_ptr<const GeoUsdConvert>
convertOverScene(const GeoDetailConstPtr& detail)
{
    const PXR_NS::UsdStageRefPtr stage = openScene();
    GeoUsdConvertOptions options;
    const GeoUsdDetailPaths paths = geoUsdDetailPaths(*detail, options);
    options.inputScene = geoUsdCaptureInputScene(stage, PXR_NS::UsdTimeCode::Default(), paths.paths, paths.sourcePaths);

    return std::make_unique<const GeoUsdConvert>(detail, options);
}

bool
anyWarningHas(const GeoUsdConvert& convert,
              const std::string& text)
{
    for (const std::string& warning : convert.warnings()) {
        if (warning.find(text) != std::string::npos) {
            return true;
        }
    }

    return false;
}

PXR_NS::SdfSpecifier
authoredSpecifier(const GeoUsdConvert& convert,
                  const char* path)
{
    PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::CreateAnonymous(".usda");
    std::string error;
    EXPECT_TRUE(convert.author(layer, PXR_NS::UsdTimeCode::Default(), &error)) << error;
    const PXR_NS::SdfPrimSpecHandle spec = layer->GetPrimAtPath(PXR_NS::SdfPath(path));
    EXPECT_TRUE(spec) << path;

    return spec ? spec->GetSpecifier() : PXR_NS::SdfNumSpecifiers;
}

// @p directions carried as the row-vector point transform @p matrix carries normals, unit length,
// or vectors.
std::vector<PXR_NS::GfVec3d>
carried(const std::vector<PXR_NS::GfVec3d>& directions,
        const PXR_NS::GfMatrix4d& matrix,
        bool normals)
{
    const PXR_NS::GfMatrix4d m = normals ? matrix.GetInverse().GetTranspose() : matrix;
    std::vector<PXR_NS::GfVec3d> out;
    for (const PXR_NS::GfVec3d& d : directions) {
        const PXR_NS::GfVec3d v = m.TransformDir(d);
        out.push_back(normals ? v.GetNormalized() : v);
    }

    return out;
}

template <typename Array>
std::vector<PXR_NS::GfVec3d>
asVec3d(const Array& values)
{
    return std::vector<PXR_NS::GfVec3d>(values.begin(), values.end());
}

std::vector<PXR_NS::GfVec3d>
pointAttribute(const GeoDetail& detail,
               const char* name)
{
    std::vector<PXR_NS::GfVec3d> out;
    const GeoAttribute* a = detail.findAttribute(eGeoAttribClassPoint, name);
    EXPECT_TRUE(a != nullptr) << name;
    if (!a) {
        return out;
    }
    for (std::size_t p = 0; p < detail.pointCount(); ++p) {
        float v[3];
        a->getTuple<float>(p, std::span<float>(v, 3));
        out.emplace_back(v[0], v[1], v[2]);
    }

    return out;
}

void
expectClose(const std::vector<PXR_NS::GfVec3d>& expected,
            const std::vector<PXR_NS::GfVec3d>& actual)
{
    ASSERT_EQ(expected.size(), actual.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_TRUE(PXR_NS::GfIsClose(expected[i], actual[i], 1e-5)) << i << ": expected " << expected[i] << ", actual " << actual[i];
    }
}

} // anonymous namespace

class GeoToSceneOverridesTest
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
        for (NodePtr& node : _nodes) {
            if (node) {
                node->destroyNode(false, false);
            }
        }
        _nodes.clear();
        SceneQueryPool::instance().releaseIdle();

        BaseTest::TearDown();
    }

    NodePtr make(const char* pluginID)
    {
        NodePtr node = createNode(QString::fromUtf8(pluginID));

        EXPECT_TRUE(bool(node));
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

    // USDRead -> USDToGeo(pattern) -> GeoTransform(+kShift in Z) -> GeoToUSD, whose Scene input
    // is the reader, or sceneInput when given.
    void build(const char* pattern,
               bool unpack,
               const NodePtr& sceneInput = NodePtr())
    {
        const std::string path = _dir.path().toStdString() + "/overrides.usda";
        {
            QFile f(QString::fromStdString(path));
            ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(kScene);
        }
        if (!_reader) {
            _reader = make(PLUGINID_NATRON_USDREAD);
        }
        _toGeo = make(PLUGINID_NATRON_USDTOGEO);
        _transform = make(PLUGINID_NATRON_GEOTRANSFORM);
        _toUSD = make(PLUGINID_NATRON_GEOTOUSD);
        ASSERT_TRUE(_reader && _toGeo && _transform && _toUSD);

        KnobFile* file = knob<KnobFile>(_reader, kUSDReadParamFile);
        ASSERT_TRUE(file != NULL);
        file->setValue(path);
        KnobString* primPattern = knob<KnobString>(_toGeo, kUSDToGeoParamPrimPattern);
        ASSERT_TRUE(primPattern != NULL);
        primPattern->setValue(std::string(pattern));
        KnobBool* unpackKnob = knob<KnobBool>(_toGeo, kUSDToGeoParamUnpack);
        ASSERT_TRUE(unpackKnob != NULL);
        unpackKnob->setValue(unpack);
        KnobDouble* translate = knob<KnobDouble>(_transform, kGeoTransformParamTranslate);
        ASSERT_TRUE(translate != NULL);
        translate->setValue(kShift, ViewSpec::all(), 2);

        connectNodes(_reader, _toGeo, 0, true);
        connectNodes(_toGeo, _transform, 0, true);
        connectNodes(_transform, _toUSD, kGeoToUSDInputGeometry, true);
        connectNodes(sceneInput ? sceneInput : _reader, _toUSD, kGeoToUSDInputScene, true);
    }

    static ScenePayload evaluate(const NodePtr& node,
                                 double time = 1.)
    {
        ScenePayload payload;
        std::string error;

        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk,
                  node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(time, ViewIdx(0)), &payload, &error))
            << error;

        return payload;
    }

    static PXR_NS::SdfLayerRefPtr geometryLayer(const ScenePayload& payload)
    {
        const std::shared_ptr<const ScenePayloadImpl>& imp = payload.getImpl();
        if (!imp || !imp->top.dependencies || imp->top.dependencies->layers.empty()) {
            return PXR_NS::SdfLayerRefPtr();
        }

        return imp->top.dependencies->layers.front();
    }

    void setFlatten(bool flatten)
    {
        KnobBool* k = knob<KnobBool>(_toGeo, kUSDToGeoParamFlattenTransforms);
        ASSERT_TRUE(k != NULL);
        k->setValue(flatten);
    }

    static PXR_NS::UsdStageRefPtr stageOf(const ScenePayload& payload,
                                          SceneQueryPool::Lease* lease)
    {
        std::string error;
        *lease = SceneQueryPool::instance().borrow(payload, &error);
        EXPECT_TRUE(lease->isValid()) << error;

        return lease->getStage();
    }

    NodePtr _reader;
    NodePtr _toGeo;
    NodePtr _transform;
    NodePtr _toUSD;

private:
    QTemporaryDir _dir;
    std::vector<NodePtr> _nodes;
};

TEST_F(GeoToSceneOverridesTest, PackedPrimsMoveAtTheirPathsUnderATransformedParent)
{
    build("/World/*", false);
    const ScenePayload source = evaluate(_reader);
    const ScenePayload result = evaluate(_toUSD);
    SceneQueryPool::Lease sourceLease;
    SceneQueryPool::Lease resultLease;
    const PXR_NS::UsdStageRefPtr before = stageOf(source, &sourceLease);
    const PXR_NS::UsdStageRefPtr after = stageOf(result, &resultLease);
    ASSERT_TRUE(before && after);

    EXPECT_EQ(primPaths(before), primPaths(after));
    PXR_NS::GfMatrix4d shift(1.);
    shift.SetTranslate(PXR_NS::GfVec3d(0., 0., kShift));
    for (const char* path : { "/World/A", "/World/C" }) {
        SCOPED_TRACE(path);
        expectClose(worldOf(before, path) * shift, worldOf(after, path));
    }
    expectClose(worldOf(before, "/World"), worldOf(after, "/World"));

    QString message;
    int type = 0;
    _toUSD->getPersistentMessage(&message, &type, false);
    EXPECT_NE((int)eMessageTypeWarning, type) << message.toStdString();
}

TEST_F(GeoToSceneOverridesTest, AnUnmovedPackedRoundTripKeepsEveryWorldTransform)
{
    build("/World/*", false);
    KnobDouble* translate = knob<KnobDouble>(_transform, kGeoTransformParamTranslate);
    ASSERT_TRUE(translate != NULL);
    translate->setValue(0., ViewSpec::all(), 2);

    const ScenePayload source = evaluate(_reader);
    const ScenePayload result = evaluate(_toUSD);
    SceneQueryPool::Lease sourceLease;
    SceneQueryPool::Lease resultLease;
    const PXR_NS::UsdStageRefPtr before = stageOf(source, &sourceLease);
    const PXR_NS::UsdStageRefPtr after = stageOf(result, &resultLease);
    ASSERT_TRUE(before && after);
    for (const char* path : { "/World", "/World/A", "/World/C" }) {
        SCOPED_TRACE(path);
        expectClose(worldOf(before, path), worldOf(after, path));
    }
}

TEST_F(GeoToSceneOverridesTest, AnUnpackedPrimIsOverriddenOnItsPointsOnly)
{
    build("/World/A", true);
    const ScenePayload source = evaluate(_reader);
    const ScenePayload result = evaluate(_toUSD);
    SceneQueryPool::Lease sourceLease;
    SceneQueryPool::Lease resultLease;
    const PXR_NS::UsdStageRefPtr before = stageOf(source, &sourceLease);
    const PXR_NS::UsdStageRefPtr after = stageOf(result, &resultLease);
    ASSERT_TRUE(before && after);
    EXPECT_EQ(primPaths(before), primPaths(after));

    const std::shared_ptr<const ScenePayloadImpl>& imp = result.getImpl();
    ASSERT_TRUE(imp && imp->top.dependencies && !imp->top.dependencies->layers.empty());
    const PXR_NS::SdfLayerRefPtr geo = imp->top.dependencies->layers.front();
    const PXR_NS::SdfPrimSpecHandle spec = geo->GetPrimAtPath(PXR_NS::SdfPath("/World/A"));
    ASSERT_TRUE(spec);
    EXPECT_EQ(PXR_NS::SdfSpecifierOver, spec->GetSpecifier());
    EXPECT_TRUE(spec->GetAttributes().get(PXR_NS::UsdGeomTokens->points));
    for (const PXR_NS::TfToken& name : { PXR_NS::UsdGeomTokens->faceVertexCounts, PXR_NS::UsdGeomTokens->faceVertexIndices,
                                         PXR_NS::TfToken(std::string("primvars:") + kGeoUsdSourceAttr) }) {
        EXPECT_FALSE(spec->GetAttributes().get(name)) << name.GetString();
    }

    PXR_NS::VtVec3fArray oldPoints;
    PXR_NS::VtVec3fArray newPoints;
    ASSERT_TRUE(PXR_NS::UsdGeomMesh(before->GetPrimAtPath(PXR_NS::SdfPath("/World/A"))).GetPointsAttr().Get(&oldPoints));
    ASSERT_TRUE(PXR_NS::UsdGeomMesh(after->GetPrimAtPath(PXR_NS::SdfPath("/World/A"))).GetPointsAttr().Get(&newPoints));
    ASSERT_EQ(oldPoints.size(), newPoints.size());
    for (std::size_t i = 0; i < oldPoints.size(); ++i) {
        EXPECT_TRUE(PXR_NS::GfIsClose(oldPoints[i] + PXR_NS::GfVec3f(0.f, 0.f, float(kShift)), newPoints[i], 1e-5)) << i;
    }
    expectClose(worldOf(before, "/World/A"), worldOf(after, "/World/A"));
}

TEST_F(GeoToSceneOverridesTest, APrunedSourcePathRaisesAWarning)
{
    _reader = make(PLUGINID_NATRON_USDREAD);
    const NodePtr prune = make(PLUGINID_NATRON_USDPRUNE);
    ASSERT_TRUE(_reader && prune);
    KnobString* pattern = knob<KnobString>(prune, kUSDPruneParamPrimPattern);
    ASSERT_TRUE(pattern != NULL);
    pattern->setValue(std::string("/World/C"));
    connectNodes(_reader, prune, 0, true);
    build("/World/*", false, prune);

    const ScenePayload result = evaluate(_toUSD);
    EXPECT_FALSE(result.isEmpty());
    QString message;
    int type = 0;
    _toUSD->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("/World/C"))) << message.toStdString();
    EXPECT_FALSE(message.contains(QString::fromUtf8("/World/A"))) << message.toStdString();
}

TEST_F(GeoToSceneOverridesTest, AFlattenedUnpackRoundTripMovesThePointsOnlyByTheEdit)
{
    build("/World/A", true);
    setFlatten(true);
    const ScenePayload source = evaluate(_reader);
    const ScenePayload result = evaluate(_toUSD);
    SceneQueryPool::Lease sourceLease;
    SceneQueryPool::Lease resultLease;
    const PXR_NS::UsdStageRefPtr before = stageOf(source, &sourceLease);
    const PXR_NS::UsdStageRefPtr after = stageOf(result, &resultLease);
    ASSERT_TRUE(before && after);
    EXPECT_EQ(primPaths(before), primPaths(after));
    expectClose(worldOf(before, "/World/A"), worldOf(after, "/World/A"));

    const std::vector<PXR_NS::GfVec3d> oldPoints = worldPoints(before, "/World/A");
    const std::vector<PXR_NS::GfVec3d> newPoints = worldPoints(after, "/World/A");
    ASSERT_EQ(oldPoints.size(), newPoints.size());
    for (std::size_t i = 0; i < oldPoints.size(); ++i) {
        EXPECT_TRUE(PXR_NS::GfIsClose(oldPoints[i] + PXR_NS::GfVec3d(0., 0., kShift), newPoints[i], 1e-4))
            << i << ": " << oldPoints[i] << " -> " << newPoints[i];
    }
}

TEST_F(GeoToSceneOverridesTest, AnUnpackedPointsPrimIsOverriddenAtItsPath)
{
    build("/World/P", true);
    GeoDetailConstPtr detail;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              _toGeo->getEffectInstance()->evaluateGeometry(EffectInstance::EvaluateGeometryArgs(1., ViewIdx(0)), &detail, &error))
        << error;
    ASSERT_TRUE(detail != nullptr);
    const GeoAttribute* path = detail->findAttribute(eGeoAttribClassPoint, "path");
    ASSERT_TRUE(path != nullptr);
    ASSERT_EQ(3, int(detail->pointCount()));
    const GeoAttribute* fromUsd = detail->findAttribute(eGeoAttribClassPoint, kGeoUsdSourceAttr);
    ASSERT_TRUE(fromUsd != nullptr);
    for (std::size_t p = 0; p < 3; ++p) {
        EXPECT_EQ(std::string("/World/P"), path->getString(p)) << p;
        EXPECT_EQ(1, fromUsd->get<std::int32_t>(p)) << p;
    }

    const ScenePayload source = evaluate(_reader);
    const ScenePayload result = evaluate(_toUSD);
    SceneQueryPool::Lease sourceLease;
    SceneQueryPool::Lease resultLease;
    const PXR_NS::UsdStageRefPtr before = stageOf(source, &sourceLease);
    const PXR_NS::UsdStageRefPtr after = stageOf(result, &resultLease);
    ASSERT_TRUE(before && after);
    EXPECT_EQ(primPaths(before), primPaths(after));
    const PXR_NS::SdfLayerRefPtr geo = geometryLayer(result);
    ASSERT_TRUE(geo);
    const PXR_NS::SdfPrimSpecHandle spec = geo->GetPrimAtPath(PXR_NS::SdfPath("/World/P"));
    ASSERT_TRUE(spec);
    EXPECT_EQ(PXR_NS::SdfSpecifierOver, spec->GetSpecifier());

    PXR_NS::VtVec3fArray oldPoints;
    PXR_NS::VtVec3fArray newPoints;
    ASSERT_TRUE(PXR_NS::UsdGeomPoints(before->GetPrimAtPath(PXR_NS::SdfPath("/World/P"))).GetPointsAttr().Get(&oldPoints));
    ASSERT_TRUE(PXR_NS::UsdGeomPoints(after->GetPrimAtPath(PXR_NS::SdfPath("/World/P"))).GetPointsAttr().Get(&newPoints));
    ASSERT_EQ(oldPoints.size(), newPoints.size());
    for (std::size_t i = 0; i < oldPoints.size(); ++i) {
        EXPECT_TRUE(PXR_NS::GfIsClose(oldPoints[i] + PXR_NS::GfVec3f(0.f, 0.f, float(kShift)), newPoints[i], 1e-5)) << i;
    }
}

TEST_F(GeoToSceneOverridesTest, AnUnpackedPrimMissingFromTheSceneGoesUnderTheRootWithAWarning)
{
    _reader = make(PLUGINID_NATRON_USDREAD);
    const NodePtr prune = make(PLUGINID_NATRON_USDPRUNE);
    ASSERT_TRUE(_reader && prune);
    KnobString* pattern = knob<KnobString>(prune, kUSDPruneParamPrimPattern);
    ASSERT_TRUE(pattern != NULL);
    pattern->setValue(std::string("/World/A"));
    connectNodes(_reader, prune, 0, true);
    build("/World/A", true, prune);

    const ScenePayload result = evaluate(_toUSD);
    SceneQueryPool::Lease lease;
    const PXR_NS::UsdStageRefPtr stage = stageOf(result, &lease);
    ASSERT_TRUE(stage);
    // Deactivating keeps the pruned prim on the stage, inactive.
    const PXR_NS::UsdPrim pruned = stage->GetPrimAtPath(PXR_NS::SdfPath("/World/A"));
    EXPECT_FALSE(pruned && pruned.IsActive());
    const PXR_NS::SdfLayerRefPtr geometry = geometryLayer(result);
    ASSERT_TRUE(geometry);
    EXPECT_FALSE(geometry->GetPrimAtPath(PXR_NS::SdfPath("/World/A")));
    EXPECT_TRUE(static_cast<bool>(PXR_NS::UsdGeomMesh(stage->GetPrimAtPath(PXR_NS::SdfPath("/geo/World/A")))));

    QString message;
    int type = 0;
    _toUSD->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("/World/A"))) << message.toStdString();
}

TEST_F(GeoToSceneOverridesTest, GeometryReadFromAStaticSceneSharesOneLayerAcrossFrames)
{
    build("/World/A", true);
    const PXR_NS::SdfLayerRefPtr first = geometryLayer(evaluate(_toUSD, 1.));
    const PXR_NS::SdfLayerRefPtr second = geometryLayer(evaluate(_toUSD, 2.));
    const PXR_NS::SdfLayerRefPtr third = geometryLayer(evaluate(_toUSD, 3.));
    ASSERT_TRUE(first && second && third);
    EXPECT_EQ(second->GetIdentifier(), third->GetIdentifier());
    EXPECT_FALSE(_toGeo->getEffectInstance()->isFrameVaryingOrAnimated_Recursive());
}

TEST_F(GeoToSceneOverridesTest, GeometryReadFromAPrimTheSceneHoldsOverridesIt)
{
    const std::unique_ptr<const GeoUsdConvert> convert = convertOverScene(quadAt("/World/A", true));
    ASSERT_EQ(std::size_t(1), convert->partitionCount());
    EXPECT_EQ(PXR_NS::SdfPath("/World/A"), convert->partitionPath(0));
    EXPECT_TRUE(convert->warnings().empty());
    EXPECT_EQ(PXR_NS::SdfSpecifierOver, authoredSpecifier(*convert, "/World/A"));
}

TEST_F(GeoToSceneOverridesTest, GeometryReadFromAVanishedPrimGoesUnderTheRootWithAWarning)
{
    const std::unique_ptr<const GeoUsdConvert> convert = convertOverScene(quadAt("/World/Gone", true));
    ASSERT_EQ(std::size_t(1), convert->partitionCount());
    EXPECT_EQ(PXR_NS::SdfPath("/geo/World/Gone"), convert->partitionPath(0));
    EXPECT_TRUE(anyWarningHas(*convert, "/World/Gone is not in the input scene"));
    EXPECT_EQ(PXR_NS::SdfSpecifierDef, authoredSpecifier(*convert, "/geo/World/Gone"));
}

TEST_F(GeoToSceneOverridesTest, UnreadGeometryAtANewAbsolutePathIsDefinedThereWithoutAWarning)
{
    const std::unique_ptr<const GeoUsdConvert> convert = convertOverScene(quadAt("/World/New", false));
    ASSERT_EQ(std::size_t(1), convert->partitionCount());
    EXPECT_EQ(PXR_NS::SdfPath("/World/New"), convert->partitionPath(0));
    EXPECT_TRUE(convert->warnings().empty());
    EXPECT_EQ(PXR_NS::SdfSpecifierDef, authoredSpecifier(*convert, "/World/New"));
}

TEST_F(GeoToSceneOverridesTest, UnreadGeometryOnAPrimTheSceneHoldsGoesUnderTheRootWithACollisionWarning)
{
    const std::unique_ptr<const GeoUsdConvert> convert = convertOverScene(quadAt("/World/A", false));
    ASSERT_EQ(std::size_t(1), convert->partitionCount());
    EXPECT_EQ(PXR_NS::SdfPath("/geo/World/A"), convert->partitionPath(0));
    EXPECT_TRUE(anyWarningHas(*convert, "/World/A is already in the input scene"));
    EXPECT_EQ(PXR_NS::SdfSpecifierDef, authoredSpecifier(*convert, "/geo/World/A"));
}

TEST_F(GeoToSceneOverridesTest, AFlattenedUnpackRoundTripKeepsNormalsAndVectors)
{
    build("/World/N", true);
    setFlatten(true);
    const ScenePayload source = evaluate(_reader);
    SceneQueryPool::Lease sourceLease;
    const PXR_NS::UsdStageRefPtr before = stageOf(source, &sourceLease);
    ASSERT_TRUE(before);
    const PXR_NS::UsdPrim beforePrim = before->GetPrimAtPath(PXR_NS::SdfPath("/World/N"));
    PXR_NS::VtVec3fArray normals;
    ASSERT_TRUE(PXR_NS::UsdGeomMesh(beforePrim).GetNormalsAttr().Get(&normals));
    PXR_NS::VtVec3fArray vectors;
    ASSERT_TRUE(PXR_NS::UsdGeomPrimvarsAPI(beforePrim).GetPrimvar(PXR_NS::TfToken("v")).Get(&vectors));
    const PXR_NS::GfMatrix4d world = worldOf(before, "/World/N");

    GeoDetailConstPtr detail;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              _toGeo->getEffectInstance()->evaluateGeometry(EffectInstance::EvaluateGeometryArgs(1., ViewIdx(0)), &detail, &error))
        << error;
    ASSERT_TRUE(detail != nullptr);
    {
        SCOPED_TRACE("world-space normals");
        expectClose(carried(asVec3d(normals), world, true), pointAttribute(*detail, "N"));
    }
    {
        SCOPED_TRACE("world-space vectors");
        expectClose(carried(asVec3d(vectors), world, false), pointAttribute(*detail, "v"));
    }

    const ScenePayload result = evaluate(_toUSD);
    SceneQueryPool::Lease resultLease;
    const PXR_NS::UsdStageRefPtr after = stageOf(result, &resultLease);
    ASSERT_TRUE(after);
    const PXR_NS::UsdPrim afterPrim = after->GetPrimAtPath(PXR_NS::SdfPath("/World/N"));
    PXR_NS::VtVec3fArray newNormals;
    ASSERT_TRUE(PXR_NS::UsdGeomMesh(afterPrim).GetNormalsAttr().Get(&newNormals));
    PXR_NS::VtVec3fArray newVectors;
    ASSERT_TRUE(PXR_NS::UsdGeomPrimvarsAPI(afterPrim).GetPrimvar(PXR_NS::TfToken("v")).Get(&newVectors));
    {
        SCOPED_TRACE("round-trip normals");
        expectClose(asVec3d(normals), asVec3d(newNormals));
    }
    {
        SCOPED_TRACE("round-trip vectors");
        expectClose(asVec3d(vectors), asVec3d(newVectors));
    }
}
