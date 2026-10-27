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
#include <memory>
#include <string>

#include <gtest/gtest.h>

// Before Qt's OpenGL headers.
#include "Gui/SceneGizmo.h"
#include "Gui/SceneViewport.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QMouseEvent>
#include <QThreadPool>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDEditTransform.h"
#include "Engine/Nodes/Scene/USDXformPrim.h"
#include "Engine/Project.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

#include "Gui/UndoHistory.h"

NATRON_NAMESPACE_USING

using namespace SceneGizmo;

namespace {
const int kSize = 256;

// A camera 10 units out on +Z looking at the origin, Y up, with a 45 degree vertical field of
// view over kSize x kSize pixels, as the scene viewer's free camera at azimuth and elevation 0.
View
frontView()
{
    PXR_NS::GfMatrix4d lookAt;

    lookAt.SetLookAt(PXR_NS::GfVec3d(0., 0., 10.), PXR_NS::GfVec3d(0.), PXR_NS::GfVec3d(0., 1., 0.));
    PXR_NS::GfFrustum frustum;
    frustum.SetPositionAndRotationFromMatrix(lookAt.GetInverse());
    frustum.SetPerspective(45., 1., 0.1, 1000.);

    return View(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix(), kSize, kSize);
}

// One pixel at the depth of the origin, in world units.
const double kPixel = 2. * 10. * std::tan(22.5 * M_PI / 180.) / kSize;

PXR_NS::GfVec2d
pixelOf(const View& view,
        const PXR_NS::GfVec3d& world)
{
    PXR_NS::GfVec2d p;

    EXPECT_TRUE(project(view, world, &p));

    return p;
}

Frame
identityFrame()
{
    return computeFrame(PXR_NS::GfMatrix4d(1.), PXR_NS::GfVec3d(0.), PXR_NS::GfVec3d(0.), PXR_NS::GfVec3d(0.), eSceneRotateOrderXYZ);
}

void
expectVecNear(const PXR_NS::GfVec3d& expected,
              const PXR_NS::GfVec3d& actual,
              double tolerance)
{
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(expected[i], actual[i], tolerance) << "component " << i;
    }
}
} // anonymous namespace

TEST(SceneGizmo, ProjectsAndCastsRaysThroughTheView)
{
    const View view = frontView();
    const PXR_NS::GfVec2d centre = pixelOf(view, PXR_NS::GfVec3d(0.));

    EXPECT_NEAR(128., centre[0], 1e-6);
    EXPECT_NEAR(128., centre[1], 1e-6);
    // Up in the world is up on screen, which is a smaller pixel row.
    const PXR_NS::GfVec2d up = pixelOf(view, PXR_NS::GfVec3d(0., 1., 0.));
    EXPECT_NEAR(128. - 1. / kPixel, up[1], 1e-6);
    PXR_NS::GfVec2d behind;
    EXPECT_FALSE(project(view, PXR_NS::GfVec3d(0., 0., 20.), &behind));

    PXR_NS::GfVec3d origin, direction;
    pickRay(view, PXR_NS::GfVec2d(128., 128.), &origin, &direction);
    expectVecNear(PXR_NS::GfVec3d(0., 0., -1.), direction, 1e-9);
    EXPECT_NEAR(kPixel, worldSizeOfPixel(view, PXR_NS::GfVec3d(0.)), 1e-9);
    // Twice as far, twice as large.
    EXPECT_NEAR(2. * kPixel, worldSizeOfPixel(view, PXR_NS::GfVec3d(0., 0., -10.)), 1e-9);
    EXPECT_NEAR(kHandleLengthPixels * kPixel, getHandleLength(view, identityFrame()), 1e-9);
}

