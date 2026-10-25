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
 * The maths are a port of darktable's raw denoise module, DARKTABLE_REF release-5.6.2 (commit
 * 20891f6c6fa7), src/iop/rawdenoise.c, Copyright (C) 2011-2026 darktable developers,
 * distributed under the GNU GPL version 3 or later. The kernel is in RawDenoiseKernel.cpp.
 */

#ifndef Engine_Nodes_Raw_RawDenoise_h
#define Engine_Nodes_Raw_RawDenoise_h

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
#include "Engine/Nodes/Raw/RawKernels.h"
#include "Engine/Nodes/Raw/RawNodeSupport.h"

#define PLUGINID_NATRON_RAWDENOISE "fr.natron.RawDenoise"
#define PLUGIN_MAJOR_NATRON_RAWDENOISE 1

#define kRawDenoiseParamThreshold "threshold"

NATRON_NAMESPACE_ENTER

/**
 * @brief darktable's raw denoise module on a prepared Bayer or X-Trans mosaic: each colour is
 * square-rooted to even out its noise, split into five wavelet bands whose details below a
 * threshold are removed, and squared back. Threshold sets the strength; the per-band strengths,
 * for all channels and for red, green and blue, are darktable's curves sampled at its five
 * bands, 0.5 being neutral. A threshold of 0 is an identity; data without a CFA passes through
 * with a warning, as darktable only denoises mosaics.
 *
 * Each render reads its window grown by what the five bands reach, so a tile is bit for bit the
 * pixels of the full frame, on the CPU and on the GPU. Renders at full resolution only.
 **/
class RawDenoise
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new RawDenoise(node);
    }

    explicit RawDenoise(NodePtr node);

    virtual ~RawDenoise();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The script name of the strength of band (0 the coarsest, kRawDenoiseBands - 1 the
     * finest) for channel (0 all, then red, green and blue).
     **/
    static std::string strengthKnobName(int channel, int band);

private:
    struct Settings;

    std::optional<Settings> resolveSettings(double time, ViewIdx view) WARN_UNUSED_RETURN;

    /**
     * @brief The source pixels a render of window needs with settings.
     **/
    static RectI sourceRegionFor(const RectI& window, const Settings& settings) WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief Bayer and X-Trans mosaics, and the pass-through and zero threshold as copies of the
     * source. Other CFA periods stay on the CPU.
     **/
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

    KnobDoubleWPtr _threshold;
    std::array<std::array<KnobDoubleWPtr, RawKernels::kRawDenoiseBands>, 4> _strengths;
    RawNodeSupport::OwnedMessage _message;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Raw_RawDenoise_h
