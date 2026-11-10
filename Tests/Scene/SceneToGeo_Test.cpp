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

#include <array>
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
#include "Engine/Geo/GeoPacked.h"
#include "Engine/GeoDetail.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Geometry/GeoUsdConvert.h"
#include "Engine/Nodes/Geometry/USDToGeo.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kReferenceScene = NATRON_TESTS_FIXTURES_DIR "/scene-reference.usda";

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

void
addQuad(GeoDetailBuilder& b,
        double x)
{
    const GeoOffset first = b.appendPoints(4);
    b.setPointPosition(first, x, 0., 0.);
    b.setPointPosition(first + 1, x + 1., 0., 0.);
    b.setPointPosition(first + 2, x + 1., 1., 0.);
    b.setPointPosition(first + 3, x, 1., 0.);
    const GeoOffset corners[4] = { first, first + 1, first + 2, first + 3 };
    b.appendPrimitive(eGeoPrimitivePolygon, corners);
}

std::string
primitivePath(const GeoDetail& detail,
              GeoOffset primitive)
{
    const GeoAttribute* path = detail.findAttribute(eGeoAttribClassPrimitive, "path");

    return path ? std::string(path->getString(std::size_t(primitive))) : std::string();
}

std::array<double, 3>
translation(const GeoMatrix4& m)
{
    return { m[3], m[7], m[11] };
}

// Reading from USD marks every primitive, which changes what GeoToUSD authors and so stays in
// the content hash; the round trip is otherwise the identity.
GeoDetail
withoutUsdSource(const GeoDetail& detail)
{
    const GeoAttribute* marker = detail.findAttribute(eGeoAttribClassPrimitive, kGeoUsdSourceAttr);
    EXPECT_TRUE(marker != nullptr);
    for (GeoIndex p = 0; marker && p < detail.primitiveCount(); ++p) {
        EXPECT_EQ(1, marker->get<std::int32_t>(std::size_t(p))) << p;
    }
    GeoDetailBuilder b(detail);
    b.removeAttribute(eGeoAttribClassPrimitive, kGeoUsdSourceAttr);

    return b.finish();
}
} // anonymous namespace

class SceneToGeoTest
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

    std::string writeScene(const std::string& name,
                           const std::string& content)
    {
        const std::string path = _dir.path().toStdString() + "/" + name;
        QFile f(QString::fromStdString(path));

        EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(content.c_str(), (qint64)content.size());

        return path;
    }

    // USDRead(path) -> USDToGeo(pattern); returns the USDToGeo node.
    NodePtr chain(const std::string& path,
                  const std::string& pattern)
    {
        NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
        NodePtr toGeo = createNode(QString::fromUtf8(PLUGINID_NATRON_USDTOGEO));

        EXPECT_TRUE(reader && toGeo);
        if (!reader || !toGeo) {
            return NodePtr();
        }
        _nodes.push_back(toGeo);
        _nodes.push_back(reader);
        KnobFile* file = dynamic_cast<KnobFile*>(reader->getKnobByName(kUSDReadParamFile).get());
        EXPECT_TRUE(file != NULL);
        if (file) {
            file->setValue(path);
        }
        KnobString* primPattern = dynamic_cast<KnobString*>(toGeo->getKnobByName(kUSDToGeoParamPrimPattern).get());
        EXPECT_TRUE(primPattern != NULL);
        if (primPattern) {
            primPattern->setValue(pattern);
        }
        connectNodes(reader, toGeo, 0, true);

        return toGeo;
    }

    static void setBool(const NodePtr& node,
                        const char* name,
                        bool value)
    {
        KnobBool* knob = dynamic_cast<KnobBool*>(node->getKnobByName(name).get());

        ASSERT_TRUE(knob != NULL);
        knob->setValue(value);
    }

    static GeoDetailConstPtr evaluate(const NodePtr& node,
                                      double time = 1.)
    {
        GeoDetailConstPtr detail;
        std::string error;

        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk, node->getEffectInstance()->evaluateGeometry(EffectInstance::EvaluateGeometryArgs(time, ViewIdx(0)), &detail, &error)) << error;

        return detail;
    }

private:
    QTemporaryDir _dir;
    std::vector<NodePtr> _nodes;
};