TEST(SceneGizmo, TheFrameFollowsTheOpSpaceThePivotAndTheRotation)
{
    // Doubles, then moves by (1, 2, 3).
    PXR_NS::GfMatrix4d space;
    space.SetScale(2.);
    space = space * PXR_NS::GfMatrix4d(1.).SetTranslate(PXR_NS::GfVec3d(1., 2., 3.));

    const Frame frame = computeFrame(space, PXR_NS::GfVec3d(1., 0., 0.), PXR_NS::GfVec3d(0., 0., 90.), PXR_NS::GfVec3d(0., 1., 0.), eSceneRotateOrderXYZ);
    // The pivot (0, 1, 0) moved by (1, 0, 0), doubled, then moved by (1, 2, 3).
    expectVecNear(PXR_NS::GfVec3d(3., 4., 3.), frame.origin, 1e-9);
    // A unit of translate along X is two world units along X.
    expectVecNear(PXR_NS::GfVec3d(2., 0., 0.), frame.axes[0], 1e-9);
    expectVecNear(PXR_NS::GfVec3d(0., 0., 2.), frame.axes[2], 1e-9);
    // The quarter turn about Z takes the prim's X to Y before the space doubles it.
    expectVecNear(PXR_NS::GfVec3d(0., 2., 0.), frame.scaleAxes[0], 1e-9);
    expectVecNear(PXR_NS::GfVec3d(-2., 0., 0.), frame.scaleAxes[1], 1e-9);
}

TEST(SceneGizmo, TheOpSpaceIsTheOpsBeforeItOverTheParent)
{
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::CreateInMemory();
    ASSERT_TRUE(bool(stage));
    PXR_NS::UsdGeomXform parent = PXR_NS::UsdGeomXform::Define(stage, PXR_NS::SdfPath("/P"));
    parent.AddTranslateOp().Set(PXR_NS::GfVec3d(10., 0., 0.));
    PXR_NS::UsdGeomXform child = PXR_NS::UsdGeomXform::Define(stage, PXR_NS::SdfPath("/P/C"));
    child.AddTranslateOp().Set(PXR_NS::GfVec3d(0., 5., 0.));
    PXR_NS::GfMatrix4d opMatrix;
    opMatrix.SetRotate(PXR_NS::GfRotation(PXR_NS::GfVec3d(0., 0., 1.), 90.));
    child.AddTransformOp(PXR_NS::UsdGeomXformOp::PrecisionDouble, PXR_NS::TfToken("T")).Set(opMatrix);
    child.AddScaleOp().Set(PXR_NS::GfVec3f(2.f));
    const std::string opName = "xformOp:transform:T";

    PXR_NS::GfMatrix4d space;
    ASSERT_TRUE(computeOpSpace(child.GetPrim(), opName, 0., &space));
    // The scale comes after the op in the order, so it applies first and is not in the space.
    expectVecNear(PXR_NS::GfVec3d(10., 5., 0.), space.Transform(PXR_NS::GfVec3d(0.)), 1e-9);
    expectVecNear(PXR_NS::GfVec3d(1., 0., 0.), space.TransformDir(PXR_NS::GfVec3d(1., 0., 0.)), 1e-9);
    // What the op and the ops after it do, placed by the space, is the prim's world transform.
    PXR_NS::GfMatrix4d scale;
    scale.SetScale(2.);
    const PXR_NS::GfMatrix4d world = child.ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode(0.));
    EXPECT_TRUE(PXR_NS::GfIsClose(world, scale * opMatrix * space, 1e-9));

    EXPECT_FALSE(computeOpSpace(child.GetPrim(), "xformOp:transform:Missing", 0., &space));
    EXPECT_FALSE(computeOpSpace(stage->GetPrimAtPath(PXR_NS::SdfPath("/Nowhere")), opName, 0., &space));

    // A reset drops the parent.
    child.SetResetXformStack(true);
    ASSERT_TRUE(computeOpSpace(child.GetPrim(), opName, 0., &space));
    expectVecNear(PXR_NS::GfVec3d(0., 5., 0.), space.Transform(PXR_NS::GfVec3d(0.)), 1e-9);
}

