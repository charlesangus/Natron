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

#ifndef Engine_Nodes_Scene_TexturePaintView_h
#define Engine_Nodes_Scene_TexturePaintView_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RectD.h"

// Free of USD: NativeRegistryAll.inc includes every node header into translation units that keep
// Py_LIMITED_API, which USD's headers cannot be compiled under.

#define PLUGINID_NATRON_TEXTUREPAINTVIEW "TexturePaintView"

#define kTexturePaintViewParamCameraToWorld "cameraToWorld"
#define kTexturePaintViewParamViewHeight "viewHeight"
#define kTexturePaintViewParamAspect "aspect"
#define kTexturePaintViewParamOrthographic "orthographic"
#define kTexturePaintViewParamNearClip "nearClip"
#define kTexturePaintViewParamFarClip "farClip"
#define kTexturePaintViewParamPrimPattern "primPattern"
#define kTexturePaintViewParamUvPrimvar "uvPrimvar"
#define kTexturePaintViewParamBaseFiles "baseFiles"
#define kTexturePaintViewParamBaseResolution "baseResolution"

NATRON_NAMESPACE_ENTER

namespace SceneRaster {
struct ExplicitCamera;
}

/**
 * @brief The image a TexturePaint session's strokes are painted over: the meshes of the scene on
 * its input that Prim Pattern selects, unlit, as the session camera sees them. A mesh pixel holds
 * the texel of the base file of its UDIM tile under its st, sampled bilinearly, when Base Files
 * names one for the tile; otherwise the constant diffuseColor of the mesh's bound
 * UsdPreviewSurface with alpha 1, or a neutral grey when that colour is missing or comes from a
 * texture. Pixels with no mesh, or whose mesh has no usable UV there, are transparent black. Only
 * the colour plane is produced.
 *
 * The image spans the paint space: kPaintSpaceSize on the view's longer side, the shorter side
 * following the camera's aspect. Strokes are stored in it, and a paint buffer of Buffer Resolution
 * is the paint space rendered at the mipmap level bufferMipmapLevel() gives, so neither the
 * strokes nor the buffer depend on the size of the Viewer they were painted in.
 *
 * Internal: TexturePaint creates one per session, outside the project, and keeps its knobs in step
 * with the session and its own knobs.
 **/
class TexturePaintView
    : public NativeEffectBase {
public:
    // The largest Buffer Resolution, so every buffer is the paint space at a mipmap level >= 0.
    static constexpr int kPaintSpaceSize = 8192;

    static constexpr InputSpec kInputs[] = {
        InputSpec { .label = "Scene", .kind = eDataKindScene, .optional = true },
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_TEXTUREPAINTVIEW,
        .label = "TexturePaintView",
        .description = "The unlit view of a TexturePaint session's meshes through its camera, which the session's "
                       "strokes are painted over.",
        .grouping = PLUGIN_GROUP_3D_USD,
        .majorVersion = 1,
        .minorVersion = 0,
        .internal = true,
        .outputKind = eDataKindImage,
        .inputs = kInputs,
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new TexturePaintView(node);
    }

    explicit TexturePaintView(NodePtr node);

    virtual ~TexturePaintView();

    virtual LayerKnobSpec getLayerKnobSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return LayerKnobSpec();
    }

    virtual StatusEnum getPreferredMetadataAt(double time,
                                              ViewIdx view,
                                              NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;

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

    /**
     * @brief The paint space of a view of aspect (width over height), in canonical coordinates
     * from the origin: kPaintSpaceSize on the longer side, the shorter one rounded to a whole
     * number and at least 1.
     **/
    static RectD paintSpaceBounds(double aspect) WARN_UNUSED_RETURN;

    // The mipmap level at which the paint space renders with bufferResolution on its longer side.
    static unsigned int bufferMipmapLevel(int bufferResolution) WARN_UNUSED_RETURN;

    /**
     * @brief The paint-space point of normalized view coordinates (u, v): (0, 0) is the bottom-left
     * corner of the view and (1, 1) the top-right one.
     **/
    static void viewToPaintSpace(double aspect, double u, double v, double* x, double* y);

    // Main thread. Only the values that differ are set, so an unchanged view keeps its cached image.
    void setCamera(const SceneRaster::ExplicitCamera& camera);
    void setPrimPattern(const std::string& pattern);
    void setUvPrimvar(const std::string& primvar);

    /**
     * @brief The base files of the colour plane, one line per UDIM tile, each the tile number, a
     * stamp that changes with the file, and the absolute path, separated by tabs; they are
     * resampled to resolution when of another size.
     **/
    void setBase(const std::string& files, int resolution);

    double getAspect() const WARN_UNUSED_RETURN;

private:
    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void initializeKnobs() OVERRIDE FINAL;

    /**
     * @brief The view over pixels, the paint space at a mipmap level, as 4 floats per pixel,
     * row-major and bottom-up. False with *error set when the scene or the camera cannot be used.
     **/
    bool renderView(double time, ViewIdx view, const RectI& pixels, std::vector<float>* rgba, std::string* error);

    KnobDoubleWPtr _cameraToWorld;
    KnobDoubleWPtr _viewHeight;
    KnobDoubleWPtr _aspect;
    KnobBoolWPtr _orthographic;
    KnobDoubleWPtr _nearClip;
    KnobDoubleWPtr _farClip;
    KnobStringWPtr _primPattern;
    KnobStringWPtr _uvPrimvar;
    KnobStringWPtr _baseFiles;
    KnobIntWPtr _baseResolution;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_TexturePaintView_h
