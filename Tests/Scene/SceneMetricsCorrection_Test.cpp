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

#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_USING

namespace {
PXR_NS::SdfLayerRefPtr
layerFromString(const std::string& text)
{
    PXR_NS::SdfLayerRefPtr layer = PXR_NS::SdfLayer::CreateAnonymous("SceneMetricsCorrectionTest.usda");

    EXPECT_TRUE(layer->ImportFromString(text));

    return layer;
}

// A file layer and the stronger layer correcting it, as USDRead stacks them.
struct Corrected {
    PXR_NS::SdfLayerRefPtr file;
    PXR_NS::SdfLayerRefPtr layer;
    PXR_NS::UsdStageRefPtr stage;
    std::size_t correctedPrims = 0;
};

Corrected
correct(const PXR_NS::SdfLayerRefPtr& file,
        const SceneMetricsCorrection& correction)
{
    Corrected result;

    result.file = file;
    result.layer = SceneOps::createNodeLayer("SceneMetricsCorrectionTest");
    result.layer->InsertSubLayerPath(file->GetIdentifier());
    const PXR_NS::UsdStageRefPtr source = PXR_NS::UsdStage::Open(file, PXR_NS::SdfLayerHandle(), PXR_NS::UsdStage::LoadAll);
    EXPECT_TRUE(bool(source));
    result.correctedPrims = SceneOps::authorMetricsCorrection(result.layer, source, correction);
    result.stage = PXR_NS::UsdStage::Open(result.layer, PXR_NS::SdfLayerHandle(), PXR_NS::UsdStage::LoadAll);

    return result;
}

SceneMetricsCorrection
zCentimetresToYMetres(bool upAxis,
                      bool units)
{
    SceneMetricsCorrection correction;

    correction.fromUpAxis = SceneMetadata::eUpAxisZ;
    correction.fromMetersPerUnit = 0.01;
    correction.toUpAxis = SceneMetadata::eUpAxisY;
    correction.toMetersPerUnit = 1.;
    correction.correctUpAxis = upAxis;
    correction.correctUnits = units;

    return correction;
}

PXR_NS::GfVec3d
worldTranslation(const PXR_NS::UsdStageRefPtr& stage,
                 const std::string& path)
{
    PXR_NS::UsdGeomXformCache cache(PXR_NS::UsdTimeCode::Default());

    return cache.GetLocalToWorldTransform(stage->GetPrimAtPath(PXR_NS::SdfPath(path))).ExtractTranslation();
}

PXR_NS::GfRange3d
worldBounds(const PXR_NS::UsdStageRefPtr& stage,
            const std::string& path)
{
    PXR_NS::UsdGeomBBoxCache cache(PXR_NS::UsdTimeCode::Default(), { PXR_NS::UsdGeomTokens->default_ });

    return cache.ComputeWorldBound(stage->GetPrimAtPath(PXR_NS::SdfPath(path))).ComputeAlignedRange();
}

void
expectNear(const PXR_NS::GfVec3d& expected,
           const PXR_NS::GfVec3d& actual)
{
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(expected[i], actual[i], 1e-9) << "component " << i;
    }
}

std::vector<std::string>
composedOrder(const PXR_NS::UsdStageRefPtr& stage,
              const std::string& path)
{
    PXR_NS::VtTokenArray order;
    std::vector<std::string> names;

    PXR_NS::UsdGeomXformable(stage->GetPrimAtPath(PXR_NS::SdfPath(path))).GetXformOpOrderAttr().Get(&order);
    for (const PXR_NS::TfToken& name : order) {
        names.push_back(name.GetString());
    }

    return names;
}

const char* const kRotate = "xformOp:rotateX:upAxisCorrection";
const char* const kScale = "xformOp:scale:unitsCorrection";

// An Xform root, a Scope root holding a mesh, and a mesh with points from (0, 0, 0) to (1, 2, 3).
const char* const kRoots = "#usda 1.0\n"
                           "def Xform \"World\"\n"
                           "{\n"
                           "    double3 xformOp:translate = (1, 2, 3)\n"
                           "    uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                           "}\n"
                           "def Scope \"Props\"\n"
                           "{\n"
                           "    def Mesh \"Box\"\n"
                           "    {\n"
                           "        point3f[] points = [(0, 0, 0), (1, 2, 3)]\n"
                           "        int[] faceVertexCounts = [2]\n"
                           "        int[] faceVertexIndices = [0, 1]\n"
                           "    }\n"
                           "}\n";
} // anonymous namespace