TEST(SceneGizmo, PixelsPickTheHandleDrawnUnderThem)
{
    const View view = frontView();
    const Frame frame = identityFrame();
    const std::vector<HandleShape> shapes = buildHandles(frame, getHandleLength(view, frame));

    ASSERT_EQ((std::size_t)kNumHandles, shapes.size());
    for (HandleKindEnum kind : { eHandleKindTranslate, eHandleKindRotate, eHandleKindScale }) {
        for (int axis = 0; axis < 3; ++axis) {
            const int handle = makeHandle(kind, axis);
            EXPECT_EQ(kind, getHandleKind(handle));
            EXPECT_EQ(axis, getHandleAxis(handle));
        }
    }
    PXR_NS::GfVec2d p;
    ASSERT_TRUE(getHandlePixel(view, frame, makeHandle(eHandleKindTranslate, 0), &p));
    EXPECT_NEAR(128. + 0.95 * kHandleLengthPixels, p[0], 1e-6);
    EXPECT_EQ(makeHandle(eHandleKindTranslate, 0), hitTest(view, shapes, p));
    ASSERT_TRUE(getHandlePixel(view, frame, makeHandle(eHandleKindTranslate, 1), &p));
    EXPECT_EQ(makeHandle(eHandleKindTranslate, 1), hitTest(view, shapes, p + PXR_NS::GfVec2d(3., 0.)));
    // The box sits on the X arrow, and wins over it.
    ASSERT_TRUE(getHandlePixel(view, frame, makeHandle(eHandleKindScale, 0), &p));
    EXPECT_EQ(makeHandle(eHandleKindScale, 0), hitTest(view, shapes, p));
    // The ring about Z faces the camera.
    ASSERT_TRUE(getHandlePixel(view, frame, makeHandle(eHandleKindRotate, 2), &p));
    EXPECT_EQ(makeHandle(eHandleKindRotate, 2), hitTest(view, shapes, p));
    EXPECT_EQ(kNoHandle, hitTest(view, shapes, PXR_NS::GfVec2d(5., 5.)));
    EXPECT_EQ(kNoHandle, hitTest(view, shapes, PXR_NS::GfVec2d(128. + 0.95 * kHandleLengthPixels, 128. + 2. * kHitTolerancePixels)));
}

