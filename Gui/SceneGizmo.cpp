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

#include "SceneGizmo.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace SceneGizmo {
namespace {
    // Where the parts of a handle sit, in handle lengths from the origin.
    const double kArrowStart = 0.15;
    const double kArrowHead = 0.1;
    const double kRingRadius = 0.7;
    const double kBoxCentre = 0.5;
    const double kBoxHalfSide = 0.04;
    const int kRingSegments = 64;

    GfVec3d
    unproject(const View& view,
              double ndcX,
              double ndcY,
              double ndcZ)
    {
        const GfVec4d p = GfVec4d(ndcX, ndcY, ndcZ, 1.) * view.inverseViewProjection;
        const double w = (std::abs(p[3]) > 1e-300) ? p[3] : 1e-300;

        return GfVec3d(p[0] / w, p[1] / w, p[2] / w);
    }

    void
    pixelToNdc(const View& view,
               const GfVec2d& pixel,
               double* x,
               double* y)
    {
        *x = 2. * pixel[0] / std::max(view.width, 1) - 1.;
        *y = 1. - 2. * pixel[1] / std::max(view.height, 1);
    }

    // Unit vectors *u and *v that make a right-handed basis with the unit vector n.
    void
    makeBasis(const GfVec3d& n,
              GfVec3d* u,
              GfVec3d* v)
    {
        const GfVec3d helper = (std::abs(n[0]) < 0.9) ? GfVec3d(1., 0., 0.) : GfVec3d(0., 1., 0.);

        *u = GfCross(n, helper).GetNormalized();
        *v = GfCross(n, *u);
    }

    double
    distanceToSegment(const GfVec2d& p,
                      const GfVec2d& a,
                      const GfVec2d& b)
    {
        const GfVec2d ab = b - a;
        const double lengthSquared = ab.GetLengthSq();
        double t = (lengthSquared > 0.) ? GfDot(p - a, ab) / lengthSquared : 0.;

        t = std::min(std::max(t, 0.), 1.);

        return (p - (a + ab * t)).GetLength();
    }

    // An axis shorter than this on screen points too nearly at the camera to drag along.
    const double kMinAxisPixels = 3.;

    /**
     * @brief The parameter s of the point origin + s * direction under pixel, measured on screen:
     * pixel is moved square onto the line's image, so motion across the line changes nothing, and
     * the ray through that foot meets the line. False if the line is (nearly) along the view.
     **/
    bool
    closestLineParameter(const View& view,
                         const GfVec3d& origin,
                         const GfVec3d& direction,
                         const GfVec2d& pixel,
                         double* s)
    {
        const double a = GfDot(direction, direction);

        if (a <= 0.) {
            return false;
        }
        // A handle's length along the line from origin, as the line's image is drawn.
        const GfVec3d tip = origin + direction * (worldSizeOfPixel(view, origin) * kHandleLengthPixels / std::sqrt(a));
        GfVec2d originPixel, tipPixel;
        if (!project(view, origin, &originPixel) || !project(view, tip, &tipPixel)) {
            return false;
        }
        const GfVec2d screenLine = tipPixel - originPixel;
        const double screenLengthSquared = screenLine.GetLengthSq();
        if (screenLengthSquared < kMinAxisPixels * kMinAxisPixels) {
            return false;
        }
        const GfVec2d foot = originPixel + screenLine * (GfDot(pixel - originPixel, screenLine) / screenLengthSquared);

        GfVec3d rayOrigin, rayDirection;
        pickRay(view, foot, &rayOrigin, &rayDirection);
        const double b = GfDot(direction, rayDirection);
        const double c = GfDot(rayDirection, rayDirection);
        const GfVec3d w = origin - rayOrigin;
        const double d = GfDot(direction, w);
        const double e = GfDot(rayDirection, w);
        const double denominator = a * c - b * b;
        if (denominator <= 1e-9 * a * c) {
            return false;
        }
        *s = (b * e - c * d) / denominator;

        return true;
    }