TEST(SceneMetricsCorrectionTest, AnglesAndScalesFollowTheMetrics)
{
    SceneMetricsCorrection correction = zCentimetresToYMetres(true, true);

    EXPECT_EQ(-90., SceneOps::getMetricsCorrectionAngle(correction));
    EXPECT_DOUBLE_EQ(0.01, SceneOps::getMetricsCorrectionScale(correction));
    std::swap(correction.fromUpAxis, correction.toUpAxis);
    std::swap(correction.fromMetersPerUnit, correction.toMetersPerUnit);
    EXPECT_EQ(90., SceneOps::getMetricsCorrectionAngle(correction));
    EXPECT_DOUBLE_EQ(100., SceneOps::getMetricsCorrectionScale(correction));

    correction.correctUpAxis = false;
    correction.correctUnits = false;
    EXPECT_EQ(0., SceneOps::getMetricsCorrectionAngle(correction));
    EXPECT_EQ(1., SceneOps::getMetricsCorrectionScale(correction));

    // Within UsdGeomLinearUnitsAre's tolerance the units are the same.
    correction.correctUnits = true;
    correction.fromMetersPerUnit = 0.01;
    correction.toMetersPerUnit = 0.01 * (1. + 1e-7);
    EXPECT_EQ(1., SceneOps::getMetricsCorrectionScale(correction));
}

TEST(SceneMetricsCorrectionTest, ZUpRotatesToYUpOnAnXformRootAndAMeshUnderAScope)
{
    const Corrected c = correct(layerFromString(kRoots), zCentimetresToYMetres(true, false));

    ASSERT_TRUE(bool(c.stage));
    EXPECT_EQ((std::size_t)2, c.correctedPrims);
    // -90 degrees about X: (x, y, z) becomes (x, z, -y).
    expectNear(PXR_NS::GfVec3d(1, 3, -2), worldTranslation(c.stage, "/World"));
    const PXR_NS::GfRange3d box = worldBounds(c.stage, "/Props/Box");
    expectNear(PXR_NS::GfVec3d(0, 0, -2), box.GetMin());
    expectNear(PXR_NS::GfVec3d(1, 3, 0), box.GetMax());
    // The Scope itself is not xformable and gets nothing; its mesh carries the correction.
    const PXR_NS::SdfPrimSpecHandle props = c.layer->GetPrimAtPath(PXR_NS::SdfPath("/Props"));
    ASSERT_TRUE(bool(props));
    EXPECT_EQ((std::size_t)0, props->GetAttributes().size());
    EXPECT_EQ(std::vector<std::string>({ kRotate }), composedOrder(c.stage, "/Props/Box"));
    // The file is never edited.
    EXPECT_FALSE(bool(c.file->GetAttributeAtPath(PXR_NS::SdfPath("/World.xformOp:rotateX:upAxisCorrection"))));
}

TEST(SceneMetricsCorrectionTest, YUpRotatesToZUp)
{
    SceneMetricsCorrection correction = zCentimetresToYMetres(true, false);

    std::swap(correction.fromUpAxis, correction.toUpAxis);
    const Corrected c = correct(layerFromString(kRoots), correction);
    ASSERT_TRUE(bool(c.stage));
    // +90 degrees about X: (x, y, z) becomes (x, -z, y).
    expectNear(PXR_NS::GfVec3d(1, -3, 2), worldTranslation(c.stage, "/World"));
    const PXR_NS::GfRange3d box = worldBounds(c.stage, "/Props/Box");
    expectNear(PXR_NS::GfVec3d(0, -3, 0), box.GetMin());
    expectNear(PXR_NS::GfVec3d(1, 0, 2), box.GetMax());
}

