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
#include <pxr/usd/usdGeom/primvarsAPI.h>
#include <pxr/usd/usdShade/materialBindingAPI.h>

#include "Global/Macros.h"

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
#include "Engine/GeoDetail.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Geometry/GeoToUSD.h"
#include "Engine/Nodes/Geometry/GeoUsdConvert.h"
#include "Engine/Nodes/Geometry/USDToGeo.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Project.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

// A mesh two levels down a translated, rotated and scaled hierarchy, translated itself, with a
// colour, texture coordinates, a vertex primvar and a material binding, its second face bound to
// another material through a GeomSubset. Nothing is animated.
const char* const kScene = "#usda 1.0\n"
                           "(\n"
                           "    metersPerUnit = 0.01\n"
                           "    upAxis = \"Y\"\n"
                           ")\n"
                           "\n"
                           "def Scope \"mtl\"\n"
                           "{\n"
                           "    def Material \"a\"\n"
                           "    {\n"
                           "    }\n"
                           "\n"
                           "    def Material \"b\"\n"
                           "    {\n"
                           "    }\n"
                           "}\n"
                           "\n"
                           "def Xform \"World\"\n"
                           "{\n"
                           "    double3 xformOp:translate = (1, 2, 3)\n"
                           "    float3 xformOp:rotateXYZ = (0, 30, 0)\n"
                           "    float3 xformOp:scale = (2, 1, 1)\n"
                           "    uniform token[] xformOpOrder = [\"xformOp:translate\", \"xformOp:rotateXYZ\", \"xformOp:scale\"]\n"
                           "\n"
                           "    def Xform \"Group\"\n"
                           "    {\n"
                           "        double3 xformOp:translate = (0, 0, -4)\n"
                           "        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                           "\n"
                           "        def Mesh \"Body\" (\n"
                           "            prepend apiSchemas = [\"MaterialBindingAPI\"]\n"
                           "        )\n"
                           "        {\n"
                           "            int[] faceVertexCounts = [4, 4]\n"
                           "            int[] faceVertexIndices = [0, 1, 4, 3, 1, 2, 5, 4]\n"
                           "            point3f[] points = [(0, 0, 0), (1, 0, 0), (2, 0, 0), (0, 1, 0), (1, 1, 0), (2, 1, 0)]\n"
                           "            color3f[] primvars:displayColor = [(1, 0.5, 0.25)] (\n"
                           "                interpolation = \"constant\"\n"
                           "            )\n"
                           "            float[] primvars:weight = [1, 2, 3, 4, 5, 6] (\n"
                           "                interpolation = \"vertex\"\n"
                           "            )\n"
                           "            texCoord2f[] primvars:st = [(0, 0), (0.5, 0), (1, 0), (0, 1), (0.5, 1), (1, 1)] (\n"
                           "                interpolation = \"vertex\"\n"
                           "            )\n"
                           "            rel material:binding = </mtl/a>\n"
                           "            double3 xformOp:translate = (3, 0, 0)\n"
                           "            uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                           "\n"
                           "            def GeomSubset \"second\" (\n"
                           "                prepend apiSchemas = [\"MaterialBindingAPI\"]\n"
                           "            )\n"
                           "            {\n"
                           "                uniform token elementType = \"face\"\n"
                           "                uniform token familyName = \"materialBind\"\n"
                           "                int[] indices = [1]\n"
                           "                rel material:binding = </mtl/b>\n"
                           "            }\n"
                           "        }\n"
                           "    }\n"
                           "}\n";

// A mesh, a points prim and an inactive mesh, the three read by their names alone.
const char* const kNamedScene = "#usda 1.0\n"
                                "(\n"
                                "    metersPerUnit = 0.01\n"
                                "    upAxis = \"Y\"\n"
                                ")\n"
                                "\n"
                                "def Xform \"World\"\n"
                                "{\n"
                                "    def Mesh \"Body\"\n"
                                "    {\n"
                                "        int[] faceVertexCounts = [4]\n"
                                "        int[] faceVertexIndices = [0, 1, 2, 3]\n"
                                "        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]\n"
                                "    }\n"
                                "\n"
                                "    def Points \"Cloud\"\n"
                                "    {\n"
                                "        point3f[] points = [(0, 0, 0), (1, 0, 0), (2, 0, 0)]\n"
                                "    }\n"
                                "\n"
                                "    def Mesh \"Off\" (\n"
                                "        active = false\n"
                                "    )\n"
                                "    {\n"
                                "        int[] faceVertexCounts = [4]\n"
                                "        int[] faceVertexIndices = [0, 1, 2, 3]\n"
                                "        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]\n"
                                "    }\n"
                                "}\n";

const char* const kBody = "/World/Group/Body";
const char* const kSubset = "/World/Group/Body/second";

PXR_NS::GfMatrix4d
worldOf(const PXR_NS::UsdStageRefPtr& stage,
        const char* path)
{
    PXR_NS::UsdGeomXformCache cache(PXR_NS::UsdTimeCode::Default());

    return cache.GetLocalToWorldTransform(stage->GetPrimAtPath(PXR_NS::SdfPath(path)));
}

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

