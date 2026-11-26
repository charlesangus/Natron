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

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include <pxr/usd/usdGeom/cube.h>

#include "BaseTest.h"
#include "CyclesTestUtils.h"

#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Geometry/GeoCopyToPoints.h"
#include "Engine/Nodes/Geometry/GeoPrimitive.h"
#include "Engine/Nodes/Geometry/GeoToUSD.h"
#include "Engine/Nodes/Geometry/GeoWrangle.h"
#include "Engine/Nodes/Scene/CyclesAovs.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/SceneLoadSet.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

/**
 * Instances rendered through the Cycles delegate land where USD puts them: each copy is scaled
 * and rotated about its own origin before it moves to its point, and only then does the
 * instancer's transform apply. Composed the other way round, a copy at point p with scale s
 * lands near s * p, so every copy of a small prototype bunches up at the origin.
 *
 * The scenes are seen by an orthographic camera at z = 10 looking down -Z with a 10 x 10 window,
 * rendered kSize x kSize, so a scene unit is kSize / 10 pixels.
 **/
namespace {
const int kSize = 128;
const double kWindow = 10.;
const double kCameraZ = 10.;
const char* const kCameraPath = "/Cam";
// Distinct from any hash the geometry layers are appended under.
const U64 kInstancerLayerHash = 0x4d36313601ull;
const U64 kCameraLayerHash = 0x4d36313602ull;
const float kCovered = 0.5f;

// The prototype box is 4 x 2 x 2, so a copy scaled by 0.25 is 1 unit wide, half a unit tall and
// deep, its front face at z = 0.25.
const double kInstanceScale = 0.25;
const double kHalfLength = 0.5;
const double kHalfWidth = 0.25;
const double kFrontZ = 0.25;
// Probes kProbe along each of a copy's axes: inside its half-length, outside its half-width, each
// more than a pixel filter's width from the edge.
const double kProbe = 0.4;

struct Expected {
    PXR_NS::GfVec3d centre; // in world space
    bool rotated = false; // 90 degrees about Z, so the long side runs along Y
};

// A layer holding only the orthographic camera.
PXR_NS::SdfLayerRefPtr
cameraLayer()
{
    const PXR_NS::SdfLayerRefPtr layer = SceneOps::createNodeLayer("camera");
    if (!layer) {
        return layer;
    }
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(layer);
    PXR_NS::UsdGeomCamera camera = PXR_NS::UsdGeomCamera::Define(stage, PXR_NS::SdfPath(kCameraPath));
    camera.AddTranslateOp().Set(PXR_NS::GfVec3d(0., 0., kCameraZ));
    camera.CreateProjectionAttr(PXR_NS::VtValue(PXR_NS::UsdGeomTokens->orthographic));
    // An orthographic aperture is in tenths of a scene unit.
    camera.CreateHorizontalApertureAttr(PXR_NS::VtValue(float(kWindow * 10.)));
    camera.CreateVerticalApertureAttr(PXR_NS::VtValue(float(kWindow * 10.)));

    return layer;
}

// The camera's request for scene at frame 1, kSize x kSize, with Cycles at a fixed sample count.
bool
cyclesRequest(const ScenePayload& scene,
              const PXR_NS::SdfLayerRefPtr& camera,
              const void* owner,
              HydraRender::Request* request)
{
    std::string error;
    request->owner = owner;
    request->scene = scene;
    request->time = PXR_NS::UsdTimeCode(1.);
    request->width = kSize;
    request->height = kSize;
    request->renderer = kRenderDelegateCyclesId;
    request->settings = CyclesTestUtils::fixedSampling(4);
    const PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(camera);
    if (!HydraRender::cameraFrustum(stage, PXR_NS::SdfPath(kCameraPath), request->time, 1., HydraRender::eFitModeFill, &request->frustum, &error)) {
        ADD_FAILURE() << error;

        return false;
    }

    return true;
}

// Where world lands in the render, in pixels, rows from the bottom as Natron's are.
PXR_NS::GfVec2d
project(const HydraRender::Request& request,
        const PXR_NS::GfVec3d& world)
{
    const PXR_NS::GfMatrix4d toClip = request.frustum.ComputeViewMatrix() * request.frustum.ComputeProjectionMatrix();
    const PXR_NS::GfVec3d ndc = toClip.Transform(world);

    return PXR_NS::GfVec2d((ndc[0] + 1.) * 0.5 * request.width, (ndc[1] + 1.) * 0.5 * request.height);
}

std::size_t
pixelAt(const PXR_NS::GfVec2d& pixel)
{
    const int x = std::clamp((int)std::floor(pixel[0]), 0, kSize - 1);
    const int y = std::clamp((int)std::floor(pixel[1]), 0, kSize - 1);

    return (std::size_t)y * (std::size_t)kSize + (std::size_t)x;
}

bool
coveredAt(const HydraRender::Result& result,
          const PXR_NS::GfVec2d& pixel)
{
    return result.rgba[pixelAt(pixel) * 4 + 3] > kCovered;
}

/**
 * Each copy covers its centre, at the depth of its front face, reaches kProbe along its long side
 * and not along its short one; every covered pixel lies on a copy, so none is near the origin.
 **/
void
expectCopiesAt(const HydraRender::Request& request,
               const HydraRender::Result& result,
               const std::vector<Expected>& copies)
{
    const std::size_t pixels = (std::size_t)kSize * (std::size_t)kSize;
    ASSERT_EQ(pixels * 4, result.rgba.size());
    ASSERT_EQ(pixels, result.depth.size());
    const double pixelsPerUnit = kSize / kWindow;

    std::vector<PXR_NS::GfVec2d> centres;
    for (const Expected& copy : copies) {
        const PXR_NS::GfVec2d centre = project(request, copy.centre);
        centres.push_back(centre);
        SCOPED_TRACE("copy at " + std::to_string(copy.centre[0]) + ", " + std::to_string(copy.centre[1]));
        ASSERT_TRUE(coveredAt(result, centre));
        EXPECT_NEAR(kCameraZ - kFrontZ, result.depth[pixelAt(centre)], 0.02);

        const PXR_NS::GfVec3d along = copy.rotated ? PXR_NS::GfVec3d(0., kProbe, 0.) : PXR_NS::GfVec3d(kProbe, 0., 0.);
        const PXR_NS::GfVec3d across = copy.rotated ? PXR_NS::GfVec3d(kProbe, 0., 0.) : PXR_NS::GfVec3d(0., kProbe, 0.);
        for (double side : { -1., 1. }) {
            EXPECT_TRUE(coveredAt(result, project(request, copy.centre + side * along))) << side;
            EXPECT_FALSE(coveredAt(result, project(request, copy.centre + side * across))) << side;
        }
    }

    // A copy's farthest corner is under 0.6 units from its centre; the origin is over 2 units
    // from every copy, so nothing within 1.5 units of it may be covered.
    const PXR_NS::GfVec2d origin = project(request, PXR_NS::GfVec3d(0.));
    const double reach = (std::hypot(kHalfLength, kHalfWidth) + 0.1) * pixelsPerUnit;
    const double emptyRadius = 1.5 * pixelsPerUnit;
    int strays = 0;
    int nearOrigin = 0;
    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            if (result.rgba[((std::size_t)y * kSize + x) * 4 + 3] <= kCovered) {
                continue;
            }
            const PXR_NS::GfVec2d p(x + 0.5, y + 0.5);
            if ((p - origin).GetLength() < emptyRadius) {
                ++nearOrigin;
            }
            bool onACopy = false;
            for (const PXR_NS::GfVec2d& centre : centres) {
                onACopy = onACopy || ((p - centre).GetLength() < reach);
            }
            if (!onACopy) {
                ++strays;
            }
        }
    }
    EXPECT_EQ(0, nearOrigin);
    EXPECT_EQ(0, strays);
}
} // anonymous namespace

