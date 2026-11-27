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
#include <array>
#include <cmath>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include "Engine/Geo/GeoOpenVDB.h"

#include "Engine/Geo/GeoPacked.h"
#include "Engine/Geo/GeoVDB.h"
#include "Engine/Nodes/Geometry/GeoCopyToPoints.h"
#include "Engine/Nodes/Geometry/GeoUsdConvert.h"

#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/pointInstancer.h>
#include <pxr/usd/usdGeom/xform.h>
#include <pxr/usd/usdGeom/xformCache.h>
#include <pxr/usd/usdVol/openVDBAsset.h>
#include <pxr/usd/usdVol/volume.h>

PXR_NAMESPACE_USING_DIRECTIVE
NATRON_NAMESPACE_USING

namespace {

openvdb::FloatGrid::Ptr
makeSphere(float radius)
{
    geoInitializeOpenVDB();

    return openvdb::tools::createLevelSetSphere<openvdb::FloatGrid>(radius, openvdb::Vec3f(0.f, 0.f, 0.f), 0.25f);
}

GeoDetailConstPtr
makeVolume(const std::vector<std::pair<GeoVDBGridBaseConstPtr, std::string>>& grids)
{
    GeoDetailBuilder b;
    for (const auto& grid : grids) {
        geoAppendVDB(b, grid.first, grid.second);
    }

    return std::make_shared<const GeoDetail>(b.finish());
}

GeoUsdConvertOptions
optionsIn(const QTemporaryDir& dir)
{
    GeoUsdConvertOptions options;
    options.volumeDirectory = dir.path().toStdString();

    return options;
}

SdfLayerRefPtr
authored(const GeoUsdConvert& convert)
{
    SdfLayerRefPtr layer = SdfLayer::CreateAnonymous("geo.usda");
    std::string err;
    EXPECT_TRUE(convert.author(layer, UsdTimeCode::Default(), &err)) << err;

    return layer;
}

SdfPath
volumePath(const GeoUsdConvert& convert)
{
    for (std::size_t i = 0; i < convert.partitionCount(); ++i) {
        if (convert.partitionKind(i) == eGeoUsdPrimVolume) {
            return convert.partitionPath(i);
        }
    }

    return SdfPath();
}

std::string
fieldFile(const UsdStageRefPtr& stage,
          const SdfPath& field)
{
    SdfAssetPath asset;
    UsdVolOpenVDBAsset(stage->GetPrimAtPath(field)).GetFilePathAttr().Get(&asset);

    return asset.GetAssetPath();
}

} // namespace

TEST(GeoVDBBridge, GridBecomesAVolumeWithAnOpenVDBField)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const GeoDetailConstPtr detail = makeVolume({ { makeSphere(1.f), "density" } });
    const GeoUsdConvert convert(detail, optionsIn(dir));
    const SdfPath path = volumePath(convert);
    ASSERT_FALSE(path.IsEmpty());
    const UsdStageRefPtr stage = UsdStage::Open(authored(convert));
    ASSERT_TRUE(static_cast<bool>(stage));

    const UsdVolVolume volume(stage->GetPrimAtPath(path));
    ASSERT_TRUE(static_cast<bool>(volume));
    const UsdVolVolume::FieldMap fields = volume.GetFieldPaths();
    ASSERT_EQ(fields.size(), 1u);
    EXPECT_EQ(fields.begin()->first, TfToken("density"));
    const SdfPath fieldPath = fields.begin()->second;
    const UsdVolOpenVDBAsset asset(stage->GetPrimAtPath(fieldPath));
    ASSERT_TRUE(static_cast<bool>(asset));
    TfToken fieldName;
    TfToken dataType;
    TfToken fieldClass;
    asset.GetFieldNameAttr().Get(&fieldName);
    asset.GetFieldDataTypeAttr().Get(&dataType);
    asset.GetFieldClassAttr().Get(&fieldClass);
    EXPECT_EQ(fieldName, TfToken("density"));
    EXPECT_EQ(dataType, TfToken("float"));
    EXPECT_EQ(fieldClass, TfToken("levelSet"));

    const std::string file = fieldFile(stage, fieldPath);
    EXPECT_EQ(std::filesystem::path(file).parent_path(), std::filesystem::path(dir.path().toStdString()));
    EXPECT_TRUE(std::filesystem::exists(file));

    VtVec3fArray extent;
    ASSERT_TRUE(volume.GetExtentAttr().Get(&extent));
    ASSERT_EQ(extent.size(), 2u);
    EXPECT_NEAR(extent[1][0], geoVDBBounds(*detail, 0).max[0], 1e-4);
}

