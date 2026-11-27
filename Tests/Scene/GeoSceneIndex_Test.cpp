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
#include <initializer_list>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Geometry/GeoPrimitive.h"
#include "Engine/Nodes/Geometry/GeoSceneIndex.h"
#include "Engine/Nodes/Geometry/GeoUsdConvert.h"

#include <pxr/imaging/hd/basisCurvesSchema.h>
#include <pxr/imaging/hd/basisCurvesTopologySchema.h>
#include <pxr/imaging/hd/extentSchema.h>
#include <pxr/imaging/hd/geomSubsetSchema.h>
#include <pxr/imaging/hd/meshSchema.h>
#include <pxr/imaging/hd/meshTopologySchema.h>
#include <pxr/imaging/hd/primOriginSchema.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/sceneIndexObserver.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/xformSchema.h>
#include <pxr/usd/usdGeom/tokens.h>

PXR_NAMESPACE_USING_DIRECTIVE
NATRON_NAMESPACE_USING

namespace {

GeoDetailConstPtr
finish(GeoDetailBuilder& b)
{
    return std::make_shared<const GeoDetail>(b.finish());
}

std::shared_ptr<GeoUsdConvert>
convertOf(const GeoDetailConstPtr& detail)
{
    return std::make_shared<GeoUsdConvert>(detail, GeoUsdConvertOptions());
}

GeoAttribute&
addPointColour(GeoDetailBuilder& b)
{
    GeoAttributeDesc desc;
    desc.name = "Cd";
    desc.storage = eGeoStorageFloat32;
    desc.tupleSize = 3;
    desc.typeInfo = eGeoTypeInfoColour;

    return b.addAttribute(eGeoAttribClassPoint, desc);
}

// A unit quad at (x, y), its corner colours derived from where they are, so a quad built in
// another order carries the same values.
void
addQuad(GeoDetailBuilder& b,
        double x,
        double y,
        GeoAttribute* colour)
{
    const GeoOffset first = b.appendPoints(4);
    const double corners[4][2] = { { x, y }, { x + 1., y }, { x + 1., y + 1. }, { x, y + 1. } };
    GeoOffset offsets[4];
    for (int i = 0; i < 4; ++i) {
        offsets[i] = first + i;
        b.setPointPosition(offsets[i], corners[i][0], corners[i][1], 0.);
        if (colour) {
            const float c[3] = { float(corners[i][0]) * 0.1f, float(corners[i][1]) * 0.1f, 0.5f };
            colour->setTuple<float>(std::size_t(offsets[i]), std::span<const float>(c, 3));
        }
    }
    b.appendPrimitive(eGeoPrimitivePolygon, offsets);
}

GeoDetailConstPtr
makeCube()
{
    GeoShapeParams params;
    params.shape = eGeoShapeCube;
    GeoDetailBuilder b;
    buildGeoShape(params, b);

    return finish(b);
}

// Loose points with a colour and a pscale of @p pscale, 2 * @p pscale, ...
GeoDetailConstPtr
makePoints(float pscale,
           double shift = 0.)
{
    GeoDetailBuilder b;
    const GeoOffset first = b.appendPoints(3);
    GeoAttribute& colour = addPointColour(b);
    GeoAttributeDesc desc;
    desc.name = "pscale";
    desc.storage = eGeoStorageFloat32;
    GeoAttribute& scale = b.addAttribute(eGeoAttribClassPoint, desc);
    for (int i = 0; i < 3; ++i) {
        b.setPointPosition(first + i, double(i) + shift, 0., 1.);
        const float c[3] = { 0.25f * float(i), 0.5f, 0.75f };
        colour.setTuple<float>(std::size_t(first + i), std::span<const float>(c, 3));
        scale.set<float>(std::size_t(first + i), pscale * float(i + 1));
    }

    return finish(b);
}

GeoDetailConstPtr
makeCurve()
{
    GeoDetailBuilder b;
    const GeoOffset first = b.appendPoints(3);
    for (int i = 0; i < 3; ++i) {
        b.setPointPosition(first + i, double(i), double(i * i), -1.);
    }
    const GeoOffset points[] = { first, first + 1, first + 2 };
    b.appendPrimitive(eGeoPrimitivePolyline, points);

    return finish(b);
}

// Quads at /geo/a (x = 0) and /geo/<bName> (x = 5), "sel" holding the /geo/a quad. @p bFirst
// builds the second quad first, so only the primitive order differs.
GeoDetailConstPtr
makeGrouped(const std::string& bName = "b",
            bool bFirst = false)
{
    GeoDetailBuilder b;
    GeoAttribute& colour = addPointColour(b);
    addQuad(b, bFirst ? 5. : 0., 0., &colour);
    addQuad(b, bFirst ? 0. : 5., 0., &colour);
    GeoAttributeDesc pathDesc;
    pathDesc.name = "path";
    pathDesc.storage = eGeoStorageString;
    GeoAttribute& path = b.addAttribute(eGeoAttribClassPrimitive, pathDesc);
    path.setString(bFirst ? 1 : 0, "/geo/a");
    path.setString(bFirst ? 0 : 1, "/geo/" + bName);
    b.addGroup(eGeoAttribClassPrimitive, "sel").add(GeoOffset(bFirst ? 1 : 0));

    return finish(b);
}

// One quad at (x, 0), plus a second one at (x, 3) when @p two.
GeoDetailConstPtr
makeQuads(double x,
          bool two = false,
          double insideMove = 0.)
{
    GeoDetailBuilder b;
    addQuad(b, x, 0., nullptr);
    if (insideMove != 0.) {
        b.setPointPosition(0, x + insideMove, insideMove, 0.);
    }
    if (two) {
        addQuad(b, x, 3., nullptr);
    }

    return finish(b);
}

GeoSceneIndexRefPtr
serve(const GeoDetailConstPtr& detail,
      const SdfPath& prefix = SdfPath::AbsoluteRootPath())
{
    GeoSceneIndexRefPtr index = GeoSceneIndex::New(prefix);
    index->update(convertOf(detail));

    return index;
}

template <typename T>
T
typed(const typename HdTypedSampledDataSource<T>::Handle& source)
{
    return source ? source->GetTypedValue(0.f) : T();
}

VtValue
primvarValue(const HdSceneIndexPrim& prim,
             const TfToken& name)
{
    const HdPrimvarSchema primvar = HdPrimvarsSchema::GetFromParent(prim.dataSource).GetPrimvar(name);
    const HdSampledDataSourceHandle value = primvar.GetPrimvarValue();

    return value ? value->GetValue(0.f) : VtValue();
}

TfToken
interpolationOf(const HdSceneIndexPrim& prim,
                const TfToken& name)
{
    return typed<TfToken>(HdPrimvarsSchema::GetFromParent(prim.dataSource).GetPrimvar(name).GetInterpolation());
}

TfToken
roleOf(const HdSceneIndexPrim& prim,
       const TfToken& name)
{
    return typed<TfToken>(HdPrimvarsSchema::GetFromParent(prim.dataSource).GetPrimvar(name).GetRole());
}

VtValue
convertValue(const GeoUsdConvert& convert,
             const SdfPath& attrPath)
{
    VtValue v;
    convert.field(attrPath, SdfFieldKeys->Default, &v);

    return v;
}

SdfPath
originOf(const HdSceneIndexPrim& prim)
{
    return HdPrimOriginSchema::GetFromParent(prim.dataSource).GetOriginPath(HdPrimOriginSchemaTokens->scenePath);
}

void
expectIdentityWorld(const HdSceneIndexPrim& prim,
                    const std::string& path)
{
    const HdXformSchema xform = HdXformSchema::GetFromParent(prim.dataSource);
    EXPECT_EQ(typed<GfMatrix4d>(xform.GetMatrix()), GfMatrix4d(1.0)) << path;
    EXPECT_TRUE(typed<bool>(xform.GetResetXformStack())) << path;
}

void
expectExtent(const HdSceneIndexPrim& prim,
             const GfVec3d& lo,
             const GfVec3d& hi,
             const std::string& path)
{
    const HdExtentSchema extent = HdExtentSchema::GetFromParent(prim.dataSource);
    EXPECT_EQ(typed<GfVec3d>(extent.GetMin()), lo) << path;
    EXPECT_EQ(typed<GfVec3d>(extent.GetMax()), hi) << path;
}

void
boundsOf(const GeoDetail& detail,
         GfVec3d* lo,
         GfVec3d* hi)
{
    for (std::size_t i = 0; i < detail.pointCount(); ++i) {
        const auto p = detail.pointPosition(GeoOffset(i));
        const GfVec3d v { GfVec3f(float(p[0]), float(p[1]), float(p[2])) };
        for (int k = 0; k < 3; ++k) {
            (*lo)[k] = i ? std::min((*lo)[k], v[k]) : v[k];
            (*hi)[k] = i ? std::max((*hi)[k], v[k]) : v[k];
        }
    }
}

// Every prim reachable from the root, the way Hydra pulls them after the adds.
std::map<SdfPath, TfToken>
pullAll(const HdSceneIndexBase& index)
{
    std::map<SdfPath, TfToken> prims;
    std::vector<SdfPath> stack { SdfPath::AbsoluteRootPath() };
    while (!stack.empty()) {
        const SdfPath path = stack.back();
        stack.pop_back();
        for (const SdfPath& child : index.GetChildPrimPaths(path)) {
            EXPECT_EQ(child.GetParentPath(), path);
            prims[child] = index.GetPrim(child).primType;
            stack.push_back(child);
        }
    }

    return prims;
}

class Recorder
    : public HdSceneIndexObserver {
public:
    void PrimsAdded(const HdSceneIndexBase& /*sender*/,
                    const AddedPrimEntries& entries) override
    {
        for (const AddedPrimEntry& e : entries) {
            added.push_back(e.primPath.GetString());
        }
    }

    void PrimsRemoved(const HdSceneIndexBase& /*sender*/,
                      const RemovedPrimEntries& entries) override
    {
        for (const RemovedPrimEntry& e : entries) {
            removed.push_back(e.primPath.GetString());
        }
    }

    void PrimsDirtied(const HdSceneIndexBase& /*sender*/,
                      const DirtiedPrimEntries& entries) override
    {
        for (const DirtiedPrimEntry& e : entries) {
            dirtied[e.primPath.GetString()].insert(e.dirtyLocators);
        }
    }

    void PrimsRenamed(const HdSceneIndexBase& sender,
                      const RenamedPrimEntries& entries) override
    {
        ConvertPrimsRenamedToRemovedAndAdded(sender, entries, this);
    }

    void clear()
    {
        added.clear();
        removed.clear();
        dirtied.clear();
    }

    bool silent() const
    {
        return added.empty() && removed.empty() && dirtied.empty();
    }

    std::vector<std::string> added;
    std::vector<std::string> removed;
    std::map<std::string, HdDataSourceLocatorSet> dirtied;
};

// Serves @p first, pulls every prim, then serves @p second and records what it sends.
struct Transition {
    Transition(const GeoDetailConstPtr& first,
               const GeoDetailConstPtr& second)
        : index(GeoSceneIndex::New())
    {
        index->AddObserver(HdSceneIndexObserverPtr(&recorder));
        index->update(convertOf(first));
        pullAll(*index);
        recorder.clear();
        index->update(convertOf(second));
    }

