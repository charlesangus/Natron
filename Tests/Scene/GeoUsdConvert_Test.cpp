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

#include "Global/Macros.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Geo/GeoPacked.h"
#include "Engine/Nodes/Geometry/GeoUsdConvert.h"

#include <pxr/usd/usdGeom/basisCurves.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usdGeom/points.h>
#include <pxr/usd/usdGeom/subset.h>

PXR_NAMESPACE_USING_DIRECTIVE
NATRON_NAMESPACE_USING

namespace {

const GeoOffset kQuad[] = { 0, 1, 2, 3 };

void
addQuad(GeoDetailBuilder& b,
        double x)
{
    const GeoOffset first = b.appendPoints(4);
    b.setPointPosition(first, x, 0., 0.);
    b.setPointPosition(first + 1, x + 1., 0., 0.);
    b.setPointPosition(first + 2, x + 1., 1., 0.);
    b.setPointPosition(first + 3, x, 1., 0.);
    GeoOffset corners[4];
    for (int i = 0; i < 4; ++i) {
        corners[i] = first + kQuad[i];
    }
    b.appendPrimitive(eGeoPrimitivePolygon, corners);
}

// Two quads at /geo/a and /geo/b with a point colour; the first quad is in the group "sel".
GeoDetailConstPtr
makeTwoPartitions()
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
    GeoAttributeDesc cdDesc;
    cdDesc.name = "Cd";
    cdDesc.storage = eGeoStorageFloat32;
    cdDesc.tupleSize = 3;
    cdDesc.typeInfo = eGeoTypeInfoColour;
    GeoAttribute& cd = b.addAttribute(eGeoAttribClassPoint, cdDesc);
    for (std::size_t p = 0; p < 8; ++p) {
        const float c[3] = { float(p) * 0.125f, 0.5f, 1.f - float(p) * 0.125f };
        cd.setTuple<float>(p, std::span<const float>(c, 3));
    }
    b.addGroup(eGeoAttribClassPrimitive, "sel").add(GeoOffset(0));

    return std::make_shared<const GeoDetail>(b.finish());
}

SdfLayerRefPtr
authored(const GeoUsdConvert& convert)
{
    SdfLayerRefPtr layer = SdfLayer::CreateAnonymous("geo.usda");
    std::string err;
    EXPECT_TRUE(convert.author(layer, UsdTimeCode::Default(), &err)) << err;

    return layer;
}

// The quad at the origin, packed once per matrix (USD row-vector convention).
GeoDetailConstPtr
makePacked(const std::vector<GfMatrix4d>& transforms)
{
    GeoDetailBuilder inner;
    addQuad(inner, 0.);
    const GeoDetailConstPtr contained = std::make_shared<const GeoDetail>(inner.finish());
    GeoDetailBuilder b;
    for (const GfMatrix4d& m : transforms) {
        const GeoOffset pivot = b.appendPoints(1);
        const GfVec3d t = m.ExtractTranslation();
        b.setPointPosition(pivot, t[0], t[1], t[2]);
        geoAppendPackedGeometry(b, pivot, contained, geoMatrixFromUsd(m));
    }

    return std::make_shared<const GeoDetail>(b.finish());
}

void
expectMatrixNear(const GfMatrix4d& expected,
                 const GfMatrix4d& actual,
                 double tolerance)
{
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            EXPECT_NEAR(expected[r][c], actual[r][c], tolerance) << "row " << r << " column " << c;
        }
    }
}

} // namespace

