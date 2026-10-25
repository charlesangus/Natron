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

#ifndef Engine_Nodes_Transform_LensDistortion_h
#define Engine_Nodes_Transform_LensDistortion_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Transform/LensModels.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#define PLUGINID_NATRON_LENSDISTORTION "LensDistortion"
#define PLUGIN_MAJOR_NATRON_LENSDISTORTION 1

#define kLensDistortionParamModel "model"
#define kLensDistortionParamDirection "direction"
#define kLensDistortionParamOutputMode "outputMode"
#define kLensDistortionParamOverscan "overscan"
#define kLensDistortionParamCropToFormat "cropToFormat"
#define kLensDistortionParamUseRoD "useRoD"

// Nuke
#define kLensDistortionParamK1 "k1"
#define kLensDistortionParamK2 "k2"
#define kLensDistortionParamCenter "center"
#define kLensDistortionParamSqueeze "anamorphicSqueeze"
#define kLensDistortionParamAsymmetric "asymmetricDistortion"

// PFBarrel
#define kLensDistortionParamPFFile "pfFile"
#define kLensDistortionParamPFC3 "pfC3"
#define kLensDistortionParamPFC5 "pfC5"
#define kLensDistortionParamPFP "pfP"
#define kLensDistortionParamPFSqueeze "pfSqueeze"

// The 3DE4 camera, shared by every 3DE model.
#define kLensDistortionParam3DEFovXA "tde4_field_of_view_xa_unit"
#define kLensDistortionParam3DEFovYA "tde4_field_of_view_ya_unit"
#define kLensDistortionParam3DEFovXB "tde4_field_of_view_xb_unit"
#define kLensDistortionParam3DEFovYB "tde4_field_of_view_yb_unit"
#define kLensDistortionParam3DEFocalLength "tde4_focal_length_cm"
#define kLensDistortionParam3DEFocusDistance "tde4_custom_focus_distance_cm"
#define kLensDistortionParam3DEFilmbackWidth "tde4_filmback_width_cm"
#define kLensDistortionParam3DEFilmbackHeight "tde4_filmback_height_cm"
#define kLensDistortionParam3DELensCenterOffsetX "tde4_lens_center_offset_x_cm"
#define kLensDistortionParam3DELensCenterOffsetY "tde4_lens_center_offset_y_cm"
#define kLensDistortionParam3DEPixelAspect "tde4_pixel_aspect"

// 3DE Classic
#define kLensDistortionParam3DEDistortion "tde4_Distortion"
#define kLensDistortionParam3DEAnamorphicSqueeze "tde4_Anamorphic_Squeeze"
#define kLensDistortionParam3DECurvatureX "tde4_Curvature_X"
#define kLensDistortionParam3DECurvatureY "tde4_Curvature_Y"
#define kLensDistortionParam3DEQuarticDistortion "tde4_Quartic_Distortion"

// 3DE4 Radial Standard Degree 4 and Radial Fisheye Degree 8
#define kLensDistortionParam3DEDistortionDegree2 "tde4_Distortion_Degree_2"
#define kLensDistortionParam3DEUDegree2 "tde4_U_Degree_2"
#define kLensDistortionParam3DEVDegree2 "tde4_V_Degree_2"
#define kLensDistortionParam3DEQuarticDistortionDegree4 "tde4_Quartic_Distortion_Degree_4"
#define kLensDistortionParam3DEUDegree4 "tde4_U_Degree_4"
#define kLensDistortionParam3DEVDegree4 "tde4_V_Degree_4"
#define kLensDistortionParam3DEPhiCylindricDirection "tde4_Phi_Cylindric_Direction"
#define kLensDistortionParam3DEBCylindricBending "tde4_B_Cylindric_Bending"
#define kLensDistortionParam3DEDegree6 "tde4_Degree_6"
#define kLensDistortionParam3DEDegree8 "tde4_Degree_8"