    // Where the ray through pixel meets the plane through origin with unit normal n.
    bool
    intersectPlane(const View& view,
                   const GfVec3d& origin,
                   const GfVec3d& n,
                   const GfVec2d& pixel,
                   GfVec3d* hit)
    {
        GfVec3d rayOrigin, rayDirection;

        pickRay(view, pixel, &rayOrigin, &rayDirection);
        const double facing = GfDot(n, rayDirection);
        if (std::abs(facing) < 1e-6) {
            return false;
        }
        const double t = GfDot(n, origin - rayOrigin) / facing;
        *hit = rayOrigin + rayDirection * t;

        return true;
    }
} // anonymous namespace

View::View(const GfMatrix4d& viewMatrix,
           const GfMatrix4d& projectionMatrix,
           int w,
           int h)
    : viewProjection(viewMatrix * projectionMatrix)
    , inverseViewProjection((viewMatrix * projectionMatrix).GetInverse())
    , width(std::max(w, 1))
    , height(std::max(h, 1))
{
}

bool
project(const View& view,
        const GfVec3d& world,
        GfVec2d* pixel)
{
    const GfVec4d clip = GfVec4d(world[0], world[1], world[2], 1.) * view.viewProjection;

    if (clip[3] <= 1e-12) {
        return false;
    }
    const double x = clip[0] / clip[3];
    const double y = clip[1] / clip[3];
    (*pixel)[0] = (x * 0.5 + 0.5) * view.width;
    (*pixel)[1] = (0.5 - y * 0.5) * view.height;

    return true;
}

void
pickRay(const View& view,
        const GfVec2d& pixel,
        GfVec3d* origin,
        GfVec3d* direction)
{
    double x, y;

    pixelToNdc(view, pixel, &x, &y);
    const GfVec3d nearPoint = unproject(view, x, y, -1.);
    const GfVec3d farPoint = unproject(view, x, y, 1.);
    *origin = nearPoint;
    *direction = (farPoint - nearPoint).GetNormalized();
}

double
worldSizeOfPixel(const View& view,
                 const GfVec3d& world)
{
    const GfVec4d clip = GfVec4d(world[0], world[1], world[2], 1.) * view.viewProjection;

    if (clip[3] <= 1e-12) {
        return 0.;
    }
    const double x = clip[0] / clip[3];
    const double y = clip[1] / clip[3];
    const double z = clip[2] / clip[3];

    return (unproject(view, x + 2. / view.width, y, z) - unproject(view, x, y, z)).GetLength();
}

Frame
computeFrame(const GfMatrix4d& space,
             const GfVec3d& translate,
             const GfVec3d& rotate,
             const GfVec3d& pivot,
             SceneRotateOrderEnum rotateOrder)
{
    Frame frame;
    const GfMatrix4d rotation = SceneNodeAuthoring::composeRotationMatrix(rotate, rotateOrder);

    // The op takes the pivot to pivot + translate; space places the op's result in the world.
    frame.origin = space.Transform(pivot + translate);
    for (int i = 0; i < 3; ++i) {
        GfVec3d axis(0.);
        axis[i] = 1.;
        frame.axes[i] = space.TransformDir(axis);
        frame.scaleAxes[i] = space.TransformDir(rotation.TransformDir(axis));
    }
    // The rotation is A * B * C over row vectors, the order's first axis A applying first.
    // Adding d to A's angle gives A(d) * A * B * C = A * B * C * (B * C)^-1 * A(d) * (B * C): a
    // turn by d about A's axis carried through B and C. So component i turns the prim about axis
    // i taken through the factors after it, and the last axis turns about itself.
    for (int position = 0; position < 3; ++position) {
        const int i = SceneNodeAuthoring::getRotateOrderAxis(rotateOrder, position);
        GfVec3d axis(0.);
        axis[i] = 1.;
        for (int later = position + 1; later < 3; ++later) {
            const int j = SceneNodeAuthoring::getRotateOrderAxis(rotateOrder, later);
            GfVec3d direction(0.);
            direction[j] = 1.;
            axis = GfMatrix4d(1.).SetRotate(GfRotation(direction, rotate[j])).TransformDir(axis);
        }
        frame.rotateAxes[i] = space.TransformDir(axis);
    }

    return frame;
}