    ~Transition()
    {
        index->RemoveObserver(HdSceneIndexObserverPtr(&recorder));
    }

    Recorder recorder;
    GeoSceneIndexRefPtr index;
};

HdDataSourceLocatorSet
locators(std::initializer_list<HdDataSourceLocator> list)
{
    HdDataSourceLocatorSet set;
    for (const HdDataSourceLocator& l : list) {
        set.insert(l);
    }

    return set;
}

std::string
describe(const HdDataSourceLocatorSet& set)
{
    std::string s;
    for (const HdDataSourceLocator& l : set) {
        s += l.GetString() + " ";
    }

    return s;
}

} // anonymous namespace

TEST(GeoSceneIndex, ABoxIsAMeshWithFaceVaryingNormalsAndSt)
{
    const GeoDetailConstPtr detail = makeCube();
    const GeoSceneIndexRefPtr index = serve(detail);
    const std::shared_ptr<const GeoUsdConvert> convert = index->convert();
    ASSERT_NE(convert, nullptr);

    const std::map<SdfPath, TfToken> prims = pullAll(*index);
    ASSERT_EQ(prims.size(), 2u);
    EXPECT_EQ(prims.at(SdfPath("/geo")), TfToken());
    EXPECT_EQ(prims.at(SdfPath("/geo/mesh_0")), HdPrimTypeTokens->mesh);

    const SdfPath meshPath("/geo/mesh_0");
    const HdSceneIndexPrim mesh = index->GetPrim(meshPath);
    ASSERT_NE(mesh.dataSource, nullptr);
    const HdMeshTopologySchema topology = HdMeshSchema::GetFromParent(mesh.dataSource).GetTopology();
    const VtIntArray counts = typed<VtIntArray>(topology.GetFaceVertexCounts());
    EXPECT_EQ(counts.size(), 24u);
    for (int c : counts) {
        EXPECT_EQ(c, 4);
    }
    EXPECT_EQ(typed<VtIntArray>(topology.GetFaceVertexIndices()), convertValue(*convert, meshPath.AppendProperty(UsdGeomTokens->faceVertexIndices)).Get<VtIntArray>());
    EXPECT_EQ(typed<TfToken>(topology.GetOrientation()), HdMeshTopologySchemaTokens->rightHanded);
    EXPECT_EQ(typed<TfToken>(HdMeshSchema::GetFromParent(mesh.dataSource).GetSubdivisionScheme()), TfToken("none"));

    const VtValue points = primvarValue(mesh, HdPrimvarsSchemaTokens->points);
    ASSERT_TRUE(points.IsHolding<VtVec3fArray>());
    EXPECT_EQ(points.UncheckedGet<VtVec3fArray>().size(), detail->pointCount());
    // Served copy-free: the primvar is the convert's own array.
    const std::shared_ptr<const GeoUsdValueMap> values = convert->partitionValues(0);
    EXPECT_EQ(points.UncheckedGet<VtVec3fArray>().cdata(), values->at(meshPath.AppendProperty(UsdGeomTokens->points)).Get<VtVec3fArray>().cdata());
    EXPECT_EQ(interpolationOf(mesh, HdPrimvarsSchemaTokens->points), HdPrimvarSchemaTokens->vertex);
    EXPECT_EQ(roleOf(mesh, HdPrimvarsSchemaTokens->points), HdPrimvarSchemaTokens->point);

    const VtValue normals = primvarValue(mesh, HdPrimvarsSchemaTokens->normals);
    ASSERT_TRUE(normals.IsHolding<VtVec3fArray>());
    EXPECT_EQ(normals.UncheckedGet<VtVec3fArray>().size(), 96u);
    EXPECT_EQ(interpolationOf(mesh, HdPrimvarsSchemaTokens->normals), HdPrimvarSchemaTokens->faceVarying);
    EXPECT_EQ(roleOf(mesh, HdPrimvarsSchemaTokens->normals), HdPrimvarSchemaTokens->normal);

    const TfToken st("st");
    const VtValue uv = primvarValue(mesh, st);
    ASSERT_TRUE(uv.IsHolding<VtVec2fArray>());
    EXPECT_EQ(uv.UncheckedGet<VtVec2fArray>().size(), 96u);
    EXPECT_EQ(interpolationOf(mesh, st), HdPrimvarSchemaTokens->faceVarying);
    EXPECT_EQ(roleOf(mesh, st), HdPrimvarSchemaTokens->textureCoordinate);

    GfVec3d lo, hi;
    boundsOf(*detail, &lo, &hi);
    expectExtent(mesh, lo, hi, meshPath.GetString());
    expectIdentityWorld(mesh, meshPath.GetString());
    EXPECT_EQ(originOf(mesh), meshPath);
}