TEST(GeoVDBBridge, RoundTripKeepsGridContentHash)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const GeoDetailConstPtr detail = makeVolume({ { makeSphere(1.f), "density" } });
    const GeoUsdConvert convert(detail, optionsIn(dir));
    const SdfPath path = volumePath(convert);
    const UsdStageRefPtr stage = UsdStage::Open(authored(convert));
    ASSERT_TRUE(static_cast<bool>(stage));

    GeoDetailBuilder b;
    std::vector<std::string> warnings;
    std::string err;
    ASSERT_TRUE(geoUsdReadPrim(stage->GetPrimAtPath(path), UsdTimeCode::Default(), nullptr, b, &warnings, &err)) << err;
    const GeoDetail back = b.finish();
    EXPECT_TRUE(warnings.empty()) << warnings.front();
    ASSERT_EQ(back.primitiveCount(), GeoIndex(1));
    ASSERT_EQ(back.primitiveType(0), eGeoPrimitiveVDB);
    EXPECT_EQ(geoVDBName(back, 0), "density");
    EXPECT_EQ(back.sharedGrids()[0]->contentHash(), detail->sharedGrids()[0]->contentHash());
}

TEST(GeoVDBBridge, SecondExportReusesTheFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const GeoDetailConstPtr detail = makeVolume({ { makeSphere(1.f), "density" } });

    const GeoUsdConvert first(detail, optionsIn(dir));
    const UsdStageRefPtr firstStage = UsdStage::Open(authored(first));
    const SdfPath field = volumePath(first).AppendChild(TfToken("density"));
    const std::string file = fieldFile(firstStage, field);
    ASSERT_TRUE(std::filesystem::exists(file));
    const std::filesystem::file_time_type written = std::filesystem::last_write_time(file);

    // A copy of the grid, so the file is found by content and not by pointer.
    const GeoDetailConstPtr again = makeVolume({ { geoVDBGrid(*detail, 0)->deepCopyGrid(), "density" } });
    const GeoUsdConvert second(again, optionsIn(dir));
    const UsdStageRefPtr secondStage = UsdStage::Open(authored(second));
    EXPECT_EQ(fieldFile(secondStage, field), file);
    EXPECT_EQ(std::filesystem::last_write_time(file), written);

    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir.path().toStdString())) {
        (void)entry;
        ++files;
    }
    EXPECT_EQ(files, 1u);

    const GeoUsdConvert bigger(makeVolume({ { makeSphere(2.f), "density" } }), optionsIn(dir));
    const UsdStageRefPtr biggerStage = UsdStage::Open(authored(bigger));
    EXPECT_NE(fieldFile(biggerStage, field), file);
}

TEST(GeoVDBBridge, ARepeatedFieldNameStartsTheNextVolume)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const GeoUsdConvert convert(makeVolume({ { makeSphere(1.f), "density" }, { makeSphere(2.f), "density" } }), optionsIn(dir));
    std::vector<SdfPath> volumes;
    for (std::size_t i = 0; i < convert.partitionCount(); ++i) {
        if (convert.partitionKind(i) == eGeoUsdPrimVolume) {
            volumes.push_back(convert.partitionPath(i));
        }
    }
    ASSERT_EQ(volumes.size(), 2u);
    const SdfPath root = GeoUsdConvertOptions().rootPath;
    EXPECT_EQ(volumes[0], root.AppendChild(TfToken("volume_0")));
    EXPECT_EQ(volumes[1], root.AppendChild(TfToken("volume_1")));
    EXPECT_EQ(convert.volumeNotes().size(), 1u);
    EXPECT_TRUE(convert.volumeWriteFailures().empty());

    const UsdStageRefPtr stage = UsdStage::Open(authored(convert));
    ASSERT_TRUE(static_cast<bool>(stage));
    std::vector<std::string> files;
    for (const SdfPath& path : volumes) {
        const UsdVolVolume::FieldMap fields = UsdVolVolume(stage->GetPrimAtPath(path)).GetFieldPaths();
        ASSERT_EQ(fields.size(), 1u);
        EXPECT_EQ(fields.begin()->first, TfToken("density"));
        files.push_back(fieldFile(stage, fields.begin()->second));

        GeoDetailBuilder b;
        std::vector<std::string> warnings;
        std::string err;
        ASSERT_TRUE(geoUsdReadPrim(stage->GetPrimAtPath(path), UsdTimeCode::Default(), nullptr, b, &warnings, &err)) << err;
        const GeoDetail back = b.finish();
        ASSERT_EQ(back.primitiveCount(), GeoIndex(1));
        EXPECT_EQ(geoVDBName(back, 0), "density");
    }
    EXPECT_NE(files[0], files[1]);
}