TEST_F(SceneToGeoTest, PackedGivesOnePackedPrimitivePerPrimAndNoTopology)
{
    const NodePtr toGeo = chain(kReferenceScene, "/World/*");
    ASSERT_TRUE(toGeo != nullptr);
    const GeoDetailConstPtr detail = evaluate(toGeo);
    ASSERT_TRUE(detail != nullptr);

    const char* const expected[] = { "/World/Box", "/World/Ball", "/World/Cam" };
    ASSERT_EQ(detail->primitiveCount(), GeoIndex(3));
    EXPECT_EQ(detail->pointCount(), GeoIndex(3));
    for (GeoOffset primitive = 0; primitive < 3; ++primitive) {
        EXPECT_EQ(detail->topology().primitiveType(primitive), eGeoPrimitivePackedUSD);
        EXPECT_EQ(detail->topology().primitiveVertices(primitive).count, GeoOffset(1));
        EXPECT_EQ(primitivePath(*detail, primitive), expected[primitive]);
        EXPECT_EQ(std::string(geoPackedUSDPath(*detail, primitive)), expected[primitive]);
        EXPECT_EQ(geoPackedUSDParentTransform(*detail, primitive), geoMatrixIdentity());
    }

    EXPECT_EQ(geoPackedTransform(*detail, 0), geoMatrixIdentity());
    const GeoBounds box = geoPackedLocalBounds(*detail, 0);
    EXPECT_EQ(box.min, (std::array<double, 3> { -1., -1., -1. }));
    EXPECT_EQ(box.max, (std::array<double, 3> { 1., 1., 1. }));

    EXPECT_EQ(translation(geoPackedTransform(*detail, 1)), (std::array<double, 3> { 3., 0., 0. }));
    EXPECT_EQ(detail->pointPosition(detail->topology().vertexPoint(detail->topology().primitiveVertices(1).start)), (std::array<double, 3> { 3., 0., 0. }));
    const GeoBounds ball = geoPackedLocalBounds(*detail, 1);
    EXPECT_EQ(ball.min, (std::array<double, 3> { -0.5, -0.5, -0.5 }));
    EXPECT_EQ(ball.max, (std::array<double, 3> { 0.5, 0.5, 0.5 }));

    EXPECT_EQ(translation(geoPackedTransform(*detail, 2)), (std::array<double, 3> { 1., 1., 10. }));
}

TEST_F(SceneToGeoTest, PackedTransformsFollowTheFrame)
{
    const NodePtr toGeo = chain(kReferenceScene, "/World/Ball");
    ASSERT_TRUE(toGeo != nullptr);
    const GeoDetailConstPtr first = evaluate(toGeo, 1.);
    const GeoDetailConstPtr last = evaluate(toGeo, 24.);
    ASSERT_TRUE(first && last);
    ASSERT_EQ(first->primitiveCount(), GeoIndex(1));
    ASSERT_EQ(last->primitiveCount(), GeoIndex(1));
    EXPECT_EQ(translation(geoPackedTransform(*first, 0)), (std::array<double, 3> { 3., 0., 0. }));
    EXPECT_EQ(translation(geoPackedTransform(*last, 0)), (std::array<double, 3> { 3., 2., 0. }));
}

TEST_F(SceneToGeoTest, UnpackGivesPolygonsInOnePointPoolAndListsSkippedPrims)
{
    const NodePtr toGeo = chain(writeScene("meshes.usda", kMeshScene), "/World/*");
    ASSERT_TRUE(toGeo != nullptr);
    setBool(toGeo, kUSDToGeoParamUnpack, true);
    const GeoDetailConstPtr detail = evaluate(toGeo);
    ASSERT_TRUE(detail != nullptr);

    ASSERT_EQ(detail->primitiveCount(), GeoIndex(2));
    EXPECT_EQ(detail->pointCount(), GeoIndex(7));
    EXPECT_EQ(detail->topology().primitiveType(0), eGeoPrimitivePolygon);
    EXPECT_EQ(detail->topology().primitiveType(1), eGeoPrimitivePolygon);
    EXPECT_EQ(detail->topology().primitiveVertices(0).count, GeoOffset(4));
    EXPECT_EQ(detail->topology().primitiveVertices(1).count, GeoOffset(3));
    EXPECT_EQ(primitivePath(*detail, 0), "/World/A");
    EXPECT_EQ(primitivePath(*detail, 1), "/World/B");
    const GeoVertexRange triangle = detail->topology().primitiveVertices(1);
    EXPECT_EQ(detail->pointPosition(detail->topology().vertexPoint(triangle.start + 1)), (std::array<double, 3> { 1., 0., 0. }));

    QString message;
    int type = 0;
    toGeo->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeInfo, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("/World/C"))) << message.toStdString();
}

