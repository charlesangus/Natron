// ***** BEGIN LICENSE BLOCK *****
// This file is part of Natron <https://natrongithub.github.io/>,
// (C) 2018-2023 The Natron developers
// (C) 2013-2018 INRIA and Alexandre Gauthier-Foichat
//
// Natron is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3 of the License, or
// (at your option) any later version.
//
// Natron is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-3.0.html>
// ***** END LICENSE BLOCK *****

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include "Global/Macros.h"

#include <cstdint>
#include <memory>
#include <set>
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
#include "Engine/Nodes/Geometry/GeoPrimitive.h"
#include "Engine/Nodes/Geometry/GeoToUSD.h"
#include "Engine/Nodes/Geometry/GeoUsdConvert.h"
#include "Engine/Nodes/Geometry/USDToGeo.h"
#include "Engine/Nodes/Scene/ScenePayloadImpl.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDPrune.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

// /a holds the mesh /a/m; /T is an Xform translated by (1, 0, 0) with nothing below it.
const char* const kScene = "#usda 1.0\n"
                           "\n"
                           "def Xform \"a\"\n"
                           "{\n"
                           "    def Mesh \"m\"\n"
                           "    {\n"
                           "        int[] faceVertexCounts = [4]\n"
                           "        int[] faceVertexIndices = [0, 1, 2, 3]\n"
                           "        point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)]\n"
                           "    }\n"
                           "}\n"
                           "\n"
                           "def Xform \"T\"\n"
                           "{\n"
                           "    double3 xformOp:translate = (1, 0, 0)\n"
                           "    uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                           "}\n";

PXR_NS::SdfLayerRefPtr
sceneLayer()
{
    PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::CreateAnonymous(".usda");
    EXPECT_TRUE(layer->ImportFromString(kScene));

    return layer;
}

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

void
addLine(GeoDetailBuilder& b)
{
    const GeoOffset first = b.appendPoints(3);
    for (int i = 0; i < 3; ++i) {
        b.setPointPosition(first + i, double(i), 2., 0.);
    }
    const GeoOffset points[3] = { first, first + 1, first + 2 };
    b.appendPrimitive(eGeoPrimitivePolyline, points);
}

void
setPrimitiveStrings(GeoDetailBuilder& b,
                    const char* name,
                    const std::vector<std::string>& values)
{
    GeoAttributeDesc desc;
    desc.name = name;
    desc.storage = eGeoStorageString;
    GeoAttribute& attribute = b.addAttribute(eGeoAttribClassPrimitive, desc);
    for (std::size_t i = 0; i < values.size(); ++i) {
        attribute.setString(i, values[i]);
    }
}

// One quad whose `path` is @p path.
GeoDetailConstPtr
quadAt(const std::string& path)
{
    GeoDetailBuilder b;
    addQuad(b, 0.);
    setPrimitiveStrings(b, "path", { path });

    return std::make_shared<const GeoDetail>(b.finish());
}

GeoUsdConvertOptions
prefixed(const char* prefix)
{
    GeoUsdConvertOptions options;
    options.rootPath = PXR_NS::SdfPath(prefix);

    return options;
}

// Converts @p detail with @p options against @p input, captured at every path the detail names.
std::unique_ptr<const GeoUsdConvert>
convertOver(const GeoDetailConstPtr& detail,
            GeoUsdConvertOptions options,
            const PXR_NS::SdfLayerRefPtr& input)
{
    const PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(input);
    const GeoUsdDetailPaths paths = geoUsdDetailPaths(*detail, options);
    options.inputScene = geoUsdCaptureInputScene(stage, PXR_NS::UsdTimeCode::Default(), paths.paths, paths.sourcePaths);

    return std::make_unique<const GeoUsdConvert>(detail, options);
}

PXR_NS::SdfLayerRefPtr
authored(const GeoUsdConvert& convert)
{
    PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::CreateAnonymous(".usda");
    std::string error;
    EXPECT_TRUE(convert.author(layer, PXR_NS::UsdTimeCode::Default(), &error)) << error;

    return layer;
}

