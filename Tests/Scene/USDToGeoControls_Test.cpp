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
#include "CacheMemoryPressureGuard.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Geo/GeoPacked.h"
#include "Engine/GeoDetail.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Geometry/GeoUsdConvert.h"
#include "Engine/Nodes/Geometry/USDToGeo.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDXformPrim.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
// Two components, the chair holding two meshes, a guide mesh only the guide purpose shows, and
// an inactive mesh. Every mesh is the unit quad, translated so that its prim is told apart.
const char* const kHierarchyScene = "#usda 1.0\n"
                                    "(\n"
                                    "    metersPerUnit = 0.01\n"
                                    "    upAxis = \"Y\"\n"
                                    ")\n"
                                    "\n"
                                    "def Xform \"World\" (\n"
                                    "    kind = \"group\"\n"
                                    ")\n"
                                    "{\n"
                                    "    def Xform \"Chair\" (\n"
                                    "        kind = \"component\"\n"
                                    "    )\n"
                                    "    {\n"
                                    "        def Mesh \"Seat\"\n"
                                    "        {\n"
                                    "            int[] faceVertexCounts = [4]\n"
                                    "            int[] faceVertexIndices = [0, 1, 2, 3]\n"
                                    "            point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]\n"
                                    "        }\n"
                                    "\n"
                                    "        def Mesh \"Leg\"\n"
                                    "        {\n"
                                    "            int[] faceVertexCounts = [4]\n"
                                    "            int[] faceVertexIndices = [0, 1, 2, 3]\n"
                                    "            point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]\n"
                                    "            double3 xformOp:translate = (0, -2, 0)\n"
                                    "            uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                                    "        }\n"
                                    "    }\n"
                                    "\n"
                                    "    def Xform \"Table\" (\n"
                                    "        kind = \"component\"\n"
                                    "    )\n"
                                    "    {\n"
                                    "        def Mesh \"Top\"\n"
                                    "        {\n"
                                    "            int[] faceVertexCounts = [4]\n"
                                    "            int[] faceVertexIndices = [0, 1, 2, 3]\n"
                                    "            point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]\n"
                                    "            double3 xformOp:translate = (5, 0, 0)\n"
                                    "            uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                                    "        }\n"
                                    "\n"
                                    "        def Mesh \"Guide\"\n"
                                    "        {\n"
                                    "            uniform token purpose = \"guide\"\n"
                                    "            int[] faceVertexCounts = [4]\n"
                                    "            int[] faceVertexIndices = [0, 1, 2, 3]\n"
                                    "            point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]\n"
                                    "        }\n"
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

// Two instances of one sphere, the second moved by 5 in X and hidden by its id.
const char* const kInstancerScene = "#usda 1.0\n"
                                    "(\n"
                                    "    metersPerUnit = 0.01\n"
                                    "    upAxis = \"Y\"\n"
                                    ")\n"
                                    "\n"
                                    "def PointInstancer \"Inst\"\n"
                                    "{\n"
                                    "    rel prototypes = [</Inst/Protos/Ball>]\n"
                                    "    int[] protoIndices = [0, 0]\n"
                                    "    point3f[] positions = [(0, 0, 0), (5, 0, 0)]\n"
                                    "    int64[] ids = [7, 8]\n"
                                    "    int64[] invisibleIds = [8]\n"
                                    "\n"
                                    "    def Scope \"Protos\"\n"
                                    "    {\n"
                                    "        def Sphere \"Ball\"\n"
                                    "        {\n"
                                    "            double radius = 1\n"
                                    "        }\n"
                                    "    }\n"
                                    "}\n";

// One instance of a prototype outside the instancer, the prototype moving from X 0 at frame 1 to
// X 19 at frame 20.
const char* const kOutsidePrototypeScene = "#usda 1.0\n"
                                           "(\n"
                                           "    metersPerUnit = 0.01\n"
                                           "    upAxis = \"Y\"\n"
                                           "    timeCodesPerSecond = 24\n"
                                           "    startTimeCode = 1\n"
                                           "    endTimeCode = 20\n"
                                           ")\n"
                                           "\n"
                                           "def Scope \"Protos\"\n"
                                           "{\n"
                                           "    def Sphere \"Ball\"\n"
                                           "    {\n"
                                           "        double radius = 1\n"
                                           "        double3 xformOp:translate.timeSamples = {\n"
                                           "            1: (0, 0, 0),\n"
                                           "            20: (19, 0, 0),\n"
                                           "        }\n"
                                           "        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                                           "    }\n"
                                           "}\n"
                                           "\n"
                                           "def PointInstancer \"Inst\"\n"
                                           "{\n"
                                           "    rel prototypes = [</Protos/Ball>]\n"
                                           "    int[] protoIndices = [0]\n"
                                           "    point3f[] positions = [(0, 0, 0)]\n"
                                           "}\n";