TEST(GeoVDBBridge, AVolumesGridsShareOneFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const GeoUsdConvert convert(makeVolume({ { makeSphere(1.f), "density" }, { makeSphere(2.f), "temperature" } }), optionsIn(dir));
    const SdfPath path = volumePath(convert);
    const UsdStageRefPtr stage = UsdStage::Open(authored(convert));
    ASSERT_TRUE(static_cast<bool>(stage));
    EXPECT_TRUE(convert.volumeNotes().empty());

    const UsdVolVolume::FieldMap fields = UsdVolVolume(stage->GetPrimAtPath(path)).GetFieldPaths();
    ASSERT_EQ(fields.size(), 2u);
    const SdfPath density = fields.at(TfToken("density"));
    const SdfPath temperature = fields.at(TfToken("temperature"));
    const std::string file = fieldFile(stage, density);
    EXPECT_EQ(fieldFile(stage, temperature), file);
    for (const auto& [field, prim] : fields) {
        const UsdVolOpenVDBAsset asset(stage->GetPrimAtPath(prim));
        TfToken fieldName;
        int index = -1;
        asset.GetFieldNameAttr().Get(&fieldName);
        asset.GetFieldIndexAttr().Get(&index);
        EXPECT_EQ(fieldName, field);
        EXPECT_EQ(index, 0);
    }

    std::vector<std::string> names;
    std::string err;
    ASSERT_TRUE(geoVDBFileGridNames(file, &names, &err)) << err;
    EXPECT_EQ(names, (std::vector<std::string> { "density", "temperature" }));
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir.path().toStdString())) {
        (void)entry;
        ++files;
    }
    EXPECT_EQ(files, 1u);
}

TEST(GeoVDBBridge, SameNamedGridsOfOneVolumeAreToldApartByFieldIndex)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const openvdb::FloatGrid::Ptr small = makeSphere(1.f);
    const openvdb::FloatGrid::Ptr large = makeSphere(2.f);
    GeoDetailBuilder b;
    geoAppendVDB(b, small, "density");
    geoAppendVDB(b, large, "density");
    GeoAttributeDesc fieldDesc;
    fieldDesc.name = kGeoUsdFieldAttr;
    fieldDesc.storage = eGeoStorageString;
    GeoAttribute& recorded = b.addAttribute(eGeoAttribClassPrimitive, fieldDesc);
    recorded.setString(0, "inner");
    recorded.setString(1, "outer");
    const GeoUsdConvert convert(std::make_shared<const GeoDetail>(b.finish()), optionsIn(dir));
    const SdfPath path = volumePath(convert);
    const UsdStageRefPtr stage = UsdStage::Open(authored(convert));
    ASSERT_TRUE(static_cast<bool>(stage));

    const UsdVolVolume::FieldMap fields = UsdVolVolume(stage->GetPrimAtPath(path)).GetFieldPaths();
    ASSERT_EQ(fields.size(), 2u);
    int inner = -1;
    int outer = -1;
    UsdVolOpenVDBAsset(stage->GetPrimAtPath(fields.at(TfToken("inner")))).GetFieldIndexAttr().Get(&inner);
    UsdVolOpenVDBAsset(stage->GetPrimAtPath(fields.at(TfToken("outer")))).GetFieldIndexAttr().Get(&outer);
    EXPECT_EQ(inner, 0);
    EXPECT_EQ(outer, 1);

    GeoDetailBuilder read;
    std::vector<std::string> warnings;
    std::string err;
    ASSERT_TRUE(geoUsdReadPrim(stage->GetPrimAtPath(path), UsdTimeCode::Default(), nullptr, read, &warnings, &err)) << err;
    const GeoDetail back = read.finish();
    ASSERT_EQ(back.primitiveCount(), GeoIndex(2));
    std::vector<openvdb::Index64> counts { geoVDBGrid(back, 0)->activeVoxelCount(), geoVDBGrid(back, 1)->activeVoxelCount() };
    std::sort(counts.begin(), counts.end());
    EXPECT_EQ(counts, (std::vector<openvdb::Index64> { small->activeVoxelCount(), large->activeVoxelCount() }));
}

