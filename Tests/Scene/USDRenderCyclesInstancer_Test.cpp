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
 * instancer's transform apply; a nested instancer's copies move with each copy of the instancer
 * that holds them. Composed the other way round, a copy at point p with scale s lands near s * p,
 * so every copy of a small prototype bunches up at the origin.
 *
 * The scenes are seen by an orthographic camera at z = 10 looking down -Z with a 10 x 10 window,
 * rendered kSize x kSize, so a scene unit is kSize / 10 pixels.
 **/
namespace {
const int kSize = 256;
const double kWindow = 10.;
const double kCameraZ = 10.;
const char* const kCameraPath = "/Cam";
// Distinct from any hash the geometry layers are appended under.
const U64 kInstancerLayerHash = 0x4d36313601ull;
const U64 kCameraLayerHash = 0x4d36313602ull;
const float kCovered = 0.5f;

// Probes sit this far inside or outside a copy's edges, about 8 pixels, well clear of the pixel
// filter.
const double kMargin = 0.3;
// Nothing may be covered this close to the origin; every copy here stays clear of it.
const double kEmptyRadius = 1.;

// A copy is the 2-unit cube, centred on its origin, carried to world by a matrix (row vectors).
// Every copy here keeps its X and Y axes square to each other and in the XY plane.
typedef PXR_NS::GfMatrix4d Copy;

PXR_NS::GfMatrix4d
scaleMatrix(const PXR_NS::GfVec3d& s)
{
    PXR_NS::GfMatrix4d m(1.);

    return m.SetScale(s);
}

PXR_NS::GfMatrix4d
turnAboutZ(double degrees)
{
    PXR_NS::GfMatrix4d m(1.);

    return m.SetRotate(PXR_NS::GfRotation(PXR_NS::GfVec3d(0., 0., 1.), degrees));
}

PXR_NS::GfMatrix4d
translateMatrix(const PXR_NS::GfVec3d& t)
{
    PXR_NS::GfMatrix4d m(1.);

    return m.SetTranslate(t);
}

// A 2-unit cube stretched to 4 x 2 x 2 by its own transform, which applies before the instance's.
bool
defineBox(const PXR_NS::UsdStageRefPtr& stage,
          const PXR_NS::SdfPath& path)
{
    PXR_NS::UsdGeomCube box = PXR_NS::UsdGeomCube::Define(stage, path);
    if (!box) {
        return false;
    }
    box.CreateSizeAttr(PXR_NS::VtValue(2.));
    box.AddScaleOp().Set(PXR_NS::GfVec3f(2.f, 1.f, 1.f));

    return true;
}

// One copy of prototype 0 per point, turned degrees about Z when turns is not empty.
void
setPoints(PXR_NS::UsdGeomPointInstancer& instancer,
          const std::vector<PXR_NS::GfVec3f>& positions,
          const std::vector<PXR_NS::GfVec3f>& scales,
          const std::vector<double>& turns)
{
    instancer.CreateProtoIndicesAttr(PXR_NS::VtValue(PXR_NS::VtIntArray(positions.size(), 0)));
    instancer.CreatePositionsAttr(PXR_NS::VtValue(PXR_NS::VtVec3fArray(positions.begin(), positions.end())));
    instancer.CreateScalesAttr(PXR_NS::VtValue(PXR_NS::VtVec3fArray(scales.begin(), scales.end())));
    if (turns.empty()) {
        return;
    }
    PXR_NS::VtQuathArray orientations;
    for (double degrees : turns) {
        const PXR_NS::GfQuatd q = PXR_NS::GfRotation(PXR_NS::GfVec3d(0., 0., 1.), degrees).GetQuat();
        orientations.push_back(PXR_NS::GfQuath(PXR_NS::GfHalf((float)q.GetReal()), PXR_NS::GfVec3h(q.GetImaginary())));
    }
    instancer.CreateOrientationsAttr(PXR_NS::VtValue(orientations));
}

// USD's own transform for each of instancer's copies, prototype transform included.
PXR_NS::VtMatrix4dArray
instanceTransforms(const PXR_NS::UsdGeomPointInstancer& instancer)
{
    PXR_NS::VtMatrix4dArray xforms;
    const PXR_NS::UsdTimeCode time(1.);
    EXPECT_TRUE(instancer.ComputeInstanceTransformsAtTime(&xforms, time, time));

    return xforms;
}

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

// Renders geometry with Cycles, seen by the orthographic camera.
bool
renderWithCamera(const ScenePayload& geometry,
                 const void* owner,
                 HydraRender::Request* request,
                 HydraRender::Result* result)
{
    const PXR_NS::SdfLayerRefPtr camera = cameraLayer();
    if (!camera) {
        ADD_FAILURE() << "no camera layer";

        return false;
    }
    const ScenePayload scene = SceneOps::appendLayer(geometry, camera, kCameraLayerHash);
    if (!cyclesRequest(scene, camera, owner, request)) {
        return false;
    }
    std::string error;
    if (!HydraRender::render(*request, result, &error)) {
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
 * Each copy covers its centre, at the depth of its front face, and its four corners pulled
 * kMargin inside along its own axes; it does not cover its corners or edge midpoints pushed
 * kMargin outside. A turn of the wrong sign, or a scale applied after the turn, moves at least one
 * of those probes across an edge. Every covered pixel lies on a copy, and none lies near the
 * origin, where copies composed in the wrong order bunch up.
 **/
void
expectCopiesAt(const HydraRender::Request& request,
               const HydraRender::Result& result,
               const std::vector<Copy>& copies)
{
    const std::size_t pixels = (std::size_t)kSize * (std::size_t)kSize;
    ASSERT_EQ(pixels * 4, result.rgba.size());
    ASSERT_EQ(pixels, result.depth.size());
    const double pixelsPerUnit = kSize / kWindow;

    std::vector<PXR_NS::GfVec2d> centres;
    std::vector<double> reaches;
    for (const Copy& copy : copies) {
        const PXR_NS::GfVec3d centre = copy.Transform(PXR_NS::GfVec3d(0.));
        SCOPED_TRACE("copy at " + std::to_string(centre[0]) + ", " + std::to_string(centre[1]));
        const double ax = copy.TransformDir(PXR_NS::GfVec3d(1., 0., 0.)).GetLength();
        const double ay = copy.TransformDir(PXR_NS::GfVec3d(0., 1., 0.)).GetLength();
        ASSERT_GT(ax, kMargin);
        ASSERT_GT(ay, kMargin);

        const PXR_NS::GfVec3d front = copy.Transform(PXR_NS::GfVec3d(0., 0., 1.));
        const PXR_NS::GfVec2d centrePixel = project(request, front);
        ASSERT_TRUE(coveredAt(result, centrePixel));
        EXPECT_NEAR(kCameraZ - front[2], result.depth[pixelAt(centrePixel)], 0.02);

        const double inX = 1. - kMargin / ax;
        const double inY = 1. - kMargin / ay;
        const double outX = 1. + kMargin / ax;
        const double outY = 1. + kMargin / ay;
        double reach = 0.;
        for (double sx : { -1., 1. }) {
            for (double sy : { -1., 1. }) {
                EXPECT_TRUE(coveredAt(result, project(request, copy.Transform(PXR_NS::GfVec3d(sx * inX, sy * inY, 1.))))) << sx << ", " << sy;
                EXPECT_FALSE(coveredAt(result, project(request, copy.Transform(PXR_NS::GfVec3d(sx * outX, sy * outY, 0.))))) << sx << ", " << sy;
                reach = std::max(reach, (copy.Transform(PXR_NS::GfVec3d(sx, sy, 0.)) - centre).GetLength());
            }
        }
        for (double side : { -1., 1. }) {
            EXPECT_FALSE(coveredAt(result, project(request, copy.Transform(PXR_NS::GfVec3d(side * outX, 0., 0.))))) << side;
            EXPECT_FALSE(coveredAt(result, project(request, copy.Transform(PXR_NS::GfVec3d(0., side * outY, 0.))))) << side;
        }
        ASSERT_GT(PXR_NS::GfVec2d(centre[0], centre[1]).GetLength() - reach, kEmptyRadius);
        centres.push_back(project(request, centre));
        reaches.push_back((reach + 0.15) * pixelsPerUnit);
    }

    const PXR_NS::GfVec2d origin = project(request, PXR_NS::GfVec3d(0.));
    const double emptyRadius = kEmptyRadius * pixelsPerUnit;
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
            for (std::size_t i = 0; i < centres.size(); ++i) {
                onACopy = onACopy || ((p - centres[i]).GetLength() < reaches[i]);
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

// A PointInstancer, itself moved, whose box prototype has a transform of its own. One copy is
// scaled unevenly and turned 30 degrees, another turned -45 degrees.
TEST_F(USDRenderCyclesInstancerTest, PointInstancerCopiesLandOnTheirPoints)
{
    if (!CyclesTestUtils::cyclesOrSkip()) {
        return;
    }
    const PXR_NS::SdfLayerRefPtr layer = SceneOps::createNodeLayer("instancer");
    ASSERT_TRUE(layer != nullptr);
    std::vector<Copy> copies;
    {
        PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(layer);
        ASSERT_TRUE(stage != nullptr);
        PXR_NS::UsdGeomPointInstancer instancer = PXR_NS::UsdGeomPointInstancer::Define(stage, PXR_NS::SdfPath("/World/Instancer"));
        ASSERT_TRUE(bool(instancer));
        instancer.AddTranslateOp().Set(PXR_NS::GfVec3d(0.5, 0., 0.));
        const PXR_NS::SdfPath box("/World/Instancer/Prototypes/Box");
        ASSERT_TRUE(defineBox(stage, box));
        instancer.CreatePrototypesRel().SetTargets({ box });
        setPoints(instancer,
                  { PXR_NS::GfVec3f(3.f, 2.f, 0.f), PXR_NS::GfVec3f(-3.f, 2.f, 0.f), PXR_NS::GfVec3f(0.f, -3.f, 0.f) },
                  { PXR_NS::GfVec3f(0.5f), PXR_NS::GfVec3f(0.8f, 0.35f, 0.5f), PXR_NS::GfVec3f(0.4f) },
                  { 0., 30., -45. });

        const PXR_NS::GfMatrix4d instancerToWorld = instancer.ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode(1.));
        for (const PXR_NS::GfMatrix4d& m : instanceTransforms(instancer)) {
            copies.push_back(m * instancerToWorld);
        }
        ASSERT_EQ(3u, copies.size());
    }
    const ScenePayload geometry = SceneOps::appendLayer(ScenePayload(), layer, kInstancerLayerHash);

    const int owner = 0;
    HydraRender::Request request;
    HydraRender::Result result;
    ASSERT_TRUE(renderWithCamera(geometry, &owner, &request, &result));
    expectCopiesAt(request, result, copies);
    HydraRender::release(&owner);
}

// An instancer whose prototype is another instancer: each inner copy is scaled and moved within
// the inner instancer, then carried by every outer copy's scale and move, then by the outer
// instancer's transform.
TEST_F(USDRenderCyclesInstancerTest, NestedInstancerCopiesLandOnTheirPoints)
{
    if (!CyclesTestUtils::cyclesOrSkip()) {
        return;
    }
    const PXR_NS::SdfLayerRefPtr layer = SceneOps::createNodeLayer("instancer");
    ASSERT_TRUE(layer != nullptr);
    std::vector<Copy> copies;
    {
        PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(layer);
        ASSERT_TRUE(stage != nullptr);
        PXR_NS::UsdGeomPointInstancer outer = PXR_NS::UsdGeomPointInstancer::Define(stage, PXR_NS::SdfPath("/World/Outer"));
        PXR_NS::UsdGeomPointInstancer inner = PXR_NS::UsdGeomPointInstancer::Define(stage, PXR_NS::SdfPath("/World/Outer/Prototypes/Inner"));
        ASSERT_TRUE(bool(outer));
        ASSERT_TRUE(bool(inner));
        outer.AddTranslateOp().Set(PXR_NS::GfVec3d(0.25, 0., 0.));
        const PXR_NS::SdfPath box("/World/Outer/Prototypes/Inner/Prototypes/Box");
        ASSERT_TRUE(defineBox(stage, box));
        inner.CreatePrototypesRel().SetTargets({ box });
        outer.CreatePrototypesRel().SetTargets({ inner.GetPath() });
        setPoints(inner,
                  { PXR_NS::GfVec3f(1.5f, 0.f, 0.f), PXR_NS::GfVec3f(-1.5f, 0.f, 0.f) },
                  { PXR_NS::GfVec3f(0.4f), PXR_NS::GfVec3f(0.25f, 0.5f, 0.5f) },
                  {});
        setPoints(outer,
                  { PXR_NS::GfVec3f(0.f, 2.5f, 0.f), PXR_NS::GfVec3f(0.f, -2.5f, 0.f) },
                  { PXR_NS::GfVec3f(1.f), PXR_NS::GfVec3f(1.5f, 0.8f, 1.f) },
                  {});

        // USD places a point of an inner copy through the inner instance's transform, then the
        // outer instance's, which includes the inner instancer's own, then the outer instancer's.
        const PXR_NS::GfMatrix4d outerToWorld = outer.ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode(1.));
        const PXR_NS::VtMatrix4dArray innerXforms = instanceTransforms(inner);
        const PXR_NS::VtMatrix4dArray outerXforms = instanceTransforms(outer);
        ASSERT_EQ(2u, innerXforms.size());
        ASSERT_EQ(2u, outerXforms.size());
        for (const PXR_NS::GfMatrix4d& o : outerXforms) {
            for (const PXR_NS::GfMatrix4d& i : innerXforms) {
                copies.push_back(i * o * outerToWorld);
            }
        }
    }
    const ScenePayload geometry = SceneOps::appendLayer(ScenePayload(), layer, kInstancerLayerHash);

    const int owner = 0;
    HydraRender::Request request;
    HydraRender::Result result;
    ASSERT_TRUE(renderWithCamera(geometry, &owner, &request, &result));
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
    // every point so that no copy takes its orientation from the card's normal, and so is scale,
    // which a wrangle creates as zero on the points that do not set it. Point 1 is scaled unevenly
    // and turned 30 degrees about Z, point 3 turned -45 degrees.
    KnobString* snippet = knob<KnobString>(wrangle, kGeoWrangleParamSnippet);
    ASSERT_TRUE(snippet != nullptr);
    snippet->setValue(std::string("f@pscale = 0.5;\n"
                                  "v@scale = {1, 1, 1};\n"
                                  "p@orient = {0, 0, 0, 1};\n"
                                  "if (@ptnum == 0) { @P = {3, 2, 0}; }\n"
                                  "if (@ptnum == 1) { @P = {-3, 2, 0}; v@scale = {1.6, 0.7, 1}; p@orient = {0, 0, 0.25881905, 0.96592583}; }\n"
                                  "if (@ptnum == 2) { @P = {0, -3, 0}; }\n"
                                  "if (@ptnum == 3) { @P = {3, -3, 0}; p@orient = {0, 0, -0.38268343, 0.92387953}; }\n"));
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
    const int owner = 0;
    HydraRender::Request request;
    HydraRender::Result result;
    ASSERT_TRUE(renderWithCamera(geometry, &owner, &request, &result));

    // The 4 x 2 x 2 box scaled by scale * pscale and turned about its centre, then moved to its
    // point.
    struct Point {
        PXR_NS::GfVec3d position;
        PXR_NS::GfVec3d scale;
        double turn;
    };
    const Point layout[4] = { { PXR_NS::GfVec3d(3., 2., 0.), PXR_NS::GfVec3d(0.5), 0. },
                              { PXR_NS::GfVec3d(-3., 2., 0.), PXR_NS::GfVec3d(0.8, 0.35, 0.5), 30. },
                              { PXR_NS::GfVec3d(0., -3., 0.), PXR_NS::GfVec3d(0.5), 0. },
                              { PXR_NS::GfVec3d(3., -3., 0.), PXR_NS::GfVec3d(0.5), -45. } };
    std::vector<Copy> copies;
    for (const Point& point : layout) {
        copies.push_back(scaleMatrix(PXR_NS::GfVec3d(2., 1., 1.)) * scaleMatrix(point.scale) * turnAboutZ(point.turn) * translateMatrix(point.position));
    }
    expectCopiesAt(request, result, copies);
    HydraRender::release(&owner);
}
