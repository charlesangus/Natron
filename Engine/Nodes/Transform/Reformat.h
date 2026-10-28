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

#ifndef Engine_Nodes_Transform_Reformat_h
#define Engine_Nodes_Transform_Reformat_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/ResampleExecutor.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Image/TransformMath.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

#define PLUGINID_NATRON_REFORMAT "net.sf.openfx.Reformat"
#define PLUGIN_MAJOR_NATRON_REFORMAT 3

#define kReformatParamUseRoD "useRoD"
#define kReformatParamType "reformatType"
#define kReformatParamTypeOptionToFormat "format"
#define kReformatParamTypeOptionToBox "box"
#define kReformatParamTypeOptionScale "scale"
#define kReformatParamTypeOptionToProjectFormat "project"
#define kReformatParamBoxSize "boxSize"
#define kReformatParamBoxFixed "boxFixed"
#define kReformatParamBoxPar "boxPar"
#define kReformatParamScale "reformatScale"
#define kReformatParamScaleUniform "reformatScaleUniform"
#define kReformatParamResize "resize"
#define kReformatParamResizeOptionNone "none"
#define kReformatParamResizeOptionWidth "width"
#define kReformatParamResizeOptionHeight "height"
#define kReformatParamResizeOptionFit "fit"
#define kReformatParamResizeOptionFill "fill"
#define kReformatParamResizeOptionDistort "distort"
#define kReformatParamCenter "reformatCentered"
#define kReformatParamFlip "flip"
#define kReformatParamFlop "flop"
#define kReformatParamTurn "turn"
#define kReformatParamPreserveBoundingBox "preserveBB"

NATRON_NAMESPACE_ENTER

class Reformat;

/**
 * @brief The knob values one Reformat evaluation reads, and the time and view they were read at,
 * at which the op asks the node for its matrix. identity is the openfx-misc
 * ReformatPlugin::isIdentity(time): no centring, flip, flop or turn, and resize None.
 **/
struct ReformatParams {
    double time = 0.;
    ViewIdx view;
    bool identity = false;
    Resampler::FilterEnum filter = Resampler::eFilterCubic;
    bool clamp = false;
    bool blackOutside = false;
};

inline constexpr InputSpec kReformatInputs[] = {
    InputSpec { .label = "Source" },
};

/**
 * @brief The Resample op of Reformat, in the matrix form. The matrix depends on the input format,
 * the project format and the source's region as well as on the knobs, so transform() asks the
 * node, which the node points reformat at.
 **/
struct ReformatOp {
    using Params = ReformatParams;

    static constexpr bool kResamplesOnGpu = true;

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_REFORMAT,
        .label = "Reformat",
        .description = "Convert the image to another format or size.\n"
                       "An image transform is computed that goes from the input format, regardless of the region of definition (RoD), to the selected format. The Resize Type parameter adjust the way the transform is computed.\n"
                       "The output format is set by this effect.\n"
                       "In order to set the output format without transforming the image content, use the NoOp effect.\n"
                       "This plugin concatenates transforms.\n"
                       "See also: https://web.archive.org/web/20220627014216/http://www.opticalenquiry.com/nuke/index.php?title=Reformat",
        .grouping = PLUGIN_GROUP_TRANSFORM,
        .majorVersion = PLUGIN_MAJOR_NATRON_REFORMAT,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/net.sf.openfx.Reformat.png",
        .inputs = kReformatInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = {},
        .layers = LayerPolicy::all(),
        .op = OpKind::Resample,
    };

    Params bake(const KnobReader& reader) const;

    bool isIdentity(const Params& params,
                    const PassContext& context) const;

    /**
     * @brief The filter, clamp and black-outside knobs, no invert and no blur.
     **/
    ResampleSettings settings(const Params& params,
                              const PassContext& context) const;

    /**
     * @brief Reformat::getInverseTransformCanonical() at the Params' time and view, with the
     * source's pixel aspect ratio. It reads the node and its upstream (formats, region of
     * definition, knobs) through accessors that render threads already use, so the GPU placement
     * and record threads may call it too.
     **/
    bool transform(const Params& params,
                   const PassContext& context,
                   double amount,
                   bool invert,
                   TransformMath::Mat3* matrix) const;

    /**
     * @brief Whether preserveBB is checked: otherwise the output is clipped to the output format,
     * which no matrix carries.
     **/
    bool canConcatenate() const;

    const Reformat* reformat = nullptr;
    KnobBoolWPtr center;
    KnobBoolWPtr flip;
    KnobBoolWPtr flop;
    KnobBoolWPtr turn;
    KnobChoiceWPtr resize;
    KnobBoolWPtr preserveBB;
    KnobChoiceWPtr filter;
    KnobBoolWPtr clamp;
    KnobBoolWPtr blackOutside;
};