TEST(GeoUsdConvert, PathPartitionsBecomeMeshes)
{
    const GeoDetailConstPtr detail = makeTwoPartitions();
    const GeoUsdConvert convert(detail, GeoUsdConvertOptions());
    ASSERT_EQ(convert.partitionCount(), 2u);
    const SdfLayerRefPtr layer = authored(convert);
    const UsdStageRefPtr stage = UsdStage::Open(layer);
    ASSERT_TRUE(static_cast<bool>(stage));

    for (const char* name : { "/geo/a", "/geo/b" }) {
        const UsdGeomMesh mesh(stage->GetPrimAtPath(SdfPath(name)));
        ASSERT_TRUE(static_cast<bool>(mesh)) << name;
        VtVec3fArray points;
        ASSERT_TRUE(mesh.GetPointsAttr().Get(&points));
        ASSERT_EQ(points.size(), 4u);
        const float x = std::string(name) == "/geo/a" ? 0.f : 5.f;
        EXPECT_EQ(points[0], GfVec3f(x, 0.f, 0.f));
        EXPECT_EQ(points[2], GfVec3f(x + 1.f, 1.f, 0.f));
        VtIntArray counts;
        VtIntArray indices;
        mesh.GetFaceVertexCountsAttr().Get(&counts);
        mesh.GetFaceVertexIndicesAttr().Get(&indices);
        EXPECT_EQ(counts, VtIntArray({ 4 }));
        EXPECT_EQ(indices, VtIntArray({ 0, 1, 2, 3 }));

        const UsdGeomPrimvar colour = UsdGeomPrimvarsAPI(mesh.GetPrim()).GetPrimvar(TfToken("displayColor"));
        ASSERT_TRUE(colour.HasAuthoredValue()) << name;
        EXPECT_EQ(colour.GetInterpolation(), UsdGeomTokens->vertex);
        VtVec3fArray colours;
        colour.Get(&colours);
        EXPECT_EQ(colours.size(), 4u);
    }

    const UsdGeomSubset subset(stage->GetPrimAtPath(SdfPath("/geo/a/sel")));
    ASSERT_TRUE(static_cast<bool>(subset));
    VtIntArray members;
    subset.GetIndicesAttr().Get(&members);
    EXPECT_EQ(members, VtIntArray({ 0 }));
    TfToken elementType;
    subset.GetElementTypeAttr().Get(&elementType);
    EXPECT_EQ(elementType, UsdGeomTokens->face);
    EXPECT_FALSE(static_cast<bool>(stage->GetPrimAtPath(SdfPath("/geo/b/sel"))));
}

TEST(GeoUsdConvert, RoundTripKeepsContentHash)
{
    const GeoDetailConstPtr detail = makeTwoPartitions();
    const GeoUsdConvert convert(detail, GeoUsdConvertOptions());
    const SdfLayerRefPtr layer = authored(convert);
    const UsdStageRefPtr stage = UsdStage::Open(layer);
    ASSERT_TRUE(static_cast<bool>(stage));

    GeoDetailBuilder b;
    std::vector<std::string> warnings;
    for (const char* name : { "/geo/a", "/geo/b" }) {
        std::string err;
        ASSERT_TRUE(geoUsdReadPrim(stage->GetPrimAtPath(SdfPath(name)), UsdTimeCode::Default(), nullptr, b, &warnings, &err)) << err;
    }
    const GeoDetail back = b.finish();
    EXPECT_TRUE(warnings.empty()) << warnings.front();
    EXPECT_EQ(back.primitiveCount(), detail->primitiveCount());
    EXPECT_EQ(back.pointCount(), detail->pointCount());
    EXPECT_EQ(back.contentHash(), detail->contentHash());
}

TEST(GeoUsdConvert, FieldQueryEqualsAuthoredLayer)
{
    const GeoUsdConvert convert(makeTwoPartitions(), GeoUsdConvertOptions());
    const SdfLayerRefPtr layer = authored(convert);
    std::size_t specs = 0;
    convert.visitSpecs([&](const SdfPath& path) {
        if (path == SdfPath::AbsoluteRootPath()) {
            return true;
        }
        ++specs;
        EXPECT_EQ(layer->GetSpecType(path), convert.specType(path)) << path.GetString();
        std::vector<TfToken> ours = convert.fieldNames(path);
        std::vector<TfToken> theirs = layer->ListFields(path);
        std::sort(ours.begin(), ours.end());
        std::sort(theirs.begin(), theirs.end());
        EXPECT_EQ(ours, theirs) << path.GetString();
        for (const TfToken& name : ours) {
            VtValue value;
            EXPECT_TRUE(convert.field(path, name, &value));
            EXPECT_EQ(value, layer->GetField(path, name)) << path.GetString() << "." << name.GetString();
        }

        return true;
    });
    EXPECT_GT(specs, 10u);
}