bool
computeOpSpace(const UsdPrim& prim,
               const std::string& opName,
               double timeCode,
               GfMatrix4d* space)
{
    const UsdGeomXformable xformable(prim);

    if (!xformable) {
        return false;
    }
    bool resetsXformStack = false;
    const std::vector<UsdGeomXformOp> ops = xformable.GetOrderedXformOps(&resetsXformStack);
    const TfToken name(opName);
    std::size_t index = ops.size();
    for (std::size_t i = 0; i < ops.size(); ++i) {
        if (ops[i].GetOpName() == name) {
            index = i;
            break;
        }
    }
    if (index == ops.size()) {
        return false;
    }
    const UsdTimeCode time(timeCode);
    // A point leaves the op, goes through the ops before it in the order, last first, then
    // through the parent's world transform.
    GfMatrix4d result(1.);
    for (std::size_t i = index; i > 0; --i) {
        result = result * ops[i - 1].GetOpTransform(time);
    }
    if (!resetsXformStack) {
        UsdGeomXformCache cache(time);
        result = result * cache.GetParentToWorldTransform(prim);
    }
    *space = result;

    return true;
}

double
getHandleLength(const View& view,
                const Frame& frame)
{
    return worldSizeOfPixel(view, frame.origin) * kHandleLengthPixels;
}

std::vector<HandleShape>
buildHandles(const Frame& frame,
             double length)
{
    std::vector<HandleShape> shapes;

    shapes.reserve(kNumHandles);
    for (int i = 0; i < 3; ++i) {
        const GfVec3d axis = frame.axes[i].GetNormalized();
        GfVec3d u, v;
        makeBasis(axis, &u, &v);

        HandleShape arrow;
        arrow.handle = makeHandle(eHandleKindTranslate, i);
        const GfVec3d tip = frame.origin + axis * length;
        const GfVec3d back = tip - axis * (kArrowHead * length);
        const double barb = 0.4 * kArrowHead * length;
        arrow.points = { frame.origin + axis * (kArrowStart * length), tip, back + u * barb, tip, back - u * barb };
        shapes.push_back(arrow);

        const GfVec3d rotateAxis = frame.rotateAxes[i].GetNormalized();
        GfVec3d ru, rv;
        makeBasis(rotateAxis, &ru, &rv);
        HandleShape ring;
        ring.handle = makeHandle(eHandleKindRotate, i);
        ring.closed = true;
        for (int k = 0; k < kRingSegments; ++k) {
            const double angle = 2. * M_PI * k / kRingSegments;
            ring.points.push_back(frame.origin + (ru * std::cos(angle) + rv * std::sin(angle)) * (kRingRadius * length));
        }
        shapes.push_back(ring);

        const GfVec3d scaleAxis = frame.scaleAxes[i].GetNormalized();
        GfVec3d su, sv;
        makeBasis(scaleAxis, &su, &sv);
        HandleShape box;
        box.handle = makeHandle(eHandleKindScale, i);
        box.closed = true;
        const GfVec3d centre = frame.origin + scaleAxis * (kBoxCentre * length);
        const double half = kBoxHalfSide * length;
        // A square across the axis, so it shows from along the arrow as well as from the side.
        box.points = { centre + (su + sv) * half, centre + (su - sv) * half, centre - (su + sv) * half, centre - (su - sv) * half };
        shapes.push_back(box);
    }

    return shapes;
}