TEST(SceneGizmo, DragsMapToKnobChanges)
{
    const View view = frontView();
    const Frame frame = identityFrame();
    double change = 0.;

    // An arrow: world units along the axis, in translate units.
    ASSERT_TRUE(computeDragChange(view, frame, makeHandle(eHandleKindTranslate, 0), pixelOf(view, PXR_NS::GfVec3d(1., 0., 0.)),
                                  pixelOf(view, PXR_NS::GfVec3d(2.5, 0., 0.)), &change));
    EXPECT_NEAR(1.5, change, 1e-6);
    // Moving across the arrow does nothing.
    ASSERT_TRUE(computeDragChange(view, frame, makeHandle(eHandleKindTranslate, 0), pixelOf(view, PXR_NS::GfVec3d(1., 0., 0.)),
                                  pixelOf(view, PXR_NS::GfVec3d(1., 1., 0.)), &change));
    EXPECT_NEAR(0., change, 1e-6);
    // In a space that doubles, a world unit is half a translate unit.
    PXR_NS::GfMatrix4d doubling;
    doubling.SetScale(2.);
    const Frame doubled = computeFrame(doubling, PXR_NS::GfVec3d(0.), PXR_NS::GfVec3d(0.), PXR_NS::GfVec3d(0.), eSceneRotateOrderXYZ);
    ASSERT_TRUE(computeDragChange(view, doubled, makeHandle(eHandleKindTranslate, 1), pixelOf(view, PXR_NS::GfVec3d(0., 1., 0.)),
                                  pixelOf(view, PXR_NS::GfVec3d(0., 3., 0.)), &change));
    EXPECT_NEAR(1., change, 1e-6);
    // The Z arrow points at the camera, so there is no line on screen to drag along.
    EXPECT_FALSE(computeDragChange(view, frame, makeHandle(eHandleKindTranslate, 2), PXR_NS::GfVec2d(128., 128.), PXR_NS::GfVec2d(140., 128.), &change));

    // A ring: degrees about its axis, counter-clockwise seen from the axis's tip.
    ASSERT_TRUE(computeDragChange(view, frame, makeHandle(eHandleKindRotate, 2), pixelOf(view, PXR_NS::GfVec3d(1., 0., 0.)),
                                  pixelOf(view, PXR_NS::GfVec3d(0., 1., 0.)), &change));
    EXPECT_NEAR(90., change, 1e-6);
    ASSERT_TRUE(computeDragChange(view, frame, makeHandle(eHandleKindRotate, 2), pixelOf(view, PXR_NS::GfVec3d(0., 1., 0.)),
                                  pixelOf(view, PXR_NS::GfVec3d(1., 1., 0.)), &change));
    EXPECT_NEAR(-45., change, 1e-6);
    // The ring about Y is edge-on from here: the ray through the centre lies in its plane.
    EXPECT_FALSE(computeDragChange(view, frame, makeHandle(eHandleKindRotate, 1), PXR_NS::GfVec2d(128., 128.), PXR_NS::GfVec2d(140., 128.), &change));

    // A box: the ratio of the distances from the origin along its axis.
    ASSERT_TRUE(computeDragChange(view, frame, makeHandle(eHandleKindScale, 0), pixelOf(view, PXR_NS::GfVec3d(1., 0., 0.)),
                                  pixelOf(view, PXR_NS::GfVec3d(3., 0., 0.)), &change));
    EXPECT_NEAR(3., change, 1e-6);
    // Through the origin and beyond, the scale stops short of zero.
    ASSERT_TRUE(computeDragChange(view, frame, makeHandle(eHandleKindScale, 0), pixelOf(view, PXR_NS::GfVec3d(1., 0., 0.)),
                                  pixelOf(view, PXR_NS::GfVec3d(-1., 0., 0.)), &change));
    EXPECT_DOUBLE_EQ(0.001, change);
    // Moving across the box's axis leaves the scale as it was.
    ASSERT_TRUE(computeDragChange(view, frame, makeHandle(eHandleKindScale, 0), pixelOf(view, PXR_NS::GfVec3d(1., 0., 0.)),
                                  pixelOf(view, PXR_NS::GfVec3d(1., 1., 0.)), &change));
    EXPECT_NEAR(1., change, 1e-6);
    // The box follows the rotated axis: after a quarter turn about Z, X scales along world Y.
    const Frame turned = computeFrame(PXR_NS::GfMatrix4d(1.), PXR_NS::GfVec3d(0.), PXR_NS::GfVec3d(0., 0., 90.), PXR_NS::GfVec3d(0.), eSceneRotateOrderXYZ);
    ASSERT_TRUE(computeDragChange(view, turned, makeHandle(eHandleKindScale, 0), pixelOf(view, PXR_NS::GfVec3d(0., 1., 0.)),
                                  pixelOf(view, PXR_NS::GfVec3d(0., 2., 0.)), &change));
    EXPECT_NEAR(2., change, 1e-6);

    EXPECT_FALSE(computeDragChange(view, frame, kNoHandle, PXR_NS::GfVec2d(0.), PXR_NS::GfVec2d(1.), &change));
}

namespace {
// A camera 10 units from the origin along eye's direction, looking at the origin.
View
viewFrom(const PXR_NS::GfVec3d& eye)
{
    const PXR_NS::GfVec3d direction = eye.GetNormalized();
    const PXR_NS::GfVec3d up = (std::abs(direction[1]) < 0.9) ? PXR_NS::GfVec3d(0., 1., 0.) : PXR_NS::GfVec3d(1., 0., 0.);
    PXR_NS::GfMatrix4d lookAt;

    lookAt.SetLookAt(direction * 10., PXR_NS::GfVec3d(0.), up);
    PXR_NS::GfFrustum frustum;
    frustum.SetPositionAndRotationFromMatrix(lookAt.GetInverse());
    frustum.SetPerspective(45., 1., 0.1, 1000.);

    return View(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix(), kSize, kSize);
}

const SceneRotateOrderEnum kAllRotateOrders[] = {
    eSceneRotateOrderXYZ, eSceneRotateOrderXZY, eSceneRotateOrderYXZ, eSceneRotateOrderYZX, eSceneRotateOrderZXY, eSceneRotateOrderZYX
};
} // anonymous namespace