namespace {

// An input volume whose field relationships are named apart from the grids they hold.
UsdStageRefPtr
inputVolumeStage(const QTemporaryDir& dir)
{
    const std::string densityFile = dir.path().toStdString() + "/input-density.vdb";
    const std::string heatFile = dir.path().toStdString() + "/input-heat.vdb";
    EXPECT_TRUE(geoWriteVDBFile(*makeSphere(1.f), "density", densityFile, nullptr));
    EXPECT_TRUE(geoWriteVDBFile(*makeSphere(2.f), "temperature", heatFile, nullptr));

    const UsdStageRefPtr stage = UsdStage::CreateInMemory("input.usda");
    const UsdVolVolume volume = UsdVolVolume::Define(stage, SdfPath("/scene/vol"));
    const auto field = [&](const char* relationship, const char* prim, const char* grid, const std::string& file) {
        const UsdVolOpenVDBAsset asset = UsdVolOpenVDBAsset::Define(stage, SdfPath("/scene/vol").AppendChild(TfToken(prim)));
        asset.CreateFilePathAttr().Set(SdfAssetPath(file));
        asset.CreateFieldNameAttr().Set(TfToken(grid));
        volume.CreateFieldRelationship(TfToken(relationship), asset.GetPath());
    };
    field("dens", "densityGrid", "density", densityFile);
    field("heat", "heatGrid", "temperature", heatFile);

    return stage;
}

UsdStageRefPtr
composedOver(const UsdStageRefPtr& input,
             const GeoDetailConstPtr& detail,
             const QTemporaryDir& dir,
             SdfLayerRefPtr* converted)
{
    GeoUsdConvertOptions options = optionsIn(dir);
    const GeoUsdDetailPaths paths = geoUsdDetailPaths(*detail, options);
    options.inputScene = geoUsdCaptureInputScene(input, UsdTimeCode::Default(), paths.paths, paths.sourcePaths);
    const GeoUsdConvert convert(detail, options);
    *converted = authored(convert);
    EXPECT_TRUE(convert.volumeWriteFailures().empty());

    const SdfLayerRefPtr root = SdfLayer::CreateAnonymous("composed.usda");
    root->InsertSubLayerPath((*converted)->GetIdentifier());
    root->InsertSubLayerPath(input->GetRootLayer()->GetIdentifier(), 1);

    return UsdStage::Open(root);
}

} // namespace

TEST(GeoVDBBridge, AFieldWithNoGridNameTakesTheGridsOwnName)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string file = dir.path().toStdString() + "/unnamed-field.vdb";
    ASSERT_TRUE(geoWriteVDBFile(*makeSphere(1.f), "density", file, nullptr));
    const UsdStageRefPtr stage = UsdStage::CreateInMemory("unnamed.usda");
    const UsdVolVolume volume = UsdVolVolume::Define(stage, SdfPath("/vol"));
    const UsdVolOpenVDBAsset asset = UsdVolOpenVDBAsset::Define(stage, SdfPath("/vol/smoke"));
    asset.CreateFilePathAttr().Set(SdfAssetPath(file));
    volume.CreateFieldRelationship(TfToken("smoke"), asset.GetPath());

    GeoDetailBuilder b;
    std::vector<std::string> warnings;
    std::string err;
    ASSERT_TRUE(geoUsdReadPrim(stage->GetPrimAtPath(SdfPath("/vol")), UsdTimeCode::Default(), nullptr, b, &warnings, &err)) << err;
    const GeoDetail back = b.finish();
    ASSERT_EQ(back.primitiveCount(), GeoIndex(1));
    EXPECT_EQ(geoVDBName(back, 0), "density");
}