// /Anim moves from X 0 at frame 1 to X 19 at frame 20, one unit a frame; /Still is the unit
// quad translated by 10 in X.
const char* const kTimeScene = "#usda 1.0\n"
                               "(\n"
                               "    metersPerUnit = 0.01\n"
                               "    upAxis = \"Y\"\n"
                               "    timeCodesPerSecond = 24\n"
                               "    startTimeCode = 1\n"
                               "    endTimeCode = 20\n"
                               ")\n"
                               "\n"
                               "def Mesh \"Anim\"\n"
                               "{\n"
                               "    int[] faceVertexCounts = [4]\n"
                               "    int[] faceVertexIndices = [0, 1, 2, 3]\n"
                               "    point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]\n"
                               "    double3 xformOp:translate.timeSamples = {\n"
                               "        1: (0, 0, 0),\n"
                               "        20: (19, 0, 0),\n"
                               "    }\n"
                               "    uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                               "}\n"
                               "\n"
                               "def Mesh \"Still\"\n"
                               "{\n"
                               "    int[] faceVertexCounts = [4]\n"
                               "    int[] faceVertexIndices = [0, 1, 2, 3]\n"
                               "    point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]\n"
                               "    double3 xformOp:translate = (10, 0, 0)\n"
                               "    uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                               "}\n";

// /Geo/Bound is two quads translated by 10 in X, bound to /mtl/a with its second face bound to
// /mtl/b through a GeomSubset; its parent carries a constant primvar it inherits.
const char* const kDataScene = "#usda 1.0\n"
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
                               "def Xform \"Geo\"\n"
                               "{\n"
                               "    float primvars:shade = 2\n"
                               "\n"
                               "    def Mesh \"Bound\" (\n"
                               "        prepend apiSchemas = [\"MaterialBindingAPI\"]\n"
                               "    )\n"
                               "    {\n"
                               "        int[] faceVertexCounts = [4, 4]\n"
                               "        int[] faceVertexIndices = [0, 1, 4, 3, 1, 2, 5, 4]\n"
                               "        point3f[] points = [(0, 0, 0), (1, 0, 0), (2, 0, 0), (0, 1, 0), (1, 1, 0), (2, 1, 0)]\n"
                               "        color3f[] primvars:displayColor = [(1, 0, 0)] (\n"
                               "            interpolation = \"constant\"\n"
                               "        )\n"
                               "        float[] primvars:foo = [1, 2, 3, 4, 5, 6] (\n"
                               "            interpolation = \"vertex\"\n"
                               "        )\n"
                               "        point3f[] primvars:rest = [(0, 0, 0), (1, 0, 0), (2, 0, 0), (0, 1, 0), (1, 1, 0), (2, 1, 0)] (\n"
                               "            interpolation = \"vertex\"\n"
                               "        )\n"
                               "        texCoord2f[] primvars:st = [(0, 0), (0.5, 0), (1, 0), (0, 1), (0.5, 1), (1, 1)] (\n"
                               "            interpolation = \"vertex\"\n"
                               "        )\n"
                               "        custom float[] weight = [0, 1, 2, 3, 4, 5]\n"
                               "        rel material:binding = </mtl/a>\n"
                               "        double3 xformOp:translate = (10, 0, 0)\n"
                               "        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                               "\n"
                               "        def GeomSubset \"second\" (\n"
                               "            prepend apiSchemas = [\"MaterialBindingAPI\"]\n"
                               "        )\n"
                               "        {\n"
                               "            uniform token elementType = \"face\"\n"
                               "            uniform token familyName = \"materialBind\"\n"
                               "            int[] indices = [1]\n"
                               "            rel material:binding = </mtl/b>\n"
                               "        }\n"
                               "    }\n"
                               "}\n";

std::string
stringAt(const GeoDetail& detail,
         GeoAttribClassEnum cls,
         const char* name,
         GeoOffset offset)
{
    const GeoAttribute* attribute = detail.findAttribute(cls, name);

    return attribute ? std::string(attribute->getString(std::size_t(offset))) : std::string();
}

std::array<double, 3>
pivotOf(const GeoDetail& detail,
        GeoOffset primitive)
{
    return detail.pointPosition(detail.topology().vertexPoint(detail.topology().primitiveVertices(primitive).start));
}