TEST(SceneGizmo, RotateOrdersComposeAsUsdRotateOps)
{
    const PXR_NS::GfVec3d rotate(30., -45., 60.);

    for (SceneRotateOrderEnum order : kAllRotateOrders) {
        // The orders are listed as USD lists its three-angle rotate ops.
        const PXR_NS::UsdGeomXformOp::Type type = (PXR_NS::UsdGeomXformOp::Type)(PXR_NS::UsdGeomXformOp::TypeRotateXYZ + (int)order);
        const PXR_NS::GfMatrix4d expected = PXR_NS::UsdGeomXformOp::GetOpTransform(type, PXR_NS::VtValue(rotate));
        EXPECT_TRUE(PXR_NS::GfIsClose(expected, SceneNodeAuthoring::composeRotationMatrix(rotate, order), 1e-9)) << "order " << (int)order;
    }
}

TEST(SceneGizmo, RotationsDecomposeBackToTheirRotateValues)
{
    const PXR_NS::GfVec3d values[] = {
        PXR_NS::GfVec3d(30., -45., 60.),
        PXR_NS::GfVec3d(-170., 20., 95.),
        PXR_NS::GfVec3d(10., 90., -25.), // gimbal lock for the orders whose middle axis is Y
        PXR_NS::GfVec3d(0., 0., 0.),
    };

    for (SceneRotateOrderEnum order : kAllRotateOrders) {
        for (const PXR_NS::GfVec3d& rotate : values) {
            const PXR_NS::GfMatrix4d rotation = SceneNodeAuthoring::composeRotationMatrix(rotate, order);
            const PXR_NS::GfVec3d decomposed = SceneNodeAuthoring::decomposeRotationMatrix(rotation, order, rotate);
            EXPECT_TRUE(PXR_NS::GfIsClose(rotation, SceneNodeAuthoring::composeRotationMatrix(decomposed, order), 1e-9))
                << "order " << (int)order << " rotate " << rotate[0] << " " << rotate[1] << " " << rotate[2];
        }
        // Away from gimbal lock the values closest to the hint are the values composed.
        const PXR_NS::GfVec3d rotate(30., -45., 60.);
        expectVecNear(rotate, SceneNodeAuthoring::decomposeRotationMatrix(SceneNodeAuthoring::composeRotationMatrix(rotate, order), order, rotate), 1e-7);
        // A hint a full turn away on X keeps the value near it rather than in -180..180.
        const PXR_NS::GfVec3d turned(390., -45., 60.);
        expectVecNear(turned, SceneNodeAuthoring::decomposeRotationMatrix(SceneNodeAuthoring::composeRotationMatrix(rotate, order), order, PXR_NS::GfVec3d(385., -40., 62.)), 1e-7);
    }
}