// @p geometry composed over @p input, as GeoToUSD sublayers it.
PXR_NS::UsdStageRefPtr
composed(const PXR_NS::SdfLayerRefPtr& geometry,
         const PXR_NS::SdfLayerRefPtr& input)
{
    PXR_NS::SdfLayerRefPtr root = PXR_NS::SdfLayer::CreateAnonymous(".usda");
    root->InsertSubLayerPath(input->GetIdentifier());
    root->InsertSubLayerPath(geometry->GetIdentifier(), 0);

    return PXR_NS::UsdStage::Open(root);
}

std::vector<std::string>
paths(const GeoUsdConvert& convert)
{
    std::vector<std::string> out;
    for (std::size_t i = 0; i < convert.partitionCount(); ++i) {
        out.push_back(convert.partitionPath(i).GetString());
    }

    return out;
}

// The prim's points in world space.
std::vector<PXR_NS::GfVec3d>
worldPoints(const PXR_NS::UsdStageRefPtr& stage,
            const char* path)
{
    std::vector<PXR_NS::GfVec3d> out;
    const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(PXR_NS::SdfPath(path));
    PXR_NS::VtVec3fArray points;
    EXPECT_TRUE(PXR_NS::UsdGeomPointBased(prim).GetPointsAttr().Get(&points)) << path;
    PXR_NS::UsdGeomXformCache cache(PXR_NS::UsdTimeCode::Default());
    const PXR_NS::GfMatrix4d world = cache.GetLocalToWorldTransform(prim);
    for (const PXR_NS::GfVec3f& point : points) {
        out.push_back(world.Transform(PXR_NS::GfVec3d(point)));
    }

    return out;
}

} // anonymous namespace

TEST(GeoToUSDPaths, AnAbsolutePathKeepsItsPlaceUnlessPrefixed)
{
    GeoUsdConvertOptions options = prefixed("/pre");
    EXPECT_EQ((std::vector<std::string> { "/a/b" }), paths(GeoUsdConvert(quadAt("/a/b"), options)));
    options.prefixAbsolutePaths = true;
    EXPECT_EQ((std::vector<std::string> { "/pre/a/b" }), paths(GeoUsdConvert(quadAt("/a/b"), options)));
}

TEST(GeoToUSDPaths, ANameGoesUnderThePrefix)
{
    GeoDetailBuilder b;
    addQuad(b, 0.);
    setPrimitiveStrings(b, "name", { "leg" });
    const GeoUsdConvert convert(std::make_shared<const GeoDetail>(b.finish()), prefixed("/pre"));
    EXPECT_EQ((std::vector<std::string> { "/pre/leg" }), paths(convert));
}

TEST(GeoToUSDPaths, TheFirstNonEmptyPathAttributeWins)
{
    GeoDetailBuilder b;
    addQuad(b, 0.);
    addQuad(b, 2.);
    setPrimitiveStrings(b, "path", { "", "p" });
    setPrimitiveStrings(b, "name", { "n", "ignored" });
    const GeoUsdConvert convert(std::make_shared<const GeoDetail>(b.finish()), prefixed("/pre"));
    const std::vector<std::string> got = paths(convert);
    EXPECT_EQ((std::set<std::string> { "/pre/n", "/pre/p" }), std::set<std::string>(got.begin(), got.end()));
}

TEST(GeoToUSDPaths, InvalidCharactersBecomeUnderscores)
{
    const GeoUsdConvert convert(quadAt("my part/left-leg"), prefixed("/pre"));
    EXPECT_EQ((std::vector<std::string> { "/pre/my_part/left_leg" }), paths(convert));
}

TEST(GeoToUSDPaths, IncompatibleTypesAtOnePathBecomeSuffixedSiblings)
{
    GeoDetailBuilder b;
    addQuad(b, 0.);
    addLine(b);
    setPrimitiveStrings(b, "path", { "/a/b", "/a/b" });
    const GeoUsdConvert convert(std::make_shared<const GeoDetail>(b.finish()), prefixed("/pre"));
    const std::vector<std::string> got = paths(convert);
    ASSERT_EQ(std::size_t(2), got.size());
    EXPECT_EQ((std::set<std::string> { "/a/b_0", "/a/b_1" }), std::set<std::string>(got.begin(), got.end()));

    const PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(authored(convert));
    ASSERT_TRUE(stage);
    std::set<std::string> types;
    for (const char* path : { "/a/b_0", "/a/b_1" }) {
        types.insert(stage->GetPrimAtPath(PXR_NS::SdfPath(path)).GetTypeName().GetString());
    }
    EXPECT_EQ((std::set<std::string> { "Mesh", "BasisCurves" }), types);
    EXPECT_FALSE(stage->GetPrimAtPath(PXR_NS::SdfPath("/a/b")).GetTypeName() == PXR_NS::TfToken("Mesh"));
}

