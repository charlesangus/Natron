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
 * The maths are a port of darktable's rawprepare module, DARKTABLE_REF release-5.6.2 (commit
 * 20891f6c6fa7), src/iop/rawprepare.c, Copyright (C) 2015-2025 darktable developers, based on
 * code by johannes hanika, distributed under the GNU GPL version 3 or later.
 */

#ifndef Engine_Nodes_Raw_RawPrepare_h
#define Engine_Nodes_Raw_RawPrepare_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Raw/RawNodeSupport.h"

#define PLUGINID_NATRON_RAWPREPARE "fr.natron.RawPrepare"
#define PLUGIN_MAJOR_NATRON_RAWPREPARE 1

#define kRawPrepareParamBlackOverride "blackLevelsOverride"
#define kRawPrepareParamBlackLevels "blackLevels"
#define kRawPrepareParamWhiteOverride "whiteLevelOverride"
#define kRawPrepareParamWhiteLevel "whiteLevel"
#define kRawPrepareParamCropOverride "cropOverride"
#define kRawPrepareParamCropLeft "cropLeft"
#define kRawPrepareParamCropTop "cropTop"
#define kRawPrepareParamCropRight "cropRight"
#define kRawPrepareParamCropBottom "cropBottom"
#define kRawPrepareParamFlatField "flatField"

enum RawPrepareFlatFieldEnum {
    eRawPrepareFlatFieldDisabled = 0,
    eRawPrepareFlatFieldEmbedded = 1,
};

NATRON_NAMESPACE_ENTER

/**
 * @brief Black and white levels and the sensor crop of a raw stream, darktable's rawprepare. Each
 * photosite becomes (value - black) / (white - black) with the black level of its cell of the
 * 2x2 block at the CFA origin, or of its channel for data without a CFA, so that black is 0 and
 * the white level is 1; the kept area is moved to the origin, the CFA origin moving with it. The
 * levels and the crop come from the source's raw/ metadata unless overridden. With Flat Field on
 * and four embedded GainMaps that fit, each photosite is also multiplied by its map's gain,
 * bilinearly interpolated, which takes the node off the GPU.
 *
 * Raw in, raw out, at the raw stage: the output's raw/stage is prepared, its black levels 0, its
 * white level 1. A source that is already prepared passes through unchanged, with a warning.
 * Mosaic photosites are never resampled, so the node renders at full resolution only.
 **/
class RawPrepare
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new RawPrepare(node);
    }

    explicit RawPrepare(NodePtr node);

    virtual ~RawPrepare();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The four embedded GainMaps of a Bayer file, indexed by the cell of the 2x2 block at
     * the sensor's top-left corner that they correct, with the grid they share. Rows run from the
     * sensor's top, as the file stores them.
     **/
    struct FlatField {
        int mapW = 0;
        int mapH = 0;
        float spacingH = 1.f;
        float spacingV = 1.f;
        float originH = 0.f;
        float originV = 0.f;
        std::array<std::vector<float>, 4> gains;
    };

    /**
     * @brief The maps flat field correction can apply, or null when maps does not hold exactly
     * four single-plane Bayer maps over the whole sensor of width x height photosites that share
     * one grid.
     **/
    static std::shared_ptr<const FlatField> acceptGainMaps(const std::vector<DngGainMap>& maps,
                                                           int width,
                                                           int height) WARN_UNUSED_RETURN;

    /**
     * @brief What a render at time and view does: the black level per cell (or channel) and the
     * scale, the source pixels kept, and whether the source is already prepared. Empty when the
     * source has no region.
     **/
    struct Settings {
        bool passThrough = false;
        // Set when the source has no raw/stage: it is not raw data, and nothing renders.
        std::optional<std::string> stageError;
        std::array<float, 4> sub = { 0.f, 0.f, 0.f, 0.f };
        std::array<float, 4> div = { 1.f, 1.f, 1.f, 1.f };
        bool hasCfa = false;
        RawLayout layout;
        RectI sourceRoD;
        // The uncropped sensor (raw/sensor), or the source's region when the source does not say.
        RectI sensor;
        RectI kept;
        // Set when Flat Field is on and the source has maps that can be applied.
        std::shared_ptr<const FlatField> flatField;
        bool flatFieldAvailable = false;
    };

    std::optional<Settings> resolveSettings(double time, ViewIdx view) WARN_UNUSED_RETURN;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;

    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool canRenderOpOnGpu(const GpuQuery& query) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOpGpu(const GpuRenderArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool getOpGpuInputLayer(int inputNb,
                                    const ImageLayerDesc& plane,
                                    double time,
                                    ViewIdx view,
                                    ImageLayerDesc* layer) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual RectI getOpGpuInputRoI(int inputNb,
                                   const RectI& outputRect,
                                   const RenderScale& scale) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    virtual ImageMetadata deriveOutputMetadata(double time,
                                               ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    void refreshOverrides();

    // Without usable maps the Flat Field choice does nothing, and says so.
    void showFlatFieldAvailability(double time);

    // While an override is off its knobs show the source's value, which is what a render uses.
    void showSourceLevels(double time);

    KnobBoolWPtr _blackOverride;
    KnobDoubleWPtr _blackLevels;
    KnobBoolWPtr _whiteOverride;
    KnobDoubleWPtr _whiteLevel;
    KnobBoolWPtr _cropOverride;
    KnobIntWPtr _cropLeft;
    KnobIntWPtr _cropTop;
    KnobIntWPtr _cropRight;
    KnobIntWPtr _cropBottom;
    KnobChoiceWPtr _flatField;
    RawNodeSupport::OwnedMessage _message;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Raw_RawPrepare_h