TEST_F(SceneToGeoTest, FlattenMovesATranslatedPrimsPoints)
{
    const NodePtr toGeo = chain(writeScene("meshes.usda", kMeshScene), "/World/A /World/B");
    ASSERT_TRUE(toGeo != nullptr);
    setBool(toGeo, kUSDToGeoParamUnpack, true);
    setBool(toGeo, kUSDToGeoParamFlattenTransforms, true);
    const GeoDetailConstPtr detail = evaluate(toGeo);
    ASSERT_TRUE(detail != nullptr);

    ASSERT_EQ(detail->primitiveCount(), GeoIndex(2));
    const GeoVertexRange quad = detail->topology().primitiveVertices(0);
    const GeoVertexRange triangle = detail->topology().primitiveVertices(1);
    EXPECT_EQ(detail->pointPosition(detail->topology().vertexPoint(quad.start + 1)), (std::array<double, 3> { 1., 0., 0. }));
    EXPECT_EQ(detail->pointPosition(detail->topology().vertexPoint(triangle.start + 1)), (std::array<double, 3> { 11., 0., 0. }));
}

// The authored layer stands in for the GeoToUSD node: read back unpacked, it is the detail.
TEST_F(SceneToGeoTest, AuthoredDetailReadsBackUnpackedAsTheSameDetail)
{
    GeoDetailBuilder b;
    addQuad(b, 0.);
    addQuad(b, 5.);
    GeoAttributeDesc pathDesc;
    pathDesc.name = "path";
    pathDesc.storage = eGeoStorageString;
    GeoAttribute& path = b.addAttribute(eGeoAttribClassPrimitive, pathDesc);
    path.setString(0, "/geo/a");
    path.setString(1, "/geo/b");
    const GeoDetailConstPtr source = std::make_shared<const GeoDetail>(b.finish());

    const GeoUsdConvert convert(source, GeoUsdConvertOptions());
    PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::CreateAnonymous("geo.usda");
    std::string error;
    ASSERT_TRUE(convert.author(layer, PXR_NS::UsdTimeCode::Default(), &error)) << error;
    const std::string file = writeScene("authored.usda", std::string());
    ASSERT_TRUE(layer->Export(file));

    const NodePtr toGeo = chain(file, "/geo/*");
    ASSERT_TRUE(toGeo != nullptr);
    setBool(toGeo, kUSDToGeoParamUnpack, true);
    const GeoDetailConstPtr back = evaluate(toGeo);
    ASSERT_TRUE(back != nullptr);
    EXPECT_EQ(back->primitiveCount(), source->primitiveCount());
    EXPECT_EQ(back->pointCount(), source->pointCount());
    EXPECT_EQ(withoutUsdSource(*back).contentHash(), source->contentHash());
}

TEST_F(SceneToGeoTest, FlattenMarksTheDetailAsWorldSpace)
{
    const NodePtr toGeo = chain(writeScene("meshes.usda", kMeshScene), "/World/A");
    ASSERT_TRUE(toGeo != nullptr);
    setBool(toGeo, kUSDToGeoParamUnpack, true);
    GeoDetailConstPtr detail = evaluate(toGeo);
    ASSERT_TRUE(detail != nullptr);
    EXPECT_EQ(detail->findAttribute(eGeoAttribClassDetail, kGeoUsdWorldSpaceAttr), nullptr);

    setBool(toGeo, kUSDToGeoParamFlattenTransforms, true);
    detail = evaluate(toGeo);
    ASSERT_TRUE(detail != nullptr);
    const GeoAttribute* worldSpace = detail->findAttribute(eGeoAttribClassDetail, kGeoUsdWorldSpaceAttr);
    ASSERT_TRUE(worldSpace != nullptr);
    EXPECT_NE(worldSpace->get<std::int32_t>(0), 0);
}

TEST_F(SceneToGeoTest, OnlyAnimatedPrimsMakeTheGeometryVaryOverTime)
{
    const NodePtr still = chain(kReferenceScene, "/World/Box");
    ASSERT_TRUE(still != nullptr);
    ASSERT_TRUE(evaluate(still, 1.) != nullptr);
    EXPECT_FALSE(still->getEffectInstance()->isFrameVaryingOrAnimated_Recursive());

    const NodePtr moving = chain(kReferenceScene, "/World/Ball");
    ASSERT_TRUE(moving != nullptr);
    ASSERT_TRUE(evaluate(moving, 1.) != nullptr);
    EXPECT_TRUE(moving->getEffectInstance()->isFrameVaryingOrAnimated_Recursive());
}