bool
hasAttribute(const GeoDetail& detail,
             const char* name)
{
    for (GeoAttribClassEnum cls : { eGeoAttribClassPoint, eGeoAttribClassVertex, eGeoAttribClassPrimitive, eGeoAttribClassDetail }) {
        if (detail.findAttribute(cls, name)) {
            return true;
        }
    }

    return false;
}

void
expectNear(const std::array<double, 3>& expected,
           const std::array<double, 3>& got)
{
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(expected[i], got[i], 1e-9) << "component " << i;
    }
}
} // anonymous namespace

class USDToGeoControlsTest
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

    // USDRead(content) -> USDToGeo(pattern); returns the USDToGeo node.
    NodePtr chain(const std::string& content,
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
            file->setValue(writeScene("scene" + std::to_string(_nodes.size()) + ".usda", content));
        }
        setString(toGeo, kUSDToGeoParamPrimPattern, pattern);
        connectNodes(reader, toGeo, 0, true);

        return toGeo;
    }

    // USDXformPrim(/Anim, translate keyed from X 0 at frame 1 to X 19 at frame 20) -> USDToGeo(/Anim).
    NodePtr animatedXformChain()
    {
        NodePtr xform = createNode(QString::fromUtf8(PLUGINID_NATRON_USDXFORMPRIM));
        NodePtr toGeo = createNode(QString::fromUtf8(PLUGINID_NATRON_USDTOGEO));

        EXPECT_TRUE(xform && toGeo);
        if (!xform || !toGeo) {
            return NodePtr();
        }
        _nodes.push_back(toGeo);
        _nodes.push_back(xform);
        setString(xform, kSceneParamPrimPath, "/Anim");
        if (KnobDouble* translate = knob<KnobDouble>(xform, kSceneParamTranslate)) {
            translate->setValueAtTime(1., 0., ViewSpec::all(), 0);
            translate->setValueAtTime(20., 19., ViewSpec::all(), 0);
        }
        setString(toGeo, kUSDToGeoParamPrimPattern, "/Anim");
        connectNodes(xform, toGeo, 0, true);

        return toGeo;
    }

    template <typename K>
    static K* knob(const NodePtr& node,
                   const char* name)
    {
        K* k = dynamic_cast<K*>(node->getKnobByName(name).get());

        EXPECT_TRUE(k != NULL) << name;

        return k;
    }

    static void setBool(const NodePtr& node,
                        const char* name,
                        bool value)
    {
        if (KnobBool* k = knob<KnobBool>(node, name)) {
            k->setValue(value);
        }
    }

    static void setString(const NodePtr& node,
                          const char* name,
                          const std::string& value)
    {
        if (KnobString* k = knob<KnobString>(node, name)) {
            k->setValue(value);
        }
    }

    static void setChoice(const NodePtr& node,
                          const char* name,
                          int value)
    {
        if (KnobChoice* k = knob<KnobChoice>(node, name)) {
            k->setValue(value);
        }
    }

    static void setDouble(const NodePtr& node,
                          const char* name,
                          double value)
    {
        if (KnobDouble* k = knob<KnobDouble>(node, name)) {
            k->setValue(value);
        }
    }

    static GeoDetailConstPtr evaluate(const NodePtr& node,
                                      double time = 1.)
    {
        GeoDetailConstPtr detail;
        std::string error;

        EXPECT_EQ(EffectInstance::eRenderRoIRetCodeOk, node->getEffectInstance()->evaluateGeometry(EffectInstance::EvaluateGeometryArgs(time, ViewIdx(0)), &detail, &error)) << error;

        return detail;
    }

    static std::vector<std::string> paths(const GeoDetail& detail)
    {
        std::vector<std::string> out;
        for (GeoIndex i = 0; i < detail.primitiveCount(); ++i) {
            out.push_back(stringAt(detail, eGeoAttribClassPrimitive, "path", detail.primitiveMap().offsetFromIndex(i)));
        }

        return out;
    }

private:
    QTemporaryDir _dir;
    std::vector<NodePtr> _nodes;
};

TEST_F(USDToGeoControlsTest, ComponentsTraversalGivesOnePackedPrimitivePerComponent)
{
    const NodePtr toGeo = chain(kHierarchyScene, "/World");
    ASSERT_TRUE(toGeo != nullptr);
    setChoice(toGeo, kUSDToGeoParamTraversal, (int)eUSDToGeoTraversalComponents);
    const GeoDetailConstPtr detail = evaluate(toGeo);
    ASSERT_TRUE(detail != nullptr);

    EXPECT_EQ(paths(*detail), (std::vector<std::string> { "/World/Chair", "/World/Table" }));
    for (GeoOffset primitive = 0; primitive < 2; ++primitive) {
        EXPECT_EQ(detail->topology().primitiveType(primitive), eGeoPrimitivePackedUSD);
    }
}