TEST(GeoVDBBridge, OverridingAnInputVolumeReauthorsItsFieldsAndBlocksDroppedOnes)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const UsdStageRefPtr input = inputVolumeStage(dir);

    GeoDetailBuilder read;
    std::vector<std::string> warnings;
    std::string err;
    ASSERT_TRUE(geoUsdReadPrim(input->GetPrimAtPath(SdfPath("/scene/vol")), UsdTimeCode::Default(), nullptr, read, &warnings, &err)) << err;
    const GeoDetailConstPtr both = std::make_shared<const GeoDetail>(read.finish());
    ASSERT_EQ(both->primitiveCount(), GeoIndex(2));
    const GeoAttribute* recorded = both->findAttribute(eGeoAttribClassPrimitive, kGeoUsdFieldAttr);
    ASSERT_NE(recorded, nullptr);
    EXPECT_EQ(recorded->getString(0), "dens");
    EXPECT_EQ(recorded->getString(1), "heat");

    SdfLayerRefPtr converted;
    const UsdStageRefPtr composed = composedOver(input, both, dir, &converted);
    ASSERT_TRUE(static_cast<bool>(composed));
    UsdVolVolume::FieldMap fields = UsdVolVolume(composed->GetPrimAtPath(SdfPath("/scene/vol"))).GetFieldPaths();
    ASSERT_EQ(fields.size(), 2u);
    EXPECT_EQ(fields[TfToken("dens")], SdfPath("/scene/vol/densityGrid"));
    EXPECT_EQ(fields[TfToken("heat")], SdfPath("/scene/vol/heatGrid"));
    EXPECT_FALSE(static_cast<bool>(converted->GetPrimAtPath(SdfPath("/scene/vol/density"))));

    // Only the density grid is left: the heat field must not outlive its grid.
    GeoDetailBuilder kept;
    geoAppendVDB(kept, geoVDBGrid(*both, 0), "density");
    GeoAttributeDesc pathDesc;
    pathDesc.name = "path";
    pathDesc.storage = eGeoStorageString;
    kept.addAttribute(eGeoAttribClassPrimitive, pathDesc).setString(0, "/scene/vol");
    GeoAttributeDesc sourceDesc;
    sourceDesc.name = kGeoUsdSourceAttr;
    sourceDesc.storage = eGeoStorageInt32;
    kept.addAttribute(eGeoAttribClassPrimitive, sourceDesc).set<std::int32_t>(0, 1);
    GeoAttributeDesc fieldDesc;
    fieldDesc.name = kGeoUsdFieldAttr;
    fieldDesc.storage = eGeoStorageString;
    kept.addAttribute(eGeoAttribClassPrimitive, fieldDesc).setString(0, "dens");
    const GeoDetailConstPtr single = std::make_shared<const GeoDetail>(kept.finish());

    SdfLayerRefPtr convertedSingle;
    const UsdStageRefPtr composedSingle = composedOver(input, single, dir, &convertedSingle);
    ASSERT_TRUE(static_cast<bool>(composedSingle));
    fields = UsdVolVolume(composedSingle->GetPrimAtPath(SdfPath("/scene/vol"))).GetFieldPaths();
    ASSERT_EQ(fields.size(), 1u);
    EXPECT_EQ(fields.begin()->first, TfToken("dens"));
    EXPECT_EQ(fields.begin()->second, SdfPath("/scene/vol/densityGrid"));
}

namespace {

// /scene/vol, a volume under a translated parent and translated itself, bound to one density grid.
UsdStageRefPtr
transformedVolumeStage(const QTemporaryDir& dir)
{
    const std::string file = dir.path().toStdString() + "/moved-density.vdb";
    EXPECT_TRUE(geoWriteVDBFile(*makeSphere(1.f), "density", file, nullptr));
    const UsdStageRefPtr stage = UsdStage::CreateInMemory("moved.usda");
    UsdGeomXform::Define(stage, SdfPath("/scene")).AddTranslateOp().Set(GfVec3d(10., 0., 0.));
    const UsdVolVolume volume = UsdVolVolume::Define(stage, SdfPath("/scene/vol"));
    volume.AddTranslateOp().Set(GfVec3d(0., 5., 0.));
    const UsdVolOpenVDBAsset asset = UsdVolOpenVDBAsset::Define(stage, SdfPath("/scene/vol/densityGrid"));
    asset.CreateFilePathAttr().Set(SdfAssetPath(file));
    asset.CreateFieldNameAttr().Set(TfToken("density"));
    volume.CreateFieldRelationship(TfToken("density"), asset.GetPath());

    return stage;
}

// The prim's grids read in world space, as their world-space bounds.
GeoBounds
worldBoundsOf(const UsdStageRefPtr& stage,
              const SdfPath& path)
{
    const UsdPrim prim = stage->GetPrimAtPath(path);
    UsdGeomXformCache xforms;
    const GfMatrix4d world = xforms.GetLocalToWorldTransform(prim);
    GeoDetailBuilder b;
    std::vector<std::string> warnings;
    std::string err;
    EXPECT_TRUE(geoUsdReadPrim(prim, UsdTimeCode::Default(), &world, b, &warnings, &err)) << err;
    const GeoDetail detail = b.finish();
    EXPECT_EQ(detail.primitiveCount(), GeoIndex(1));

    return detail.primitiveCount() > 0 ? geoVDBBounds(detail, 0) : GeoBounds();
}

void
expectSameBounds(const GeoBounds& a,
                 const GeoBounds& b)
{
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(a.min[i], b.min[i], 1e-6);
        EXPECT_NEAR(a.max[i], b.max[i], 1e-6);
    }
}

} // namespace

