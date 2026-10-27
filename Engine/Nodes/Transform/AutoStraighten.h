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

#ifndef Engine_Nodes_Transform_AutoStraighten_h
#define Engine_Nodes_Transform_AutoStraighten_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <list>
#include <mutex>
#include <optional>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/ResampleExecutor.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Image/TransformMath.h"
#include "Engine/Nodes/Transform/AshiftFit.h"
#include "Engine/Nodes/Transform/AshiftLines.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

#define PLUGINID_NATRON_AUTOSTRAIGHTEN "AutoStraighten"
#define PLUGIN_MAJOR_NATRON_AUTOSTRAIGHTEN 1

#define kAutoStraightenParamRotation "rotation"
#define kAutoStraightenParamLensShiftV "lensShiftV"
#define kAutoStraightenParamLensShiftH "lensShiftH"
#define kAutoStraightenParamShear "shear"
#define kAutoStraightenParamAutoCrop "autoCrop"
#define kAutoStraightenParamLensModel "lensModel"
#define kAutoStraightenParamFocalLength "focalLength"
#define kAutoStraightenParamCropFactor "cropFactor"
#define kAutoStraightenParamLensDependence "lensDependence"
#define kAutoStraightenParamAspectAdjust "aspectAdjust"
#define kAutoStraightenParamStructure "structure"
#define kAutoStraightenParamEnhance "enhance"
#define kAutoStraightenParamFitParameters "fitParameters"
#define kAutoStraightenParamFitVertical "fitVertical"
#define kAutoStraightenParamFitHorizontal "fitHorizontal"
#define kAutoStraightenParamFitBoth "fitBoth"
#define kAutoStraightenParamTopLeft "topLeft"
#define kAutoStraightenParamTopRight "topRight"
#define kAutoStraightenParamBottomLeft "bottomLeft"
#define kAutoStraightenParamBottomRight "bottomRight"

NATRON_NAMESPACE_ENTER

class AutoStraighten;

/**
 * @brief The knob values one AutoStraighten evaluation reads. The model is darktable's as the
 * pipeline uses it (commit_params()): the stored rotation is the negated knob value.
 **/
struct AutoStraightenParams {
    double time = 0.;
    ViewIdx view;
    Ashift::Model model;
    Ashift::CropModeEnum crop = Ashift::eCropLargest;
    Resampler::FilterEnum filter = Resampler::eFilterCubic;
    bool clamp = false;
    bool blackOutside = false;
};

inline constexpr InputSpec kAutoStraightenInputs[] = {
    InputSpec { .label = "Source" },
};

/**
 * @brief The Resample op of AutoStraighten, in the matrix form: the homography and the crop depend
 * on the source format, so transform() asks the node.
 **/
struct AutoStraightenOp {
    using Params = AutoStraightenParams;

    static constexpr bool kResamplesOnGpu = true;

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_AUTOSTRAIGHTEN,
        .label = "AutoStraighten",
        .description = "Correct converging lines and a tilted horizon: rotation, vertical and horizontal lens shift "
                       "(keystone) and shear, fitted automatically to the structure of the image or to a rectangle "
                       "placed in the viewer, with an automatic crop of the empty borders.\n"
                       "A port of darktable's rotate and perspective module (ashift).",
        .grouping = PLUGIN_GROUP_TRANSFORM,
        .majorVersion = PLUGIN_MAJOR_NATRON_AUTOSTRAIGHTEN,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/AutoStraighten.png",
        .inputs = kAutoStraightenInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = {},
        .layers = LayerPolicy::all(),
        .op = OpKind::Resample,
    };

    Params bake(const KnobReader& reader) const;

    bool isIdentity(const Params& params,
                    const PassContext& context) const;

    ResampleSettings settings(const Params& params,
                              const PassContext& context) const;

    bool transform(const Params& params,
                   const PassContext& context,
                   double amount,
                   bool invert,
                   TransformMath::Mat3* matrix) const;

    /**
     * @brief Never: the output is cropped to the corrected format, which no matrix carries.
     **/
    bool canConcatenate() const;

    std::optional<GpuCostHint> costHint(const Params& params,
                                        const PassContext& context) const;

    const AutoStraighten* node = nullptr;
    KnobDoubleWPtr rotation;
    KnobDoubleWPtr lensShiftV;
    KnobDoubleWPtr lensShiftH;
    KnobDoubleWPtr shear;
    KnobChoiceWPtr autoCrop;
    KnobChoiceWPtr lensModel;
    KnobDoubleWPtr focalLength;
    KnobDoubleWPtr cropFactor;
    KnobDoubleWPtr lensDependence;
    KnobDoubleWPtr aspectAdjust;
    KnobChoiceWPtr filter;
    KnobBoolWPtr clamp;
    KnobBoolWPtr blackOutside;
};

static_assert(GpuResampleOp<AutoStraightenOp>);