TEST(SceneMetricsCorrectionTest, CentimetresScaleToMetres)
{
    const Corrected before = correct(layerFromString(kRoots), zCentimetresToYMetres(false, false));
    const Corrected c = correct(layerFromString(kRoots), zCentimetresToYMetres(false, true));

    ASSERT_TRUE(before.stage && c.stage);
    expectNear(PXR_NS::GfVec3d(0.01, 0.02, 0.03), worldTranslation(c.stage, "/World"));
    const PXR_NS::GfRange3d original = worldBounds(before.stage, "/Props/Box");
    const PXR_NS::GfRange3d scaled = worldBounds(c.stage, "/Props/Box");
    expectNear(original.GetMin() * 0.01, scaled.GetMin());
    expectNear(original.GetMax() * 0.01, scaled.GetMax());
    EXPECT_EQ(std::vector<std::string>({ kScale, "xformOp:translate" }), composedOrder(c.stage, "/World"));
}

TEST(SceneMetricsCorrectionTest, AnOrderAuthoredInASublayerIsKeptAfterThePrependedOps)
{
    PXR_NS::SdfLayerRefPtr base = layerFromString("#usda 1.0\n"
                                                  "over \"Sub\"\n"
                                                  "{\n"
                                                  "    double3 xformOp:translate = (0, 0, 5)\n"
                                                  "    float xformOp:rotateZ = 30\n"
                                                  "    uniform token[] xformOpOrder = [\"xformOp:translate\", \"xformOp:rotateZ\"]\n"
                                                  "}\n");
    PXR_NS::SdfLayerRefPtr file = layerFromString("#usda 1.0\ndef Xform \"Sub\"\n{\n}\n");
    file->InsertSubLayerPath(base->GetIdentifier());

    const Corrected c = correct(file, zCentimetresToYMetres(true, true));
    ASSERT_TRUE(bool(c.stage));
    EXPECT_EQ(std::vector<std::string>({ kRotate, kScale, "xformOp:translate", "xformOp:rotateZ" }), composedOrder(c.stage, "/Sub"));
    expectNear(PXR_NS::GfVec3d(0, 0.05, 0), worldTranslation(c.stage, "/Sub"));
}

TEST(SceneMetricsCorrectionTest, AResetStaysFirstAndAResettingDescendantGetsItsOwnCorrection)
{
    const Corrected c = correct(layerFromString("#usda 1.0\n"
                                                "def Xform \"Root\"\n"
                                                "{\n"
                                                "    double3 xformOp:translate = (0, 0, 1)\n"
                                                "    uniform token[] xformOpOrder = [\"!resetXformStack!\", \"xformOp:translate\"]\n"
                                                "    def Xform \"Child\"\n"
                                                "    {\n"
                                                "        double3 xformOp:translate = (0, 0, 2)\n"
                                                "        uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
                                                "    }\n"
                                                "    def Xform \"Escaped\"\n"
                                                "    {\n"
                                                "        double3 xformOp:translate = (0, 0, 3)\n"
                                                "        uniform token[] xformOpOrder = [\"!resetXformStack!\", \"xformOp:translate\"]\n"
                                                "    }\n"
                                                "}\n"),
                                zCentimetresToYMetres(true, false));

    ASSERT_TRUE(bool(c.stage));
    EXPECT_EQ((std::size_t)2, c.correctedPrims);
    EXPECT_EQ(std::vector<std::string>({ "!resetXformStack!", kRotate, "xformOp:translate" }), composedOrder(c.stage, "/Root"));
    // Under a corrected parent: nothing of its own.
    EXPECT_EQ(std::vector<std::string>({ "xformOp:translate" }), composedOrder(c.stage, "/Root/Child"));
    EXPECT_EQ(std::vector<std::string>({ "!resetXformStack!", kRotate, "xformOp:translate" }), composedOrder(c.stage, "/Root/Escaped"));
    expectNear(PXR_NS::GfVec3d(0, 1, 0), worldTranslation(c.stage, "/Root"));
    expectNear(PXR_NS::GfVec3d(0, 3, 0), worldTranslation(c.stage, "/Root/Child"));
    expectNear(PXR_NS::GfVec3d(0, 3, 0), worldTranslation(c.stage, "/Root/Escaped"));
}