TEST(SceneGizmo, EachRingTurnsThePrimAboutItselfThroughItsOwnAngle)
{
    // A quarter turn about Y in XYZ order carries the X angle's axis to -Z: the X ring faces Z.
    const Frame quarter = computeFrame(PXR_NS::GfMatrix4d(1.), PXR_NS::GfVec3d(0.), PXR_NS::GfVec3d(0., 90., 0.), PXR_NS::GfVec3d(0.), eSceneRotateOrderXYZ);
    expectVecNear(PXR_NS::GfVec3d(0., 0., -1.), quarter.rotateAxes[0], 1e-9);
    expectVecNear(PXR_NS::GfVec3d(0., 1., 0.), quarter.rotateAxes[1], 1e-9);
    expectVecNear(PXR_NS::GfVec3d(0., 0., 1.), quarter.rotateAxes[2], 1e-9);

    const PXR_NS::GfVec3d rotates[] = {
        PXR_NS::GfVec3d(0.), PXR_NS::GfVec3d(0., 90., 0.), PXR_NS::GfVec3d(30., -45., 60.), PXR_NS::GfVec3d(120., 75., -150.), PXR_NS::GfVec3d(-20., 10., 85.)
    };
    const double kTurn = 5.;

    for (SceneRotateOrderEnum order : kAllRotateOrders) {
        for (const PXR_NS::GfVec3d& rotate : rotates) {
            const Frame frame = computeFrame(PXR_NS::GfMatrix4d(1.), PXR_NS::GfVec3d(0.), rotate, PXR_NS::GfVec3d(0.), order);
            const PXR_NS::GfMatrix4d before = SceneNodeAuthoring::composeRotationMatrix(rotate, order);
            const double length = 1.;
            const std::vector<HandleShape> shapes = buildHandles(frame, length);

            for (int axis = 0; axis < 3; ++axis) {
                const std::string where = "order " + std::to_string((int)order) + " rotate (" + std::to_string(rotate[0]) + ", " + std::to_string(rotate[1]) + ", " + std::to_string(rotate[2]) + ") axis " + std::to_string(axis);
                const PXR_NS::GfVec3d n = frame.rotateAxes[axis].GetNormalized();
                // The ring is drawn about the axis its drag is measured about.
                const HandleShape& ring = shapes[axis * 3 + 1];
                ASSERT_EQ(makeHandle(eHandleKindRotate, axis), ring.handle);
                for (const PXR_NS::GfVec3d& p : ring.points) {
                    EXPECT_NEAR(0., PXR_NS::GfDot(p - frame.origin, n), 1e-9) << where;
                }
                // A small drag along the ring, seen from near its axis so the ring faces the camera.
                const View view = viewFrom(n + PXR_NS::GfVec3d(0.2, 0.3, 0.1));
                const PXR_NS::GfVec3d start = ring.points[0];
                const PXR_NS::GfVec3d end = PXR_NS::GfMatrix4d(1.).SetRotate(PXR_NS::GfRotation(n, kTurn)).Transform(start);
                double change = 0.;
                ASSERT_TRUE(computeDragChange(view, frame, ring.handle, pixelOf(view, start), pixelOf(view, end), &change)) << where;
                EXPECT_NEAR(kTurn, change, 1e-6) << where;

                // The viewer adds the change to the ring's own component only; the prim then
                // turns about the drawn ring's axis, after its rotation.
                PXR_NS::GfVec3d after = rotate;
                after[axis] += change;
                const PXR_NS::GfMatrix4d expected = before * PXR_NS::GfMatrix4d(1.).SetRotate(PXR_NS::GfRotation(n, change));
                EXPECT_TRUE(PXR_NS::GfIsClose(expected, SceneNodeAuthoring::composeRotationMatrix(after, order), 1e-9)) << where;
            }
        }
    }
}

// ---------------------------------------------------------------------------

namespace {
void
sendMouse(QWidget* widget,
          QEvent::Type type,
          const QPointF& pos,
          Qt::MouseButton button,
          Qt::MouseButtons buttons)
{
    QMouseEvent event(type, pos, widget->mapToGlobal(pos), button, buttons, Qt::NoModifier);

    QCoreApplication::sendEvent(widget, &event);
}
} // anonymous namespace