TEST(GeoSceneIndex, LoosePointsCarryWidthsAndDisplayColor)
{
    const GeoDetailConstPtr detail = makePoints(0.5f);
    const GeoSceneIndexRefPtr index = serve(detail);
    const std::map<SdfPath, TfToken> prims = pullAll(*index);
    const SdfPath path("/geo/points_0");
    ASSERT_EQ(prims.count(path), 1u);
    EXPECT_EQ(prims.at(path), HdPrimTypeTokens->points);

    const HdSceneIndexPrim prim = index->GetPrim(path);
    const VtValue points = primvarValue(prim, HdPrimvarsSchemaTokens->points);
    ASSERT_TRUE(points.IsHolding<VtVec3fArray>());
    EXPECT_EQ(points.UncheckedGet<VtVec3fArray>(), VtVec3fArray({ GfVec3f(0.f, 0.f, 1.f), GfVec3f(1.f, 0.f, 1.f), GfVec3f(2.f, 0.f, 1.f) }));

    const VtValue widths = primvarValue(prim, HdPrimvarsSchemaTokens->widths);
    ASSERT_TRUE(widths.IsHolding<VtFloatArray>());
    EXPECT_EQ(widths, convertValue(*index->convert(), path.AppendProperty(UsdGeomTokens->widths)));
    EXPECT_EQ(widths.UncheckedGet<VtFloatArray>().size(), 3u);
    EXPECT_EQ(interpolationOf(prim, HdPrimvarsSchemaTokens->widths), HdPrimvarSchemaTokens->vertex);

    const TfToken displayColor("displayColor");
    const VtValue colour = primvarValue(prim, displayColor);
    ASSERT_TRUE(colour.IsHolding<VtVec3fArray>());
    EXPECT_EQ(colour.UncheckedGet<VtVec3fArray>()[2], GfVec3f(0.5f, 0.5f, 0.75f));
    EXPECT_EQ(interpolationOf(prim, displayColor), HdPrimvarSchemaTokens->vertex);
    EXPECT_EQ(roleOf(prim, displayColor), HdPrimvarSchemaTokens->color);

    expectExtent(prim, GfVec3d(0., 0., 1.), GfVec3d(2., 0., 1.), path.GetString());
    expectIdentityWorld(prim, path.GetString());
    EXPECT_EQ(originOf(prim), path);
}