TEST_F(USDToGeoControlsTest, GprimsTraversalSkipsOtherPurposesAndInactivePrims)
{
    const NodePtr toGeo = chain(kHierarchyScene, "/World");
    ASSERT_TRUE(toGeo != nullptr);
    setChoice(toGeo, kUSDToGeoParamTraversal, (int)eUSDToGeoTraversalGprims);
    const GeoDetailConstPtr detail = evaluate(toGeo);
    ASSERT_TRUE(detail != nullptr);
    EXPECT_EQ(paths(*detail), (std::vector<std::string> { "/World/Chair/Seat", "/World/Chair/Leg", "/World/Table/Top" }));

    setString(toGeo, kUSDToGeoParamPurpose, "default guide");
    const GeoDetailConstPtr withGuides = evaluate(toGeo);
    ASSERT_TRUE(withGuides != nullptr);
    EXPECT_EQ(paths(*withGuides), (std::vector<std::string> { "/World/Chair/Seat", "/World/Chair/Leg", "/World/Table/Top", "/World/Table/Guide" }));
}

TEST_F(USDToGeoControlsTest, AnUnknownPurposeIsAnError)
{
    const NodePtr toGeo = chain(kHierarchyScene, "/World/*");
    ASSERT_TRUE(toGeo != nullptr);
    setString(toGeo, kUSDToGeoParamPurpose, "default rendr");
    GeoDetailConstPtr detail;
    std::string error;
    EXPECT_NE(EffectInstance::eRenderRoIRetCodeOk, toGeo->getEffectInstance()->evaluateGeometry(EffectInstance::EvaluateGeometryArgs(1., ViewIdx(0)), &detail, &error));
}

TEST_F(USDToGeoControlsTest, AnEmptyPurposeListIsAnError)
{
    const NodePtr toGeo = chain(kHierarchyScene, "/World/*");
    ASSERT_TRUE(toGeo != nullptr);
    for (const char* blank : { "", " , " }) {
        SCOPED_TRACE(blank);
        setString(toGeo, kUSDToGeoParamPurpose, blank);
        GeoDetailConstPtr detail;
        std::string error;
        EXPECT_NE(EffectInstance::eRenderRoIRetCodeOk, toGeo->getEffectInstance()->evaluateGeometry(EffectInstance::EvaluateGeometryArgs(1., ViewIdx(0)), &detail, &error));
    }
}

TEST_F(USDToGeoControlsTest, NameAttributeHoldsEachPrimsName)
{
    const NodePtr toGeo = chain(kHierarchyScene, "/World/Chair/*");
    ASSERT_TRUE(toGeo != nullptr);
    setString(toGeo, kUSDToGeoParamNameAttribute, "name");
    setString(toGeo, kUSDToGeoParamPathAttribute, "srcpath");
    const GeoDetailConstPtr detail = evaluate(toGeo);
    ASSERT_TRUE(detail != nullptr);

    ASSERT_EQ(detail->primitiveCount(), GeoIndex(2));
    EXPECT_TRUE(detail->findAttribute(eGeoAttribClassPrimitive, "path") == nullptr);
    EXPECT_EQ(stringAt(*detail, eGeoAttribClassPrimitive, "srcpath", 0), "/World/Chair/Seat");
    EXPECT_EQ(stringAt(*detail, eGeoAttribClassPrimitive, "name", 0), "Seat");
    EXPECT_EQ(stringAt(*detail, eGeoAttribClassPrimitive, "name", 1), "Leg");

    // The same attributes when unpacked.
    setBool(toGeo, kUSDToGeoParamUnpack, true);
    const GeoDetailConstPtr unpacked = evaluate(toGeo);
    ASSERT_TRUE(unpacked != nullptr);
    ASSERT_EQ(unpacked->primitiveCount(), GeoIndex(2));
    EXPECT_EQ(stringAt(*unpacked, eGeoAttribClassPrimitive, "srcpath", 1), "/World/Chair/Leg");
    EXPECT_EQ(stringAt(*unpacked, eGeoAttribClassPrimitive, "name", 1), "Leg");
}