std::string
boundMaterial(const PXR_NS::UsdStageRefPtr& stage,
              const char* path)
{
    const PXR_NS::UsdShadeMaterial material = PXR_NS::UsdShadeMaterialBindingAPI(stage->GetPrimAtPath(PXR_NS::SdfPath(path))).ComputeBoundMaterial();

    return material ? material.GetPath().GetString() : std::string();
}

// The primvar's flattened value with its interpolation, as text so that any value type compares.
std::string
primvarOf(const PXR_NS::UsdStageRefPtr& stage,
          const char* name)
{
    const PXR_NS::UsdGeomPrimvar primvar = PXR_NS::UsdGeomPrimvarsAPI(stage->GetPrimAtPath(PXR_NS::SdfPath(kBody))).GetPrimvar(PXR_NS::TfToken(name));
    PXR_NS::VtValue value;
    if (!primvar || !primvar.ComputeFlattened(&value, PXR_NS::UsdTimeCode::Default())) {
        return std::string("missing");
    }

    return primvar.GetInterpolation().GetString() + " " + primvar.GetTypeName().GetAsToken().GetString() + " " + PXR_NS::TfStringify(value);
}

void
expectClose(const PXR_NS::GfMatrix4d& expected,
            const PXR_NS::GfMatrix4d& actual)
{
    EXPECT_TRUE(PXR_NS::GfIsClose(expected, actual, 1e-6)) << "expected " << expected << "\nactual   " << actual;
}

} // anonymous namespace

class GeoRoundTripTest
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

    // USDRead -> USDToGeo(unpack) -> GeoToUSD, whose Scene input is the reader.
    void build(const char* scene = kScene)
    {
        const std::string path = _dir.path().toStdString() + "/roundtrip.usda";
        {
            QFile f(QString::fromStdString(path));
            ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(scene);
        }
        _reader = make(PLUGINID_NATRON_USDREAD);
        _toGeo = make(PLUGINID_NATRON_USDTOGEO);
        _toUSD = make(PLUGINID_NATRON_GEOTOUSD);
        ASSERT_TRUE(_reader && _toGeo && _toUSD);

        KnobFile* file = dynamic_cast<KnobFile*>(_reader->getKnobByName(kUSDReadParamFile).get());
        ASSERT_TRUE(file != NULL);
        file->setValue(path);
        KnobString* primPattern = dynamic_cast<KnobString*>(_toGeo->getKnobByName(kUSDToGeoParamPrimPattern).get());
        ASSERT_TRUE(primPattern != NULL);
        primPattern->setValue(std::string("/World"));
        KnobChoice* traversal = dynamic_cast<KnobChoice*>(_toGeo->getKnobByName(kUSDToGeoParamTraversal).get());
        ASSERT_TRUE(traversal != NULL);
        traversal->setValue((int)eUSDToGeoTraversalGprims);
        KnobBool* unpack = dynamic_cast<KnobBool*>(_toGeo->getKnobByName(kUSDToGeoParamUnpack).get());
        ASSERT_TRUE(unpack != NULL);
        unpack->setValue(true);

        KnobChoice* other = dynamic_cast<KnobChoice*>(_toUSD->getKnobByName(kGeoToUSDParamOtherPrimitives).get());
        ASSERT_TRUE(other != NULL);
        other->setValueFromID("define", 0);

        connectNodes(_reader, _toGeo, 0, true);
        connectNodes(_toGeo, _toUSD, kGeoToUSDInputGeometry, true);
        connectNodes(_reader, _toUSD, kGeoToUSDInputScene, true);
    }

    static ScenePayload evaluate(const NodePtr& node)
    {
        ScenePayload payload;
        std::string error;

        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk,
                  node->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &payload, &error))
            << error;

        return payload;
    }

    static PXR_NS::UsdStageRefPtr stageOf(const ScenePayload& payload,
                                          SceneQueryPool::Lease* lease)
    {
        std::string error;
        *lease = SceneQueryPool::instance().borrow(payload, SceneLoadSet::everything(), &error);
        EXPECT_TRUE(lease->isValid()) << error;

        return lease->getStage();
    }

    void setBool(const char* name,
                 bool value)
    {
        KnobBool* k = dynamic_cast<KnobBool*>(_toGeo->getKnobByName(name).get());
        ASSERT_TRUE(k != NULL) << name;
        k->setValue(value);
    }

    void setString(const char* name,
                   const std::string& value)
    {
        KnobString* k = dynamic_cast<KnobString*>(_toGeo->getKnobByName(name).get());
        ASSERT_TRUE(k != NULL) << name;
        k->setValue(value);
    }

    GeoDetailConstPtr geometry() const
    {
        GeoDetailConstPtr detail;
        std::string error;

        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk,
                  _toGeo->getEffectInstance()->evaluateGeometry(EffectInstance::EvaluateGeometryArgs(1., ViewIdx(0)), &detail, &error))
            << error;

        return detail;
    }

    // Checks that the output scene is the input scene in everything the round trip carries.
    void expectRoundTripIsTheIdentity()
    {
        const ScenePayload source = evaluate(_reader);
        const ScenePayload result = evaluate(_toUSD);
        SceneQueryPool::Lease sourceLease;
        SceneQueryPool::Lease resultLease;
        const PXR_NS::UsdStageRefPtr before = stageOf(source, &sourceLease);
        const PXR_NS::UsdStageRefPtr after = stageOf(result, &resultLease);
        ASSERT_TRUE(before && after);

        for (const char* path : { "/World", "/World/Group", kBody }) {
            SCOPED_TRACE(path);
            expectClose(worldOf(before, path), worldOf(after, path));
        }
        const std::vector<PXR_NS::GfVec3d> expected = worldPoints(before, kBody);
        const std::vector<PXR_NS::GfVec3d> actual = worldPoints(after, kBody);
        ASSERT_EQ(expected.size(), actual.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            EXPECT_TRUE(PXR_NS::GfIsClose(expected[i], actual[i], 1e-5)) << i << ": expected " << expected[i] << ", actual " << actual[i];
        }
        for (const char* primvar : { "displayColor", "weight", "st" }) {
            EXPECT_EQ(primvarOf(before, primvar), primvarOf(after, primvar)) << primvar;
        }
        EXPECT_EQ(boundMaterial(after, kBody), "/mtl/a");
        EXPECT_EQ(boundMaterial(after, kSubset), "/mtl/b");
        // What the reader records about where the geometry came from stays out of the scene.
        const PXR_NS::UsdGeomPrimvarsAPI primvars(after->GetPrimAtPath(PXR_NS::SdfPath(kBody)));
        for (const char* recorded : { kGeoUsdXformAttr, kGeoUsdMaterialPathAttr, kGeoUsdSourceAttr }) {
            EXPECT_FALSE(primvars.HasPrimvar(PXR_NS::TfToken(recorded))) << recorded;
        }
    }

    NodePtr _reader;
    NodePtr _toGeo;
    NodePtr _toUSD;

