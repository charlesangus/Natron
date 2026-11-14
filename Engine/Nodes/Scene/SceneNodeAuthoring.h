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

#include <functional>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Scene/PrimSelector.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/SceneLoadSet.h"

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
    // When that material is a USDProject3D projection in Geometry space: the grafted path of its
    // shader, which the geometry gives its own placement (authorProjectionGeometryFrame()). Empty
    // otherwise.
    std::string geometryProjectionShader;
    // Whether that projection holds its camera, and at which project frame.
    bool projectionHeld = false;
    double projectionHoldFrame = 0.;
    // A hash of what the geometry's layer reads of payload: its metadata, materialPath and the
    // projection's fields. A
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

/**
 * @brief The live texture of layer of the image the node connected to consumer's input inputNb
 * renders: the node, its content hash and whether it varies over time, without rendering it, over
 * the project's frame range. False when nothing is connected. A node keys its layer on these (appendLiveTextureSource) and
 * authors them through recordLiveTexture().
 **/
bool getLiveTextureSource(const EffectInstance& consumer,
                          int inputNb,
                          const std::string& layer,
                          LiveTextureSource* source);

// What a layer authoring source reads of it.
void appendLiveTextureSource(const LiveTextureSource& source,
                             ContentHasher* h);

/**
 * @brief Records source as authored in edit's layer, over edit's frames, and returns it as
 * recorded: its getUri(frame) is what the layer authors at frame, and its getWindow(frame) where
 * that texture lies over the image, found without rendering.
 **/
LiveTextureSource recordLiveTexture(const SceneLayerEdit& edit,
                                    const LiveTextureSource& source);

// True when the window of recorded changes over the frames it was recorded for.
bool liveTextureWindowVaries(const LiveTextureSource& recorded);

/**
 * @brief Connects the texture coordinates st of a texture showing recorded to coordinates, through
 * a UsdTransform2d defined at transformPath that takes 0 to 1 over the image's format to the
 * texture's whole window, time-sampled when the window changes over edit's frames. Coordinates
 * outside 0 to 1 then read the overscan, and the texture's wrap mode applies beyond it.
 **/
bool connectLiveTextureCoordinates(const PXR_NS::UsdStageRefPtr& stage,
                                   const SceneLayerEdit& edit,
                                   const PXR_NS::SdfPath& transformPath,
                                   const LiveTextureSource& recorded,
                                   const PXR_NS::UsdShadeOutput& coordinates,
                                   const PXR_NS::UsdShadeInput& st);

/**
 * @brief Connects the opacity of the UsdPreviewSurface surface to the alpha of the UsdUVTexture
 * texture, in the "presence" opacity mode: the whole shaded colour, emission and specular included,
 * is scaled by the alpha, as a premultiplied image composited over what is behind it. Storm then
 * draws the surface in its translucent pass.
 **/
bool connectOpacityToAlpha(PXR_NS::UsdShadeShader surface,
                           PXR_NS::UsdShadeShader texture);

/**
 * @brief Authors the inputs of the UsdPreviewSurface surface that leave only its emission showing,
 * and creates its emissiveColor input for the caller to feed.
 **/
bool authorUnlitSurfaceInputs(PXR_NS::UsdShadeShader surface,
                              PXR_NS::UsdShadeInput* emissive);

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

