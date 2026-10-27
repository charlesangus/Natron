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

#ifndef NATRON_GUI_SCENEGUIDES_H
#define NATRON_GUI_SCENEGUIDES_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Gui/SceneGizmo.h"

// The scene viewer's camera and light guides, free of GL: which prims get one, their shapes as
// world-space line segments at a time, and which guide a pixel is on. Storm draws neither, so the
// viewer draws these over its image. Includes USD, so only SceneViewport.cpp and tests include it.

NATRON_NAMESPACE_ENTER

namespace SceneGuides {
enum GuideKindEnum {
    eGuideKindCamera = 0,
    eGuideKindDistantLight,
    eGuideKindSphereLight,
    eGuideKindRectLight,
    eGuideKindDiskLight,
    eGuideKindCylinderLight,
    eGuideKindDomeLight,
    eGuideKindOtherLight
};

// How far from a guide's drawn line a pixel may be and still be on it.
const double kHitTolerancePixels = 4.;

struct GuidePrim {
    PXR_NS::SdfPath path;
    GuideKindEnum kind = eGuideKindCamera;
};

struct Guide {
    std::string path;
    GuideKindEnum kind = eGuideKindCamera;
    // Pairs of points, one line segment per pair, in world space.
    std::vector<PXR_NS::GfVec3d> segments;
};

// The kind's script name, as "camera" or "rectLight".
const char* getGuideKindName(GuideKindEnum kind);

// The kind of guide prim gets. False if it gets none: it is neither a camera nor a light, or it is
// a gprim that carries the light API, which Storm draws itself.
bool getGuideKind(const PXR_NS::UsdPrim& prim, GuideKindEnum* kind);

// The prims of stage that get a guide, in traversal order, and in *hasLights whether stage has
// any UsdLux light, a gprim with the light API included. Depends only on the stage's composition,
// so a viewer keeps it until the stage changes.
std::vector<GuidePrim> findGuidePrims(const PXR_NS::UsdStageRefPtr& stage, bool* hasLights = nullptr);

// A camera's frustum from its apex out to length (a box from its plane out to length when it is
// orthographic), with the film-back rectangle at the far end and a triangle above it marking up.
void buildCameraLines(const PXR_NS::GfCamera& camera, double length, std::vector<PXR_NS::GfVec3d>* segments);

// The icon of a light of kind, in world space through localToWorld. length sets the size of the
// parts the light's own attributes do not size (arrows, the dome icon).
void buildLightLines(const PXR_NS::UsdPrim& prim,
                     GuideKindEnum kind,
                     const PXR_NS::GfMatrix4d& localToWorld,
                     PXR_NS::UsdTimeCode time,
                     double length,
                     std::vector<PXR_NS::GfVec3d>* segments);

// The guides of prims at time. Invisible prims get none.
std::vector<Guide> buildGuides(const PXR_NS::UsdStageRefPtr& stage,
                               const std::vector<GuidePrim>& prims,
                               PXR_NS::UsdTimeCode time,
                               double length);

// The guide under pixel, the index of the nearest within kHitTolerancePixels on screen, or -1.
// Guides at skipPath are ignored. *world is the point on the guide under the pixel.
int hitTest(const SceneGizmo::View& view,
            const std::vector<Guide>& guides,
            const PXR_NS::GfVec2d& pixel,
            const std::string& skipPath,
            PXR_NS::GfVec3d* world);

// The guides with a segment end inside the pixel rectangle [lo, hi], or crossing it.
std::vector<int> guidesInRect(const SceneGizmo::View& view,
                              const std::vector<Guide>& guides,
                              const PXR_NS::GfVec2d& lo,
                              const PXR_NS::GfVec2d& hi,
                              const std::string& skipPath);
} // namespace SceneGuides

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_SCENEGUIDES_H