// 3DE4 Anamorphic Degree 6 and Anamorphic Standard Degree 4
#define kLensDistortionParam3DECx02 "tde4_Cx02_Degree_2"
#define kLensDistortionParam3DECy02 "tde4_Cy02_Degree_2"
#define kLensDistortionParam3DECx22 "tde4_Cx22_Degree_2"
#define kLensDistortionParam3DECy22 "tde4_Cy22_Degree_2"
#define kLensDistortionParam3DECx04 "tde4_Cx04_Degree_4"
#define kLensDistortionParam3DECy04 "tde4_Cy04_Degree_4"
#define kLensDistortionParam3DECx24 "tde4_Cx24_Degree_4"
#define kLensDistortionParam3DECy24 "tde4_Cy24_Degree_4"
#define kLensDistortionParam3DECx44 "tde4_Cx44_Degree_4"
#define kLensDistortionParam3DECy44 "tde4_Cy44_Degree_4"
#define kLensDistortionParam3DELensRotation "tde4_Lens_Rotation"
#define kLensDistortionParam3DESqueezeX "tde4_Squeeze_X"
#define kLensDistortionParam3DESqueezeY "tde4_Squeeze_Y"
#define kLensDistortionParam3DECx06 "tde4_Cx06_Degree_6"
#define kLensDistortionParam3DECy06 "tde4_Cy06_Degree_6"
#define kLensDistortionParam3DECx26 "tde4_Cx26_Degree_6"
#define kLensDistortionParam3DECy26 "tde4_Cy26_Degree_6"
#define kLensDistortionParam3DECx46 "tde4_Cx46_Degree_6"
#define kLensDistortionParam3DECy46 "tde4_Cy46_Degree_6"
#define kLensDistortionParam3DECx66 "tde4_Cx66_Degree_6"
#define kLensDistortionParam3DECy66 "tde4_Cy66_Degree_6"

// PanoTools
#define kLensDistortionParamPanoToolsA "pt_a"
#define kLensDistortionParamPanoToolsB "pt_b"
#define kLensDistortionParamPanoToolsC "pt_c"
#define kLensDistortionParamPanoToolsD "pt_d"
#define kLensDistortionParamPanoToolsE "pt_e"
#define kLensDistortionParamPanoToolsG "pt_g"
#define kLensDistortionParamPanoToolsT "pt_t"

// Option IDs of the model choice, in LensModels::ModelType order.
#define kLensDistortionModelNuke "nuke"
#define kLensDistortionModelPFBarrel "pfbarrel"
#define kLensDistortionModel3DEClassic "3declassic"
#define kLensDistortionModel3DEAnamorphic6 "3deanamorphic6"
#define kLensDistortionModel3DEFisheye8 "3defisheye8"
#define kLensDistortionModel3DEStandard "3deradial4"
#define kLensDistortionModel3DEAnamorphic4 "3deanamorphic4"
#define kLensDistortionModelPanoTools "panotools"

#define kLensDistortionDirectionDistort "distort"
#define kLensDistortionDirectionUndistort "undistort"

#define kLensDistortionOutputImage "image"
#define kLensDistortionOutputSTMap "stmap"

#define kLensDistortionInputSource 0
#define kLensDistortionInputMask 1

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kLensDistortionInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

inline constexpr NodeSpec kLensDistortionNodeSpec = {
    .id = PLUGINID_NATRON_LENSDISTORTION,
    .label = "LensDistortion",
    .description = "Add or remove lens distortion, or produce an STMap that applies that transform.\n"
                   "The lens model is defined over a reference format, by default the Source format. The output "
                   "format is the reference format, grown on every side by Overscan so that it can hold the parts of "
                   "the distorted or undistorted image that leave the reference format.\n"
                   "Each output pixel reads the Source where the model sends it: Distort runs the model's undistortion "
                   "and Undistort its distortion, solving the direction the model has no closed form for with Newton's "
                   "method. Filtering widens with the model's Jacobian, so strong distortion does not alias. A pixel "
                   "where the model has no solution, outside its domain or where Newton does not converge, is black.\n"
                   "In STMap output mode the output holds, in red and green, the normalized position in the Source "
                   "format each pixel reads, and in alpha 1 where the model has a solution and 0 where it has none. "
                   "Fed to the UV input of STMap with the footage on its Source input, it applies the same transform "
                   "without solving the model again for every frame, unless the parameters are animated.\n"
                   "Strong Nuke distortion on an anamorphic format leaves black corners: the solve leaves the model's "
                   "domain there.",
    .grouping = PLUGIN_GROUP_TRANSFORM,
    .majorVersion = PLUGIN_MAJOR_NATRON_LENSDISTORTION,
    .minorVersion = 0,
    .icon = NATRON_IMAGES_PATH "NativeNodes/LensDistortion.png",
    .inputs = kLensDistortionInputs,
};

inline constexpr ImagePipelineSpec kLensDistortionPipeline = {
    .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
    .layers = LayerPolicy::all(),
    .op = OpKind::Custom,
};