private:
    QTemporaryDir _dir;
    std::vector<NodePtr> _nodes;
};

TEST_F(GeoRoundTripTest, AnUnpackedRoundTripKeepsPointsPrimvarsTransformsAndBindings)
{
    build();
    expectRoundTripIsTheIdentity();
}

TEST_F(GeoRoundTripTest, AFlattenedRoundTripWithTheInverseAppliedKeepsTheSame)
{
    build();
    setBool(kUSDToGeoParamFlattenTransforms, true);
    setBool(kUSDToGeoParamApplyInverseUsdTransform, true);
    expectRoundTripIsTheIdentity();
}

TEST_F(GeoRoundTripTest, PrimsReadByNameAloneArePlacedByThatNameAndCarryNoReaderRecords)
{
    build(kNamedScene);
    setString(kUSDToGeoParamPathAttribute, std::string());
    setString(kUSDToGeoParamNameAttribute, "name");
    setBool(kUSDToGeoParamExcludeInactivePrimitives, false);

    const GeoDetailConstPtr detail = geometry();
    ASSERT_TRUE(detail != nullptr);
    EXPECT_TRUE(detail->findAttribute(eGeoAttribClassPrimitive, "path") == nullptr);
    EXPECT_TRUE(detail->findAttribute(eGeoAttribClassPrimitive, "name") != nullptr);
    EXPECT_TRUE(detail->findAttribute(eGeoAttribClassPoint, "name") != nullptr);
    EXPECT_TRUE(detail->findAttribute(eGeoAttribClassPrimitive, kGeoUsdActiveAttr) != nullptr);

    const ScenePayload result = evaluate(_toUSD);
    SceneQueryPool::Lease resultLease;
    const PXR_NS::UsdStageRefPtr after = stageOf(result, &resultLease);
    ASSERT_TRUE(after);

    KnobString* rootPath = dynamic_cast<KnobString*>(_toUSD->getKnobByName(kGeoToUSDParamPathPrefix).get());
    ASSERT_TRUE(rootPath != NULL);
    const PXR_NS::UsdPrim body = after->GetPrimAtPath(PXR_NS::SdfPath(rootPath->getValue() + "/Body"));
    EXPECT_TRUE(body && body.IsA<PXR_NS::UsdGeomMesh>());
    for (const PXR_NS::UsdPrim& prim : after->Traverse(PXR_NS::UsdPrimAllPrimsPredicate)) {
        const PXR_NS::UsdGeomPrimvarsAPI primvars(prim);
        for (const char* recorded : { "name", kGeoUsdActiveAttr }) {
            EXPECT_FALSE(primvars.HasPrimvar(PXR_NS::TfToken(recorded))) << prim.GetPath() << " " << recorded;
        }
    }
}
