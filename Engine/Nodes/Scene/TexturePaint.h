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

#ifndef Engine_Nodes_Scene_TexturePaint_h
#define Engine_Nodes_Scene_TexturePaint_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Scene/PrimSelector.h"

// Free of USD: NativeRegistryAll.inc includes every node header into translation units that keep
// Py_LIMITED_API, which USD's headers cannot be compiled under.

#define PLUGINID_NATRON_TEXTUREPAINT "TexturePaint"

#define kTexturePaintParamPrimPattern "primPattern"
#define kTexturePaintParamUvPrimvar "uvPrimvar"
#define kTexturePaintParamTileResolution "tileResolution"
#define kTexturePaintParamBufferResolution "bufferResolution"
#define kTexturePaintParamTextureMode "textureMode"
#define kTexturePaintParamBakeInputsPath "bakeInputsPath"
#define kTexturePaintParamBakeInputs "bakeInputs"
#define kTexturePaintParamBakedInputs "bakedInputs"
#define kTexturePaintParamBakeOutputPath "bakeOutputPath"
#define kTexturePaintParamOutputTile "outputTile"

/**
 * The output metadata names the material input each plane of the texture set feeds, under this
 * prefix followed by the plane: <prefix>rgba is "diffuseColor", and the plane of any other paint
 * layer, such as <prefix>specular, holds that layer's name.
 **/
#define kTexturePaintMetadataPurposePrefix "natron/texturePaint/purpose/"
#define kTexturePaintPurposeDiffuseColor "diffuseColor"

// The knobs every session layer carries (RotoLayer::getHostKnobs()).
#define kTexturePaintSessionParamCameraToWorld "cameraToWorld"
#define kTexturePaintSessionParamViewHeight "viewHeight"
#define kTexturePaintSessionParamAspect "aspect"
#define kTexturePaintSessionParamOrthographic "orthographic"
#define kTexturePaintSessionParamNearClip "nearClip"
#define kTexturePaintSessionParamFarClip "farClip"
#define kTexturePaintSessionParamFrozen "frozen"

NATRON_NAMESPACE_ENTER

namespace SceneRaster {
struct ExplicitCamera;
}

/**
 * @brief Paints on the meshes of a scene through the Viewer's 3D view, into one UDIM texture set
 * the matched meshes share. The node hosts a paint RotoContext: its strokes are RotoPaint's,
 * stored in normalized view coordinates. A session, the strokes painted through one view camera,
 * is a layer under the base layer carrying that camera and whether the session is frozen (baked
 * into the Bake Output files) as layer knobs, saved with the project.
 *
 * The output is the UDIM tile Output Tile names, at Tile Resolution: the colour plane, and the
 * Layer knob's plane when it is another one and a session has strokes. Tiles are baked from the
 * sessions on first use and cached on disk (TexturePaintCache), keyed by a hash of the strokes,
 * the sessions and their cameras, the scene, the knobs that shape the texture and the Bake Inputs
 * files, which once baked are the base the sessions are painted over.
 **/