class LensDistortion
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new LensDistortion(node);
    }

    explicit LensDistortion(NodePtr node);

    virtual ~LensDistortion();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual std::string getInputHint(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The double knobs of the lens models, in panel order. Two-dimensional ones (the
     * centres and the asymmetry) are one entry each.
     **/
    enum ModelParam {
        eParamK1 = 0,
        eParamK2,
        eParamCenter,
        eParamSqueeze,
        eParamAsymmetric,
        eParamPFC3,
        eParamPFC5,
        eParamPFP,
        eParamPFSqueeze,
        eParam3DEFovXA,
        eParam3DEFovYA,
        eParam3DEFovXB,
        eParam3DEFovYB,
        eParam3DEFocalLength,
        eParam3DEFocusDistance,
        eParam3DEFilmbackWidth,
        eParam3DEFilmbackHeight,
        eParam3DELensCenterOffsetX,
        eParam3DELensCenterOffsetY,
        eParam3DEPixelAspect,
        eParam3DEDistortion,
        eParam3DEAnamorphicSqueeze,
        eParam3DECurvatureX,
        eParam3DECurvatureY,
        eParam3DEQuarticDistortion,
        eParam3DEDistortionDegree2,
        eParam3DEUDegree2,
        eParam3DEVDegree2,
        eParam3DEQuarticDistortionDegree4,
        eParam3DEUDegree4,
        eParam3DEVDegree4,
        eParam3DEPhiCylindricDirection,
        eParam3DEBCylindricBending,
        eParam3DECx02,
        eParam3DECy02,
        eParam3DECx22,
        eParam3DECy22,
        eParam3DECx04,
        eParam3DECy04,
        eParam3DECx24,
        eParam3DECy24,
        eParam3DECx44,
        eParam3DECy44,
        eParam3DECx06,
        eParam3DECy06,
        eParam3DECx26,
        eParam3DECy26,
        eParam3DECx46,
        eParam3DECy46,
        eParam3DECx66,
        eParam3DECy66,
        eParam3DELensRotation,
        eParam3DESqueezeX,
        eParam3DESqueezeY,
        eParam3DEDegree6,
        eParam3DEDegree8,
        eParamPanoToolsA,
        eParamPanoToolsB,
        eParamPanoToolsC,
        eParamPanoToolsD,
        eParamPanoToolsE,
        eParamPanoToolsG,
        eParamPanoToolsT,
        eParamCount
    };

private:
    // The knob values a render reads, at one time and view.
    struct Settings {
        LensModels::Direction direction = LensModels::eDirectionDistort;
        bool stmap = false;
        Resampler::FilterEnum filter = Resampler::eFilterCubic;
        bool clamp = false;
        bool blackOutside = true;
    };

    // Where the lens model lives. Lens pixels are canonical coordinates with x divided by the
    // reference pixel aspect ratio, at render scale 1: the model's coordinates, and the output's
    // pixel coordinates at scale 1.
    struct Geometry {
        // The reference format in lens pixels, and its pixel aspect ratio.
        RectD reference;
        double par = 1.;
        // The output format: the reference format grown by overscan, rounded to whole pixels.
        RectI output;
        // The Source format in Source pixels at scale 1 (its region of definition with useRoD or
        // without a format), and its pixel aspect ratio. Empty without a Source.
        RectD sourceFormat;
        double sourcePar = 1.;
    };

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    // Transparent black while Source is disconnected.
    virtual Route route(double time,
                        ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum getPreferredMetadata(NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;

    void readSettings(double time, ViewIdx view, Settings* settings) const;

    void getGeometry(double time, ViewIdx view, Geometry* geometry);

    // The model the knobs give at (time, view) over the reference format.
    LensModels::Model buildModel(double time, ViewIdx view, const Geometry& geometry) const WARN_UNUSED_RETURN;

    double paramValue(ModelParam param, double time, ViewIdx view, int dimension = 0) const WARN_UNUSED_RETURN;

    // Shows the knobs of the selected model and hides the others.
    void updateModelVisibility();

    // Reads the PFBarrel file the File knob names into the PFBarrel knobs, keyframed when the
    // file has more than one key. False, with a message, when the file cannot be read or parsed.
    bool loadPFBarrelFile();

    ExtentKnobs _extentKnobs;
    KnobChoiceWPtr _model;
    KnobChoiceWPtr _direction;
    KnobChoiceWPtr _outputMode;
    KnobDoubleWPtr _overscan;
    KnobFileWPtr _pfFile;
    KnobDoubleWPtr _params[eParamCount];
    KnobChoiceWPtr _filter;
    KnobBoolWPtr _clamp;
    KnobBoolWPtr _blackOutside;
    KnobBoolWPtr _cropToFormat;
    KnobBoolWPtr _useRoD;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_LensDistortion_h