TEST(GeoToUSDPaths, TwoPartitionsOfOneTypeAtOnePathMerge)
{
    GeoDetailBuilder b;
    addQuad(b, 0.);
    addQuad(b, 2.);
    setPrimitiveStrings(b, "path", { "/a/b", "/a/b" });
    const GeoUsdConvert convert(std::make_shared<const GeoDetail>(b.finish()), prefixed("/pre"));
    EXPECT_EQ((std::vector<std::string> { "/a/b" }), paths(convert));
}

TEST(GeoToUSDPaths, GeometryWithNoPathAttributeIsNamedByType)
{
    GeoDetailBuilder b;
    addQuad(b, 0.);
    const GeoUsdConvert convert(std::make_shared<const GeoDetail>(b.finish()), prefixed("/pre"));
    EXPECT_EQ((std::vector<std::string> { "/pre/mesh_0" }), paths(convert));
}

TEST(GeoToUSDPaths, DefineAuthorsADefOnAPrimTheInputHolds)
{
    GeoUsdConvertOptions options = prefixed("/pre");
    options.otherPrimitives = eGeoUsdOtherDefine;
    const std::unique_ptr<const GeoUsdConvert> convert = convertOver(quadAt("/a/m"), options, sceneLayer());
    ASSERT_EQ(std::size_t(1), convert->partitionCount());
    EXPECT_FALSE(convert->isPartitionOverlay(0));
    const PXR_NS::SdfPrimSpecHandle spec = authored(*convert)->GetPrimAtPath(PXR_NS::SdfPath("/a/m"));
    ASSERT_TRUE(spec);
    EXPECT_EQ(PXR_NS::SdfSpecifierDef, spec->GetSpecifier());
    EXPECT_TRUE(spec->GetAttributes().get(PXR_NS::UsdGeomTokens->points));
}

TEST(GeoToUSDPaths, OverlayAuthorsAnOverWithTheData)
{
    GeoUsdConvertOptions options = prefixed("/pre");
    options.otherPrimitives = eGeoUsdOtherOverlay;
    const std::unique_ptr<const GeoUsdConvert> convert = convertOver(quadAt("/a/m"), options, sceneLayer());
    ASSERT_EQ(std::size_t(1), convert->partitionCount());
    EXPECT_TRUE(convert->isPartitionOverlay(0));
    EXPECT_TRUE(convert->overlaysWithoutPrim().empty());
    const PXR_NS::SdfLayerRefPtr layer = authored(*convert);
    const PXR_NS::SdfPrimSpecHandle spec = layer->GetPrimAtPath(PXR_NS::SdfPath("/a/m"));
    ASSERT_TRUE(spec);
    EXPECT_EQ(PXR_NS::SdfSpecifierOver, spec->GetSpecifier());
    EXPECT_TRUE(spec->GetAttributes().get(PXR_NS::UsdGeomTokens->points));
    const PXR_NS::SdfPrimSpecHandle parent = layer->GetPrimAtPath(PXR_NS::SdfPath("/a"));
    ASSERT_TRUE(parent);
    EXPECT_EQ(PXR_NS::SdfSpecifierOver, parent->GetSpecifier());
}

TEST(GeoToUSDPaths, OverlayTransformsAuthorsAnOverWithOnlyTheTransform)
{
    GeoUsdConvertOptions options = prefixed("/pre");
    options.otherPrimitives = eGeoUsdOtherOverlayTransforms;
    const std::unique_ptr<const GeoUsdConvert> convert = convertOver(quadAt("/a/m"), options, sceneLayer());
    ASSERT_EQ(std::size_t(1), convert->partitionCount());
    const PXR_NS::SdfPrimSpecHandle spec = authored(*convert)->GetPrimAtPath(PXR_NS::SdfPath("/a/m"));
    ASSERT_TRUE(spec);
    EXPECT_EQ(PXR_NS::SdfSpecifierOver, spec->GetSpecifier());
    std::set<std::string> names;
    for (const PXR_NS::SdfAttributeSpecHandle& attribute : spec->GetAttributes()) {
        names.insert(attribute->GetName());
    }
    // xformOpOrder is what makes the op count.
    EXPECT_EQ((std::set<std::string> { "xformOp:transform", "xformOpOrder" }), names);
}

