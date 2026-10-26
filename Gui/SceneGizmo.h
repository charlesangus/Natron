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

#ifndef NATRON_GUI_SCENEGIZMO_H
#define NATRON_GUI_SCENEGIZMO_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

// The geometry of the scene viewer's transform handles, free of GL: where the handles of a
// USDTransform's op sit in the world, which handle a pixel is on, and the knob change a drag
// from one pixel to another makes. Includes USD, so only SceneViewport.cpp and tests include it.

NATRON_NAMESPACE_ENTER

namespace SceneGizmo {
enum HandleKindEnum {
    eHandleKindTranslate = 0, // an arrow along an axis
    eHandleKindRotate, // a ring about an axis
    eHandleKindScale // a box on an axis
};

// A handle is kind * 3 + axis (0 for X, 1 for Y, 2 for Z).
const int kNoHandle = -1;
const int kNumHandles = 9;

inline int
makeHandle(HandleKindEnum kind,
           int axis)
{
    return (int)kind * 3 + axis;
}

inline HandleKindEnum
getHandleKind(int handle)
{
    return (HandleKindEnum)(handle / 3);
}

inline int
getHandleAxis(int handle)
{
    return handle % 3;
}

// The handles' length, in pixels.
const double kHandleLengthPixels = 90.;
// How far from a handle's drawn line a pixel may be and still be on it.
const double kHitTolerancePixels = 6.;

/**
 * @brief A camera's view of the world in a width x height view whose pixels are counted from the
 * top left. The matrices are Gf (row-vector) matrices, as GfFrustum computes them.
 **/
struct View {
    PXR_NS::GfMatrix4d viewProjection;
    PXR_NS::GfMatrix4d inverseViewProjection;
    int width = 1;
    int height = 1;

    View() = default;

    View(const PXR_NS::GfMatrix4d& viewMatrix,
         const PXR_NS::GfMatrix4d& projectionMatrix,
         int w,
         int h);
};

// The pixel world projects to. False, with *pixel unset, if world is behind the camera.
bool project(const View& view, const PXR_NS::GfVec3d& world, PXR_NS::GfVec2d* pixel);

// The ray from the camera through pixel: *origin on the near plane, *direction unit length.
void pickRay(const View& view, const PXR_NS::GfVec2d& pixel, PXR_NS::GfVec3d* origin, PXR_NS::GfVec3d* direction);

// The world length one pixel spans across the view at the depth of world.
double worldSizeOfPixel(const View& view, const PXR_NS::GfVec3d& world);

/**
 * @brief Where an op's handles sit. origin is the op's pivot after the op's move; axes[i] is the
 * world direction, and length, that one unit of translate along axis i moves the prim by;
 * rotateAxes[i] is the world direction the prim turns about when component i of rotate grows:
 * axis i turned by the rotations that follow it in the rotate order; scaleAxes[i] is the world
 * direction of the prim's axis i after the op's rotation, the direction scale along axis i
 * stretches it in.
 **/
struct Frame {
    PXR_NS::GfVec3d origin;
    PXR_NS::GfVec3d axes[3];
    PXR_NS::GfVec3d rotateAxes[3];
    PXR_NS::GfVec3d scaleAxes[3];
};

/**
 * @brief The frame of an op with translate, rotate (degrees, in rotateOrder) and pivot, whose
 * result space is placed in the world by space (a Gf matrix).
 **/
Frame computeFrame(const PXR_NS::GfMatrix4d& space,
                   const PXR_NS::GfVec3d& translate,
                   const PXR_NS::GfVec3d& rotate,
                   const PXR_NS::GfVec3d& pivot,
                   SceneRotateOrderEnum rotateOrder);

/**
 * @brief The matrix that places the result of the op of prim named opName in the world at
 * timeCode: the ops before it in the prim's op order applied to the parent's world transform, or
 * to nothing if the order resets the transform stack. False if the prim has no such op.
 **/
bool computeOpSpace(const PXR_NS::UsdPrim& prim,
                    const std::string& opName,
                    double timeCode,
                    PXR_NS::GfMatrix4d* space);

// A handle as a world-space polyline, with the handles a viewer draws and picks.
struct HandleShape {
    int handle = kNoHandle;
    std::vector<PXR_NS::GfVec3d> points;
    bool closed = false;
};

// The nine handles of frame, length world units long.
std::vector<HandleShape> buildHandles(const Frame& frame, double length);

// The handle length that is kHandleLengthPixels long on screen at the frame's origin.
double getHandleLength(const View& view, const Frame& frame);

/**
 * @brief The handle drawn under pixel, within kHitTolerancePixels, or kNoHandle. The nearest
 * wins, except that a scale box wins over the arrow it sits on, and an arrow over a ring within
 * half a pixel of it.
 **/
int hitTest(const View& view, const std::vector<HandleShape>& shapes, const PXR_NS::GfVec2d& pixel);

// A point at the middle of handle, as a pixel, to aim at. False if it is behind the camera.
bool getHandlePixel(const View& view, const Frame& frame, int handle, PXR_NS::GfVec2d* pixel);

/**
 * @brief The knob change dragging handle from pressPixel to pixel makes, for the frame the op had
 * at the press: for an arrow, the amount added to translate along its axis; for a ring, the
 * degrees added to its component of rotate, measured about the ring's axis; for a box, the factor scale along its axis is
 * multiplied by (at least 0.001). False if the drag says nothing, such as a ring seen edge-on.
 **/
bool computeDragChange(const View& view,
                       const Frame& frame,
                       int handle,
                       const PXR_NS::GfVec2d& pressPixel,
                       const PXR_NS::GfVec2d& pixel,
                       double* change);
} // namespace SceneGizmo

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_SCENEGIZMO_H
