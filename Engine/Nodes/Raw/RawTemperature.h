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
 * A port of the raw-domain part of darktable's white balance module, DARKTABLE_REF
 * release-5.6.2 (commit 20891f6c6fa7), src/iop/temperature.c, Copyright (C) 2009-2026 darktable
 * developers, distributed under the GNU GPL version 3 or later. Its camera presets are read
 * through RawWbPresets, from darktable's data/wb_presets.json.
 */

#ifndef Engine_Nodes_Raw_RawTemperature_h
#define Engine_Nodes_Raw_RawTemperature_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <optional>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Raw/RawColorMath.h"
#include "Engine/Nodes/Raw/RawNodeSupport.h"

#define PLUGINID_NATRON_RAWTEMPERATURE "fr.natron.RawTemperature"
#define PLUGIN_MAJOR_NATRON_RAWTEMPERATURE 1

#define kRawTemperatureParamMode "mode"
#define kRawTemperatureParamPreset "preset"
#define kRawTemperatureParamFinetune "finetune"
#define kRawTemperatureParamTemperature "temperature"
#define kRawTemperatureParamTint "tint"
#define kRawTemperatureParamRed "red"
#define kRawTemperatureParamGreen "green"
#define kRawTemperatureParamBlue "blue"
#define kRawTemperatureParamSpotBottomLeft "spotBottomLeft"
#define kRawTemperatureParamSpotSize "spotSize"
#define kRawTemperatureParamPick "pick"

NATRON_NAMESPACE_ENTER

enum RawTemperatureModeEnum {
    eRawTemperatureModeAsShot = 0,
    eRawTemperatureModeCameraReference,
    eRawTemperatureModeCustom,
    eRawTemperatureModeCameraPreset,
    eRawTemperatureModeFromImageArea,
    eRawTemperatureModeAsShotToReference
};

inline constexpr InputSpec kRawTemperatureInputs[] = {
    InputSpec { .label = "Source", .kind = eDataKindRaw },
};

/**
 * @brief The white balance multipliers of darktable's white balance module, applied to prepared
 * raw data: per colour of the CFA on a mosaic, per channel on linear data, nothing on one-channel
 * data without a CFA. As Shot takes the camera's multipliers from raw/wb_coeffs; Camera
 * Reference the multipliers that make D65 white neutral through the camera matrix; Custom the
 * red, green and blue knobs, which editing the temperature or the tint sets through the camera
 * matrix (sRGB when the source has none), as darktable's sliders do; Camera Preset the setting
 * of darktable's preset database that the Preset knob names, for the source's raw/make and
 * raw/model, at the Finetune knob's tuning; From Image Area the red, green and blue knobs as the
 * Pick button last set them from the source's mean over the spot area; As Shot to Reference the
 * as-shot multipliers, with raw/wb_late_correction telling RawInputColor to move them on to the
 * Camera Reference ones before its matrix. Every mode publishes the Camera Reference multipliers
 * as raw/wb_d65_coeffs.
 **/
struct RawTemperatureOp {
    struct Params {
        RawLayout layout;
        std::array<float, 3> coeffs = { 1.f, 1.f, 1.f };
    };

    static constexpr NodeSpec kNodeSpec = {
        .id = PLUGINID_NATRON_RAWTEMPERATURE,
        .label = "RawTemperature",
        .description = "Applies the white balance to prepared raw data, by the camera's as-shot multipliers, the camera "
                       "reference (D65) multipliers, a custom temperature and tint or red, green and blue multipliers, "
                       "one of the camera's white balance presets, or the mean of an area of the image; or by the as-shot "
                       "multipliers corrected to the reference ones in RawInputColor. Goes after RawPrepare.",
        .grouping = PLUGIN_GROUP_RAW,
        .majorVersion = PLUGIN_MAJOR_NATRON_RAWTEMPERATURE,
        .minorVersion = 0,
        .icon = NATRON_IMAGES_PATH "NativeNodes/" PLUGINID_NATRON_RAWTEMPERATURE ".png",
        .outputKind = eDataKindRaw,
        .inputs = kRawTemperatureInputs,
    };

    static constexpr ImagePipelineSpec kPipeline = {
        .layers = LayerPolicy::none(),
        .op = OpKind::Point,
        .rawStage = RawStageRequirement::Prepared,
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
     * @brief The multipliers mode gives for source, As Shot or Camera Reference. Without usable
     * multipliers in the file, As Shot falls back as darktable's _find_coeffs() does: on the
     * camera reference multipliers of the colour matrix, then on the camera's first preset,
     * normalised to green 1. Without a colour matrix, Camera Reference falls back as darktable's
     * reload_defaults() does, on the camera's daylight preset. Both end on darktable's last
     * resort, (2, 1, 1.5).
     **/
    static RawColorMath::Vec3 modeMultipliers(RawTemperatureModeEnum mode,
                                              const ImageMetadata& source);

    /**
     * @brief The multipliers of the camera preset called name at tune, for the camera source
     * comes from; nothing when that camera has no such preset.
     **/
    static std::optional<RawColorMath::Vec3> presetMultipliers(const ImageMetadata& source,
                                                               const std::string& name,
                                                               int tune);

    /**
     * @brief The matrices temperature and tint go through for source: its camera's, or sRGB.
     **/
    static RawColorMath::CameraMatrices sourceMatrices(const ImageMetadata& source);

    /**
     * @brief The multipliers darktable's color_picker_apply() takes from the picked mean colour:
     * each channel's reciprocal relative to green's, clamped to [0, 8], a channel at or under
     * 0.001 counting as 1.
     **/
    static std::array<float, 3> pickedCoefficients(const std::array<float, 3>& mean);

    KnobChoiceWPtr mode;
    KnobChoiceWPtr preset;
    KnobIntWPtr finetune;
    KnobDoubleWPtr temperature;
    KnobDoubleWPtr tint;
    KnobDoubleWPtr red;
    KnobDoubleWPtr green;
    KnobDoubleWPtr blue;
    KnobDoubleWPtr spotBottomLeft;
    KnobDoubleWPtr spotSize;
    KnobButtonWPtr pick;
};

static_assert(PointOp<RawTemperatureOp>);

class RawTemperature
    : public ImageNode<RawTemperatureOp> {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new RawTemperature(node);
    }

    explicit RawTemperature(NodePtr node);

    virtual ~RawTemperature();

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;

    virtual void onMetadataRefreshed(const NodeMetadata& metadata) OVERRIDE FINAL;

    virtual ImageMetadata deriveOutputMetadata(double time,
                                               ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    void refreshEnabled();

    /**
     * @brief Fills the Preset menu with the settings darktable lists for the source's camera,
     * keeping the chosen one by name, and gives Finetune that setting's tunings for its range.
     **/
    void refreshPresets();

    /**
     * @brief While the spot area knobs are at their defaults, moves those defaults to darktable's
     * default picker box, 2% to 98% of the source's format.
     **/
    void refreshSpotDefaults();

    /**
     * @brief Reads the source at full scale over the spot area, averages it per colour of the
     * CFA (per channel without one), and stores the multipliers of that mean in the red, green
     * and blue knobs with the mode set to From Image Area. False, changing nothing, when there
     * is no source or the area misses it.
     **/
    bool pickSpot(double time,
                  ViewIdx view);

    void setCoefficientKnobs(const RawColorMath::Vec3& coeffs);

    void setTemperatureKnobs(const RawColorMath::CameraMatrices& matrices,
                             const RawColorMath::Vec3& coeffs);

    bool _syncing;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Raw_RawTemperature_h