TEST(GeoSceneIndex, APolylineIsALinearBasisCurve)
{
    const GeoSceneIndexRefPtr index = serve(makeCurve());
    const SdfPath path("/geo/curves_0");
    const HdSceneIndexPrim prim = index->GetPrim(path);
    EXPECT_EQ(prim.primType, HdPrimTypeTokens->basisCurves);
    const HdBasisCurvesTopologySchema topology = HdBasisCurvesSchema::GetFromParent(prim.dataSource).GetTopology();
    EXPECT_EQ(typed<VtIntArray>(topology.GetCurveVertexCounts()), VtIntArray({ 3 }));
    EXPECT_EQ(typed<TfToken>(topology.GetType()), HdTokens->linear);
    EXPECT_EQ(typed<TfToken>(topology.GetWrap()), HdTokens->nonperiodic);

    const VtValue points = primvarValue(prim, HdPrimvarsSchemaTokens->points);
    ASSERT_TRUE(points.IsHolding<VtVec3fArray>());
    EXPECT_EQ(points.UncheckedGet<VtVec3fArray>(), VtVec3fArray({ GfVec3f(0.f, 0.f, -1.f), GfVec3f(1.f, 1.f, -1.f), GfVec3f(2.f, 4.f, -1.f) }));
    EXPECT_EQ(interpolationOf(prim, HdPrimvarsSchemaTokens->points), HdPrimvarSchemaTokens->vertex);
    expectExtent(prim, GfVec3d(0., 0., -1.), GfVec3d(2., 4., -1.), path.GetString());
    expectIdentityWorld(prim, path.GetString());
    EXPECT_EQ(originOf(prim), path);
}