TEST(SceneMetricsCorrectionTest, AnOpNameThePrimHasTakesAFreeSuffix)
{
    const Corrected c = correct(layerFromString("#usda 1.0\n"
                                                "def Xform \"World\"\n"
                                                "{\n"
                                                "    double xformOp:rotateX:upAxisCorrection = 90\n"
                                                "    uniform token[] xformOpOrder = [\"xformOp:rotateX:upAxisCorrection\"]\n"
                                                "}\n"),
                                zCentimetresToYMetres(true, false));

    ASSERT_TRUE(bool(c.stage));
    EXPECT_EQ(std::vector<std::string>({ std::string(kRotate) + "_2", kRotate }), composedOrder(c.stage, "/World"));
    PXR_NS::UsdGeomXformCache cache(PXR_NS::UsdTimeCode::Default());
    EXPECT_TRUE(PXR_NS::GfIsClose(cache.GetLocalToWorldTransform(c.stage->GetPrimAtPath(PXR_NS::SdfPath("/World"))), PXR_NS::GfMatrix4d(1.), 1e-9));
}

TEST(SceneMetricsCorrectionTest, CameraLengthsAreRescaledWithTheirTimeSamples)
{
    const char* const camera = "#usda 1.0\n"
                               "def Camera \"Cam\"\n"
                               "{\n"
                               "    float2 clippingRange = (1, 100000)\n"
                               "    float2 clippingRange.timeSamples = { 1: (1, 1000), 10: (2, 2000) }\n"
                               "    float focusDistance.timeSamples = { 1: 500, 10: 800 }\n"
                               "    float focalLength = 35\n"
                               "    float horizontalAperture = 36\n"
                               "}\n";
    const Corrected c = correct(layerFromString(camera), zCentimetresToYMetres(false, true));

    ASSERT_TRUE(bool(c.stage));
    const PXR_NS::UsdGeomCamera cam(c.stage->GetPrimAtPath(PXR_NS::SdfPath("/Cam")));
    ASSERT_TRUE(bool(cam));
    PXR_NS::GfVec2f range;
    ASSERT_TRUE(cam.GetClippingRangeAttr().Get(&range, PXR_NS::UsdTimeCode::Default()));
    EXPECT_FLOAT_EQ(0.01f, range[0]);
    EXPECT_FLOAT_EQ(1000.f, range[1]);
    ASSERT_TRUE(cam.GetClippingRangeAttr().Get(&range, 10.));
    EXPECT_FLOAT_EQ(0.02f, range[0]);
    EXPECT_FLOAT_EQ(20.f, range[1]);
    float value = 0.f;
    ASSERT_TRUE(cam.GetFocusDistanceAttr().Get(&value, 1.));
    EXPECT_FLOAT_EQ(5.f, value);
    ASSERT_TRUE(cam.GetFocusDistanceAttr().Get(&value, 10.));
    EXPECT_FLOAT_EQ(8.f, value);
    // Only the samples the file has: no default is invented for focusDistance.
    EXPECT_NE(PXR_NS::UsdResolveInfoSourceDefault, cam.GetFocusDistanceAttr().GetResolveInfo(PXR_NS::UsdTimeCode::Default()).GetSource());
    ASSERT_TRUE(cam.GetFocalLengthAttr().Get(&value));
    EXPECT_FLOAT_EQ(0.35f, value);
    ASSERT_TRUE(cam.GetHorizontalApertureAttr().Get(&value));
    EXPECT_FLOAT_EQ(0.36f, value);

    // Sample times move into the correcting layer's time by the offset it sublayers the file with.
    SceneMetricsCorrection offset = zCentimetresToYMetres(false, true);
    offset.sourceToLayer = PXR_NS::SdfLayerOffset(5.);
    const Corrected shifted = correct(layerFromString(camera), offset);
    const std::set<double> times = shifted.layer->ListTimeSamplesForPath(PXR_NS::SdfPath("/Cam.focusDistance"));
    EXPECT_EQ(std::set<double>({ 6., 15. }), times);
}

TEST(SceneMetricsCorrectionTest, MatchingMetricsOrNoToggleAuthorNothing)
{
    SceneMetricsCorrection matching = zCentimetresToYMetres(true, true);

    matching.toUpAxis = matching.fromUpAxis;
    matching.toMetersPerUnit = matching.fromMetersPerUnit;
    const Corrected same = correct(layerFromString(kRoots), matching);
    EXPECT_EQ((std::size_t)0, same.correctedPrims);
    EXPECT_TRUE(same.layer->GetRootPrims().empty());

    const Corrected off = correct(layerFromString(kRoots), zCentimetresToYMetres(false, false));
    EXPECT_EQ((std::size_t)0, off.correctedPrims);
    EXPECT_TRUE(off.layer->GetRootPrims().empty());
}
