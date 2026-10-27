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

#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Gui/SceneGuides.h"

#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdLux/cylinderLight.h>
#include <pxr/usd/usdLux/diskLight.h>

NATRON_NAMESPACE_USING

using PXR_NS::GfVec2d;
using PXR_NS::GfVec3d;

namespace {
const double kLength = 10.;
const GfVec3d kCameraPosition(1., 2., 3.);

bool
hasPoint(const std::vector<GfVec3d>& points,
         const GfVec3d& p)
{
    for (const GfVec3d& q : points) {
        if ((q - p).GetLength() < 1e-6) {
            return true;
        }
    }

    return false;
}

PXR_NS::UsdGeomCamera
defineCamera(const PXR_NS::UsdStageRefPtr& stage,
             const std::string& path,
             bool orthographic)
{
    PXR_NS::UsdGeomCamera camera = PXR_NS::UsdGeomCamera::Define(stage, PXR_NS::SdfPath(path));

    camera.AddTranslateOp().Set(kCameraPosition);
    if (orthographic) {
        camera.CreateProjectionAttr().Set(PXR_NS::UsdGeomTokens->orthographic);
        // 20 by 10 tenths of a unit: a 2 by 1 box.
        camera.CreateHorizontalApertureAttr().Set(20.f);
        camera.CreateVerticalApertureAttr().Set(10.f);
    } else {
        camera.CreateFocalLengthAttr().Set(50.f);
        camera.CreateHorizontalApertureAttr().Set(36.f);
        camera.CreateVerticalApertureAttr().Set(24.f);
    }

    return camera;
}

std::vector<SceneGuides::Guide>
guidesOf(const PXR_NS::UsdStageRefPtr& stage)
{
    return SceneGuides::buildGuides(stage, SceneGuides::findGuidePrims(stage), PXR_NS::UsdTimeCode::Default(), kLength);
}
} // anonymous namespace

TEST(SceneGuides, PerspectiveCameraFrustumMatchesGfCamera)
{
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::CreateInMemory();
    PXR_NS::UsdGeomCamera camera = defineCamera(stage, "/Cam", false);
    const std::vector<SceneGuides::Guide> guides = guidesOf(stage);

    ASSERT_EQ(guides.size(), 1u);
    EXPECT_EQ(guides[0].path, "/Cam");
    EXPECT_EQ(guides[0].kind, SceneGuides::eGuideKindCamera);
    const std::vector<GfVec3d>& segments = guides[0].segments;
    ASSERT_FALSE(segments.empty());
    EXPECT_EQ(segments.size() % 2, 0u);

    PXR_NS::GfFrustum frustum = camera.GetCamera(PXR_NS::UsdTimeCode::Default()).GetFrustum();
    frustum.SetNearFar(PXR_NS::GfRange1d(1., kLength));
    const std::vector<GfVec3d> corners = frustum.ComputeCorners();
    ASSERT_EQ(corners.size(), 8u);
    for (int i = 4; i < 8; ++i) {
        EXPECT_TRUE(hasPoint(segments, corners[i])) << "far corner " << i;
    }
    // A 50 mm lens on a 36x24 back spans 36/50 by 24/50 of the distance.
    const double halfWidth = kLength * 18. / 50.;
    const double halfHeight = kLength * 12. / 50.;
    EXPECT_TRUE(hasPoint(segments, kCameraPosition + GfVec3d(-halfWidth, -halfHeight, -kLength)));
    EXPECT_TRUE(hasPoint(segments, kCameraPosition + GfVec3d(halfWidth, halfHeight, -kLength)));
    EXPECT_TRUE(hasPoint(segments, kCameraPosition));
    bool hasLights = true;
    SceneGuides::findGuidePrims(stage, &hasLights);
    EXPECT_FALSE(hasLights);
}

TEST(SceneGuides, OrthographicCameraIsABox)
{
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::CreateInMemory();
    defineCamera(stage, "/Ortho", true);
    const std::vector<SceneGuides::Guide> guides = guidesOf(stage);

    ASSERT_EQ(guides.size(), 1u);
    const std::vector<GfVec3d>& segments = guides[0].segments;
    for (int i = 0; i < 8; ++i) {
        const GfVec3d corner((i & 1) ? 1. : -1., (i & 2) ? 0.5 : -0.5, (i & 4) ? -kLength : 0.);
        EXPECT_TRUE(hasPoint(segments, kCameraPosition + corner)) << "box corner " << i;
    }
    // The four edges along the view are parallel: no apex.
    int depthEdges = 0;
    for (std::size_t k = 0; k + 1 < segments.size(); k += 2) {
        const GfVec3d d = segments[k + 1] - segments[k];
        if ((std::abs(d[0]) < 1e-9) && (std::abs(d[1]) < 1e-9) && (std::abs(std::abs(d[2]) - kLength) < 1e-9)) {
            ++depthEdges;
        }
    }
    EXPECT_EQ(depthEdges, 4);
    EXPECT_FALSE(hasPoint(segments, kCameraPosition + GfVec3d(0., 0., -kLength)));
}

