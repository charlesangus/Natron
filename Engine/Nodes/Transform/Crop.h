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

#ifndef Engine_Nodes_Transform_Crop_h
#define Engine_Nodes_Transform_Crop_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/RectD.h"
#include "Engine/ViewIdx.h"

#define PLUGINID_NATRON_CROP "net.sf.openfx.CropPlugin"
#define PLUGIN_MAJOR_NATRON_CROP 3

#define kCropParamSoftness "softness"
#define kCropParamReformat "reformat"
#define kCropParamIntersect "intersect"
#define kCropParamBlackOutside "blackOutside"

NATRON_NAMESPACE_ENTER

class Crop;

inline constexpr InputSpec kCropInputs[] = {
    InputSpec { .label = "Source" },
};

/**
 * @brief The Resample op of Crop, in the offset form. The offset and the matte depend on the crop
 * rectangle, which the node computes from its extent knobs and its source, so both ask the node,
 * which the node points crop at.
 **/
struct CropOp {
    struct Params {
        double time = 0.;
        ViewIdx view;
        bool reformat = false;
        bool blackOutside = false;
        double softness = 0.;
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_CROP,
        .label = "Crop",
        .description = "Removes everything outside the defined rectangle and optionally adds black edges so everything outside is black.\n"
                       "If 'Reformat' is checked, the crop rectangle, moved to (0,0), becomes the output format; with the 'Format' extent the output pixel aspect ratio is also set to this of the format.\n"
                       "This plugin does not concatenate transforms.",
        .grouping = PLUGIN_GROUP_TRANSFORM,
        .majorVersion = PLUGIN_MAJOR_NATRON_CROP,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/net.sf.openfx.CropPlugin.png",
        .inputs = kCropInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = {},
        .layers = LayerPolicy::all(),
        .op = OpKind::Resample,
    };

    static constexpr bool kClipsSourceToRoI = true;

    static constexpr bool kResamplesOnGpu = true;

    Params bake(const KnobReader& reader) const;

    /**
     * @brief Always false: even an unshifted crop changes the region of definition.
     **/
    bool isIdentity(const Params& params,
                    const PassContext& context) const;

    /**
     * @brief With reformat, the translation that brings the crop rectangle, rounded to the
     * nearest pixels at the context's scale, to the origin; zero otherwise.
     **/
    PixelOffset offset(const Params& params,
                       const PassContext& context) const;

    /**
     * @brief The black border and the softness fade, or an unset matte when neither applies. Each
     * axis weighs 0 on the border pixels and, with blackOutside, outside the rectangle; else 1 at
     * least softness inside it, and the smoothstep of the distance over softness nearer the edge.
     * The fade is the product of the two axes' smoothsteps, and every black pixel lies on a black
     * row or column, which makes the matte separable.
     **/
    ResampleMatte matte(const Params& params,
                        const PassContext& context) const;

    // Not const: the matte reads the node's metadata at its frame, which caches them.
    Crop* crop = nullptr;
    KnobDoubleWPtr softness;
    KnobBoolWPtr reformat;
    KnobBoolWPtr blackOutside;
};

static_assert(GpuOffsetResampleOp<CropOp> && !MatrixResampleOp<CropOp>);

/**
 * @brief Crop: keeps the pixels inside a rectangle, optionally fading them to black at its edges
 * (softness), translating the rectangle to the origin and making it the output format
 * (reformat), clipping it to the source's region of definition (intersect) and adding a black
 * border around it (blackOutside). The rectangle comes from the same extent knobs as a generator
 * (Format, Size, Project or Default, with Size as the initial extent), shown with the host
 * rectangle overlay while it is editable. Knob names, defaults, region of definition, clip
 * preferences and per-pixel arithmetic are the openfx-misc CropPlugin's, except that reformat
 * there sets the output format for the Format and Project extents only, and that a pixel whose
 * fade weight is exactly 0 (its centre on the rectangle's edge, without blackOutside) is written
 * 0 rather than multiplied, so a NaN or infinite source pixel there becomes 0. It registers under
 * that plug-in's ID two majors above.
 *
 * Every channel of every requested plane is cropped; the unprocessed-channel copy stays with
 * the host. The regions of definition and of interest are the node's own, not the offset form's:
 * they follow the crop rectangle, which the source's region only bounds.
 **/
class Crop
    : public ImageNode<CropOp> {
public:
    typedef ExtentKnobs::ExtentEnum ExtentEnum;
    static constexpr ExtentEnum eExtentFormat = ExtentKnobs::eExtentFormat;
    static constexpr ExtentEnum eExtentSize = ExtentKnobs::eExtentSize;
    static constexpr ExtentEnum eExtentProject = ExtentKnobs::eExtentProject;
    static constexpr ExtentEnum eExtentDefault = ExtentKnobs::eExtentDefault;

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Crop(node);
    }

    explicit Crop(NodePtr node);

    virtual ~Crop();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    ExtentEnum getExtent() const WARN_UNUSED_RETURN;

    /**
     * @brief The crop rectangle at (time, view) in canonical coordinates, and its pixel aspect
     * ratio, as the openfx-misc CropPlugin::getCropRectangle() computes it.
     *
     * useIntersect lets the intersect knob clip the rectangle to the source's region of
     * definition, and forceIntersect clips it whatever the knob says. useReformat moves the
     * rectangle's bottom left corner to the origin, and useBlackOutside grows it by one pixel
     * at scale on every side.
     **/
    void getCropRectangle(double time,
                          ViewIdx view,
                          const RenderScale& scale,
                          bool useIntersect,
                          bool forceIntersect,
                          bool useBlackOutside,
                          bool useReformat,
                          RectD* cropRect,
                          double* par) const;

protected:
    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    virtual StatusEnum getPreferredMetadataAt(double time,
                                              ViewIdx view,
                                              NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    /**
     * @brief Keeps the rectangle overlay on while the crop is not reformatted.
     **/
    void updateRectangleEnable();

    ExtentKnobs _extentKnobs;
    KnobBoolWPtr _intersect;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_Crop_h
