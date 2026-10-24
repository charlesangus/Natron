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

/*
 * The parameters and interface of darktable's color calibration module, DARKTABLE_REF
 * release-5.6.2 (commit 20891f6c6fa7), src/iop/channelmixerrgb.c, Copyright (C) 2020-2026
 * darktable developers, distributed under the GNU GPL version 3 or later: its params, their
 * defaults and ranges, its notebook pages, _update_illuminants(), _update_approx_cct(),
 * _illum_xy_callback(), gui_changed(), reload_defaults(), _get_white_balance_coeff(),
 * _dev_is_D65_chroma(), _declare_cat_on_pipe() and _set_trouble_messages(). The pixel maths are
 * in RawColorCalibrationMath.
 */

#ifndef Engine_Nodes_Raw_RawColorCalibration_h
#define Engine_Nodes_Raw_RawColorCalibration_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Raw/RawColorCalibrationMath.h"
#include "Engine/Nodes/Raw/RawNodeSupport.h"

#define PLUGINID_NATRON_RAWCOLORCALIBRATION "fr.natron.RawColorCalibration"
#define PLUGIN_MAJOR_NATRON_RAWCOLORCALIBRATION 1

#define kRawColorCalibrationParamCatOverride "catOverride"
#define kRawColorCalibrationParamAdaptation "adaptation"
#define kRawColorCalibrationParamApproxCct "approxCct"
#define kRawColorCalibrationParamIlluminant "illuminant"
#define kRawColorCalibrationParamFluorescent "illumFluo"
#define kRawColorCalibrationParamLed "illumLed"
#define kRawColorCalibrationParamTemperature "temperature"
#define kRawColorCalibrationParamHue "illumHue"
#define kRawColorCalibrationParamChroma "illumChroma"
#define kRawColorCalibrationParamX "illumX"
#define kRawColorCalibrationParamY "illumY"
#define kRawColorCalibrationParamGamut "gamut"
#define kRawColorCalibrationParamClip "clip"
#define kRawColorCalibrationParamVersion "saturationVersion"

// The rows of the R, G, B, colorfulness, brightness and gray pages: a slider per input channel,
// named with the suffix R, G or B, and a Normalize Channels toggle.
#define kRawColorCalibrationParamRed "red"
#define kRawColorCalibrationParamGreen "green"
#define kRawColorCalibrationParamBlue "blue"
#define kRawColorCalibrationParamSaturation "saturation"
#define kRawColorCalibrationParamLightness "lightness"
#define kRawColorCalibrationParamGrey "grey"
#define kRawColorCalibrationParamNormalizeSuffix "Normalize"

// The tabs.
#define kRawColorCalibrationTabCat "catTab"
#define kRawColorCalibrationTabRed "redTab"
#define kRawColorCalibrationTabGreen "greenTab"
#define kRawColorCalibrationTabBlue "blueTab"
#define kRawColorCalibrationTabColorfulness "colorfulnessTab"
#define kRawColorCalibrationTabBrightness "brightnessTab"
#define kRawColorCalibrationTabGray "grayTab"

NATRON_NAMESPACE_ENTER

/**
 * @brief The rows of darktable's mixing pages, in the order of its params.
 **/
enum RawColorCalibrationRowEnum {
    eRawColorCalibrationRowRed = 0,
    eRawColorCalibrationRowGreen,
    eRawColorCalibrationRowBlue,
    eRawColorCalibrationRowSaturation,
    eRawColorCalibrationRowLightness,
    eRawColorCalibrationRowGrey,
    eRawColorCalibrationRowCount
};

/**
 * @brief The illuminant at index in the Illuminant choice, D for an index out of range; and the
 * index of an illuminant, -1 for one the choice does not offer.
 **/
RawColorCalibrationMath::Illuminant rawColorCalibrationIlluminantAt(int index);
int rawColorCalibrationIlluminantIndex(RawColorCalibrationMath::Illuminant illuminant);

/**
 * @brief The name of the slider for input channel c (0 to 2) of row.
 **/
std::string rawColorCalibrationRowKnobName(RawColorCalibrationRowEnum row,
                                           int c);
std::string rawColorCalibrationNormalizeKnobName(RawColorCalibrationRowEnum row);

/**
 * @brief darktable's approximate correlated colour temperature label for an illuminant
 * chromaticity: "CCT: 5003 K (daylight)", "(black body)", "(invalid)", or "CCT: undefined".
 **/
std::string rawColorCalibrationCctText(float x,
                                       float y);

/**
 * @brief darktable's reload_defaults() for the image source describes: the illuminant, the
 * adaptation, the temperature and the chromaticity the node uses while its Override File
 * Defaults is off. Pipe and none when a RawColorCalibration upstream already adapts
 * (raw/cat_applied); for a raw source, the camera's white balance through
 * find_temperature_from_raw_coeffs() and _check_if_close_to_daylight(); D 5003 K and CAT16
 * otherwise.
 **/
struct RawColorCalibrationSourceDefaults {
    RawColorCalibrationMath::Illuminant illuminant;
    RawColorCalibrationMath::Adaptation adaptation;
    float temperature;
    float x;
    float y;
};

RawColorCalibrationSourceDefaults rawColorCalibrationSourceDefaults(const ImageMetadata& source);