class USDRenderCyclesInstancerTest
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
        HydraRender::releaseAllCpuEngines();
        SceneQueryPool::instance().releaseIdle();
        BaseTest::TearDown();
    }

    NodePtr make(const char* pluginID)
    {
        NodePtr node = createNode(QString::fromUtf8(pluginID));
        EXPECT_TRUE(node != nullptr) << pluginID;
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
        EXPECT_TRUE(k != nullptr) << name;

        return k;
    }

private:
    std::vector<NodePtr> _nodes;
};

// A PointInstancer, itself moved, whose box prototype has a transform of its own.
TEST_F(USDRenderCyclesInstancerTest, PointInstancerCopiesLandOnTheirPoints)
{
    if (!CyclesTestUtils::cyclesOrSkip()) {
        return;
    }
    const PXR_NS::SdfLayerRefPtr layer = SceneOps::createNodeLayer("instancer");
    ASSERT_TRUE(layer != nullptr);
    const PXR_NS::GfVec3d instancerOffset(0.5, 0., 0.);
    const std::vector<PXR_NS::GfVec3f> positions = { PXR_NS::GfVec3f(3.f, 2.f, 0.f), PXR_NS::GfVec3f(-3.f, 2.f, 0.f), PXR_NS::GfVec3f(0.f, -3.f, 0.f) };
    const int rotatedIndex = 1;
    {
        PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(layer);
        ASSERT_TRUE(stage != nullptr);
        PXR_NS::UsdGeomPointInstancer instancer = PXR_NS::UsdGeomPointInstancer::Define(stage, PXR_NS::SdfPath("/World/Instancer"));
        ASSERT_TRUE(bool(instancer));
        instancer.AddTranslateOp().Set(instancerOffset);
        // A 2-unit cube stretched to 4 x 2 x 2 by its own transform, which applies before the
        // instance's.
        PXR_NS::UsdGeomCube box = PXR_NS::UsdGeomCube::Define(stage, PXR_NS::SdfPath("/World/Instancer/Prototypes/Box"));
        ASSERT_TRUE(bool(box));
        box.CreateSizeAttr(PXR_NS::VtValue(2.));
        box.AddScaleOp().Set(PXR_NS::GfVec3f(2.f, 1.f, 1.f));
        instancer.CreatePrototypesRel().SetTargets({ box.GetPath() });

        const PXR_NS::GfQuatd quarterTurn = PXR_NS::GfRotation(PXR_NS::GfVec3d(0., 0., 1.), 90.).GetQuat();
        PXR_NS::VtQuathArray orientations(positions.size(), PXR_NS::GfQuath::GetIdentity());
        orientations[rotatedIndex] = PXR_NS::GfQuath(PXR_NS::GfHalf((float)quarterTurn.GetReal()), PXR_NS::GfVec3h(quarterTurn.GetImaginary()));
        PXR_NS::VtVec3fArray pointPositions;
        for (const PXR_NS::GfVec3f& p : positions) {
            pointPositions.push_back(p);
        }
        instancer.CreateProtoIndicesAttr(PXR_NS::VtValue(PXR_NS::VtIntArray(positions.size(), 0)));
        instancer.CreatePositionsAttr(PXR_NS::VtValue(pointPositions));
        instancer.CreateScalesAttr(PXR_NS::VtValue(PXR_NS::VtVec3fArray(positions.size(), PXR_NS::GfVec3f((float)kInstanceScale))));
        instancer.CreateOrientationsAttr(PXR_NS::VtValue(orientations));
    }
    const PXR_NS::SdfLayerRefPtr camera = cameraLayer();
    ASSERT_TRUE(camera != nullptr);
    const ScenePayload scene = SceneOps::appendLayer(SceneOps::appendLayer(ScenePayload(), layer, kInstancerLayerHash), camera, kCameraLayerHash);

    const int owner = 0;
    HydraRender::Request request;
    ASSERT_TRUE(cyclesRequest(scene, camera, &owner, &request));
    HydraRender::Result result;
    std::string error;
    ASSERT_TRUE(HydraRender::render(request, &result, &error)) << error;

    std::vector<Expected> copies;
    for (std::size_t i = 0; i < positions.size(); ++i) {
        Expected copy;
        copy.centre = PXR_NS::GfVec3d(positions[i]) + instancerOffset;
        copy.rotated = ((int)i == rotatedIndex);
        copies.push_back(copy);
    }
    expectCopiesAt(request, result, copies);
    HydraRender::release(&owner);
}

