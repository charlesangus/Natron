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

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include "Engine/Geo/GeoOpenVDB.h"

#include "Engine/Geo/GeoVDB.h"
#include "Engine/Nodes/Geometry/GeoUsdConvert.h"

#include <pxr/usd/sdf/assetPath.h>
#include <pxr/usd/usd/stage.h>
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

TEST(GeoVDBBridge, SameNamedGridsGetSuffixedFields)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const GeoUsdConvert convert(makeVolume({ { makeSphere(1.f), "density" }, { makeSphere(2.f), "density" } }), optionsIn(dir));
    const SdfPath path = volumePath(convert);
    const UsdStageRefPtr stage = UsdStage::Open(authored(convert));
    ASSERT_TRUE(static_cast<bool>(stage));

    const UsdVolVolume::FieldMap fields = UsdVolVolume(stage->GetPrimAtPath(path)).GetFieldPaths();
    ASSERT_EQ(fields.size(), 2u);
    EXPECT_EQ(fields.count(TfToken("density")), 1u);
    EXPECT_EQ(fields.count(TfToken("density_1")), 1u);
    EXPECT_EQ(convert.volumeNotes().size(), 1u);
    EXPECT_TRUE(convert.volumeWriteFailures().empty());

    GeoDetailBuilder b;
    std::vector<std::string> warnings;
    std::string err;
    ASSERT_TRUE(geoUsdReadPrim(stage->GetPrimAtPath(path), UsdTimeCode::Default(), nullptr, b, &warnings, &err)) << err;
    const GeoDetail back = b.finish();
    ASSERT_EQ(back.primitiveCount(), GeoIndex(2));
    EXPECT_EQ(geoVDBName(back, 0), "density");
    EXPECT_EQ(geoVDBName(back, 1), "density");
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
    UsdVolOpenVDBAsset(input->GetPrimAtPath(SdfPath("/scene/vol/densityGrid"))).CreateFieldIndexAttr().Set(1);

    GeoDetailBuilder read;
    std::vector<std::string> warnings;
    std::string err;
    ASSERT_TRUE(geoUsdReadPrim(input->GetPrimAtPath(SdfPath("/scene/vol")), UsdTimeCode::Default(), nullptr, read, &warnings, &err)) << err;
    const GeoDetailConstPtr both = std::make_shared<const GeoDetail>(read.finish());

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
