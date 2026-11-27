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

#include <memory>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Geometry/GeoPrimitive.h"
#include "Engine/Nodes/Geometry/GeoSceneIndex.h"
#include "Engine/Nodes/Geometry/GeoSdfFileFormat.h"
#include "Engine/Nodes/Geometry/GeoUsdConvert.h"
#include "Tests/Scene/GeoSceneIndexParity.h"

#include <pxr/imaging/hd/containerDataSourceEditor.h>
#include <pxr/imaging/hd/primvarSchema.h>
#include <pxr/imaging/hd/primvarsSchema.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/retainedSceneIndex.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImaging/sceneIndex.h>

PXR_NAMESPACE_USING_DIRECTIVE
NATRON_NAMESPACE_USING

namespace {

GeoDetailConstPtr
finish(GeoDetailBuilder& b)
{
    return std::make_shared<const GeoDetail>(b.finish());
}

GeoAttribute&
addAttribute(GeoDetailBuilder& b,
             GeoAttribClassEnum cls,
             const char* name,
             int tupleSize,
             GeoTypeInfoEnum typeInfo = eGeoTypeInfoNone)
{
    GeoAttributeDesc desc;
    desc.name = name;
    desc.storage = eGeoStorageFloat32;
    desc.tupleSize = tupleSize;
    desc.typeInfo = typeInfo;

    return b.addAttribute(cls, desc);
}

void
addQuad(GeoDetailBuilder& b,
        double x,
        GeoAttribute& colour)
{
    const GeoOffset first = b.appendPoints(4);
    const double corners[4][2] = { { x, 0. }, { x + 1., 0. }, { x + 1., 1. }, { x, 1. } };
    GeoOffset offsets[4];
    for (int i = 0; i < 4; ++i) {
        offsets[i] = first + i;
        b.setPointPosition(offsets[i], corners[i][0], corners[i][1], 0.25);
        const float c[3] = { float(corners[i][0]) * 0.1f, float(corners[i][1]), 0.5f };
        colour.setTuple<float>(std::size_t(offsets[i]), std::span<const float>(c, 3));
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

GeoDetailConstPtr
makePoints()
{
    GeoDetailBuilder b;
    const GeoOffset first = b.appendPoints(4);
    GeoAttribute& colour = addAttribute(b, eGeoAttribClassPoint, "Cd", 3, eGeoTypeInfoColour);
    GeoAttribute& scale = addAttribute(b, eGeoAttribClassPoint, "pscale", 1);
    GeoAttribute& weight = addAttribute(b, eGeoAttribClassPoint, "weight", 1);
    for (int i = 0; i < 4; ++i) {
        b.setPointPosition(first + i, double(i), double(i % 2), -2.);
        const float c[3] = { 0.2f * float(i), 0.1f, 0.9f };
        colour.setTuple<float>(std::size_t(first + i), std::span<const float>(c, 3));
        scale.set<float>(std::size_t(first + i), 0.1f * float(i + 1));
        weight.set<float>(std::size_t(first + i), float(i) * 0.5f);
    }

    return finish(b);
}

GeoDetailConstPtr
makeCurves()
{
    GeoDetailBuilder b;
    GeoAttribute& width = addAttribute(b, eGeoAttribClassPrimitive, "tension", 1);
    for (int c = 0; c < 2; ++c) {
        const GeoOffset first = b.appendPoints(3 + c);
        std::vector<GeoOffset> points;
        for (int i = 0; i < 3 + c; ++i) {
            b.setPointPosition(first + i, double(i), double(c), double(i * c));
            points.push_back(first + i);
        }
        b.appendPrimitive(eGeoPrimitivePolyline, std::span<const GeoOffset>(points.data(), points.size()));
        width.set<float>(std::size_t(c), 0.5f + float(c));
    }

    return finish(b);
}

GeoDetailConstPtr
makeGrouped()
{
    GeoDetailBuilder b;
    GeoAttribute& colour = addAttribute(b, eGeoAttribClassPoint, "Cd", 3, eGeoTypeInfoColour);
    addQuad(b, 0., colour);
    addQuad(b, 2., colour);
    addQuad(b, 4., colour);
    GeoAttributeDesc pathDesc;
    pathDesc.name = "path";
    pathDesc.storage = eGeoStorageString;
    GeoAttribute& path = b.addAttribute(eGeoAttribClassPrimitive, pathDesc);
    path.setString(0, "/geo/shapes/a");
    path.setString(1, "/geo/shapes/a");
    path.setString(2, "/geo/b");
    b.addGroup(eGeoAttribClassPrimitive, "sel").add(GeoOffset(1));
    auto& all = b.addGroup(eGeoAttribClassPrimitive, "all");
    for (int i = 0; i < 3; ++i) {
        all.add(GeoOffset(i));
    }

    return finish(b);
}

// UsdImaging's scene index over @p detail served through a natrongeo layer on a stage.
class Reference {
public:
    Reference(const std::string& key,
              const GeoDetailConstPtr& detail)
        : _key(key)
    {
        std::string error;
        EXPECT_TRUE(GeoLayers::registerPlugin(&error)) << error;
        GeoLayers::setProvider(_key, [detail]() { return detail; });
        const SdfLayerRefPtr layer = SdfLayer::FindOrOpen(GeoLayers::identifier(_key));
        EXPECT_NE(layer, nullptr) << key;
        if (layer) {
            _stage = UsdStage::Open(layer);
        }
        EXPECT_NE(_stage, nullptr) << key;
        _index = UsdImagingSceneIndex::New(HdRetainedContainerDataSource::New(),
                                           [](const HdSceneIndexBaseRefPtr& input) { return input; });
        _index->SetStage(_stage);
    }

    ~Reference()
    {
        _index->SetStage(UsdStageRefPtr());
        GeoLayers::removeProvider(_key);
    }

    const HdSceneIndexBase& index() const
    {
        return *_index;
    }

private:
    std::string _key;
    UsdStageRefPtr _stage;
    UsdImagingSceneIndexRefPtr _index;
};

std::string
joined(const std::vector<std::string>& lines)
{
    std::string s;
    for (const std::string& line : lines) {
        s += line + "\n";
    }

    return s;
}

void
expectParity(const char* key,
             const GeoDetailConstPtr& detail,
             const SdfPath& prefix = SdfPath::AbsoluteRootPath())
{
    const Reference reference(key, detail);
    const GeoSceneIndexRefPtr ours = GeoSceneIndex::New(prefix);
    ours->update(std::make_shared<GeoUsdConvert>(detail, GeoUsdConvertOptions()));
    GeoSceneIndexParity::Options options;
    options.prefix = prefix;
    const std::vector<std::string> mismatches = GeoSceneIndexParity::compare(*ours, reference.index(), options);
    EXPECT_TRUE(mismatches.empty()) << key << ":\n"
                                    << joined(mismatches);
    EXPECT_FALSE(ours->GetChildPrimPaths(prefix).empty()) << key;
}

// Every prim of @p source, with @p locator set to @p value on the prim at @p edited.
HdRetainedSceneIndexRefPtr
copyWithEdit(const HdSceneIndexBase& source,
             const SdfPath& edited,
             const HdDataSourceLocator& locator,
             const HdDataSourceBaseHandle& value)
{
    HdRetainedSceneIndex::AddedPrimEntries entries;
    std::vector<SdfPath> stack { SdfPath::AbsoluteRootPath() };
    while (!stack.empty()) {
        const SdfPath path = stack.back();
        stack.pop_back();
        for (const SdfPath& child : source.GetChildPrimPaths(path)) {
            HdSceneIndexPrim prim = source.GetPrim(child);
            if (child == edited) {
                prim.dataSource = HdContainerDataSourceEditor(prim.dataSource).Set(locator, value).Finish();
            }
            entries.push_back({ child, prim.primType, prim.dataSource });
            stack.push_back(child);
        }
    }
    const HdRetainedSceneIndexRefPtr copy = HdRetainedSceneIndex::New();
    copy->AddPrims(entries);

    return copy;
}

} // anonymous namespace

TEST(GeoSceneIndexParity, ABoxMatchesUsdImaging)
{
    expectParity("parity-box", makeCube());
}

TEST(GeoSceneIndexParity, PointsMatchUsdImaging)
{
    expectParity("parity-points", makePoints());
}

TEST(GeoSceneIndexParity, CurvesMatchUsdImaging)
{
    expectParity("parity-curves", makeCurves());
}

TEST(GeoSceneIndexParity, PathsAndSubsetsMatchUsdImagingUnderAPrefix)
{
    expectParity("parity-grouped", makeGrouped(), SdfPath("/viewer/geo"));
}

TEST(GeoSceneIndexParity, AMismatchNamesThePrimAndTheField)
{
    const GeoDetailConstPtr detail = makeGrouped();
    const Reference reference("parity-mismatch", detail);
    const GeoSceneIndexRefPtr ours = GeoSceneIndex::New();
    ours->update(std::make_shared<GeoUsdConvert>(makePoints(), GeoUsdConvertOptions()));
    const std::vector<std::string> mismatches = GeoSceneIndexParity::compare(*ours, reference.index());
    bool named = false;
    for (const std::string& m : mismatches) {
        named = named || m == "/geo/b: prim missing from ours";
    }
    EXPECT_TRUE(named) << joined(mismatches);
}

TEST(GeoSceneIndexParity, AMismatchedPrimvarNamesThePrimAndThePrimvar)
{
    const GeoDetailConstPtr detail = makePoints();
    const Reference reference("parity-primvar-mismatch", detail);
    const GeoSceneIndexRefPtr ours = GeoSceneIndex::New();
    ours->update(std::make_shared<GeoUsdConvert>(detail, GeoUsdConvertOptions()));
    ASSERT_TRUE(GeoSceneIndexParity::compare(*ours, reference.index()).empty());

    const SdfPath path("/geo/points_0");
    const TfToken displayColor("displayColor");
    const HdDataSourceLocator locator = HdPrimvarsSchema::GetDefaultLocator().Append(displayColor).Append(HdPrimvarSchemaTokens->primvarValue);
    const HdRetainedSceneIndexRefPtr edited = copyWithEdit(*ours, path, locator, HdRetainedTypedSampledDataSource<VtVec3fArray>::New(VtVec3fArray(4, GfVec3f(9.f))));
    const std::vector<std::string> mismatches = GeoSceneIndexParity::compare(*edited, reference.index());
    ASSERT_EQ(mismatches.size(), 1u) << joined(mismatches);
    const std::string expected = "/geo/points_0: primvars/displayColor value differs";
    EXPECT_EQ(mismatches[0].compare(0, expected.size(), expected), 0) << mismatches[0];
}