static_assert(GpuResampleOp<ReformatOp>);

/**
 * @brief Reformat: resamples the source so that its format (or its region of definition, with
 * useRoD) maps onto an output format chosen from a format list, a box, a scale of the source or
 * the project format, resized to the width, the height, to fit, to fill or distorted, optionally
 * centred, flipped, flopped and turned by 90 degrees. The node sets its output format, and its
 * pixel aspect ratio except with the Scale type. Knob names, defaults, visibility, output format,
 * transform, region of definition and region of interest are the openfx-misc ReformatPlugin's
 * (major 2); it registers under that plug-in's ID one major above. Resampling goes through
 * Resampler, so the output matches the OpenFX one to rounding.
 *
 * Upstream transforms are always concatenated into this node; it concatenates downstream (it
 * reports a transform the host may fold into the next node) only while preserveBB is checked,
 * since otherwise its output is clipped to the output format.
 **/
class Reformat
    : public ImageNode<ReformatOp> {
public:
    enum ReformatTypeEnum {
        eReformatTypeToFormat = 0,
        eReformatTypeToBox,
        eReformatTypeScale,
        eReformatTypeToProjectFormat
    };

    enum ResizeEnum {
        eResizeNone = 0,
        eResizeWidth,
        eResizeHeight,
        eResizeFit,
        eResizeFill,
        eResizeDistort
    };

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new Reformat(node);
    }

    explicit Reformat(NodePtr node);

    virtual ~Reformat();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    virtual bool supportsMultipleClipPARs() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual bool supportsRenderQuality() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    /**
     * @brief The framework's region, clipped to the output format unless preserveBB is checked.
     **/
    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    ReformatTypeEnum getReformatType() const WARN_UNUSED_RETURN;

    /**
     * @brief The type at (time, view), for what is derived at a frame: the choice can be keyed.
     **/
    ReformatTypeEnum getReformatType(double time,
                                     ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The PAR of the source's output at (time, view), or 1 without a source.
     **/
    double getSourcePixelAspectRatio(double time,
                                     ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The output pixel aspect ratio, the rectangle (in pixels at render scale 1, not
     * rounded) the input format is mapped to, and, when format is not null, the output format
     * in pixels, as the openfx-misc ReformatPlugin::getOutputFormat() computes them. srcPar is
     * the source's pixel aspect ratio.
     **/
    void computeOutputFormat(double time,
                             ViewIdx view,
                             double srcPar,
                             double* par,
                             RectD* rect,
                             RectI* format) const;

    /**
     * @brief The canonical matrix from output to source (invert false) or from source to output
     * (invert true), or false when there is no source or the mapping is degenerate.
     **/
    bool getInverseTransformCanonical(double time,
                                      ViewIdx view,
                                      bool invert,
                                      double srcPar,
                                      TransformMath::Mat3* matrix) const WARN_UNUSED_RETURN;

protected:
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
     * @brief The box the output format is computed from: its size in pixels, its pixel aspect
     * ratio and whether the output is cropped to it. Returns false for the To Box type, whose box
     * is the user's, true when the box was computed from the type.
     **/
    bool getBoxValues(double time,
                      ViewIdx view,
                      double srcPar,
                      int* w,
                      int* h,
                      double* par,
                      bool* boxFixed) const;

    /**
     * @brief The input format in pixels at render scale 1 (a RectD, as it is the source's region
     * of definition with useRoD) and its pixel aspect ratio.
     **/
    void getInputFormat(double time,
                        ViewIdx view,
                        double srcPar,
                        double* par,
                        RectD* rect) const;

    /**
     * @brief The source's region of definition at time, or false when there is no source.
     **/
    bool getSourceRegionOfDefinition(double time,
                                     ViewIdx view,
                                     const RenderScale& scale,
                                     RectD* rod) const;

    /**
     * @brief The project's default format in canonical coordinates, and its pixel aspect ratio.
     **/
    RectD getProjectRect(double* par) const WARN_UNUSED_RETURN;

    /**
     * @brief Shows the knobs the current type uses and hides the others.
     **/
    void refreshVisibility();

    void updateClampEnabled(double time, ViewSpec view);

    KnobBoolWPtr _useRoD;
    KnobChoiceWPtr _type;
    KnobChoiceWPtr _format;
    KnobIntWPtr _formatSize;
    KnobDoubleWPtr _formatPar;
    KnobIntWPtr _boxSize;
    KnobBoolWPtr _boxFixed;
    KnobDoubleWPtr _boxPar;
    KnobDoubleWPtr _scale;
    KnobBoolWPtr _scaleUniform;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_Reformat_h