// Packed copies of a GeoCopyToPoints reach USD as a PointInstancer of the same layout.
TEST_F(USDRenderCyclesInstancerTest, PackedCopiesToPointsLandOnTheirPoints)
{
    if (!CyclesTestUtils::cyclesOrSkip()) {
        return;
    }
    const NodePtr box = make(PLUGINID_NATRON_GEOPRIMITIVE);
    const NodePtr points = make(PLUGINID_NATRON_GEOPRIMITIVE);
    const NodePtr wrangle = make(PLUGINID_NATRON_GEOWRANGLE);
    const NodePtr copy = make(PLUGINID_NATRON_GEOCOPYTOPOINTS);
    const NodePtr toUSD = make(PLUGINID_NATRON_GEOTOUSD);
    ASSERT_TRUE(box && points && wrangle && copy && toUSD);

    KnobChoice* type = knob<KnobChoice>(box, kGeoPrimitiveParamType);
    KnobDouble* size = knob<KnobDouble>(box, kGeoPrimitiveParamCubeSize);
    ASSERT_TRUE(type != nullptr);
    ASSERT_TRUE(size != nullptr);
    type->setValueFromID("cube", 0);
    size->setValue(4., ViewSpec::all(), 0);
    size->setValue(2., ViewSpec::all(), 1);
    size->setValue(2., ViewSpec::all(), 2);

    // The default card's four corners, moved away from the origin; orient (x, y, z, w) is set on
    // every point so that no copy takes its orientation from the card's normal.
    KnobString* snippet = knob<KnobString>(wrangle, kGeoWrangleParamSnippet);
    ASSERT_TRUE(snippet != nullptr);
    snippet->setValue(std::string("f@pscale = 0.25;\n"
                                  "p@orient = {0, 0, 0, 1};\n"
                                  "if (@ptnum == 0) { @P = {3, 2, 0}; }\n"
                                  "if (@ptnum == 1) { @P = {-3, 2, 0}; p@orient = {0, 0, 0.70710678, 0.70710678}; }\n"
                                  "if (@ptnum == 2) { @P = {0, -3, 0}; }\n"
                                  "if (@ptnum == 3) { @P = {3, -3, 0}; }\n"));
    KnobBool* pack = knob<KnobBool>(copy, kGeoCopyToPointsParamPackAndInstance);
    ASSERT_TRUE(pack != nullptr);
    pack->setValue(true);

    connectNodes(points, wrangle, 0, true);
    connectNodes(box, copy, 0, true);
    connectNodes(wrangle, copy, 1, true);
    connectNodes(copy, toUSD, kGeoToUSDInputGeometry, true);

    ScenePayload geometry;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              toUSD->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &geometry, &error))
        << error;
    {
        // The copies must reach Cycles as instances for this to test the instancer.
        SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(geometry, SceneLoadSet::everything(), &error);
        ASSERT_TRUE(lease.getStage() != nullptr) << error;
        bool hasInstancer = false;
        for (const PXR_NS::UsdPrim& prim : lease.getStage()->Traverse()) {
            hasInstancer = hasInstancer || prim.IsA<PXR_NS::UsdGeomPointInstancer>();
        }
        ASSERT_TRUE(hasInstancer);
    }
    const PXR_NS::SdfLayerRefPtr camera = cameraLayer();
    ASSERT_TRUE(camera != nullptr);
    const ScenePayload scene = SceneOps::appendLayer(geometry, camera, kCameraLayerHash);

    const int owner = 0;
    HydraRender::Request request;
    ASSERT_TRUE(cyclesRequest(scene, camera, &owner, &request));
    HydraRender::Result result;
    ASSERT_TRUE(HydraRender::render(request, &result, &error)) << error;

    const double corners[4][2] = { { 3., 2. }, { -3., 2. }, { 0., -3. }, { 3., -3. } };
    std::vector<Expected> copies;
    for (int i = 0; i < 4; ++i) {
        Expected c;
        c.centre = PXR_NS::GfVec3d(corners[i][0], corners[i][1], 0.);
        c.rotated = (i == 1);
        copies.push_back(c);
    }
    expectCopiesAt(request, result, copies);
    HydraRender::release(&owner);
}