/**
 * @brief Whether source comes from a camera raw file, whose white balance the camera illuminant
 * reads (darktable's dt_image_is_matrix_correction_supported()).
 **/
bool rawColorCalibrationIsRawSource(const ImageMetadata& source);

/**
 * @brief darktable's _get_white_balance_coeff() on source's raw/ metadata: the ratio of the D65
 * multipliers to the ones RawTemperature applied, 1 when it applied the camera reference (or the
 * as-shot ones with the late correction). True, with a ratio of 1, for a source that is not raw.
 **/
bool rawColorCalibrationWhiteBalanceRatio(const ImageMetadata& source,
                                          RawColorCalibrationMath::Vec4* customWb);

inline constexpr InputSpec kRawColorCalibrationInputs[] = {
    InputSpec { .label = "Source" },
};

/**
 * @brief darktable's color calibration (channelmixerrgb) on the working space: a chromatic
 * adaptation from the scene illuminant to D50, a 3x3 channel mix in the adaptation's space, a
 * gamut compression in xyY, colorfulness and brightness, and an optional gray mix. Working RGB
 * goes to XYZ (D50) through the project working space's matrix to XYZ (D65) and a Bradford
 * adaptation to D50, as darktable's linear working profile does.
 **/
struct RawColorCalibrationOp {
    using Params = RawColorCalibrationMath::Kernel;

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_RAWCOLORCALIBRATION,
        .label = "RawColorCalibration",
        .description = "darktable's color calibration: white balance by chromatic adaptation to D50, a channel mixer, "
                       "gamut compression, colorfulness, brightness and gray. Works on the project's working space, "
                       "after RawInputColor.",
        .grouping = PLUGIN_GROUP_RAW,
        .majorVersion = PLUGIN_MAJOR_NATRON_RAWCOLORCALIBRATION,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/" PLUGINID_NATRON_RAWCOLORCALIBRATION ".png",
        .inputs = kRawColorCalibrationInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .stages = { Stage::PassThroughUnprocessed },
        .layers = LayerPolicy::color(true, true, true, false),
        .op = OpKind::Point,
    };

    void createKnobs(KnobHolder* holder,
                     const KnobPagePtr& page);

    Params bake(const KnobReader& reader,
                const ImageMetadata& source) const;

    bool isIdentity(const Params& params) const;

    void row(const Params& params,
             const RowIO& io) const;

    GpuOpRef gpu(const Params& params) const;

    /**
     * @brief The knobs' values as darktable's params.
     **/
    RawColorCalibrationMath::Settings settings(const KnobReader& reader) const;

    /**
     * @brief The matrix from the working space of the project of holder to XYZ (D50); linear
     * Rec. 709's when the project has no matrix to XYZ.
     **/
    static RawColorCalibrationMath::Mat3 workingToXyzD50(const KnobHolder* holder);

    KnobHolder* holder = nullptr;
    KnobBoolWPtr catOverride;
    KnobChoiceWPtr adaptation;
    KnobStringWPtr approxCct;
    KnobChoiceWPtr illuminant;
    KnobChoiceWPtr fluorescent;
    KnobChoiceWPtr led;
    KnobDoubleWPtr temperature;
    KnobDoubleWPtr hue;
    KnobDoubleWPtr chroma;
    KnobDoubleWPtr x;
    KnobDoubleWPtr y;
    KnobDoubleWPtr gamut;
    KnobBoolWPtr clip;
    KnobDoubleWPtr rows[eRawColorCalibrationRowCount][3];
    KnobBoolWPtr normalize[eRawColorCalibrationRowCount];
    KnobChoiceWPtr version;
};

static_assert(PointOp<RawColorCalibrationOp>);

class RawColorCalibration
    : public ImageNode<RawColorCalibrationOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new RawColorCalibration(node);
    }

    explicit RawColorCalibration(NodePtr node);

    virtual ~RawColorCalibration();

private:
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;

    virtual void onMetadataRefreshed(const NodeMetadata& metadata) OVERRIDE FINAL;

    // raw/cat_applied while the node adapts (darktable's _declare_cat_on_pipe()).
    virtual ImageMetadata deriveOutputMetadata(double time,
                                               ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // While Override File Defaults is off the CAT knobs show the source's defaults and are
    // disabled; the adaptation is disabled while the illuminant is the camera's.
    void refreshEnabled();

    // reload_defaults() into the knobs, while Override File Defaults is off.
    void showSourceDefaults();

    // The camera illuminant is offered for a raw source only (or while it is the value).
    void refreshCameraOption();

    // _update_illuminants().
    void refreshVisibility();

    // gui_changed()'s tail: the hue and chroma of x and y, and the CCT label.
    void showIlluminant();

    // _illum_xy_callback(): x, y and the temperature from the hue and chroma.
    void applyHueChroma();

    // gui_changed() for an edit of the illuminant, its source or the temperature.
    void syncIlluminant(bool illuminantChanged,
                        bool temperatureChanged);

    // The gray normalization warning and _set_trouble_messages(), in the node's one message.
    void refreshWarnings();

    // gui_update() shows the saturation algorithm only for an edit made before version 3.
    void refreshVersionVisibility();

    RawNodeSupport::OwnedMessage _message;
    RawColorCalibrationMath::Illuminant _lastIlluminant;
    float _lastDaylightTemperature;
    float _lastBlackBodyTemperature;
    bool _syncing;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Raw_RawColorCalibration_h