TEST(SceneGuides, EachLightTypeHasLinesAtItsPosition)
{
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::CreateInMemory();
    struct Expected {
        const char* path;
        SceneGuides::GuideKindEnum kind;
        GfVec3d position;
    };
    const std::vector<Expected> expected = {
        { "/Distant", SceneGuides::eGuideKindDistantLight, GfVec3d(5., 0., -2.) },
        { "/Sphere", SceneGuides::eGuideKindSphereLight, GfVec3d(5., 10., -2.) },
        { "/Rect", SceneGuides::eGuideKindRectLight, GfVec3d(5., 20., -2.) },
        { "/Disk", SceneGuides::eGuideKindDiskLight, GfVec3d(5., 30., -2.) },
        { "/Cylinder", SceneGuides::eGuideKindCylinderLight, GfVec3d(5., 40., -2.) },
        { "/Dome", SceneGuides::eGuideKindDomeLight, GfVec3d(5., 50., -2.) },
    };
    PXR_NS::UsdLuxDistantLight::Define(stage, PXR_NS::SdfPath("/Distant"));
    PXR_NS::UsdLuxSphereLight::Define(stage, PXR_NS::SdfPath("/Sphere")).CreateRadiusAttr().Set(2.f);
    PXR_NS::UsdLuxRectLight::Define(stage, PXR_NS::SdfPath("/Rect"));
    PXR_NS::UsdLuxDiskLight::Define(stage, PXR_NS::SdfPath("/Disk"));
    PXR_NS::UsdLuxCylinderLight::Define(stage, PXR_NS::SdfPath("/Cylinder"));
    PXR_NS::UsdLuxDomeLight::Define(stage, PXR_NS::SdfPath("/Dome"));
    for (const Expected& e : expected) {
        PXR_NS::UsdGeomXformable(stage->GetPrimAtPath(PXR_NS::SdfPath(e.path))).AddTranslateOp().Set(e.position);
    }
    // A mesh carrying the light API is drawn by Storm; an invisible light gets no guide.
    PXR_NS::UsdGeomMesh mesh = PXR_NS::UsdGeomMesh::Define(stage, PXR_NS::SdfPath("/MeshLight"));
    PXR_NS::UsdLuxLightAPI::Apply(mesh.GetPrim());
    PXR_NS::UsdLuxSphereLight hidden = PXR_NS::UsdLuxSphereLight::Define(stage, PXR_NS::SdfPath("/Hidden"));
    hidden.CreateVisibilityAttr().Set(PXR_NS::UsdGeomTokens->invisible);

    const std::vector<SceneGuides::Guide> guides = guidesOf(stage);
    ASSERT_EQ(guides.size(), expected.size());
    bool hasLights = false;
    SceneGuides::findGuidePrims(stage, &hasLights);
    EXPECT_TRUE(hasLights);
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const SceneGuides::Guide& guide = guides[i];
        EXPECT_EQ(guide.path, expected[i].path);
        EXPECT_EQ(guide.kind, expected[i].kind) << guide.path;
        ASSERT_FALSE(guide.segments.empty()) << guide.path;
        PXR_NS::GfRange3d box;
        for (const GfVec3d& p : guide.segments) {
            box.UnionWith(p);
        }
        EXPECT_TRUE(box.Contains(expected[i].position)) << guide.path;
        EXPECT_GT(box.GetSize().GetLength(), 0.) << guide.path;
        EXPECT_LT((box.GetMidpoint() - expected[i].position).GetLength(), kLength) << guide.path;
    }
    // The sphere's guide is its radius.
    EXPECT_TRUE(hasPoint(guides[1].segments, expected[1].position + GfVec3d(2., 0., 0.)));
}

TEST(SceneGuides, HitTestAndRect)
{
    PXR_NS::GfFrustum frustum;
    frustum.SetPerspective(60., 1., 0.1, 100.);
    const SceneGizmo::View view(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix(), 200, 200);

    SceneGuides::Guide line;
    line.path = "/Line";
    line.segments = { GfVec3d(-1., 0., -5.), GfVec3d(1., 0., -5.) };
    SceneGuides::Guide far = line;
    far.path = "/Far";
    far.segments = { GfVec3d(-1., 5., -5.), GfVec3d(1., 5., -5.) };
    const std::vector<SceneGuides::Guide> guides = { line, far };

    GfVec3d world;
    EXPECT_EQ(SceneGuides::hitTest(view, guides, GfVec2d(100., 102.), "", &world), 0);
    EXPECT_LT((world - GfVec3d(0., 0., -5.)).GetLength(), 0.1);
    EXPECT_EQ(SceneGuides::hitTest(view, guides, GfVec2d(100., 120.), "", &world), -1);
    EXPECT_EQ(SceneGuides::hitTest(view, guides, GfVec2d(100., 100.), "/Line", &world), -1);

    const std::vector<int> inRect = SceneGuides::guidesInRect(view, guides, GfVec2d(90., 90.), GfVec2d(110., 110.), "");
    ASSERT_EQ(inRect.size(), 1u);
    EXPECT_EQ(inRect[0], 0);
    EXPECT_TRUE(SceneGuides::guidesInRect(view, guides, GfVec2d(0., 150.), GfVec2d(50., 199.), "").empty());
}