TEST_F(USDToGeoControlsTest, AnInactivePrimIsSkippedUnlessInactivePrimsAreIncluded)
{
    const NodePtr toGeo = chain(kHierarchyScene, "/World/*");
    ASSERT_TRUE(toGeo != nullptr);
    const GeoDetailConstPtr detail = evaluate(toGeo);
    ASSERT_TRUE(detail != nullptr);
    EXPECT_EQ(paths(*detail), (std::vector<std::string> { "/World/Chair", "/World/Table" }));
    EXPECT_TRUE(detail->findAttribute(eGeoAttribClassPrimitive, kGeoUsdActiveAttr) == nullptr);

    setBool(toGeo, kUSDToGeoParamExcludeInactivePrimitives, false);
    const GeoDetailConstPtr all = evaluate(toGeo);
    ASSERT_TRUE(all != nullptr);
    ASSERT_EQ(paths(*all), (std::vector<std::string> { "/World/Chair", "/World/Table", "/World/Off" }));
    const GeoAttribute* active = all->findAttribute(eGeoAttribClassPrimitive, kGeoUsdActiveAttr);
    ASSERT_TRUE(active != nullptr);
    EXPECT_EQ(active->get<std::int32_t>(0), 1);
    EXPECT_EQ(active->get<std::int32_t>(1), 1);
    EXPECT_EQ(active->get<std::int32_t>(2), 0);

    setBool(toGeo, kUSDToGeoParamUnpack, true);
    setString(toGeo, kUSDToGeoParamPrimPattern, "/World/Chair/Seat /World/Off");
    const GeoDetailConstPtr unpacked = evaluate(toGeo);
    ASSERT_TRUE(unpacked != nullptr);
    ASSERT_EQ(unpacked->primitiveCount(), GeoIndex(2));
    const GeoAttribute* unpackedActive = unpacked->findAttribute(eGeoAttribClassPrimitive, kGeoUsdActiveAttr);
    ASSERT_TRUE(unpackedActive != nullptr);
    EXPECT_EQ(unpackedActive->get<std::int32_t>(0), 1);
    EXPECT_EQ(unpackedActive->get<std::int32_t>(1), 0);
}

TEST_F(USDToGeoControlsTest, ImportPointInstancesGivesOnePackedPrimitivePerInstance)
{
    const NodePtr toGeo = chain(kInstancerScene, "/Inst");
    ASSERT_TRUE(toGeo != nullptr);
    const GeoDetailConstPtr whole = evaluate(toGeo);
    ASSERT_TRUE(whole != nullptr);
    EXPECT_EQ(paths(*whole), (std::vector<std::string> { "/Inst" }));

    setBool(toGeo, kUSDToGeoParamImportPointInstances, true);
    const GeoDetailConstPtr detail = evaluate(toGeo);
    ASSERT_TRUE(detail != nullptr);
    ASSERT_EQ(paths(*detail), (std::vector<std::string> { "/Inst/Protos/Ball", "/Inst/Protos/Ball" }));
    for (GeoOffset primitive = 0; primitive < 2; ++primitive) {
        EXPECT_EQ(std::string(geoPackedUSDPath(*detail, primitive)), "/Inst/Protos/Ball");
        EXPECT_EQ(geoPackedUSDParentTransform(*detail, primitive), geoMatrixIdentity());
    }
    const GeoMatrix4 second = geoPackedTransform(*detail, 1);
    expectNear({ 5., 0., 0. }, { second[3], second[7], second[11] });
    expectNear({ 5., 0., 0. }, pivotOf(*detail, 1));

    const GeoAttribute* id = detail->findAttribute(eGeoAttribClassPoint, "id");
    ASSERT_TRUE(id != nullptr);
    EXPECT_EQ(id->get<std::int64_t>(std::size_t(detail->topology().vertexPoint(detail->topology().primitiveVertices(0).start))), 7);
    EXPECT_EQ(id->get<std::int64_t>(std::size_t(detail->topology().vertexPoint(detail->topology().primitiveVertices(1).start))), 8);
    EXPECT_EQ(stringAt(*detail, eGeoAttribClassPrimitive, "usdvisibility", 0), "inherited");
    EXPECT_EQ(stringAt(*detail, eGeoAttribClassPrimitive, "usdvisibility", 1), "invisible");
}

TEST_F(USDToGeoControlsTest, AnAnimatedPrototypeOutsideTheInstancerMakesTheInstancesVary)
{
    // A stale frame-1 entry must still be there to be served at frame 20.
    DisableUnreachableRAMPurging noPurging;
    const NodePtr toGeo = chain(kOutsidePrototypeScene, "/Inst");
    ASSERT_TRUE(toGeo != nullptr);
    setBool(toGeo, kUSDToGeoParamImportPointInstances, true);
    setChoice(toGeo, kUSDToGeoParamPivotLocation, (int)eUSDToGeoPivotLocationOrigin);
    const GeoDetailConstPtr first = evaluate(toGeo, 1.);
    ASSERT_TRUE(first != nullptr);
    ASSERT_EQ(first->primitiveCount(), GeoIndex(1));
    expectNear({ 0., 0., 0. }, pivotOf(*first, 0));
    EXPECT_TRUE(toGeo->getEffectInstance()->isFrameVaryingOrAnimated_Recursive());

    const GeoDetailConstPtr last = evaluate(toGeo, 20.);
    ASSERT_TRUE(last != nullptr);
    ASSERT_EQ(last->primitiveCount(), GeoIndex(1));
    expectNear({ 19., 0., 0. }, pivotOf(*last, 0));
}