TEST(GeoSceneIndex, PrimitiveGroupsAreGeomSubsetsUnderThePrefix)
{
    const SdfPath prefix("/viewer/geo0");
    const GeoSceneIndexRefPtr index = serve(makeGrouped(), prefix);
    const std::map<SdfPath, TfToken> prims = pullAll(*index);
    const std::map<SdfPath, TfToken> expected {
        { SdfPath("/viewer"), TfToken() },
        { SdfPath("/viewer/geo0"), TfToken() },
        { SdfPath("/viewer/geo0/geo"), TfToken() },
        { SdfPath("/viewer/geo0/geo/a"), HdPrimTypeTokens->mesh },
        { SdfPath("/viewer/geo0/geo/a/sel"), HdPrimTypeTokens->geomSubset },
        { SdfPath("/viewer/geo0/geo/b"), HdPrimTypeTokens->mesh },
    };
    EXPECT_EQ(prims, expected);
    EXPECT_EQ(index->indexPath(SdfPath("/geo/a")), SdfPath("/viewer/geo0/geo/a"));

    const HdSceneIndexPrim subset = index->GetPrim(SdfPath("/viewer/geo0/geo/a/sel"));
    const HdGeomSubsetSchema schema = HdGeomSubsetSchema::GetFromParent(subset.dataSource);
    EXPECT_EQ(typed<TfToken>(schema.GetType()), HdGeomSubsetSchemaTokens->typeFaceSet);
    EXPECT_EQ(typed<VtIntArray>(schema.GetIndices()), VtIntArray({ 0 }));
    EXPECT_EQ(originOf(subset), SdfPath("/geo/a/sel"));

    const HdSceneIndexPrim b = index->GetPrim(SdfPath("/viewer/geo0/geo/b"));
    EXPECT_EQ(originOf(b), SdfPath("/geo/b"));
    const VtValue points = primvarValue(b, HdPrimvarsSchemaTokens->points);
    ASSERT_TRUE(points.IsHolding<VtVec3fArray>());
    EXPECT_EQ(points.UncheckedGet<VtVec3fArray>()[0], GfVec3f(5.f, 0.f, 0.f));
    EXPECT_EQ(typed<VtIntArray>(HdMeshSchema::GetFromParent(b.dataSource).GetTopology().GetFaceVertexCounts()), VtIntArray({ 4 }));
    expectExtent(b, GfVec3d(5., 0., 0.), GfVec3d(6., 1., 0.), "/geo/b");
    expectIdentityWorld(b, "/geo/b");
    EXPECT_EQ(interpolationOf(b, TfToken("displayColor")), HdPrimvarSchemaTokens->vertex);

    EXPECT_EQ(index->GetPrim(SdfPath("/geo/a")).dataSource, nullptr);
    EXPECT_EQ(index->GetPrim(SdfPath("/viewer/geo0/geo/missing")).dataSource, nullptr);
}