// The scene viewer's handles for a USDEditTransform, driven by mouse events on the widget: no GL is
// needed to pick and drag them, only to draw them.
class SceneGizmoDragTest
    : public ::testing::Test {
protected:
    void SetUp() override
    {
        _app = appPTR->getTopLevelInstance();
        ASSERT_TRUE(bool(_app));
        _app->getProject()->reset(false, true);
        _prim = create(PLUGINID_NATRON_USDXFORMPRIM);
        _transform = create(PLUGINID_NATRON_USDEDITTRANSFORM);
        ASSERT_TRUE(_prim && _transform);
        KnobString* primPath = dynamic_cast<KnobString*>(_prim->getKnobByName(kSceneParamPrimPath).get());
        KnobString* pattern = dynamic_cast<KnobString*>(_transform->getKnobByName(kUSDEditTransformParamPrimPattern).get());
        ASSERT_TRUE(primPath && pattern);
        primPath->setValue(std::string("/Thing"));
        pattern->setValue(std::string("/Thing"));
        ASSERT_TRUE(_transform->connectInput(_prim, 0));
        _effect = dynamic_cast<USDEditTransform*>(_transform->getEffectInstance().get());
        ASSERT_TRUE(_effect != NULL);
        _translate = _effect->getTransformKnobs().translate.lock();
        ASSERT_TRUE(bool(_translate));
        _viewport.reset(new SceneViewport);
        _viewport->resize(kSize, kSize);
        _viewport->setUndoHistory(&_history);
        NodePtr transform = _transform;
        _viewport->setTransformNodeProvider([transform]() {
            return transform;
        });
        showScene();
        SceneViewportRenderer::FreeCamera camera;
        camera.distance = 10.;
        camera.fovY = 45.;
        _viewport->getRenderer().setFreeCamera(camera);
        _viewport->refreshTransformHandles();
    }

    void TearDown() override
    {
        _viewport.reset();
        _history.clear();
        _translate.reset();
        SceneQueryPool::instance().releaseIdle();
        if (_app) {
            _app->getProject()->reset(false, true);
        }
        QThreadPool::globalInstance()->waitForDone();
    }

    NodePtr create(const char* pluginID)
    {
        CreateNodeArgs args(pluginID, _app->getProject());

        return _app->createNode(args);
    }

    // Evaluates the transform and shows its scene at frame 1, as the viewer does after an edit.
    void showScene()
    {
        ScenePayload payload;
        std::string error;

        ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
                  _transform->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &payload, &error))
            << error;
        _viewport->setScene(payload, 1., 24.);
        // The viewport's worker syncs its stage, which the handles are placed from.
        QElapsedTimer timer;
        timer.start();
        while (!_viewport->isSceneSynced() && (timer.elapsed() < 30000)) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
        ASSERT_TRUE(_viewport->isSceneSynced());
        ASSERT_TRUE(_viewport->getError().isEmpty()) << _viewport->getError().toStdString();
    }

    QPointF handlePixel(int handle)
    {
        double x = 0., y = 0.;

        EXPECT_TRUE(_viewport->getRenderer().getTransformHandlePixel(handle, kSize, kSize, &x, &y));

        // Widget coordinates: the viewport's pixels are device pixels, a ratio of 1 here.
        return QPointF(std::floor(x), std::floor(y));
    }

    AppInstancePtr _app;
    NodePtr _prim, _transform;
    USDEditTransform* _effect = NULL;
    KnobDoublePtr _translate;
    UndoHistory _history;
    std::unique_ptr<SceneViewport> _viewport;
};