TEST_F(USDToGeoControlsTest, AStaticSampleReadsTheImportFrameAtEveryFrame)
{
    const NodePtr toGeo = chain(kTimeScene, "/Anim");
    ASSERT_TRUE(toGeo != nullptr);
    setChoice(toGeo, kUSDToGeoParamPivotLocation, (int)eUSDToGeoPivotLocationOrigin);
    const GeoDetailConstPtr animated = evaluate(toGeo, 20.);
    ASSERT_TRUE(animated != nullptr);
    ASSERT_EQ(animated->primitiveCount(), GeoIndex(1));
    expectNear({ 19., 0., 0. }, pivotOf(*animated, 0));

    setChoice(toGeo, kUSDToGeoParamTimeSample, (int)eUSDToGeoTimeSampleStatic);
    setDouble(toGeo, kUSDToGeoParamImportFrame, 10.);
    const GeoDetailConstPtr first = evaluate(toGeo, 1.);
    const GeoDetailConstPtr last = evaluate(toGeo, 20.);
    ASSERT_TRUE(first && last);
    ASSERT_EQ(first->primitiveCount(), GeoIndex(1));
    ASSERT_EQ(last->primitiveCount(), GeoIndex(1));
    expectNear({ 9., 0., 0. }, pivotOf(*first, 0));
    expectNear({ 9., 0., 0. }, pivotOf(*last, 0));

    // The same when unpacked: the points are read at the import frame.
    setBool(toGeo, kUSDToGeoParamUnpack, true);
    setBool(toGeo, kUSDToGeoParamFlattenTransforms, true);
    const GeoDetailConstPtr unpackedFirst = evaluate(toGeo, 1.);
    const GeoDetailConstPtr unpackedLast = evaluate(toGeo, 20.);
    ASSERT_TRUE(unpackedFirst && unpackedLast);
    ASSERT_EQ(unpackedFirst->pointCount(), GeoIndex(4));
    ASSERT_EQ(unpackedLast->pointCount(), GeoIndex(4));
    for (GeoIndex i = 0; i < 4; ++i) {
        expectNear(unpackedFirst->pointPosition(unpackedFirst->pointMap().offsetFromIndex(i)), unpackedLast->pointPosition(unpackedLast->pointMap().offsetFromIndex(i)));
    }
    expectNear({ 9., 0., 0. }, unpackedFirst->pointPosition(unpackedFirst->pointMap().offsetFromIndex(0)));
}

TEST_F(USDToGeoControlsTest, AStaticSampleIsTimeInvariantBelowAnAnimatedInput)
{
    // The cache-hit check needs the frame-1 entry to still be there.
    DisableUnreachableRAMPurging noPurging;
    const NodePtr animated = animatedXformChain();
    ASSERT_TRUE(animated != nullptr);
    setChoice(animated, kUSDToGeoParamPivotLocation, (int)eUSDToGeoPivotLocationOrigin);
    const GeoDetailConstPtr atFrame20 = evaluate(animated, 20.);
    ASSERT_TRUE(atFrame20 != nullptr);
    ASSERT_EQ(atFrame20->primitiveCount(), GeoIndex(1));
    expectNear({ 19., 0., 0. }, pivotOf(*atFrame20, 0));
    EXPECT_TRUE(animated->getEffectInstance()->isFrameVaryingOrAnimated_Recursive());

    setChoice(animated, kUSDToGeoParamTimeSample, (int)eUSDToGeoTimeSampleStatic);
    setDouble(animated, kUSDToGeoParamImportFrame, 10.);
    EXPECT_FALSE(animated->getEffectInstance()->isFrameVaryingOrAnimated_Recursive());
    const GeoDetailConstPtr first = evaluate(animated, 1.);
    const GeoDetailConstPtr last = evaluate(animated, 20.);
    ASSERT_TRUE(first && last);
    ASSERT_EQ(first->primitiveCount(), GeoIndex(1));
    const NodePtr xform = animated->getInput(0);
    ASSERT_TRUE(xform != nullptr);
    KnobDouble* translate = knob<KnobDouble>(xform, kSceneParamTranslate);
    ASSERT_TRUE(translate != NULL);
    expectNear({ translate->getValueAtTime(10., 0), 0., 0. }, pivotOf(*first, 0));
    EXPECT_EQ(first.get(), last.get());
}