TEST(GeoSceneIndex, AnEmptyDetailServesNothing)
{
    GeoDetailBuilder b;
    const GeoSceneIndexRefPtr index = serve(finish(b), SdfPath("/viewer"));
    EXPECT_TRUE(index->GetChildPrimPaths(SdfPath::AbsoluteRootPath()).empty());
    EXPECT_EQ(index->GetPrim(SdfPath("/viewer")).dataSource, nullptr);
}

TEST(GeoSceneIndex, AMoveDirtiesPointsAndExtent)
{
    const Transition t(makeQuads(0.), makeQuads(3.));
    EXPECT_TRUE(t.recorder.added.empty());
    EXPECT_TRUE(t.recorder.removed.empty());
    ASSERT_EQ(t.recorder.dirtied.size(), 1u);
    const HdDataSourceLocatorSet& dirty = t.recorder.dirtied.at("/geo/mesh_0");
    EXPECT_EQ(dirty, locators({ HdPrimvarsSchema::GetPointsLocator(), HdExtentSchema::GetDefaultLocator() })) << describe(dirty);

    const VtValue points = primvarValue(t.index->GetPrim(SdfPath("/geo/mesh_0")), HdPrimvarsSchemaTokens->points);
    ASSERT_TRUE(points.IsHolding<VtVec3fArray>());
    EXPECT_EQ(points.UncheckedGet<VtVec3fArray>()[0], GfVec3f(3.f, 0.f, 0.f));
}

TEST(GeoSceneIndex, AMoveInsideTheBoundsDirtiesOnlyPoints)
{
    const Transition t(makeQuads(0.), makeQuads(0., false, 0.5));
    ASSERT_EQ(t.recorder.dirtied.size(), 1u);
    const HdDataSourceLocatorSet& dirty = t.recorder.dirtied.at("/geo/mesh_0");
    EXPECT_EQ(dirty, locators({ HdPrimvarsSchema::GetPointsLocator() })) << describe(dirty);
    EXPECT_TRUE(t.recorder.added.empty());
    EXPECT_TRUE(t.recorder.removed.empty());
}