TEST(GeoUsdConvert, NoPathFallsBackToPerTypePrims)
{
    GeoDetailBuilder b;
    addQuad(b, 0.);
    const GeoOffset line = b.appendPoints(3);
    for (int i = 0; i < 3; ++i) {
        b.setPointPosition(line + i, double(i), 2., 0.);
    }
    const GeoOffset linePoints[] = { line, line + 1, line + 2 };
    b.appendPrimitive(eGeoPrimitivePolyline, linePoints);
    const GeoOffset loose = b.appendPoints(2);
    b.setPointPosition(loose, 0., 0., 4.);
    b.setPointPosition(loose + 1, 1., 0., 4.);
    const GeoUsdConvert convert(std::make_shared<const GeoDetail>(b.finish()), GeoUsdConvertOptions());

    const UsdStageRefPtr stage = UsdStage::Open(authored(convert));
    ASSERT_TRUE(static_cast<bool>(stage));
    EXPECT_TRUE(static_cast<bool>(UsdGeomMesh(stage->GetPrimAtPath(SdfPath("/geo/mesh_0")))));
    const UsdGeomBasisCurves curves(stage->GetPrimAtPath(SdfPath("/geo/curves_0")));
    ASSERT_TRUE(static_cast<bool>(curves));
    VtIntArray counts;
    curves.GetCurveVertexCountsAttr().Get(&counts);
    EXPECT_EQ(counts, VtIntArray({ 3 }));
    const UsdGeomPoints points(stage->GetPrimAtPath(SdfPath("/geo/points_0")));
    ASSERT_TRUE(static_cast<bool>(points));
    VtVec3fArray positions;
    points.GetPointsAttr().Get(&positions);
    EXPECT_EQ(positions.size(), 2u);
}

TEST(GeoUsdConvert, AReflectedInstanceKeepsItsHandedness)
{
    GfMatrix4d mirrored(1.);
    mirrored.SetScale(GfVec3d(-1., 2., 1.));
    mirrored.SetTranslateOnly(GfVec3d(3., 0., 0.));
    GfMatrix4d mirroredRotated = GfMatrix4d(1.).SetScale(GfVec3d(1., 1., -0.5)) * GfMatrix4d(1.).SetRotate(GfRotation(GfVec3d(0., 1., 0.), 30.));
    mirroredRotated.SetTranslateOnly(GfVec3d(0., 4., 0.));
    const std::vector<GfMatrix4d> transforms { mirrored, mirroredRotated };
    const GeoUsdConvert convert(makePacked(transforms), GeoUsdConvertOptions());
    EXPECT_TRUE(convert.shearedInstances().empty());

    const UsdStageRefPtr stage = UsdStage::Open(authored(convert));
    ASSERT_TRUE(static_cast<bool>(stage));
    const UsdGeomPointInstancer instancer(stage->GetPrimAtPath(SdfPath("/geo/instancer_0")));
    ASSERT_TRUE(static_cast<bool>(instancer));
    VtArray<GfMatrix4d> computed;
    ASSERT_TRUE(instancer.ComputeInstanceTransformsAtTime(&computed, UsdTimeCode::Default(), UsdTimeCode::Default()));
    ASSERT_EQ(transforms.size(), computed.size());
    for (std::size_t i = 0; i < transforms.size(); ++i) {
        SCOPED_TRACE(i);
        EXPECT_LT(computed[i].GetDeterminant3(), 0.);
        // Orientations are half-precision quaternions.
        expectMatrixNear(transforms[i], computed[i], 2e-3);
    }
}

TEST(GeoUsdConvert, ShearedPackedGeometryBecomesNativeInstances)
{
    GfMatrix4d sheared(1.);
    sheared.SetRow(1, GfVec4d(0.5, 1., 0., 0.));
    sheared.SetTranslateOnly(GfVec3d(2., 0., 1.));
    const GeoUsdConvert convert(makePacked({ GfMatrix4d(1.), sheared }), GeoUsdConvertOptions());
    ASSERT_EQ(1u, convert.shearedInstances().size());
    EXPECT_EQ(SdfPath("/geo/instances_0"), convert.shearedInstances().front());

    const UsdStageRefPtr stage = UsdStage::Open(authored(convert));
    ASSERT_TRUE(static_cast<bool>(stage));
    EXPECT_FALSE(stage->GetPrimAtPath(SdfPath("/geo/instancer_0")).IsValid());
    const UsdPrim container = stage->GetPrimAtPath(SdfPath("/geo/instances_0"));
    ASSERT_TRUE(container.IsValid());
    std::vector<GfMatrix4d> worlds;
    for (const UsdPrim& child : container.GetChildren()) {
        if (child.IsInstance()) {
            worlds.push_back(UsdGeomXformable(child).ComputeLocalToWorldTransform(UsdTimeCode::Default()));
        }
    }
    ASSERT_EQ(2u, worlds.size());
    expectMatrixNear(GfMatrix4d(1.), worlds[0], 1e-9);
    expectMatrixNear(sheared, worlds[1], 1e-9);
}