TEST_F(USDToGeoControlsTest, PivotLocationPutsThePointAtTheCentroidOrTheOrigin)
{
    const NodePtr toGeo = chain(kTimeScene, "/Still");
    ASSERT_TRUE(toGeo != nullptr);
    const GeoDetailConstPtr centroid = evaluate(toGeo);
    ASSERT_TRUE(centroid != nullptr);
    ASSERT_EQ(centroid->primitiveCount(), GeoIndex(1));
    expectNear({ 10.5, 0.5, 0. }, pivotOf(*centroid, 0));

    setChoice(toGeo, kUSDToGeoParamPivotLocation, (int)eUSDToGeoPivotLocationOrigin);
    const GeoDetailConstPtr origin = evaluate(toGeo);
    ASSERT_TRUE(origin != nullptr);
    ASSERT_EQ(origin->primitiveCount(), GeoIndex(1));
    expectNear({ 10., 0., 0. }, pivotOf(*origin, 0));
    // The pivot does not move the contents.
    EXPECT_EQ(geoPackedTransform(*origin, 0), geoPackedTransform(*centroid, 0));
}

TEST_F(USDToGeoControlsTest, ImportPrimvarsFiltersByUsdName)
{
    const NodePtr toGeo = chain(kDataScene, "/Geo/Bound");
    ASSERT_TRUE(toGeo != nullptr);
    setBool(toGeo, kUSDToGeoParamUnpack, true);
    const GeoDetailConstPtr all = evaluate(toGeo);
    ASSERT_TRUE(all != nullptr);
    EXPECT_TRUE(hasAttribute(*all, "Cd"));
    EXPECT_TRUE(hasAttribute(*all, "foo"));

    setString(toGeo, kUSDToGeoParamImportPrimvars, "* ^displayColor");
    const GeoDetailConstPtr filtered = evaluate(toGeo);
    ASSERT_TRUE(filtered != nullptr);
    EXPECT_FALSE(hasAttribute(*filtered, "Cd"));
    EXPECT_TRUE(hasAttribute(*filtered, "foo"));
    EXPECT_TRUE(hasAttribute(*filtered, "uv"));
}

TEST_F(USDToGeoControlsTest, InheritedPrimvarsAndCustomAttributesAreReadOnRequest)
{
    const NodePtr toGeo = chain(kDataScene, "/Geo/Bound");
    ASSERT_TRUE(toGeo != nullptr);
    setBool(toGeo, kUSDToGeoParamUnpack, true);
    const GeoDetailConstPtr plain = evaluate(toGeo);
    ASSERT_TRUE(plain != nullptr);
    EXPECT_FALSE(hasAttribute(*plain, "shade"));
    EXPECT_FALSE(hasAttribute(*plain, "weight"));

    setBool(toGeo, kUSDToGeoParamImportInheritedPrimvars, true);
    setString(toGeo, kUSDToGeoParamImportAttributes, "weight");
    const GeoDetailConstPtr detail = evaluate(toGeo);
    ASSERT_TRUE(detail != nullptr);
    EXPECT_TRUE(hasAttribute(*detail, "shade"));
    const GeoAttribute* weight = detail->findAttribute(eGeoAttribClassPoint, "weight");
    ASSERT_TRUE(weight != nullptr);
    EXPECT_EQ(weight->get<float>(std::size_t(detail->pointMap().offsetFromIndex(5))), 5.f);
}

TEST_F(USDToGeoControlsTest, TranslateStToUvAndNonTransformingPrimvars)
{
    const NodePtr toGeo = chain(kDataScene, "/Geo/Bound");
    ASSERT_TRUE(toGeo != nullptr);
    setBool(toGeo, kUSDToGeoParamUnpack, true);
    const GeoDetailConstPtr detail = evaluate(toGeo);
    ASSERT_TRUE(detail != nullptr);
    EXPECT_TRUE(hasAttribute(*detail, "uv"));
    EXPECT_FALSE(hasAttribute(*detail, "st"));
    const GeoAttribute* rest = detail->findAttribute(eGeoAttribClassPoint, "rest");
    ASSERT_TRUE(rest != nullptr);
    EXPECT_EQ(rest->typeInfo(), eGeoTypeInfoNone);

    setBool(toGeo, kUSDToGeoParamTranslateStToUv, false);
    setString(toGeo, kUSDToGeoParamNonTransformingPrimvars, "");
    const GeoDetailConstPtr kept = evaluate(toGeo);
    ASSERT_TRUE(kept != nullptr);
    EXPECT_TRUE(hasAttribute(*kept, "st"));
    EXPECT_FALSE(hasAttribute(*kept, "uv"));
    const GeoAttribute* transforming = kept->findAttribute(eGeoAttribClassPoint, "rest");
    ASSERT_TRUE(transforming != nullptr);
    EXPECT_EQ(transforming->typeInfo(), eGeoTypeInfoPoint);
}