TEST_F(SceneGizmoDragTest, DraggingAnArrowSetsTranslateAsOneUndoableEdit)
{
    ASSERT_TRUE(_viewport->getRenderer().hasTransformHandles());
    std::string primPath, opName;
    ASSERT_TRUE(_effect->getHandleTarget(&primPath, &opName));
    EXPECT_EQ(std::string("/Thing"), primPath);

    const QPointF press = handlePixel(makeHandle(eHandleKindTranslate, 0));
    EXPECT_EQ(makeHandle(eHandleKindTranslate, 0), _viewport->getRenderer().hitTestTransformHandles(press.x() + 0.5, press.y() + 0.5, kSize, kSize));
    sendMouse(_viewport.get(), QEvent::MouseButtonPress, press, Qt::LeftButton, Qt::LeftButton);
    sendMouse(_viewport.get(), QEvent::MouseMove, press + QPointF(10., 0.), Qt::NoButton, Qt::LeftButton);
    sendMouse(_viewport.get(), QEvent::MouseMove, press + QPointF(20., 3.), Qt::NoButton, Qt::LeftButton);
    sendMouse(_viewport.get(), QEvent::MouseButtonRelease, press + QPointF(20., 3.), Qt::LeftButton, Qt::NoButton);

    // Twenty pixels at the depth of the origin, along X only.
    const double moved = 20. * kPixel;
    EXPECT_NEAR(moved, _translate->getValue(0), 0.05 * moved);
    EXPECT_DOUBLE_EQ(0., _translate->getValue(1));
    EXPECT_DOUBLE_EQ(0., _translate->getValue(2));
    // Both moves are one edit.
    ASSERT_EQ(1, _history.count());

    // The drag reaches the scene: the prim moves by the dragged amount.
    ScenePayload payload;
    std::string error;
    ASSERT_EQ(EffectInstance::eRenderRoIRetCodeOk,
              _transform->getEffectInstance()->evaluateScene(EffectInstance::EvaluateSceneArgs(1., ViewIdx(0)), &payload, &error))
        << error;
    {
        SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(payload, &error);
        PXR_NS::UsdStageRefPtr stage = lease.getStage();
        ASSERT_TRUE(bool(stage)) << error;
        const PXR_NS::GfMatrix4d world = PXR_NS::UsdGeomXformable(stage->GetPrimAtPath(PXR_NS::SdfPath("/Thing"))).ComputeLocalToWorldTransform(PXR_NS::UsdTimeCode(1.));
        EXPECT_NEAR(_translate->getValue(0), world.ExtractTranslation()[0], 1e-9);
    }

    _history.undo();
    EXPECT_DOUBLE_EQ(0., _translate->getValue(0));
    _history.redo();
    EXPECT_NEAR(moved, _translate->getValue(0), 0.05 * moved);

    // A second drag is a second edit.
    showScene();
    _viewport->refreshTransformHandles();
    const QPointF again = handlePixel(makeHandle(eHandleKindTranslate, 0));
    sendMouse(_viewport.get(), QEvent::MouseButtonPress, again, Qt::LeftButton, Qt::LeftButton);
    sendMouse(_viewport.get(), QEvent::MouseMove, again + QPointF(-10., 0.), Qt::NoButton, Qt::LeftButton);
    sendMouse(_viewport.get(), QEvent::MouseButtonRelease, again + QPointF(-10., 0.), Qt::LeftButton, Qt::NoButton);
    EXPECT_EQ(2, _history.count());
    EXPECT_NEAR(10. * kPixel, _translate->getValue(0), 0.1 * moved);
    _history.undo();
    EXPECT_NEAR(moved, _translate->getValue(0), 0.05 * moved);
}

TEST_F(SceneGizmoDragTest, NoHandlesWithoutAMatchOrANode)
{
    // A click away from the handles does not edit.
    sendMouse(_viewport.get(), QEvent::MouseButtonPress, QPointF(5., 5.), Qt::LeftButton, Qt::LeftButton);
    sendMouse(_viewport.get(), QEvent::MouseButtonRelease, QPointF(5., 5.), Qt::LeftButton, Qt::NoButton);
    EXPECT_EQ(0, _history.count());

    // A pattern that matches nothing names no target.
    KnobString* pattern = dynamic_cast<KnobString*>(_transform->getKnobByName(kUSDEditTransformParamPrimPattern).get());
    pattern->setValue(std::string("/Elsewhere"));
    showScene();
    _viewport->refreshTransformHandles();
    EXPECT_FALSE(_viewport->getRenderer().hasTransformHandles());

    // Nor does a viewer with no node.
    pattern->setValue(std::string("/Thing"));
    showScene();
    _viewport->setTransformNodeProvider([]() {
        return NodePtr();
    });
    _viewport->refreshTransformHandles();
    EXPECT_FALSE(_viewport->getRenderer().hasTransformHandles());
}