TEST(GeoToUSDPaths, OverlayOnAPrimTheInputLacksIsListed)
{
    GeoUsdConvertOptions options = prefixed("/pre");
    options.otherPrimitives = eGeoUsdOtherOverlay;
    const std::unique_ptr<const GeoUsdConvert> convert = convertOver(quadAt("/a/missing"), options, sceneLayer());
    ASSERT_EQ(std::size_t(1), convert->partitionCount());
    EXPECT_EQ(PXR_NS::SdfPath("/a/missing"), convert->partitionPath(0));
    EXPECT_EQ((std::vector<PXR_NS::SdfPath> { PXR_NS::SdfPath("/a/missing") }), convert->overlaysWithoutPrim());
}

TEST(GeoToUSDPaths, DefineOnlyLeafPrimitivesAuthorsIntermediatesAsOvers)
{
    GeoUsdConvertOptions options = prefixed("/pre");
    options.defineOnlyLeafPrimitives = true;
    const PXR_NS::SdfLayerRefPtr layer = authored(GeoUsdConvert(quadAt("/x/y/z"), options));
    for (const char* path : { "/x", "/x/y" }) {
        const PXR_NS::SdfPrimSpecHandle spec = layer->GetPrimAtPath(PXR_NS::SdfPath(path));
        ASSERT_TRUE(spec) << path;
        EXPECT_EQ(PXR_NS::SdfSpecifierOver, spec->GetSpecifier()) << path;
    }
    EXPECT_EQ(PXR_NS::SdfSpecifierDef, layer->GetPrimAtPath(PXR_NS::SdfPath("/x/y/z"))->GetSpecifier());
}

TEST(GeoToUSDPaths, IntermediatePrimsTakeTheParentPrimitiveType)
{
    GeoUsdConvertOptions options = prefixed("/pre");
    options.parentPrimitiveType = PXR_NS::TfToken("Scope");
    const PXR_NS::SdfLayerRefPtr layer = authored(GeoUsdConvert(quadAt("/x/y"), options));
    const PXR_NS::SdfPrimSpecHandle spec = layer->GetPrimAtPath(PXR_NS::SdfPath("/x"));
    ASSERT_TRUE(spec);
    EXPECT_EQ(PXR_NS::SdfSpecifierDef, spec->GetSpecifier());
    EXPECT_EQ(std::string("Scope"), spec->GetTypeName().GetString());
}

TEST(GeoToUSDPaths, OneComponentKindAuthoringMarksThePrefix)
{
    GeoDetailBuilder b;
    addQuad(b, 0.);
    GeoUsdConvertOptions options = prefixed("/pre");
    options.kindAuthoring = eGeoUsdKindOneComponent;
    const PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(authored(GeoUsdConvert(std::make_shared<const GeoDetail>(b.finish()), options)));
    ASSERT_TRUE(stage);
    PXR_NS::TfToken kind;
    EXPECT_TRUE(PXR_NS::UsdModelAPI(stage->GetPrimAtPath(PXR_NS::SdfPath("/pre"))).GetKind(&kind));
    EXPECT_EQ(PXR_NS::KindTokens->component, kind);
}

// World-space geometry (as USDToGeo writes with flattened transforms) under /T, translated by
// (1, 0, 0) in the input.
GeoDetailConstPtr
worldSpaceQuadUnderT()
{
    GeoDetailBuilder b;
    addQuad(b, 0.);
    setPrimitiveStrings(b, "path", { "/T/m" });
    GeoAttributeDesc desc;
    desc.name = kGeoUsdWorldSpaceAttr;
    desc.storage = eGeoStorageInt32;
    b.addAttribute(eGeoAttribClassDetail, desc).set<std::int32_t>(0, 1);

    return std::make_shared<const GeoDetail>(b.finish());
}

