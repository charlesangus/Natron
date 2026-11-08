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
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/GeoDetail.h"
#include "Engine/Nodes/Geometry/GeoUVProject.h"
#include "Engine/Nodes/Scene/GeoCameraProjection.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_USING

namespace {
// A camera at the origin looking down -Z with a square 36 mm aperture behind a 50 mm lens: at
// distance d its frame is d * 0.36 wide.
ScenePayload
cameraScene()
{
    PXR_NS::SdfLayerRefPtr layer = SceneOps::createNodeLayer("GeoUVProjectCameraTest");
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(layer);
    PXR_NS::UsdGeomCamera camera = PXR_NS::UsdGeomCamera::Define(stage, PXR_NS::SdfPath("/Cam"));
    camera.CreateFocalLengthAttr().Set(50.f);
    camera.CreateHorizontalApertureAttr().Set(36.f);
    camera.CreateVerticalApertureAttr().Set(36.f);

    return SceneOps::appendLayer(ScenePayload(), layer, 0x6e0c4a3ULL);
}

// One quad at z, spanning [-h, h] in X and Y.
GeoDetail
quad(double h,
     double z)
{
    GeoDetailBuilder b;
    const GeoOffset first = b.appendPoints(4);
    b.setPointPosition(first, -h, -h, z);
    b.setPointPosition(first + 1, h, -h, z);
    b.setPointPosition(first + 2, h, h, z);
    b.setPointPosition(first + 3, -h, h, z);
    const std::array<GeoOffset, 4> corners = { first, first + 1, first + 2, first + 3 };
    b.appendPrimitive(eGeoPrimitivePolygon, corners);

    return b.finish();
}
} // anonymous namespace

TEST(GeoUVProjectCamera, ACameraFacingQuadFillingTheFrameMapsToTheUnitSquare)
{
    const ScenePayload scene = cameraScene();
    ASSERT_FALSE(scene.isEmpty());
    EXPECT_EQ(std::vector<std::string> { "/Cam" }, GeoCameraProjection::cameraPaths(scene));

    GeoUVProjectParams params;
    params.projection = eGeoUVProjectionCamera;
    std::string usedPath;
    std::string error;
    ASSERT_TRUE(GeoCameraProjection::worldToFrame(scene, std::string(), 1., 24., 1., 0, &params.cameraWorldToFrame, &usedPath, &error))
        << error;
    EXPECT_EQ("/Cam", usedPath);

    const double z = -10.;
    GeoDetailBuilder b { quad(-z * 0.36 / 2., z) };
    ASSERT_TRUE(projectGeoUVs(b, params, &error)) << error;
    const GeoDetail out = b.finish();
    const GeoAttribute* uv = out.findAttribute(eGeoAttribClassVertex, "uv");
    ASSERT_NE(nullptr, uv);
    out.vertexMap().forEachLive([&](GeoIndex, GeoOffset vertex) {
        const std::array<double, 3> p = out.pointPosition(out.vertexPoint(vertex));
        EXPECT_NEAR(p[0] > 0. ? 1. : 0., uv->get<float>((std::size_t)vertex, 0), 1e-5);
        EXPECT_NEAR(p[1] > 0. ? 1. : 0., uv->get<float>((std::size_t)vertex, 1), 1e-5);
    });
}

TEST(GeoUVProjectCamera, AMissingCameraPathFails)
{
    std::array<double, 16> matrix {};
    std::string usedPath;
    std::string error;
    EXPECT_FALSE(GeoCameraProjection::worldToFrame(cameraScene(), "/Nowhere", 1., 24., 1., 0, &matrix, &usedPath, &error));
    EXPECT_FALSE(error.empty());
}