int
hitTest(const View& view,
        const std::vector<HandleShape>& shapes,
        const GfVec2d& pixel)
{
    int best = kNoHandle;
    double bestScore = std::numeric_limits<double>::max();

    for (const HandleShape& shape : shapes) {
        const std::size_t n = shape.points.size();
        double distance = std::numeric_limits<double>::max();
        if (n == 1) {
            GfVec2d p;
            if (project(view, shape.points[0], &p)) {
                distance = (p - pixel).GetLength();
            }
        }
        const std::size_t numSegments = shape.closed ? n : ((n > 0) ? n - 1 : 0);
        for (std::size_t k = 0; (n > 1) && (k < numSegments); ++k) {
            GfVec2d a, b;
            if (project(view, shape.points[k], &a) && project(view, shape.points[(k + 1) % n], &b)) {
                distance = std::min(distance, distanceToSegment(pixel, a, b));
            }
        }
        if (distance > kHitTolerancePixels) {
            continue;
        }
        // A box sits on its arrow: anywhere on the box picks the box. An arrow wins a tie with a
        // ring seen edge-on along it, as it is drawn over the rings.
        const HandleKindEnum kind = getHandleKind(shape.handle);
        const double score = (kind == eHandleKindScale) ? distance - kHitTolerancePixels : (kind == eHandleKindTranslate) ? distance - 0.5
                                                                                                                          : distance;
        if (score < bestScore) {
            bestScore = score;
            best = shape.handle;
        }
    }

    return best;
}

bool
getHandlePixel(const View& view,
               const Frame& frame,
               int handle,
               GfVec2d* pixel)
{
    if ((handle < 0) || (handle >= kNumHandles)) {
        return false;
    }
    const double length = getHandleLength(view, frame);
    const int axis = getHandleAxis(handle);
    GfVec3d point;
    switch (getHandleKind(handle)) {
    case eHandleKindTranslate:
        // Near the tip, clear of the box and the rings.
        point = frame.origin + frame.axes[axis].GetNormalized() * (0.95 * length);
        break;
    case eHandleKindRotate: {
        GfVec3d u, v;
        makeBasis(frame.rotateAxes[axis].GetNormalized(), &u, &v);
        // On the ring, where buildHandles() draws it, half way between two of its basis directions.
        point = frame.origin + (u + v).GetNormalized() * (kRingRadius * length);
        break;
    }
    case eHandleKindScale:
        point = frame.origin + frame.scaleAxes[axis].GetNormalized() * (kBoxCentre * length);
        break;
    }

    return project(view, point, pixel);
}

bool
computeDragChange(const View& view,
                  const Frame& frame,
                  int handle,
                  const GfVec2d& pressPixel,
                  const GfVec2d& pixel,
                  double* change)
{
    if ((handle < 0) || (handle >= kNumHandles)) {
        return false;
    }
    const int axis = getHandleAxis(handle);
    switch (getHandleKind(handle)) {
    case eHandleKindTranslate: {
        // The line is parametrised by translate units along the axis.
        double s0, s1;
        if (!closestLineParameter(view, frame.origin, frame.axes[axis], pressPixel, &s0) || !closestLineParameter(view, frame.origin, frame.axes[axis], pixel, &s1)) {
            return false;
        }
        *change = s1 - s0;

        return true;
    }
    case eHandleKindRotate: {
        const GfVec3d n = frame.rotateAxes[axis].GetNormalized();
        GfVec3d p0, p1;
        if (!intersectPlane(view, frame.origin, n, pressPixel, &p0) || !intersectPlane(view, frame.origin, n, pixel, &p1)) {
            return false;
        }
        const GfVec3d v0 = p0 - frame.origin;
        const GfVec3d v1 = p1 - frame.origin;
        if ((v0.GetLength() <= 0.) || (v1.GetLength() <= 0.)) {
            return false;
        }
        // Positive turns are counter-clockwise seen from the axis's tip, as GfRotation's.
        *change = std::atan2(GfDot(n, GfCross(v0, v1)), GfDot(v0, v1)) * 180. / M_PI;

        return true;
    }
    case eHandleKindScale: {
        const GfVec3d direction = frame.scaleAxes[axis].GetNormalized();
        double s0, s1;
        if (!closestLineParameter(view, frame.origin, direction, pressPixel, &s0) || !closestLineParameter(view, frame.origin, direction, pixel, &s1) || (std::abs(s0) < 1e-12)) {
            return false;
        }
        *change = std::max(s1 / s0, 0.001);

        return true;
    }
    }

    return false;
} // computeDragChange
} // namespace SceneGizmo

NATRON_NAMESPACE_EXIT