TEST(GeoVDBBridge, AVolumeKeepsItsPlaceUnderTransformedPrims)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const UsdStageRefPtr input = transformedVolumeStage(dir);
    const GeoBounds expected = worldBoundsOf(input, SdfPath("/scene/vol"));
    EXPECT_NEAR(expected.min[0] + expected.max[0], 20., 1e-3);
    EXPECT_NEAR(expected.min[1] + expected.max[1], 10., 1e-3);

    for (const bool worldSpace : { false, true }) {
        const UsdPrim prim = input->GetPrimAtPath(SdfPath("/scene/vol"));
        UsdGeomXformCache xforms;
        const GfMatrix4d world = xforms.GetLocalToWorldTransform(prim);
        GeoDetailBuilder b;
        std::vector<std::string> warnings;
        std::string err;
        ASSERT_TRUE(geoUsdReadPrim(prim, UsdTimeCode::Default(), worldSpace ? &world : nullptr, b, &warnings, &err)) << err;
        if (worldSpace) {
            GeoAttributeDesc desc;
            desc.name = kGeoUsdWorldSpaceAttr;
            desc.storage = eGeoStorageInt32;
            b.addAttribute(eGeoAttribClassDetail, desc).set<std::int32_t>(0, 1);
        }
        const GeoDetailConstPtr detail = std::make_shared<const GeoDetail>(b.finish());

        SdfLayerRefPtr converted;
        const UsdStageRefPtr composed = composedOver(input, detail, dir, &converted);
        ASSERT_TRUE(static_cast<bool>(composed));
        expectSameBounds(worldBoundsOf(composed, SdfPath("/scene/vol")), expected);
        // Out of world space the over leaves the input's transform alone.
        EXPECT_EQ(static_cast<bool>(converted->GetAttributeAtPath(SdfPath("/scene/vol.xformOpOrder"))), worldSpace) << "world space " << worldSpace;
    }
}

TEST(GeoVDBBridge, ANewVolumeOutOfWorldSpaceSitsInItsParentsFrame)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const GeoDetailConstPtr detail = makeVolume({ { makeSphere(1.f), "density" } });
    const GeoUsdConvert convert(detail, optionsIn(dir));
    const SdfPath path = volumePath(convert);
    const UsdStageRefPtr stage = UsdStage::Open(authored(convert));
    ASSERT_TRUE(static_cast<bool>(stage));
    UsdGeomXformCache xforms;
    EXPECT_EQ(xforms.GetLocalToWorldTransform(stage->GetPrimAtPath(path)), GfMatrix4d(1.));
}

TEST(GeoVDBBridge, AnUnnamedGridIsStoredAndNamedAsItsField)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const GeoDetailConstPtr detail = makeVolume({ { makeSphere(1.f), "" } });
    const GeoUsdConvert convert(detail, optionsIn(dir));
    const UsdStageRefPtr stage = UsdStage::Open(authored(convert));
    ASSERT_TRUE(static_cast<bool>(stage));
    const UsdVolVolume::FieldMap fields = UsdVolVolume(stage->GetPrimAtPath(volumePath(convert))).GetFieldPaths();
    ASSERT_EQ(fields.size(), 1u);
    EXPECT_EQ(fields.begin()->first, TfToken("field"));
    TfToken fieldName;
    UsdVolOpenVDBAsset(stage->GetPrimAtPath(fields.begin()->second)).GetFieldNameAttr().Get(&fieldName);
    EXPECT_EQ(fieldName, TfToken("field"));
    std::vector<std::string> names;
    std::string err;
    ASSERT_TRUE(geoVDBFileGridNames(fieldFile(stage, fields.begin()->second), &names, &err)) << err;
    EXPECT_EQ(names, std::vector<std::string> { "field" });
}

TEST(GeoVDBBridge, ReauthoredFieldsIndexTheirOwnFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const UsdStageRefPtr input = inputVolumeStage(dir);
    const openvdb::FloatGrid::Ptr first = makeSphere(0.5f);
    const openvdb::FloatGrid::Ptr second = makeSphere(1.f);
    ASSERT_TRUE(geoWriteVDBFile({ { first.get(), "density" }, { second.get(), "density" } }, dir.path().toStdString() + "/input-density.vdb", nullptr));
    UsdVolOpenVDBAsset(input->GetPrimAtPath(SdfPath("/scene/vol/densityGrid"))).CreateFieldIndexAttr().Set(1);

    GeoDetailBuilder read;
    std::vector<std::string> warnings;
    std::string err;
    ASSERT_TRUE(geoUsdReadPrim(input->GetPrimAtPath(SdfPath("/scene/vol")), UsdTimeCode::Default(), nullptr, read, &warnings, &err)) << err;
    const GeoDetailConstPtr both = std::make_shared<const GeoDetail>(read.finish());
    ASSERT_EQ(both->primitiveCount(), GeoIndex(2));
    GeoIndex density = 0;
    if (geoVDBName(*both, 1) == "density") {
        density = 1;
    }
    EXPECT_EQ(geoVDBGrid(*both, density)->activeVoxelCount(), second->activeVoxelCount());

    SdfLayerRefPtr converted;
    const UsdStageRefPtr composed = composedOver(input, both, dir, &converted);
    ASSERT_TRUE(static_cast<bool>(composed));
    int index = -1;
    UsdVolOpenVDBAsset(composed->GetPrimAtPath(SdfPath("/scene/vol/densityGrid"))).GetFieldIndexAttr().Get(&index);
    EXPECT_EQ(index, 0);
}

