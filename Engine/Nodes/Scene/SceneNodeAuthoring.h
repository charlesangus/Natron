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

class ContentHasher;

/**
 * @brief What a geometry node's Material input adds to its scene: see
 * SceneNodeAuthoring::graftMaterialInput().
 **/
struct SceneMaterialGraft {
    // The input's root prims grafted under the geometry prim, for the geometry node to stack its
    // own layer on. Empty when the input is.
    ScenePayload payload;
    // Where the input's first material landed, which the geometry binds itself to. Empty when the
    // input holds no material.
    std::string materialPath;
    // A hash of what the geometry's layer reads of payload: its metadata and materialPath. A
    // geometry node keys its layer on it (SceneLayerMemo::Key::eUpstreamReadFacts).
    U64 factsDigest = 0;
    std::vector<SceneUnmappedLink> unmappedLinks;
};

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
 * @brief The project's meters per unit, which the node's layer authors.
 **/
double getMetersPerUnit(const SceneLayerEdit& edit);

/**
 * @brief Appends to h what getTimeCodesPerSecond() and openLayerStage() read of upstream: whether there is one, and its stage metadata. A node that keys its layer on the
 * upstream facts it reads (SceneLayerMemo::Key::eUpstreamReadFacts) hashes these with its own.
 **/
/**
 * @brief The file or frame pattern knob names, as an asset path to author: project variables
 * expanded, and anchored, a path still relative made absolute against the working directory. A
 * node's layer is anonymous, with no directory a relative asset path could resolve against; USDWrite
 * makes the paths relative to the file it writes when asked. Empty when the knob is.
 **/
std::string getAnchoredFilePath(const KnobFilePtr& knob);

void appendUpstreamMetadata(const ScenePayload& upstream,
                            ContentHasher* h);

/**
 * @brief Authors edit.layer's stage metadata: its time codes per second, and the project's upAxis
 * and metersPerUnit from edit.
 **/
void authorLayerMetadata(const SceneLayerEdit& edit);

/**
 * @brief authorLayerMetadata(), then opens a stage on edit.layer alone, so the node can author
 * through the Usd schema API with edit.layer as the edit target.
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
 * @brief For a geometry node's optional Material input: grafts every root prim of material under
 * the geometry prim at primPath through SceneOps::reparentRoots, memoized in memo, so the material
 * lives and moves with the geometry and each geometry node holds its own copy. The graft defines
 * primPath and its missing ancestors typeless: the geometry's own layer, stacked on
 * graft->payload, gives them their types and binds the prim with authorMaterialBinding().
 * graft->materialPath is the grafted path of material's first UsdShadeMaterial, depth first.
 * Returns false with *errorMessage set when primPath is not an absolute prim path, a root of
 * material is at primPath itself, or authoring fails.
 **/
bool graftMaterialInput(const ScenePayload& material,
                        const std::string& primPath,
                        const std::string& layerTag,
                        SceneGraftMemo* memo,
                        SceneMaterialGraft* graft,
                        std::string* errorMessage);

/**
 * @brief Binds prim to the material at materialPath (material:binding, with
 * UsdShadeMaterialBindingAPI applied). Does nothing when materialPath is empty.
 **/
bool authorMaterialBinding(const PXR_NS::UsdPrim& prim,
                           const std::string& materialPath,
                           std::string* errorMessage);

/**
 * @brief Authors knobs as xformable's op stack: xformOp:translate, xformOp:rotate<order> and
 * xformOp:scale, as the prim's whole op order.
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

// Whether any of knobs, or pivot, is animated.
bool isTransformAnimated(const SceneTransformKnobs& knobs,
                         const KnobDoublePtr& pivot);

/**
 * @brief composeTransformMatrix() of the knobs' values at frame, scale multiplied by uniform
 * scale, about pivot. The identity when a knob is gone.
 **/
PXR_NS::GfMatrix4d getTransformMatrixAt(const SceneTransformKnobs& knobs,
                                        const KnobDoublePtr& pivot,
                                        double frame);

// The axis (0 for X, 1 for Y, 2 for Z) rotateOrder turns about at position (0 first, 2 last).
int getRotateOrderAxis(SceneRotateOrderEnum rotateOrder, int position);

// The rotation part of composeTransformMatrix(), as a Gf (row-vector) matrix.
PXR_NS::GfMatrix4d composeRotationMatrix(const PXR_NS::GfVec3d& rotate,
                                         SceneRotateOrderEnum rotateOrder);

} // namespace SceneNodeAuthoring

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_SceneNodeAuthoring_h