TEST_F(USDToGeoControlsTest, UsdXformIsTheWorldMatrix)
{
    const NodePtr toGeo = chain(kDataScene, "/Geo/Bound");
    ASSERT_TRUE(toGeo != nullptr);
    setBool(toGeo, kUSDToGeoParamUnpack, true);
    const GeoDetailConstPtr detail = evaluate(toGeo);
    ASSERT_TRUE(detail != nullptr);
    ASSERT_EQ(detail->primitiveCount(), GeoIndex(2));
    const GeoAttribute* xform = detail->findAttribute(eGeoAttribClassPrimitive, kGeoUsdXformAttr);
    ASSERT_TRUE(xform != nullptr);
    EXPECT_EQ(xform->tupleSize(), 16);
    EXPECT_EQ(xform->typeInfo(), eGeoTypeInfoNone);
    const GeoMatrix4 expected = geoMatrixFromUsd(PXR_NS::GfMatrix4d(1.).SetTranslate(PXR_NS::GfVec3d(10., 0., 0.)));
    for (GeoOffset primitive = 0; primitive < 2; ++primitive) {
        GeoMatrix4 got {};
        xform->getTuple<double>(std::size_t(primitive), std::span<double>(got.data(), got.size()));
        EXPECT_EQ(got, expected) << primitive;
    }

    setBool(toGeo, kUSDToGeoParamWriteUsdXform, false);
    const GeoDetailConstPtr without = evaluate(toGeo);
    ASSERT_TRUE(without != nullptr);
    EXPECT_TRUE(without->findAttribute(eGeoAttribClassPrimitive, kGeoUsdXformAttr) == nullptr);
}

TEST_F(USDToGeoControlsTest, MaterialPathFollowsTheBindingAndItsSubsets)
{
    const NodePtr toGeo = chain(kDataScene, "/Geo/Bound");
    ASSERT_TRUE(toGeo != nullptr);
    setBool(toGeo, kUSDToGeoParamUnpack, true);
    const GeoDetailConstPtr detail = evaluate(toGeo);
    ASSERT_TRUE(detail != nullptr);
    ASSERT_EQ(detail->primitiveCount(), GeoIndex(2));
    EXPECT_EQ(stringAt(*detail, eGeoAttribClassPrimitive, kGeoUsdMaterialPathAttr, 0), "/mtl/a");
    EXPECT_EQ(stringAt(*detail, eGeoAttribClassPrimitive, kGeoUsdMaterialPathAttr, 1), "/mtl/b");

    setBool(toGeo, kUSDToGeoParamImportMaterialPath, false);
    const GeoDetailConstPtr without = evaluate(toGeo);
    ASSERT_TRUE(without != nullptr);
    EXPECT_TRUE(without->findAttribute(eGeoAttribClassPrimitive, kGeoUsdMaterialPathAttr) == nullptr);
}

TEST_F(USDToGeoControlsTest, ApplyingTheInverseUsdTransformLeavesThePointsLocal)
{
    const NodePtr toGeo = chain(kDataScene, "/Geo/Bound");
    ASSERT_TRUE(toGeo != nullptr);
    setBool(toGeo, kUSDToGeoParamUnpack, true);
    setBool(toGeo, kUSDToGeoParamFlattenTransforms, true);
    const GeoDetailConstPtr world = evaluate(toGeo);
    ASSERT_TRUE(world != nullptr);
    expectNear({ 10., 0., 0. }, world->pointPosition(world->pointMap().offsetFromIndex(0)));
    EXPECT_TRUE(world->findAttribute(eGeoAttribClassDetail, kGeoUsdWorldSpaceAttr) != nullptr);

    setBool(toGeo, kUSDToGeoParamApplyInverseUsdTransform, true);
    const GeoDetailConstPtr local = evaluate(toGeo);
    ASSERT_TRUE(local != nullptr);
    ASSERT_EQ(local->pointCount(), GeoIndex(6));
    expectNear({ 0., 0., 0. }, local->pointPosition(local->pointMap().offsetFromIndex(0)));
    expectNear({ 2., 1., 0. }, local->pointPosition(local->pointMap().offsetFromIndex(5)));
    EXPECT_TRUE(local->findAttribute(eGeoAttribClassDetail, kGeoUsdWorldSpaceAttr) == nullptr);
}
