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

#include "SceneGuides.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usdGeom/gprim.h>
#include <pxr/usd/usdLux/cylinderLight.h>
#include <pxr/usd/usdLux/diskLight.h>

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace SceneGuides {
namespace {
    const int kCircleSegments = 32;
    // The smallest a light's own size draws at, in display lengths, so a tiny light stays visible.
    const double kMinLightSize = 0.03;

    void
    addSegment(const GfMatrix4d& m,
               const GfVec3d& a,
               const GfVec3d& b,
               std::vector<GfVec3d>* segments)
    {
        segments->push_back(m.Transform(a));
        segments->push_back(m.Transform(b));
    }

    // An arc about centre in the plane of the unit vectors u and v, from angle a0 to a1.
    void
    addArc(const GfMatrix4d& m,
           const GfVec3d& centre,
           const GfVec3d& u,
           const GfVec3d& v,
           double radius,
           double a0,
           double a1,
           std::vector<GfVec3d>* segments)
    {
        const int n = std::max(4, (int)std::ceil(kCircleSegments * std::abs(a1 - a0) / (2. * M_PI)));
        GfVec3d previous = centre + (u * std::cos(a0) + v * std::sin(a0)) * radius;

        for (int i = 1; i <= n; ++i) {
            const double a = a0 + (a1 - a0) * i / n;
            const GfVec3d p = centre + (u * std::cos(a) + v * std::sin(a)) * radius;
            addSegment(m, previous, p, segments);
            previous = p;
        }
    }

    void
    addCircle(const GfMatrix4d& m,
              const GfVec3d& centre,
              const GfVec3d& u,
              const GfVec3d& v,
              double radius,
              std::vector<GfVec3d>* segments)
    {
        addArc(m, centre, u, v, radius, 0., 2. * M_PI, segments);
    }

    // An arrow from start along the unit vector direction, with a four-sided head.
    void
    addArrow(const GfMatrix4d& m,
             const GfVec3d& start,
             const GfVec3d& direction,
             double length,
             std::vector<GfVec3d>* segments)
    {
        const GfVec3d tip = start + direction * length;
        const GfVec3d helper = (std::abs(direction[0]) < 0.9) ? GfVec3d(1., 0., 0.) : GfVec3d(0., 1., 0.);
        const GfVec3d u = GfCross(direction, helper).GetNormalized();
        const GfVec3d v = GfCross(direction, u);
        const double head = length * 0.15;
        const GfVec3d base = tip - direction * head;

        addSegment(m, start, tip, segments);
        addSegment(m, tip, base + u * head * 0.4, segments);
        addSegment(m, tip, base - u * head * 0.4, segments);
        addSegment(m, tip, base + v * head * 0.4, segments);
        addSegment(m, tip, base - v * head * 0.4, segments);
    }

    template <typename T>
    double
    attributeValue(const UsdAttribute& attribute,
                   UsdTimeCode time,
                   double fallback)
    {
        T value;

        if (attribute && attribute.Get(&value, time)) {
            return (double)value;
        }

        return fallback;
    }

    double
    distanceToSegment(const GfVec2d& p,
                      const GfVec2d& a,
                      const GfVec2d& b,
                      double* t)
    {
        const GfVec2d ab = b - a;
        const double lengthSquared = ab.GetLengthSq();

        *t = (lengthSquared > 0.) ? GfDot(p - a, ab) / lengthSquared : 0.;
        *t = std::min(std::max(*t, 0.), 1.);

        return (p - (a + ab * *t)).GetLength();
    }

    double
    clipW(const SceneGizmo::View& view,
          const GfVec3d& world)
    {
        return (GfVec4d(world[0], world[1], world[2], 1.) * view.viewProjection)[3];
    }

    bool
    segmentMeetsRect(GfVec2d a,
                     GfVec2d b,
                     const GfVec2d& lo,
                     const GfVec2d& hi)
    {
        // Liang-Barsky: clip the segment's parameter range to each side of the rectangle.
        const GfVec2d d = b - a;
        double t0 = 0., t1 = 1.;
        const double p[4] = { -d[0], d[0], -d[1], d[1] };
        const double q[4] = { a[0] - lo[0], hi[0] - a[0], a[1] - lo[1], hi[1] - a[1] };

        for (int i = 0; i < 4; ++i) {
            if (p[i] == 0.) {
                if (q[i] < 0.) {
                    return false;
                }
                continue;
            }
            const double r = q[i] / p[i];
            if (p[i] < 0.) {
                t0 = std::max(t0, r);
            } else {
                t1 = std::min(t1, r);
            }
            if (t0 > t1) {
                return false;
            }
        }

        return true;
    }
} // anonymous namespace

const char*
getGuideKindName(GuideKindEnum kind)
{
    switch (kind) {
    case eGuideKindCamera:
        return "camera";
    case eGuideKindDistantLight:
        return "distantLight";
    case eGuideKindSphereLight:
        return "sphereLight";
    case eGuideKindRectLight:
        return "rectLight";
    case eGuideKindDiskLight:
        return "diskLight";
    case eGuideKindCylinderLight:
        return "cylinderLight";
    case eGuideKindDomeLight:
        return "domeLight";
    case eGuideKindOtherLight:
        return "light";
    }

    return "";
}

bool
getGuideKind(const UsdPrim& prim,
             GuideKindEnum* kind)
{
    if (prim.IsA<UsdGeomCamera>()) {
        *kind = eGuideKindCamera;

        return true;
    }
    if (!prim.HasAPI<UsdLuxLightAPI>() || prim.IsA<UsdGeomGprim>()) {
        return false;
    }
    if (prim.IsA<UsdLuxDistantLight>()) {
        *kind = eGuideKindDistantLight;
    } else if (prim.IsA<UsdLuxSphereLight>()) {
        *kind = eGuideKindSphereLight;
    } else if (prim.IsA<UsdLuxRectLight>()) {
        *kind = eGuideKindRectLight;
    } else if (prim.IsA<UsdLuxDiskLight>()) {
        *kind = eGuideKindDiskLight;
    } else if (prim.IsA<UsdLuxCylinderLight>()) {
        *kind = eGuideKindCylinderLight;
    } else if (prim.IsA<UsdLuxDomeLight>()) {
        *kind = eGuideKindDomeLight;
    } else {
        *kind = eGuideKindOtherLight;
    }

    return true;
}

std::vector<GuidePrim>
findGuidePrims(const UsdStageRefPtr& stage,
               bool* hasLights)
{
    std::vector<GuidePrim> prims;

    if (hasLights) {
        *hasLights = false;
    }
    if (!stage) {
        return prims;
    }
    for (const UsdPrim& prim : stage->Traverse()) {
        if (hasLights && !*hasLights && prim.HasAPI<UsdLuxLightAPI>()) {
            *hasLights = true;
        }
        GuidePrim guide;
        if (getGuideKind(prim, &guide.kind)) {
            guide.path = prim.GetPath();
            prims.push_back(guide);
        }
    }

    return prims;
}

void
buildCameraLines(const GfCamera& camera,
                 double length,
                 std::vector<GfVec3d>* segments)
{
    GfFrustum frustum = camera.GetFrustum();
    const bool orthographic = camera.GetProjection() == GfCamera::Orthographic;

    length = std::max(length, 1e-6);
    // A perspective frustum's near plane only sets where its sides start; the sides are drawn from
    // the apex, so any small near gives the same far rectangle.
    frustum.SetNearFar(GfRange1d(orthographic ? 0. : length * 1e-3, length));
    const std::vector<GfVec3d> c = frustum.ComputeCorners();
    if (c.size() != 8) {
        return;
    }
    const GfMatrix4d identity(1.);
    const GfVec3d apex = frustum.GetPosition();

    // The far rectangle: left bottom, right bottom, left top, right top.
    addSegment(identity, c[4], c[5], segments);
    addSegment(identity, c[5], c[7], segments);
    addSegment(identity, c[7], c[6], segments);
    addSegment(identity, c[6], c[4], segments);
    for (int i = 0; i < 4; ++i) {
        if (orthographic) {
            addSegment(identity, c[i], c[i + 4], segments);
        } else {
            addSegment(identity, apex, c[i + 4], segments);
        }
    }
    if (orthographic) {
        addSegment(identity, c[0], c[1], segments);
        addSegment(identity, c[1], c[3], segments);
        addSegment(identity, c[3], c[2], segments);
        addSegment(identity, c[2], c[0], segments);
    }
    // The up marker: a triangle on the far rectangle's top edge.
    const GfVec3d topLeft = c[6];
    const GfVec3d topRight = c[7];
    const GfVec3d up = (c[6] - c[4]) * 0.25;
    const GfVec3d topMid = (topLeft + topRight) * 0.5;
    const GfVec3d half = (topRight - topLeft) * 0.2;
    addSegment(identity, topMid - half, topMid + up, segments);
    addSegment(identity, topMid + up, topMid + half, segments);
    addSegment(identity, topMid + half, topMid - half, segments);
} // buildCameraLines

void
buildLightLines(const UsdPrim& prim,
                GuideKindEnum kind,
                const GfMatrix4d& localToWorld,
                UsdTimeCode time,
                double length,
                std::vector<GfVec3d>* segments)
{
    const GfMatrix4d& m = localToWorld;
    const GfVec3d o(0., 0., 0.);
    const GfVec3d x(1., 0., 0.), y(0., 1., 0.), z(0., 0., 1.);
    const double minSize = length * kMinLightSize;

    switch (kind) {
    case eGuideKindCamera:
        break;
    case eGuideKindDistantLight: {
        // A disc of parallel rays shining down -Z.
        const double r = length * 0.15;
        addCircle(m, o, x, y, r, segments);
        addArrow(m, o, -z, length * 0.6, segments);
        for (int i = 0; i < 4; ++i) {
            const double a = i * M_PI * 0.5;
            const GfVec3d start = (x * std::cos(a) + y * std::sin(a)) * r;
            addSegment(m, start, start - z * length * 0.4, segments);
        }
        break;
    }
    case eGuideKindSphereLight: {
        const double r = std::max(attributeValue<float>(UsdLuxSphereLight(prim).GetRadiusAttr(), time, 0.5), minSize);
        addCircle(m, o, x, y, r, segments);
        addCircle(m, o, y, z, r, segments);
        addCircle(m, o, z, x, r, segments);
        break;
    }
    case eGuideKindRectLight: {
        const UsdLuxRectLight light(prim);
        const double w = std::max(attributeValue<float>(light.GetWidthAttr(), time, 1.), minSize) * 0.5;
        const double h = std::max(attributeValue<float>(light.GetHeightAttr(), time, 1.), minSize) * 0.5;
        const GfVec3d lb(-w, -h, 0.), rb(w, -h, 0.), rt(w, h, 0.), lt(-w, h, 0.);
        addSegment(m, lb, rb, segments);
        addSegment(m, rb, rt, segments);
        addSegment(m, rt, lt, segments);
        addSegment(m, lt, lb, segments);
        addSegment(m, lb, rt, segments);
        addSegment(m, rb, lt, segments);
        addArrow(m, o, -z, std::max(std::min(w, h), length * 0.25), segments);
        break;
    }
    case eGuideKindDiskLight: {
        const double r = std::max(attributeValue<float>(UsdLuxDiskLight(prim).GetRadiusAttr(), time, 0.5), minSize);
        addCircle(m, o, x, y, r, segments);
        addArrow(m, o, -z, std::max(r, length * 0.25), segments);
        break;
    }
    case eGuideKindCylinderLight: {
        const UsdLuxCylinderLight light(prim);
        const double r = std::max(attributeValue<float>(light.GetRadiusAttr(), time, 0.5), minSize);
        const double half = std::max(attributeValue<float>(light.GetLengthAttr(), time, 1.), minSize) * 0.5;
        addCircle(m, x * half, y, z, r, segments);
        addCircle(m, -x * half, y, z, r, segments);
        for (int i = 0; i < 4; ++i) {
            const double a = i * M_PI * 0.5;
            const GfVec3d rim = (y * std::cos(a) + z * std::sin(a)) * r;
            addSegment(m, rim - x * half, rim + x * half, segments);
        }
        break;
    }
    case eGuideKindDomeLight: {
        // A dome icon at the origin: the horizon and two arcs over the +Y pole.
        const double r = length * 0.25;
        addCircle(m, o, x, z, r, segments);
        addArc(m, o, x, y, r, 0., M_PI, segments);
        addArc(m, o, z, y, r, 0., M_PI, segments);
        break;
    }
    case eGuideKindOtherLight: {
        const double r = length * 0.1;
        addSegment(m, -x * r, x * r, segments);
        addSegment(m, -y * r, y * r, segments);
        addSegment(m, -z * r, z * r, segments);
        break;
    }
    } // switch
} // buildLightLines

std::vector<Guide>
buildGuides(const UsdStageRefPtr& stage,
            const std::vector<GuidePrim>& prims,
            UsdTimeCode time,
            double length)
{
    std::vector<Guide> guides;

    if (!stage) {
        return guides;
    }
    UsdGeomXformCache xformCache(time);
    guides.reserve(prims.size());
    for (const GuidePrim& source : prims) {
        const UsdPrim prim = stage->GetPrimAtPath(source.path);
        if (!prim) {
            continue;
        }
        const UsdGeomImageable imageable(prim);
        if (imageable && imageable.ComputeVisibility(time) == UsdGeomTokens->invisible) {
            continue;
        }
        Guide guide;
        guide.path = source.path.GetString();
        guide.kind = source.kind;
        const GfMatrix4d localToWorld = xformCache.GetLocalToWorldTransform(prim);
        if (source.kind == eGuideKindCamera) {
            GfCamera camera = UsdGeomCamera(prim).GetCamera(time);
            camera.SetTransform(localToWorld);
            buildCameraLines(camera, length, &guide.segments);
        } else {
            buildLightLines(prim, source.kind, localToWorld, time, length, &guide.segments);
        }
        if (!guide.segments.empty()) {
            guides.push_back(std::move(guide));
        }
    }

    return guides;
} // buildGuides

int
hitTest(const SceneGizmo::View& view,
        const std::vector<Guide>& guides,
        const GfVec2d& pixel,
        const std::string& skipPath,
        GfVec3d* world)
{
    int best = -1;
    double bestDistance = kHitTolerancePixels;

    for (std::size_t g = 0; g < guides.size(); ++g) {
        const Guide& guide = guides[g];
        if (guide.path == skipPath) {
            continue;
        }
        for (std::size_t k = 0; k + 1 < guide.segments.size(); k += 2) {
            const GfVec3d& wa = guide.segments[k];
            const GfVec3d& wb = guide.segments[k + 1];
            GfVec2d a, b;
            if (!SceneGizmo::project(view, wa, &a) || !SceneGizmo::project(view, wb, &b)) {
                continue;
            }
            double t;
            const double distance = distanceToSegment(pixel, a, b, &t);
            if (distance > bestDistance) {
                continue;
            }
            bestDistance = distance;
            best = (int)g;
            if (world) {
                // t runs evenly on screen; undo the perspective divide to find it along the line.
                const double inverseA = 1. / clipW(view, wa);
                const double inverseB = 1. / clipW(view, wb);
                const double s = t * inverseB / ((1. - t) * inverseA + t * inverseB);
                *world = wa + (wb - wa) * s;
            }
        }
    }

    return best;
} // hitTest

std::vector<int>
guidesInRect(const SceneGizmo::View& view,
             const std::vector<Guide>& guides,
             const GfVec2d& lo,
             const GfVec2d& hi,
             const std::string& skipPath)
{
    std::vector<int> hits;

    for (std::size_t g = 0; g < guides.size(); ++g) {
        const Guide& guide = guides[g];
        if (guide.path == skipPath) {
            continue;
        }
        for (std::size_t k = 0; k + 1 < guide.segments.size(); k += 2) {
            GfVec2d a, b;
            if (SceneGizmo::project(view, guide.segments[k], &a) && SceneGizmo::project(view, guide.segments[k + 1], &b)
                && segmentMeetsRect(a, b, lo, hi)) {
                hits.push_back((int)g);
                break;
            }
        }
    }

    return hits;
}
} // namespace SceneGuides

NATRON_NAMESPACE_EXIT