TEST(GeoSceneIndex, APscaleChangeDirtiesOnlyWidths)
{
    const Transition t(makePoints(0.5f), makePoints(0.25f));
    ASSERT_EQ(t.recorder.dirtied.size(), 1u);
    const HdDataSourceLocatorSet& dirty = t.recorder.dirtied.at("/geo/points_0");
    EXPECT_EQ(dirty, locators({ HdPrimvarsSchema::GetWidthsLocator() })) << describe(dirty);
    EXPECT_TRUE(t.recorder.added.empty());
    EXPECT_TRUE(t.recorder.removed.empty());
}

TEST(GeoSceneIndex, APointCountChangeDirtiesTheTopology)
{
    const Transition t(makeQuads(0.), makeQuads(0., true));
    EXPECT_TRUE(t.recorder.added.empty());
    EXPECT_TRUE(t.recorder.removed.empty());
    ASSERT_EQ(t.recorder.dirtied.size(), 1u);
    const HdDataSourceLocatorSet& dirty = t.recorder.dirtied.at("/geo/mesh_0");
    EXPECT_TRUE(dirty.Contains(HdMeshSchema::GetTopologyLocator())) << describe(dirty);
    EXPECT_TRUE(dirty.Contains(HdPrimvarsSchema::GetPointsLocator())) << describe(dirty);
    EXPECT_FALSE(dirty.Contains(HdMeshSchema::GetDefaultLocator())) << describe(dirty);
}

TEST(GeoSceneIndex, APathChangeRemovesAndAddsOnly)
{
    const Transition t(makeGrouped("b"), makeGrouped("c"));
    EXPECT_EQ(t.recorder.removed, std::vector<std::string>({ "/geo/b" }));
    EXPECT_EQ(t.recorder.added, std::vector<std::string>({ "/geo/c" }));
    EXPECT_TRUE(t.recorder.dirtied.empty()) << t.recorder.dirtied.begin()->first;
    EXPECT_EQ(t.index->GetPrim(SdfPath("/geo/b")).dataSource, nullptr);
    EXPECT_EQ(t.index->GetPrim(SdfPath("/geo/c")).primType, HdPrimTypeTokens->mesh);
}

TEST(GeoSceneIndex, AnIdenticalDetailSendsNothing)
{
    const GeoDetailConstPtr detail = makeGrouped();
    const Transition same(detail, detail);
    EXPECT_TRUE(same.recorder.silent());
    const Transition rebuilt(makeCube(), makeCube());
    EXPECT_TRUE(rebuilt.recorder.silent());
}

TEST(GeoSceneIndex, PathsComeFromTheConvertNotThePrimitiveOrder)
{
    const Transition t(makeGrouped("b", false), makeGrouped("b", true));
    EXPECT_TRUE(t.recorder.added.empty());
    EXPECT_TRUE(t.recorder.removed.empty());
    EXPECT_TRUE(t.recorder.dirtied.empty()) << t.recorder.dirtied.begin()->first << ": " << describe(t.recorder.dirtied.begin()->second);
    const VtValue points = primvarValue(t.index->GetPrim(SdfPath("/geo/a")), HdPrimvarsSchemaTokens->points);
    ASSERT_TRUE(points.IsHolding<VtVec3fArray>());
    EXPECT_EQ(points.UncheckedGet<VtVec3fArray>()[0], GfVec3f(0.f, 0.f, 0.f));
}

TEST(GeoSceneIndex, EmptyingTheDetailRemovesEverything)
{
    GeoDetailBuilder empty;
    const Transition t(makeGrouped(), finish(empty));
    EXPECT_EQ(t.recorder.removed, std::vector<std::string>({ "/geo" }));
    EXPECT_TRUE(t.recorder.added.empty());
    EXPECT_TRUE(t.index->GetChildPrimPaths(SdfPath::AbsoluteRootPath()).empty());
}