class TexturePaint
    : public NativeEffectBase {
public:
    enum TextureModeEnum {
        eTextureModeStatic = 0,
        eTextureModeAnimated
    };

    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Scene", .kind = eDataKindScene },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_TEXTUREPAINT,
        .label = "TexturePaint",
        .description = "Paint on the meshes of the input scene that Prim Pattern selects, in the Viewer's 3D view, with "
                       "RotoPaint's brushes. The matched meshes share one UDIM texture set, laid out by their UV Primvar. "
                       "Strokes land in a paint buffer of Buffer Resolution through the view camera they were painted "
                       "through; each such camera is a session. Sessions are projected into the UDIM tiles, at Tile "
                       "Resolution, with occlusion. Bake Inputs Path and Bake Output Path name the texture files the "
                       "node writes, with a <UDIM> token for the tile number. The output image is the tile Output Tile "
                       "names.",
        .grouping = PLUGIN_GROUP_3D_USD,
        .majorVersion = 1,
        .minorVersion = 0,
        .outputKind = eDataKindImage,
        .inputs = kInputs,
        .primPathKnob = kTexturePaintParamPrimPattern,
        .primPathKnobTakesPattern = true,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new TexturePaint(node);
    }

    explicit TexturePaint(NodePtr node);

    virtual ~TexturePaint();

    virtual CurveContextKindEnum getCurveContextKind() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return eCurveContextKindPaint;
    }

    // The internal paint nodes take their mask from the strokes, not from a render of this node.
    virtual bool isRotoPaintNode() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual LayerKnobSpec getLayerKnobSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return LayerKnobSpec(LayerKnobSpec::eKindLayerSelect, LayerKnobSpec::eRoleTarget, true);
    }

    // The scene input is not an image the strokes could merge over: each session's own view is.
    virtual NodePtr getPaintTreeSource() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return NodePtr();
    }

    virtual NodePtr getPaintTreeSourceFor(const RotoDrawableItem& item) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual NodePtr getPaintTreeRevealSource(const RotoDrawableItem& item, int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool isPaintTreeRoot(const RotoLayer& layer) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void onCurveLayerCreated(const RotoLayerPtr& layer) OVERRIDE FINAL;

    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual bool isMultiPlanar() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual void getComponentsNeededAndProduced(double time,
                                                ViewIdx view,
                                                EffectInstance::ComponentsNeededMap* comps,
                                                double* passThroughTime,
                                                int* passThroughView,
                                                int* passThroughInputNb) OVERRIDE FINAL;

    virtual StatusEnum getPreferredMetadataAt(double time,
                                              ViewIdx view,
                                              NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void onEffectCreated(bool mayCreateFileDialog, const CreateNodeArgs& args) OVERRIDE FINAL;

    virtual Route route(double time, ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum render(const RenderActionArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The scene input is read by the node itself, not pulled as an image.
    virtual FramesNeededMap getFramesNeeded(double time, ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    // The width and height of a UDIM tile, from Tile Resolution.
    int getTileResolution() const WARN_UNUSED_RETURN;

    // The paint buffer's longer side, from Buffer Resolution.
    int getBufferResolution() const WARN_UNUSED_RETURN;

    TextureModeEnum getTextureMode() const WARN_UNUSED_RETURN;

    /**
     * @brief Starts a session painting through camera: a new layer at the top of the base layer,
     * selected, which strokes painted next go into. Main thread.
     **/
    RotoLayerPtr addSession(const SceneRaster::ExplicitCamera& camera);

    // The sessions, the layers directly under the base layer, oldest first.
    std::list<RotoLayerPtr> getSessions() const WARN_UNUSED_RETURN;

    // False when layer does not carry the session knobs, as the base layer does not.
    static bool getSessionCamera(const RotoLayerPtr& layer, SceneRaster::ExplicitCamera* camera) WARN_UNUSED_RETURN;
    static void setSessionCamera(const RotoLayerPtr& layer, const SceneRaster::ExplicitCamera& camera);

    static bool isSessionFrozen(const RotoLayerPtr& layer) WARN_UNUSED_RETURN;
    static void setSessionFrozen(const RotoLayerPtr& layer, bool frozen);

    // The session item is painted in: the layer under the base layer it is in. Null outside one.
    static RotoLayerPtr getSessionOf(const RotoItem& item) WARN_UNUSED_RETURN;

    /**
     * @brief A session's paint buffer: Buffer Resolution on the view's longer side, the other
     * following the session camera's aspect, as 4 floats per pixel (the plane's channels in
     * order, zero past them), row-major and bottom-up.
     **/
    struct SessionBuffer {
        int width = 0;
        int height = 0;
        std::vector<float> rgba;
        // 1 where the strokes changed the view; paint of exactly the view's value reads as 0.
        std::vector<std::uint8_t> touched;
    };

    // The size renderSessionBuffer() gives session's buffer at the current Buffer Resolution.
    void getSessionBufferSize(const RotoLayerPtr& session, int* width, int* height) const;

    /**
     * @brief Renders session's strokes over its view into *buffer, for plane. The strokes paint
     * the Layer knob's plane, and of it only the channels the knob selects; any other plane, or
     * channel, is the view's, untouched. Independent of the node's own output. Main thread: the
     * session's view is created on first use. False with *error set when it cannot be rendered.
     **/
    bool renderSessionBuffer(const RotoLayerPtr& session,
                             const ImageLayerDesc& plane,
                             double time,
                             ViewIdx view,
                             SessionBuffer* buffer,
                             std::string* error);

    // The view session's strokes are painted over, created on first use. Main thread.
    NodePtr getSessionView(const RotoLayerPtr& session);

    /**
     * @brief The planes the node outputs at time and view: the colour plane, then the Layer
     * knob's plane when it is not the colour plane and a session has strokes.
     **/
    std::list<ImageLayerDesc> getOutputPlanes(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief Bake Inputs: bakes the look of the materials bound to the matched meshes at time
     * into the Bake Inputs Path files (MaterialBake), one per UDIM tile they cover and material
     * input, and makes them the base the strokes are painted over. *files receives the files
     * written, *warning what the bake stood fallbacks in for. False with *error set on failure.
     * Waits for the bake, which runs on a thread of its own so that live textures can render.
     **/
    bool bakeInputs(double time,
                    ViewIdx view,
                    std::vector<std::string>* files,
                    std::string* warning,
                    std::string* error);

    /**
     * @brief The Bake Inputs files the tiles are painted over, as the last bake wrote them:
     * the file of each purpose at each tile, Bake Inputs Path expanded. Empty before any bake.
     **/
    std::vector<std::string> getBakedInputFiles() const WARN_UNUSED_RETURN;

protected:
    virtual ImageMetadata deriveOutputMetadata(double time, ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    struct SessionView {
        RotoLayerWPtr session;
        NodePtr node;
    };

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;

    NodePtr findSessionView(const RotoLayer* session) const WARN_UNUSED_RETURN;

    // Creates session's view if it has none and brings it in step; *created tells which.
    NodePtr ensureSessionView(const RotoLayerPtr& session, bool* created);

    void syncSessionView(const RotoLayerPtr& session, const NodePtr& viewNode);

    // Brings every view in step after a change to the scene input or a knob they copy.
    void syncSessionViews();

    // renderSessionBuffer() over viewNode, which must be session's view. Any thread.
    bool renderSessionBufferFrom(const NodePtr& viewNode,
                                 const RotoLayerPtr& session,
                                 const ImageLayerDesc& plane,
                                 double time,
                                 ViewIdx view,
                                 SessionBuffer* buffer,
                                 std::string* error);

    // The plane strokes paint: the Layer knob's, or the colour plane when it selects none.
    ImageLayerDesc getPaintedPlane(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    bool sessionHasStrokes(const RotoLayerPtr& session) const WARN_UNUSED_RETURN;

    // Bake Inputs Path with [Project] expanded; false with *error set unless it is absolute.
    bool getBakeInputsPattern(std::string* pattern, std::string* error) const WARN_UNUSED_RETURN;

    // The purposes and UDIM tiles the last Bake Inputs wrote, from the saved record of it.
    void getBakedInputs(std::vector<std::string>* purposes, std::vector<int>* tiles) const;

    // The baked file of purpose at tile, empty when the last bake wrote none.
    std::string getBakedInputPath(const std::string& purpose, int tile) const WARN_UNUSED_RETURN;

    // What the tiles of view are cached under, whatever the frame; changes with anything they depend on.
    U64 getTileCacheKey(ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief Bakes and caches planes of tile at time and view unless they are cached already;
     * *paths receives the file of each plane, in order, and *note the overlap warning of the
     * tile, empty when its meshes' UV layouts do not overlap. False with *error set on failure,
     * and with *error empty when the render was aborted.
     **/
    bool ensureTiles(double time,
                     ViewIdx view,
                     int tile,
                     const std::list<ImageLayerDesc>& planes,
                     std::vector<std::string>* paths,
                     std::string* note,
                     std::string* error);

    // Views are looked up from render threads while the main thread creates them.
    mutable std::mutex _viewsMutex;
    std::map<const RotoLayer*, SessionView> _views;

    // Render threads asking for one tile at once bake it once.
    std::mutex _bakeMutex;

    PrimSelector _primPattern;
    KnobStringWPtr _uvPrimvar;
    KnobChoiceWPtr _tileResolution;
    KnobChoiceWPtr _bufferResolution;
    KnobChoiceWPtr _textureMode;
    KnobOutputFileWPtr _bakeInputsPath;
    KnobButtonWPtr _bakeInputs;
    // What the last Bake Inputs wrote, saved with the project, so the tiles keep painting over
    // those files and their cache key changes with each bake.
    KnobStringWPtr _bakedInputs;
    KnobOutputFileWPtr _bakeOutputPath;
    KnobIntWPtr _outputTile;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_TexturePaint_h