/**
 * @brief AutoStraighten: darktable's rotate and perspective module. The source is warped by the
 * module's homography through Resampler; the output format is the corrected image's bounding box,
 * cropped by autoCrop. The fit buttons detect lines in the source (or take the four sides of the
 * rectangle in Rectangle mode) and fit the rotation, the lens shifts and the shear to them.
 **/
class AutoStraighten
    : public ImageNode<AutoStraightenOp> {
public:
    enum LensModelEnum {
        eLensModelGeneric = 0,
        eLensModelSpecific
    };

    enum StructureEnum {
        eStructureDetectedLines = 0,
        eStructureRectangle
    };

    enum FitParametersEnum {
        eFitParametersAll = 0,
        eFitParametersRotation,
        eFitParametersLensShift,
        eFitParametersRotationAndLensShift
    };

    enum FitButtonEnum {
        eFitButtonVertical = 0,
        eFitButtonHorizontal,
        eFitButtonBoth
    };

    /**
     * @brief The geometry the warp is computed on: the source format in pixels, its pixel aspect
     * ratio, darktable's square-pixel image size, the crop box and the output format.
     **/
    struct Geometry {
        RectI source;
        double par = 1.;
        int width = 0;
        int height = 0;
        Ashift::CropBox crop;
        int outWidth = 0;
        int outHeight = 0;
        RectI output;
        bool bypass = true;
    };

    using CropOverride = std::function<bool(const Ashift::Model& model, int width, int height, Ashift::CropModeEnum mode, Ashift::CropBox* box)>;

    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new AutoStraighten(node);
    }

    explicit AutoStraighten(NodePtr node);

    virtual ~AutoStraighten();

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    virtual bool supportsMultipleClipPARs() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual bool supportsRenderQuality() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return true;
    }

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The model the knobs give at (time, view).
     **/
    Ashift::Model getModel(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The source format and pixel aspect ratio at (time, view): the source's output format,
     * or the project's without a source.
     **/
    void getSourceFormat(double time, ViewIdx view, RectI* format, double* par) const;

    /**
     * @brief The geometry for a source format, a model and a crop mode.
     **/
    Geometry computeGeometry(const RectI& source, double par, const Ashift::Model& model, Ashift::CropModeEnum crop) const WARN_UNUSED_RETURN;

    /**
     * @brief The geometry at (time, view), from the knobs and the source format.
     **/
    Geometry getGeometry(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    /**
     * @brief The canonical matrix from output to source (invert false) or from source to output
     * (invert true), or false when the warp is bypassed.
     **/
    static bool canonicalMatrix(const Geometry& geometry, const Ashift::Model& model, bool invert, TransformMath::Mat3* matrix) WARN_UNUSED_RETURN;

    /**
     * @brief Replaces the automatic crop, for tests that pin darktable's crop box.
     **/
    void setCropOverride(const CropOverride& cropOverride);

    /**
     * @brief Runs a fit button at (time, view): detects the structure (or takes the rectangle),
     * fits and sets the knobs as one undo step. Returns false, with a message on the node, when
     * the fit failed and the knobs are unchanged.
     **/
    bool fit(FitButtonEnum button, double time, ViewIdx view);

    /**
     * @brief The lines the last fit at time used, in darktable's square-pixel source coordinates
     * (y down), for drawing in the viewer; empty when no fit ran at time.
     **/
    Ashift::LineSet lines(double time) const WARN_UNUSED_RETURN;

    /**
     * @brief The fit axes darktable runs for a button and a fitParameters choice.
     **/
    static int fitAxes(FitButtonEnum button, FitParametersEnum parameters) WARN_UNUSED_RETURN;

    /**
     * @brief The four rectangle lines at (time, view) in darktable's source coordinates.
     **/
    Ashift::LineSet rectangleLines(double time, ViewIdx view) const WARN_UNUSED_RETURN;

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

    bool detectStructure(double time, ViewIdx view, Ashift::LineSet* lines, std::string* error);

    void refreshVisibility();

    void updateClampEnabled(double time, ViewSpec view);

    KnobChoiceWPtr _structure;
    KnobChoiceWPtr _enhance;
    KnobChoiceWPtr _fitParameters;
    KnobButtonWPtr _fitVertical;
    KnobButtonWPtr _fitHorizontal;
    KnobButtonWPtr _fitBoth;
    KnobDoubleWPtr _corners[4];
    mutable std::mutex _mutex;
    CropOverride _cropOverride;
    mutable Geometry _cachedGeometry;
    mutable Ashift::Model _cachedModel;
    mutable Ashift::CropModeEnum _cachedCrop = Ashift::eCropLargest;
    mutable bool _hasCachedGeometry = false;
    Ashift::LineSet _lastLines;
    double _lastLinesTime = 0.;
    bool _hasLastLines = false;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_AutoStraighten_h
