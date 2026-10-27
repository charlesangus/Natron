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
 * Ported from darktable, DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7),
 * src/iop/denoiseprofile.c, with what it calls from src/common/nlmeans_core.c,
 * src/common/eaw.c and src/common/math.h. darktable is distributed under the GNU GPL, version 3
 * or (at your option) any later version, and so is this port: GPL-3.0-or-later.
 *
 *     This file is part of darktable,
 *     Copyright (C) 2011-2026 darktable developers.
 *
 *     darktable is free software: you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation, either version 3 of the License, or
 *     (at your option) any later version.
 *
 *     darktable is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with darktable.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef Engine_Nodes_Filter_DenoiseProfiled_h
#define Engine_Nodes_Filter_DenoiseProfiled_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Filter/DenoiseProfiledKernels.h"
#include "Engine/Nodes/Filter/NoiseProfiles.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Raw/RawNodeSupport.h"

#define PLUGINID_NATRON_DENOISEPROFILED "DenoiseProfiled"
#define PLUGIN_MAJOR_NATRON_DENOISEPROFILED 1

#define kDenoiseProfiledParamProfile "profile"
#define kDenoiseProfiledParamProfileAuto "auto"
#define kDenoiseProfiledParamProfileGeneric "generic"
#define kDenoiseProfiledParamProfileUsed "profileUsed"
#define kDenoiseProfiledParamProfileA "profileA"
#define kDenoiseProfiledParamProfileB "profileB"
#define kDenoiseProfiledParamWbAdaptive "wbAdaptive"
#define kDenoiseProfiledParamMode "mode"
#define kDenoiseProfiledParamModeNlm "nlm"
#define kDenoiseProfiledParamModeNlmAuto "nlmAuto"
#define kDenoiseProfiledParamModeWavelets "wavelets"
#define kDenoiseProfiledParamModeWaveletsAuto "waveletsAuto"
#define kDenoiseProfiledParamPatchSize "patchSize"
#define kDenoiseProfiledParamSearchRadius "searchRadius"
#define kDenoiseProfiledParamScattering "scattering"
#define kDenoiseProfiledParamCentralPixelWeight "centralPixelWeight"
#define kDenoiseProfiledParamColorMode "colorMode"
#define kDenoiseProfiledParamColorModeRgb "rgb"
#define kDenoiseProfiledParamColorModeY0U0V0 "y0u0v0"
#define kDenoiseProfiledParamRgbCurves "rgbCurves"
#define kDenoiseProfiledParamY0U0V0Curves "y0u0v0Curves"
#define kDenoiseProfiledParamAdjustAutoset "adjustAutoset"
#define kDenoiseProfiledParamStrength "strength"
#define kDenoiseProfiledParamPreserveShadows "preserveShadows"
#define kDenoiseProfiledParamBiasCorrection "biasCorrection"

// What RawNodeSupport::noteFrameComputed() names the wavelet path's whole-frame result.
#define kDenoiseProfiledFrameWavelets "DenoiseProfiled.wavelets"

NATRON_NAMESPACE_ENTER

/**
 * @brief darktable's denoise (profiled) module: a generalised Anscombe transform, set by the
 * camera's noise profile at the plate's ISO, makes the noise even, non-local means or edge-aware
 * a-trous wavelets remove it, and the inverse transform, with bias correction, restores the
 * values. Works on the colour channels; alpha passes through.
 *
 * Non-local means reads its window grown by the search reach and the patch radius, so a tile is
 * the full frame's pixels. The wavelets threshold each band by statistics of the whole frame, so
 * that path reads the whole source and keeps its last result for the other tiles of the frame.
 **/
class DenoiseProfiled
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new DenoiseProfiled(node);
    }

    explicit DenoiseProfiled(NodePtr node);

    virtual ~DenoiseProfiled();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The camera a plate comes from: raw/make, raw/model and raw/iso, else exif/Make,
     * exif/Model and exif/PhotographicSensitivity (or exif/ISOSpeedRatings). iso is 0 when unknown.
     **/
    struct Camera {
        std::string make;
        std::string model;
        int iso = 0;
    };

    static Camera cameraOf(const ImageMetadata& metadata) WARN_UNUSED_RETURN;

    /**
     * @brief Why the profile may not fit the plate, empty when it is camera RGB: profiles are
     * measured on camera RGB after demosaicing and before the input colour transform.
     **/
    static std::string cameraSpaceNotice(const ImageMetadata& metadata) WARN_UNUSED_RETURN;

private:
    struct Resolved;

    std::optional<Resolved> resolve(double time, ViewIdx view) WARN_UNUSED_RETURN;

    /**
     * @brief The profiles of camera, from the database, kept for the last camera asked.
     **/
    std::vector<NoiseProfiles::Profile> profilesFor(const Camera& camera) WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;
    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;
    virtual void onKnobsLoaded() OVERRIDE FINAL;
    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;
    virtual void onCurrentFrameMetadataChanged(const NodeMetadata& metadata) OVERRIDE FINAL;
    virtual void purgeCaches() OVERRIDE FINAL;

    /**
     * @brief The profile menu for the connected camera, the profile used, and the defaults
     * darktable derives from the camera's profile.
     **/
    void refreshProfiles();

    /**
     * @brief Shows the knobs of the chosen mode and colour mode, as darktable does.
     **/
    void refreshVisibility();

    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    KnobChoiceWPtr _profile;
    KnobStringWPtr _profileUsed;
    KnobDoubleWPtr _profileA;
    KnobDoubleWPtr _profileB;
    KnobBoolWPtr _wbAdaptive;
    KnobChoiceWPtr _mode;
    KnobIntWPtr _patchSize;
    KnobIntWPtr _searchRadius;
    KnobDoubleWPtr _scattering;
    KnobDoubleWPtr _centralPixelWeight;
    KnobChoiceWPtr _colorMode;
    KnobParametricWPtr _rgbCurves;
    KnobParametricWPtr _y0u0v0Curves;
    KnobDoubleWPtr _adjustAutoset;
    KnobDoubleWPtr _strength;
    KnobDoubleWPtr _preserveShadows;
    KnobDoubleWPtr _biasCorrection;

    std::mutex _profilesMutex;
    bool _profilesValid;
    std::string _profilesKey;
    std::vector<NoiseProfiles::Profile> _profiles;

    std::mutex _frameMutex;
    bool _frameValid;
    std::string _frameKey;
    std::shared_ptr<const std::vector<float>> _frame;

    RawNodeSupport::OwnedMessage _message;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_DenoiseProfiled_h