TEST(GeoToUSDPaths, AdjustingForTheInputHierarchyKeepsTheWorldPosition)
{
    const PXR_NS::SdfLayerRefPtr input = sceneLayer();
    GeoUsdConvertOptions options = prefixed("/pre");
    options.adjustTransformsForInputHierarchy = true;
    const std::unique_ptr<const GeoUsdConvert> convert = convertOver(worldSpaceQuadUnderT(), options, input);
    const PXR_NS::SdfLayerRefPtr geometry = authored(*convert);
    const PXR_NS::UsdStageRefPtr stage = composed(geometry, input);
    ASSERT_TRUE(stage);
    const std::vector<PXR_NS::GfVec3d> world = worldPoints(stage, "/T/m");
    ASSERT_EQ(std::size_t(4), world.size());
    EXPECT_TRUE(PXR_NS::GfIsClose(PXR_NS::GfVec3d(0., 0., 0.), world[0], 1e-6)) << world[0];
    EXPECT_TRUE(PXR_NS::GfIsClose(PXR_NS::GfVec3d(1., 1., 0.), world[2], 1e-6)) << world[2];
}

TEST(GeoToUSDPaths, WithoutTheAdjustmentTheInputHierarchyOffsetsTheGeometry)
{
    const PXR_NS::SdfLayerRefPtr input = sceneLayer();
    GeoUsdConvertOptions options = prefixed("/pre");
    options.adjustTransformsForInputHierarchy = false;
    const std::unique_ptr<const GeoUsdConvert> convert = convertOver(worldSpaceQuadUnderT(), options, input);
    const PXR_NS::SdfLayerRefPtr geometry = authored(*convert);
    const PXR_NS::UsdStageRefPtr stage = composed(geometry, input);
    ASSERT_TRUE(stage);
    const std::vector<PXR_NS::GfVec3d> world = worldPoints(stage, "/T/m");
    ASSERT_EQ(std::size_t(4), world.size());
    EXPECT_TRUE(PXR_NS::GfIsClose(PXR_NS::GfVec3d(1., 0., 0.), world[0], 1e-6)) << world[0];
    EXPECT_TRUE(PXR_NS::GfIsClose(PXR_NS::GfVec3d(2., 1., 0.), world[2], 1e-6)) << world[2];
}

TEST(GeoToUSDPaths, CreateXformsGivesEachPackedPrimitiveAnXform)
{
    GeoDetailBuilder inner;
    addQuad(inner, 0.);
    const GeoDetailConstPtr card = std::make_shared<const GeoDetail>(inner.finish());
    GeoDetailBuilder b;
    for (int i = 0; i < 2; ++i) {
        PXR_NS::GfMatrix4d m(1.);
        m.SetTranslate(PXR_NS::GfVec3d(3. * i, 0., 0.));
        const GeoOffset pivot = b.appendPoints(1);
        b.setPointPosition(pivot, 3. * i, 0., 0.);
        geoAppendPackedGeometry(b, pivot, card, geoMatrixFromUsd(m));
    }
    GeoUsdConvertOptions options = prefixed("/pre");
    options.packedMode = eGeoUsdPackedCreateXforms;
    const GeoUsdConvert convert(std::make_shared<const GeoDetail>(b.finish()), options);
    ASSERT_EQ(std::size_t(2), convert.partitionCount());
    const PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(authored(convert));
    ASSERT_TRUE(stage);
    for (std::size_t i = 0; i < 2; ++i) {
        const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(convert.partitionPath(i));
        EXPECT_EQ(std::string("Xform"), prim.GetTypeName().GetString()) << convert.partitionPath(i);
        bool hasMesh = false;
        for (const PXR_NS::UsdPrim& child : prim.GetDescendants()) {
            hasMesh = hasMesh || static_cast<bool>(PXR_NS::UsdGeomMesh(child));
        }
        EXPECT_TRUE(hasMesh) << convert.partitionPath(i);
    }
}