TEST(GeoVDBBridge, EnsuringVolumeFilesPutsBackOneThatIsGone)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const GeoDetailConstPtr detail = makeVolume({ { makeSphere(1.f), "density" } });
    const GeoUsdConvertOptions options = optionsIn(dir);
    EXPECT_FALSE(geoUsdEnsureVolumeFiles(*detail, options, []() { return true; }));
    ASSERT_TRUE(geoUsdEnsureVolumeFiles(*detail, options, GeoVDBAbortFunc()));

    const GeoUsdConvert convert(detail, options);
    const UsdStageRefPtr stage = UsdStage::Open(authored(convert));
    ASSERT_TRUE(static_cast<bool>(stage));
    const UsdVolVolume::FieldMap fields = UsdVolVolume(stage->GetPrimAtPath(volumePath(convert))).GetFieldPaths();
    ASSERT_EQ(fields.size(), 1u);
    const std::string file = fieldFile(stage, fields.begin()->second);
    ASSERT_TRUE(std::filesystem::exists(file));
    std::filesystem::remove(file);
    ASSERT_TRUE(geoUsdEnsureVolumeFiles(*detail, options, GeoVDBAbortFunc()));
    EXPECT_TRUE(std::filesystem::exists(file));
}

namespace {

// Packed copies of a density volume, on points spread and scaled apart.
GeoDetailConstPtr
packedVolumeCopies(int kCopies = 500)
{
    GeoDetailBuilder targets;
    const GeoOffset first = targets.appendPoints(kCopies);
    GeoAttribute& pscale = targets.addAttribute(eGeoAttribClassPoint, GeoAttributeDesc { "pscale", eGeoStorageFloat32, 1, eGeoTypeInfoNone, false });
    for (int i = 0; i < kCopies; ++i) {
        targets.setPointPosition(first + i, 3. * (i % 25), 3. * (i / 25), 0.5 * i);
        pscale.set<float>((std::size_t)(first + i), 1.f + 0.01f * i);
    }
    const GeoDetail target = targets.finish();

    GeoCopyToPointsParams params;
    params.packAndInstance = true;
    params.useTargetOrientations = false;
    params.useImplicitNormal = false;
    GeoDetailBuilder out;
    std::string error;
    EXPECT_TRUE(copyGeometryToPoints(makeVolume({ { makeSphere(1.f), "density" } }), target, params, out, &error)) << error;

    return std::make_shared<const GeoDetail>(out.finish());
}

std::size_t
filesIn(const QTemporaryDir& dir)
{
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir.path().toStdString())) {
        (void)entry;
        ++files;
    }

    return files;
}

} // namespace

TEST(GeoVDBBridge, PackedVolumeCopiesAreOnePrototypeOfAPointInstancer)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const GeoDetailConstPtr copies = packedVolumeCopies();
    ASSERT_EQ(copies->primitiveCount(), GeoIndex(500));
    const GeoUsdConvert convert(copies, optionsIn(dir));
    EXPECT_TRUE(convert.volumeWriteFailures().empty());
    SdfPath instancerPath;
    for (std::size_t i = 0; i < convert.partitionCount(); ++i) {
        EXPECT_NE(convert.partitionKind(i), eGeoUsdPrimVolume);
        if (convert.partitionKind(i) == eGeoUsdPrimInstancer) {
            EXPECT_TRUE(instancerPath.IsEmpty());
            instancerPath = convert.partitionPath(i);
        }
    }
    ASSERT_FALSE(instancerPath.IsEmpty());
    EXPECT_EQ(filesIn(dir), 1u);

    const UsdStageRefPtr stage = UsdStage::Open(authored(convert));
    ASSERT_TRUE(static_cast<bool>(stage));
    const UsdGeomPointInstancer instancer(stage->GetPrimAtPath(instancerPath));
    ASSERT_TRUE(static_cast<bool>(instancer));
    VtMatrix4dArray xforms;
    ASSERT_TRUE(instancer.ComputeInstanceTransformsAtTime(&xforms, UsdTimeCode::Default(), UsdTimeCode::Default()));
    ASSERT_EQ(xforms.size(), 500u);
    for (std::size_t i = 0; i < xforms.size(); ++i) {
        const GeoMatrix4 expected = geoPackedTransform(*copies, copies->primitiveMap().offsetFromIndex(GeoIndex(i)));
        const GeoMatrix4 got = geoMatrixFromUsd(xforms[i]);
        for (std::size_t k = 0; k < 16; ++k) {
            ASSERT_NEAR(got[k], expected[k], 1e-3 * (1. + std::abs(expected[k]))) << "instance " << i << " element " << k;
        }
    }

    SdfPathVector prototypes;
    instancer.GetPrototypesRel().GetTargets(&prototypes);
    ASSERT_EQ(prototypes.size(), 1u);
    std::vector<UsdVolVolume> volumes;
    for (const UsdPrim& prim : stage->Traverse()) {
        if (prim.IsA<UsdVolVolume>()) {
            volumes.emplace_back(prim);
        }
    }
    ASSERT_EQ(volumes.size(), 1u);
    EXPECT_TRUE(volumes[0].GetPath().HasPrefix(prototypes[0]));
    const UsdVolVolume::FieldMap fields = volumes[0].GetFieldPaths();
    ASSERT_EQ(fields.size(), 1u);
    EXPECT_TRUE(fields.begin()->second.HasPrefix(prototypes[0]));
    const UsdVolOpenVDBAsset asset(stage->GetPrimAtPath(fields.begin()->second));
    ASSERT_TRUE(static_cast<bool>(asset));
    TfToken fieldName;
    asset.GetFieldNameAttr().Get(&fieldName);
    EXPECT_EQ(fieldName, TfToken("density"));
    const std::string file = fieldFile(stage, fields.begin()->second);
    EXPECT_EQ(std::filesystem::path(file).parent_path(), std::filesystem::path(dir.path().toStdString()));
    EXPECT_TRUE(std::filesystem::exists(file));

    std::filesystem::remove(file);
    ASSERT_TRUE(geoUsdEnsureVolumeFiles(*copies, optionsIn(dir), GeoVDBAbortFunc()));
    EXPECT_TRUE(std::filesystem::exists(file));
    EXPECT_EQ(filesIn(dir), 1u);
}