// getPrimPath() with the knob read at time and view.
bool getPrimPath(const KnobStringPtr& knob,
                 double time,
                 ViewIdx view,
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
 * materialContext is the context material was evaluated on (getMaterialInputContext()).
 * Returns false with *errorMessage set when primPath is not an absolute prim path, a root of
 * material is at primPath itself, or authoring fails.
 **/
bool graftMaterialInput(const ScenePayload& material,
                        const SceneEvaluationContext& materialContext,
                        const std::string& primPath,
                        const std::string& layerTag,
                        SceneGraftMemo* memo,
                        SceneMaterialGraft* graft,
                        std::string* errorMessage);

/**
 * @brief The context the material input of graftMaterialInput() is evaluated on, for an output
 * evaluated on ctx, and the reach that input gathers as output paths, for the geometry at primPath.
 **/
SceneEvaluationContext getMaterialInputContext(const SceneEvaluationContext& ctx,
                                               const std::string& primPath);
void mapMaterialInputReach(SceneGraftMemo* memo,
                           const std::string& primPath,
                           const std::vector<std::string>& in,
                           std::vector<std::string>* out);

/**
 * @brief Whether shader is a USDProject3D projection: its glslfx source is the projection shader.
 **/
bool isProjectionShader(const PXR_NS::UsdShadeShader& shader);

/**
 * @brief For a geometry node whose Material input is a projection in Geometry space
 * (graft.geometryProjectionShader set): authors the shader's geometryFrame input on stage, the
 * geometry's local to world transform as the node places it, frameAt(frame) at a project frame:
 * once at the projection's hold frame when it holds its camera, else as a default or, when
 * animated, at every frame of getSampleFrames(). Does nothing for any other material.
 **/
bool authorProjectionGeometryFrame(const PXR_NS::UsdStageRefPtr& stage,
                                   const SceneLayerEdit& edit,
                                   const SceneMaterialGraft& graft,
                                   bool animated,
                                   const std::function<PXR_NS::GfMatrix4d(double)>& frameAt);

/**
 * @brief Binds prim to the material at materialPath (material:binding, with
 * UsdShadeMaterialBindingAPI applied). Does nothing when materialPath is empty.
 **/
bool authorMaterialBinding(const PXR_NS::UsdPrim& prim,
                           const std::string& materialPath,
                           std::string* errorMessage);

/**
 * @brief For a node assigning one shared material: grafts every root prim of material under the
 * scope root /<scope> (typed Scope) through SceneOps::reparentRoots, memoized in memo, a root named
 * like the scope passed through rather than nested. graft->materialPath is the grafted path of
 * material's first UsdShadeMaterial, depth first. materialContext is the context material was
 * evaluated on (getSharedMaterialContext()). Returns false with *errorMessage set when scope is
 * empty or authoring fails.
 **/
bool graftSharedMaterial(const ScenePayload& material,
                         const SceneEvaluationContext& materialContext,
                         const std::string& scope,
                         const std::string& layerTag,
                         SceneGraftMemo* memo,
                         SceneMaterialGraft* graft,
                         std::string* errorMessage);

/**
 * @brief The context the material input of graftSharedMaterial() is evaluated on, for an output
 * evaluated on ctx, and the reach that input gathers as output paths, for scope.
 **/
SceneEvaluationContext getSharedMaterialContext(const SceneEvaluationContext& ctx,
                                                const std::string& scope);
void mapSharedMaterialReach(SceneGraftMemo* memo,
                            const std::string& scope,
                            const std::vector<std::string>& in,
                            std::vector<std::string>* out);

/**
 * @brief Sets *mapped to the context target evaluates on when consumer evaluates on ctx: ctx is
 * mapped through each node between them by getSceneInputContext(), and the sets of several routes
 * are united. Its id is the one target's evaluation on the first route stores its result under.
 * *ownSet, when given, is set to whether the united set differs from that route's, in which case
 * an evaluation on it needs an id of its own. False, leaving *mapped alone, when no scene input
 * chain leads from consumer to target.
 **/
bool mapContextDownToNode(const EffectInstance* consumer,
                          const EffectInstance* target,
                          const SceneEvaluationContext& ctx,
                          double time,
                          ViewIdx view,
                          SceneEvaluationContext* mapped,
                          bool* ownSet = nullptr);

/**
 * @brief The context of the viewer showing effect, for work on effect's scene done outside an
 * evaluation, mapped down to effect's own paths: of the viewers whose scene input reaches effect,
 * the most recently active, else the first. *viewerContext, when given, is set to that viewer's
 * own context, unmapped, and *ownSet to mapContextDownToNode()'s. False, leaving them alone, when
 * no viewer's scene input reaches effect. Main thread.
 **/
bool getShowingViewerContext(EffectInstance* effect,
                             SceneEvaluationContext* context,
                             SceneEvaluationContext* viewerContext = nullptr,
                             bool* ownSet = nullptr);

/**
 * @brief Evaluates input at time on context into *scene for a menu refresh running as job, and
 * borrows its stage into *lease with context's load set. False when job is cancelled, input is
 * gone or the scene is empty or does not compose. Off the main thread, the reference to input
 * it takes is released on the main thread (releaseOnMainThread()).
 **/
bool borrowMenuScene(const EffectInstanceWPtr& input,
                     double time,
                     const SceneEvaluationContext& context,
                     const SceneMenuJob& job,
                     ScenePayload* scene,
                     SceneQueryPool::Lease* lease);

/**
 * @brief Releases *effect, on the main thread when called off it: a worker that dropped an
 * effect's last reference would destroy its node there.
 **/
void releaseOnMainThread(std::shared_ptr<const EffectInstance>* effect);

// Holds an effect for a scope that may end on a worker, and releases it with releaseOnMainThread().
class EffectReleasedOnMainThread {
public:
    explicit EffectReleasedOnMainThread(EffectInstancePtr effect)
        : _effect(std::move(effect))
    {
    }

    ~EffectReleasedOnMainThread()
    {
        std::shared_ptr<const EffectInstance> effect = std::move(_effect);
        releaseOnMainThread(&effect);
    }

    EffectReleasedOnMainThread(const EffectReleasedOnMainThread&) = delete;
    EffectReleasedOnMainThread& operator=(const EffectReleasedOnMainThread&) = delete;

    const EffectInstancePtr& get() const
    {
        return _effect;
    }

private:
    EffectInstancePtr _effect;
};

/**
 * @brief How many unloaded payloads a menu listing what the whole of stage holds did not search:
 * 0 when context loads everything.
 **/
std::size_t countMenuUnsearched(const PXR_NS::UsdStageRefPtr& stage,
                                const SceneEvaluationContext& context);

/**
 * @brief The context a camera input that is read but not composed into the output is evaluated
 * on: only the payloads on the way to cameraPath, whatever ctx loads, so that no load of the
 * output reaches it. ctx itself when cameraPath is not an absolute prim path, as when the first
 * camera of the input is looked for.
 **/
SceneEvaluationContext getCameraInputContext(const SceneEvaluationContext& ctx,
                                             const std::string& cameraPath);

/**
 * @brief The context a camera menu searches a camera input on: what the viewer's ctx loads,
 * so the cameras the viewer loaded are listed, and the payloads on the way to cameraPath, so
 * the chosen camera is. ctx itself when cameraPath is not an absolute prim path.
 **/
SceneEvaluationContext getCameraMenuContext(const SceneEvaluationContext& ctx,
                                            const std::string& cameraPath);

/**
 * @brief One collection-based material binding authorCollectionBinding() writes on a root prim.
 **/
struct SceneCollectionBinding {
    // The prims the collection includes, its expansion rule expandPrims taking their descendants.
    std::vector<PXR_NS::SdfPath> includes;
    std::string materialPath;
    // Names both the collection (collection:<name>) and the binding; a single identifier.
    std::string bindingName;
    // UsdShadeTokens->allPurpose, full or preview.
    PXR_NS::TfToken purpose;
    // UsdShadeTokens->strongerThanDescendants or weakerThanDescendants.
    PXR_NS::TfToken strength;
    // The root's collection-binding properties already composed, strongest first: see
    // getCollectionBindingNames().
    std::vector<PXR_NS::TfToken> existingBindings;
};

/**
 * @brief The collection-based material binding properties of prim, in native property order,
 * which is their binding strength, strongest first, across every purpose.
 **/
std::vector<PXR_NS::TfToken> getCollectionBindingNames(const PXR_NS::UsdPrim& prim);

/**
 * @brief Binds binding.materialPath to the prims binding.includes names, on root: applies
 * UsdCollectionAPI collection:<name> including them (expandPrims) and UsdShadeMaterialBindingAPI,
 * authors material:binding:collection[:<purpose>]:<name> = [collection, material] with
 * bindMaterialAs = binding.strength, and writes root's propertyOrder with that binding first, then
 * binding.existingBindings, so it is the strongest collection binding on root. Returns false with
 * *errorMessage set when the binding name is not an identifier or authoring fails.
 **/
bool authorCollectionBinding(const PXR_NS::UsdPrim& root,
                             const SceneCollectionBinding& binding,
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

/**
 * @brief The rotate values (degrees, indexed by axis) that composeRotationMatrix() turns into
 * rotation in rotateOrder. rotation's upper 3x3 must be a rotation. Of the equivalent values,
 * the ones closest to hint are returned, so that a value edited a little at a time does not jump.
 **/
PXR_NS::GfVec3d decomposeRotationMatrix(const PXR_NS::GfMatrix4d& rotation,
                                        SceneRotateOrderEnum rotateOrder,
                                        const PXR_NS::GfVec3d& hint);

// What matchPattern() found.
struct PatternMatches {
    std::vector<PXR_NS::SdfPath> paths;
    /**
     * In an evaluation that leaves payloads unloaded, when nothing matched: how many unloaded
     * payloads lie where the pattern could match and were not searched.
     **/
    std::size_t unsearched = 0;
};

// Whether a prim the pattern selects is one the node acts on.
typedef std::function<bool(const PXR_NS::UsdPrim& prim)> PatternFilter;

/**
 * @brief The prims pattern selects on upstream composed with context's load set that filter (if
 * set) keeps, in SceneOps::selectPrims() order. A union of plain prim paths in an evaluation that leaves payloads unloaded
 * is not searched for: a path matches when its prim exists, or when the prim above it that
 * exists is an unloaded payload and there is no filter, so a render loading that payload
 * finds the node's edit on it. filterKey stands for what filter keeps, so cache, which
 * remembers the matches per context, can tell when they still hold: for the same upstream,
 * pattern and filterKey, and a load set equal to the remembered one or changed only where the
 * pattern cannot reach, they are taken without a query. A stage is borrowed into *lease, with
 * context's load set, when there is none yet and one is needed; a caller reading the stage
 * afterwards borrows it the same way when *lease is still empty. The pattern's reach goes to
 * context's reach, when set. Returns false with *errorMessage set when pattern does not parse or
 * the stage does not compose.
 **/
bool matchPattern(PrimPatternCache* cache,
                  const ScenePayload& upstream,
                  const std::string& pattern,
                  const SceneEvaluationContext& context,
                  U64 filterKey,
                  const PatternFilter& filter,
                  SceneQueryPool::Lease* lease,
                  PatternMatches* matches,
                  std::string* errorMessage);

/**
 * @brief The warning a node gives when pattern matches nothing, saying how many unloaded
 * branches were not searched when there are some. consequence finishes the sentence, such as
 * "the scene is passed through unchanged".
 **/
std::string describeNoMatch(const std::string& pattern,
                            const PatternMatches& matches,
                            const std::string& consequence);

} // namespace SceneNodeAuthoring

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_SceneNodeAuthoring_h
