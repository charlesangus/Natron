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
 * developers, distributed under the GNU GPL version 3 or later.
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

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Raw/RawColorMath.h"
#include "Engine/Nodes/Raw/RawNodeSupport.h"

#define PLUGINID_NATRON_RAWTEMPERATURE "fr.natron.RawTemperature"
#define PLUGIN_MAJOR_NATRON_RAWTEMPERATURE 1

#define kRawTemperatureParamMode "mode"
#define kRawTemperatureParamTemperature "temperature"
#define kRawTemperatureParamTint "tint"
#define kRawTemperatureParamRed "red"
#define kRawTemperatureParamGreen "green"
#define kRawTemperatureParamBlue "blue"

NATRON_NAMESPACE_ENTER

enum RawTemperatureModeEnum {
    eRawTemperatureModeAsShot = 0,
    eRawTemperatureModeCameraReference,
    eRawTemperatureModeCustom
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
 * matrix (sRGB when the source has none), as darktable's sliders do.
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
                       "reference (D65) multipliers, or a custom temperature and tint or red, green and blue multipliers. "
                       "Goes after RawPrepare.",
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
     * @brief The multipliers mode gives for source: As Shot falls back on Camera Reference when
     * the file has no usable multipliers, and both on darktable's last resort, (2, 1, 1.5).
     **/
    static RawColorMath::Vec3 modeMultipliers(RawTemperatureModeEnum mode,
                                              const ImageMetadata& source);

    /**
     * @brief The matrices temperature and tint go through for source: its camera's, or sRGB.
     **/
    static RawColorMath::CameraMatrices sourceMatrices(const ImageMetadata& source);

    KnobChoiceWPtr mode;
    KnobDoubleWPtr temperature;
    KnobDoubleWPtr tint;
    KnobDoubleWPtr red;
    KnobDoubleWPtr green;
    KnobDoubleWPtr blue;
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
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual ImageMetadata deriveOutputMetadata(double time,
                                               ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    void refreshEnabled();

    void setCoefficientKnobs(const RawColorMath::Vec3& coeffs);

    void setTemperatureKnobs(const RawColorMath::CameraMatrices& matrices,
                             const RawColorMath::Vec3& coeffs);

    bool _syncing;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Raw_RawTemperature_h