TEST(GeoVDBBridge, UnpackedVolumeCopiesAreOneVolumeEachWithItsFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    constexpr int kCopies = 4;
    const GeoDetailConstPtr copies = packedVolumeCopies(kCopies);
    ASSERT_EQ(copies->primitiveCount(), GeoIndex(kCopies));
    std::vector<std::array<double, 3>> centres;
    for (int i = 0; i < kCopies; ++i) {
        centres.push_back(geoTransformPoint(geoPackedTransform(*copies, copies->primitiveMap().offsetFromIndex(GeoIndex(i))), { 0., 0., 0. }));
    }

    GeoUsdConvertOptions options = optionsIn(dir);
    options.packedMode = eGeoUsdPackedUnpack;
    const GeoUsdConvert convert(copies, options);
    EXPECT_TRUE(convert.volumeWriteFailures().empty());
    std::size_t volumePartitions = 0;
    for (std::size_t i = 0; i < convert.partitionCount(); ++i) {
        EXPECT_NE(convert.partitionKind(i), eGeoUsdPrimInstancer);
        if (convert.partitionKind(i) == eGeoUsdPrimVolume) {
            ++volumePartitions;
        }
    }
    EXPECT_EQ(volumePartitions, std::size_t(kCopies));

    const UsdStageRefPtr stage = UsdStage::Open(authored(convert));
    ASSERT_TRUE(static_cast<bool>(stage));
    std::vector<std::string> files;
    for (const UsdPrim& prim : stage->Traverse()) {
        if (!prim.IsA<UsdVolVolume>()) {
            continue;
        }
        const UsdVolVolume::FieldMap fields = UsdVolVolume(prim).GetFieldPaths();
        ASSERT_EQ(fields.size(), 1u) << prim.GetPath();
        files.push_back(fieldFile(stage, fields.begin()->second));
        EXPECT_TRUE(std::filesystem::exists(files.back())) << files.back();
    }
    ASSERT_EQ(files.size(), std::size_t(kCopies));

    // Each copy's file holds the grid where its copy was placed.
    std::vector<char> found(centres.size(), 0);
    for (const std::string& file : files) {
        GeoVDBGridBaseConstPtr grid;
        std::string error;
        ASSERT_TRUE(geoReadVDBGrid(file, "density", &grid, &error)) << error;
        const GeoBounds bounds = geoVDBGridBounds(*grid);
        for (std::size_t c = 0; c < centres.size(); ++c) {
            bool near = true;
            for (int k = 0; k < 3; ++k) {
                near = near && std::abs(0.5 * (bounds.min[k] + bounds.max[k]) - centres[c][k]) < 0.3;
            }
            if (near) {
                found[c] = 1;
            }
        }
    }
    for (std::size_t c = 0; c < centres.size(); ++c) {
        EXPECT_TRUE(found[c]) << "no volume file holds copy " << c;
    }
}
