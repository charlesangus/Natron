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
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Geometry/GeoPrimitive.h"
#include "Engine/Nodes/Geometry/GeoSceneIndex.h"
#include "Engine/Nodes/Geometry/GeoSdfFileFormat.h"
#include "Engine/Nodes/Geometry/GeoUsdConvert.h"
#include "Tests/Scene/GeoSceneIndexFixtures.h"
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
using namespace GeoSceneIndexFixtures;

namespace {

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

TEST(GeoSceneIndexParity, ASingleCurveMatchesUsdImaging)
{
    expectParity("parity-curve", makeCurve());
}

TEST(GeoSceneIndexParity, PathsAndSubsetsMatchUsdImagingUnderAPrefix)
{
    expectParity("parity-grouped", makeGrouped(), SdfPath("/viewer/geo"));
    expectParity("parity-nested", makeNested(), SdfPath("/viewer/geo"));
}

TEST(GeoSceneIndexParity, AnAncestorTransformMatchesUsdImaging)
{
    expectParity("parity-xformed", makeTransformedParent(1., 2., 3.));
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
    const HdRetainedSceneIndexRefPtr edited = copyWithEdit(*ours, path, locator, HdRetainedTypedSampledDataSource<VtVec3fArray>::New(VtVec3fArray(3, GfVec3f(9.f))));
    const std::vector<std::string> mismatches = GeoSceneIndexParity::compare(*edited, reference.index());
    ASSERT_EQ(mismatches.size(), 1u) << joined(mismatches);
    const std::string expected = "/geo/points_0: primvars/displayColor value differs";
    EXPECT_EQ(mismatches[0].compare(0, expected.size(), expected), 0) << mismatches[0];
}