class GeoToUSDPathsNodeTest
    : public BaseTest {
protected:
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

    // A grid into a GeoToUSD with Path Prefix @p prefix.
    void buildGrid(const char* prefix)
    {
        _grid = make(PLUGINID_NATRON_GEOPRIMITIVE);
        _toUSD = make(PLUGINID_NATRON_GEOTOUSD);
        ASSERT_TRUE(_grid && _toUSD);
        KnobChoice* type = knob<KnobChoice>(_grid, kGeoPrimitiveParamType);
        ASSERT_TRUE(type != NULL);
        type->setValueFromID("grid", 0);
        KnobString* path = knob<KnobString>(_toUSD, kGeoToUSDParamPathPrefix);
        ASSERT_TRUE(path != NULL);
        path->setValue(std::string(prefix));
        connectNodes(_grid, _toUSD, kGeoToUSDInputGeometry, true);
    }

    NodePtr reader()
    {
        const std::string path = _dir.path().toStdString() + "/paths.usda";
        {
            QFile f(QString::fromStdString(path));
            EXPECT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
            f.write(kScene);
        }
        const NodePtr node = make(PLUGINID_NATRON_USDREAD);
        if (node) {
            KnobFile* file = knob<KnobFile>(node, kUSDReadParamFile);
            if (file) {
                file->setValue(path);
            }
        }

        return node;
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

    QString message(int* type) const
    {
        QString text;
        _toUSD->getPersistentMessage(&text, type, false);

        return text;
    }

    NodePtr _grid;
    NodePtr _toUSD;

private:
    QTemporaryDir _dir;
    std::vector<NodePtr> _nodes;
};

TEST_F(GeoToUSDPathsNodeTest, ThePathPrefixDefaultsToTheNodeName)
{
    _toUSD = make(PLUGINID_NATRON_GEOTOUSD);
    ASSERT_TRUE(_toUSD != nullptr);
    KnobString* path = knob<KnobString>(_toUSD, kGeoToUSDParamPathPrefix);
    ASSERT_TRUE(path != NULL);
    EXPECT_EQ("/" + _toUSD->getScriptName_mt_safe(), path->getValue());
}

TEST_F(GeoToUSDPathsNodeTest, TheMessageListsEachPrimAndWhetherItWasDefined)
{
    buildGrid("/pre");
    EXPECT_FALSE(evaluate(_toUSD).isEmpty());
    int type = 0;
    const QString text = message(&type);
    EXPECT_EQ((int)eMessageTypeInfo, type) << text.toStdString();
    EXPECT_TRUE(text.contains(QString::fromUtf8("/pre/mesh_0"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QString::fromUtf8("defined"))) << text.toStdString();
}

TEST_F(GeoToUSDPathsNodeTest, OverlayOnAPrunedPrimWarnsAndListsIt)
{
    const NodePtr scene = reader();
    const NodePtr toGeo = make(PLUGINID_NATRON_USDTOGEO);
    const NodePtr prune = make(PLUGINID_NATRON_USDPRUNE);
    _toUSD = make(PLUGINID_NATRON_GEOTOUSD);
    ASSERT_TRUE(scene && toGeo && prune && _toUSD);
    knob<KnobString>(toGeo, kUSDToGeoParamPrimPattern)->setValue(std::string("/a/m"));
    knob<KnobBool>(toGeo, kUSDToGeoParamUnpack)->setValue(true);
    knob<KnobString>(prune, kUSDPruneParamPrimPattern)->setValue(std::string("/a/m"));
    knob<KnobChoice>(_toUSD, kGeoToUSDParamOtherPrimitives)->setValueFromID("overlay", 0);
    connectNodes(scene, toGeo, 0, true);
    connectNodes(scene, prune, 0, true);
    connectNodes(toGeo, _toUSD, kGeoToUSDInputGeometry, true);
    connectNodes(prune, _toUSD, kGeoToUSDInputScene, true);

    EXPECT_FALSE(evaluate(_toUSD).isEmpty());
    int type = 0;
    const QString text = message(&type);
    EXPECT_EQ((int)eMessageTypeWarning, type) << text.toStdString();
    EXPECT_TRUE(text.contains(QString::fromUtf8("/a/m  overlaid"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QString::fromUtf8("not in the Scene input"))) << text.toStdString();
}

TEST_F(GeoToUSDPathsNodeTest, LoadAsReferencePayloadsTheGeometryLayerOntoThePrefix)
{
    buildGrid("/ref");
    knob<KnobBool>(_toUSD, kGeoToUSDParamLoadAsReference)->setValue(true);

    const ScenePayload payload = evaluate(_toUSD);
    const std::shared_ptr<const ScenePayloadImpl>& imp = payload.getImpl();
    ASSERT_TRUE(imp && imp->top.layer && imp->top.dependencies && !imp->top.dependencies->layers.empty());
    const PXR_NS::SdfLayerRefPtr geometry = imp->top.dependencies->layers.front();
    // Referenced, not sublayered.
    EXPECT_TRUE(imp->top.layer->GetSubLayerPaths().empty());
    const PXR_NS::SdfPrimSpecHandle prim = imp->top.layer->GetPrimAtPath(PXR_NS::SdfPath("/ref"));
    ASSERT_TRUE(prim);
    EXPECT_EQ(PXR_NS::SdfSpecifierDef, prim->GetSpecifier());
    const PXR_NS::SdfPayloadsProxy::ListProxy appended = prim->GetPayloadList().GetAppendedItems();
    ASSERT_EQ(std::size_t(1), appended.size());
    EXPECT_EQ(geometry->GetIdentifier(), PXR_NS::SdfPayload(appended[0]).GetAssetPath());
    EXPECT_TRUE(prim->GetReferenceList().GetAppendedItems().empty());

    SceneQueryPool::Lease lease;
    const PXR_NS::UsdStageRefPtr stage = stageOf(payload, &lease);
    ASSERT_TRUE(stage);
    EXPECT_TRUE(static_cast<bool>(PXR_NS::UsdGeomMesh(stage->GetPrimAtPath(PXR_NS::SdfPath("/ref/mesh_0")))));
}

TEST_F(GeoToUSDPathsNodeTest, AStrongerReferenceIsPrependedAndCreatesTypedParents)
{
    buildGrid("/set/ref");
    knob<KnobBool>(_toUSD, kGeoToUSDParamLoadAsReference)->setValue(true);
    knob<KnobChoice>(_toUSD, kGeoToUSDParamReferenceType)->setValueFromID("strongerReference", 0);
    knob<KnobChoice>(_toUSD, kGeoToUSDParamParentPrimitiveKind)->setValueFromID("group", 0);

    const ScenePayload payload = evaluate(_toUSD);
    const std::shared_ptr<const ScenePayloadImpl>& imp = payload.getImpl();
    ASSERT_TRUE(imp && imp->top.layer);
    const PXR_NS::SdfPrimSpecHandle prim = imp->top.layer->GetPrimAtPath(PXR_NS::SdfPath("/set/ref"));
    ASSERT_TRUE(prim);
    EXPECT_EQ(std::size_t(1), prim->GetReferenceList().GetPrependedItems().size());
    EXPECT_TRUE(prim->GetPayloadList().GetAppendedItems().empty());
    const PXR_NS::SdfPrimSpecHandle parent = imp->top.layer->GetPrimAtPath(PXR_NS::SdfPath("/set"));
    ASSERT_TRUE(parent);
    EXPECT_EQ(std::string("Xform"), parent->GetTypeName().GetString());
    EXPECT_EQ(PXR_NS::KindTokens->group, parent->GetKind());

    SceneQueryPool::Lease lease;
    const PXR_NS::UsdStageRefPtr stage = stageOf(payload, &lease);
    ASSERT_TRUE(stage);
    EXPECT_TRUE(static_cast<bool>(PXR_NS::UsdGeomMesh(stage->GetPrimAtPath(PXR_NS::SdfPath("/set/ref/mesh_0")))));
}

TEST_F(GeoToUSDPathsNodeTest, ASpecificReferencePrimitiveMustBeAPrimPath)
{
    buildGrid("/ref");
    knob<KnobBool>(_toUSD, kGeoToUSDParamLoadAsReference)->setValue(true);
    knob<KnobChoice>(_toUSD, kGeoToUSDParamReferencePrimitive)->setValueFromID("specific", 0);
    knob<KnobString>(_toUSD, kGeoToUSDParamReferencePrimitivePath)->setValue(std::string("not a path"));

    ScenePayload payload;
    std::string error;
    EXPECT_NE(EffectInstance::eRenderRoIRetCodeOk,
              _toUSD->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &payload, &error));
}
