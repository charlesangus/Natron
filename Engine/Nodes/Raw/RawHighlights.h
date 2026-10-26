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
 * A port of darktable's highlight reconstruction module, DARKTABLE_REF release-5.6.2 (commit
 * 20891f6c6fa7), src/iop/highlights.c with src/iop/hlreconstruct/opposed.c, lch.c, laplacian.c and the
 * reference average of segbased.c, Copyright (C) 2009-2026 darktable developers, distributed
 * under the GNU GPL version 3 or later.
 */

#ifndef Engine_Nodes_Raw_RawHighlights_h
#define Engine_Nodes_Raw_RawHighlights_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Raw/RawNodeSupport.h"

#define PLUGINID_NATRON_RAWHIGHLIGHTS "fr.natron.RawHighlights"
#define PLUGIN_MAJOR_NATRON_RAWHIGHLIGHTS 1

#define kRawHighlightsParamMode "mode"
#define kRawHighlightsParamClip "clip"
#define kRawHighlightsParamClipOverride "clipOverride"
#define kRawHighlightsParamCombine "combine"
#define kRawHighlightsParamCandidating "candidating"
#define kRawHighlightsParamRecovery "recovery"
#define kRawHighlightsParamStrength "strength"
#define kRawHighlightsParamNoiseLevel "noiseLevel"
#define kRawHighlightsParamIterations "iterations"
#define kRawHighlightsParamScales "scales"
#define kRawHighlightsParamSolidColor "solidColor"

// What RawNodeSupport::noteFrameComputed() names this node's whole-frame computations.
#define kRawHighlightsFrameOpposedChroma "RawHighlights.opposedChroma"
#define kRawHighlightsFrameReconstructColor "RawHighlights.reconstructColor"
#define kRawHighlightsFrameSegments "RawHighlights.segments"
#define kRawHighlightsFrameLaplacian "RawHighlights.laplacian"

NATRON_NAMESPACE_ENTER

enum RawHighlightsModeEnum {
    eRawHighlightsModeClip = 0,
    eRawHighlightsModeUnclipped,
    eRawHighlightsModeOpposed,
    eRawHighlightsModeLCh,
    eRawHighlightsModeInpaint,
    eRawHighlightsModeSegments,
    eRawHighlightsModeLaplacian
};

/**
 * @brief The sensor a reconstruction mode needs, as darktable's highlights gui_changed() checks.
 * A mode added later declares its requirement in rawHighlightsModeRequirement() and the
 * sanitising follows.
 **/
enum RawHighlightsRequirementEnum {
    eRawHighlightsRequiresAny = 0,
    eRawHighlightsRequiresCfa,
    eRawHighlightsRequiresBayer
};

RawHighlightsRequirementEnum rawHighlightsModeRequirement(RawHighlightsModeEnum mode) WARN_UNUSED_RETURN;

bool rawHighlightsModeAvailable(RawHighlightsModeEnum mode,
                                const RawLayout& layout) WARN_UNUSED_RETURN;

/**
 * @brief mode when the layout can run it, Inpaint Opposed otherwise, which sets *fellBack.
 **/
RawHighlightsModeEnum sanitiseRawHighlightsMode(RawHighlightsModeEnum mode,
                                                const RawLayout& layout,
                                                bool* fellBack) WARN_UNUSED_RETURN;

/**
 * @brief Highlight reconstruction on prepared raw data, darktable's highlights module. Clip
 * clips every photosite to the clipping threshold; Unclipped leaves the data alone; Inpaint
 * Opposed rebuilds a clipped photosite from the opposed colours around it plus a chrominance
 * offset measured over the whole image next to the clipped areas; LCh rebuilds clipped Bayer and
 * X-Trans photosites keeping lightness and scaling chroma, and is Inpaint Opposed on data without
 * a CFA, as in darktable. Reconstruct Color rebuilds a clipped photosite from the next photosite of
 * another colour along rows and columns, by ratios learned from the unclipped photosites on the
 * way. Segmentation Based starts from Inpaint Opposed, segments the clipped areas of each colour
 * plane and rebuilds each segment from a candidate found next to it; Rebuild also lifts the areas
 * where every colour is clipped by a gradient grown inwards from their edges. Reconstruct Color and
 * Segmentation Based need a CFA and work over the whole frame, which they keep per source and
 * settings, one float per photosite. Guided Laplacians needs a Bayer mosaic: it diffuses the
 * clipped areas of a bilinear RGB image at a quarter of the size, which it keeps per source and
 * settings, and each render blends the upsampled result over its window by the clipping mask.
 *
 * The clipping threshold shows the smaller of 1 and raw/linear_response_limit until Override
 * Clipping Threshold is on.
 *
 * The threshold is the clipping threshold times the white level (1 after RawPrepare); Inpaint
 * Opposed scales it by the white balance already applied, read from raw/wb_applied. Inpaint
 * Opposed reads the whole source for its chrominance, which it computes once per source and
 * settings, so every tile of a render agrees.
 **/
class RawHighlights
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new RawHighlights(node);
    }

    explicit RawHighlights(NodePtr node);

    virtual ~RawHighlights();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief Drops the whole-frame results kept under RawNodeSupport::frameKey(), as Clear Caches
     * and memory pressure ask; the next render computes them again.
     **/
    virtual void purgeCaches() OVERRIDE FINAL;

private:
    struct Settings;

    std::optional<Settings> resolveSettings(double time, ViewIdx view) WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual void onInputChanged(int inputNo) OVERRIDE FINAL;

    void refreshOverride();

    void showSourceClip(double time);

    void refreshModeWarning(double time);

    void refreshModeKnobsVisibility();

    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief Clip Highlights, and Reconstruct in LCh on a Bayer or X-Trans mosaic. Inpaint Opposed
     * measures its chrominance over the whole source with double sums, which a region's strips
     * and the GPU's float arithmetic cannot reproduce, so it stays on the CPU, as does LCh without
     * a CFA, which is Inpaint Opposed. Reconstruct Color, Segmentation Based and Guided Laplacians
     * work over the whole frame and stay on the CPU.
     **/
    virtual bool canRenderOpOnGpu(const GpuQuery& query) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOpGpu(const GpuRenderArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool getOpGpuInputLayer(int inputNb,
                                    const ImageLayerDesc& plane,
                                    double time,
                                    ViewIdx view,
                                    ImageLayerDesc* layer) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual RectI getOpGpuInputRoI(int inputNb,
                                   double time,
                                   ViewIdx view,
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

    KnobChoiceWPtr _mode;
    KnobDoubleWPtr _clip;
    KnobBoolWPtr _clipOverride;
    KnobIntWPtr _combine;
    KnobDoubleWPtr _candidating;
    KnobChoiceWPtr _recovery;
    KnobDoubleWPtr _strength;
    KnobDoubleWPtr _noiseLevel;
    KnobIntWPtr _iterations;
    KnobChoiceWPtr _scales;
    KnobDoubleWPtr _solidColor;
    RawNodeSupport::OwnedMessage _message;

    // The chrominance of Inpaint Opposed, for the source and settings its key names.
    std::mutex _chromaMutex;
    bool _chromaValid;
    U64 _chromaKey;
    std::array<float, 3> _chroma;

    // The Reconstruct Color or Segmentation Based mosaic of the whole source, or the downsampled
    // Guided Laplacians reconstruction, for the source and settings its key names.
    std::mutex _frameMutex;
    bool _frameValid;
    U64 _frameKey;
    std::shared_ptr<const std::vector<float>> _frame;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Raw_RawHighlights_h
