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

#ifndef Engine_Nodes_Scene_SceneNodeAuthoring_h
#define Engine_Nodes_Scene_SceneNodeAuthoring_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief What the scene nodes that author prims from knobs share: a stage to author their layer
 * through with the Usd API, and the time policy for knob values. A knob that is not animated
 * authors the attribute's default; an animated one (keyframes or an expression in any dimension)
 * authors a time sample at every frame of the project range, at the timecode of the same instant.
 **/
namespace SceneNodeAuthoring {
/**
 * @brief The time codes per second of the node's layer: the upstream stage's, so the layer's
 * samples need no rate conversion, or the project frame rate for a node with no scene input, so
 * frame f is timecode f.
 **/
double getTimeCodesPerSecond(const SceneLayerEdit& edit);

/**
 * @brief The meters per unit of the scene the node's layer goes on top of: the upstream stage's,
 * or USD's fallback (centimetres) for a node with no scene input.
 **/
double getMetersPerUnit(const SceneLayerEdit& edit);

/**
 * @brief Sets edit.layer's time codes per second and opens a stage on it alone, so the node can
 * author through the Usd schema API with edit.layer as the edit target.
 **/
PXR_NS::UsdStageRefPtr openLayerStage(const SceneLayerEdit& edit,
                                      std::string* errorMessage);

/**
 * @brief The absolute prim path the knob names. Returns false with *errorMessage set if it is
 * not one.
 **/
bool getPrimPath(const KnobStringPtr& knob,
                 PXR_NS::SdfPath* path,
                 std::string* errorMessage);

// Whether any dimension of knob has keyframes or an expression.
bool isAnimated(const KnobIPtr& knob);

// The frames animated knobs are sampled at: every whole frame of the project range.
std::vector<double> getSampleFrames(const SceneLayerEdit& edit);

// The timecode of the node's layer at frame.
PXR_NS::UsdTimeCode frameToTimeCode(const SceneLayerEdit& edit,
                                    double frame);

/**
 * @brief Authors attribute from valueAt(frame): its default when animated is false, else a
 * time sample at every frame of getSampleFrames().
 **/
template <typename VALUE_AT>
bool
authorValue(const PXR_NS::UsdAttribute& attribute,
            const SceneLayerEdit& edit,
            bool animated,
            const VALUE_AT& valueAt)
{
    if (!attribute) {
        return false;
    }
    if (!animated) {
        return attribute.Set(valueAt(edit.firstFrame));
    }
    const std::vector<double> frames = getSampleFrames(edit);
    for (double frame : frames) {
        if (!attribute.Set(valueAt(frame), frameToTimeCode(edit, frame))) {
            return false;
        }
    }

    return true;
}

/**
 * @brief Authors knobs as xformable's op stack: xformOp:translate, xformOp:rotate<order> and
 * xformOp:scale, replacing whatever op order a weaker layer gives the prim.
 **/
bool authorTransform(const SceneTransformKnobs& knobs,
                     const PXR_NS::UsdGeomXformable& xformable,
                     const SceneLayerEdit& edit,
                     std::string* errorMessage);
/**
 * @brief Whether name matches glob, in which * stands for any run of characters (none included)
 * and ? for any one character.
 **/
bool matchesGlob(const std::string& name,
                 const std::string& glob);

/**
 * @brief The prims of stage a prim pattern names. The pattern is a whitespace-separated list of
 * absolute prim paths whose names may hold the wildcards of matchesGlob(), so a last name of *
 * names every child of the prim before it. Only active, defined, non-abstract prims match. *paths holds them
 * in the order the pattern names them, each once. Returns false with *errorMessage set when an
 * entry is not an absolute path; an empty pattern names no prim.
 **/
bool matchPrims(const PXR_NS::UsdStageRefPtr& stage,
                const std::string& pattern,
                std::vector<PXR_NS::SdfPath>* paths,
                std::string* errorMessage);

/**
 * @brief The transform that scales by scale, turns by rotate (degrees, in the rotate order) and
 * moves by translate, about pivot: in column-vector notation T(translate) T(pivot) R S
 * T(-pivot). Returned as a Gf matrix, which multiplies row vectors, so it is the transpose of
 * that product.
 **/
PXR_NS::GfMatrix4d composeTransformMatrix(const PXR_NS::GfVec3d& translate,
                                          const PXR_NS::GfVec3d& rotate,
                                          const PXR_NS::GfVec3d& scale,
                                          const PXR_NS::GfVec3d& pivot,
                                          SceneRotateOrderEnum rotateOrder);

// The axis (0 for X, 1 for Y, 2 for Z) rotateOrder turns about at position (0 first, 2 last).
int getRotateOrderAxis(SceneRotateOrderEnum rotateOrder, int position);

// The rotation part of composeTransformMatrix(), as a Gf (row-vector) matrix.
PXR_NS::GfMatrix4d composeRotationMatrix(const PXR_NS::GfVec3d& rotate,
                                         SceneRotateOrderEnum rotateOrder);

} // namespace SceneNodeAuthoring

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_SceneNodeAuthoring_h
