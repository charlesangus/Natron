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
 * 20891f6c6fa7), src/iop/highlights.c with src/iop/hlreconstruct/opposed.c, lch.c and the
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
#include <mutex>
#include <optional>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Raw/RawNodeSupport.h"

#define PLUGINID_NATRON_RAWHIGHLIGHTS "fr.natron.RawHighlights"
#define PLUGIN_MAJOR_NATRON_RAWHIGHLIGHTS 1

#define kRawHighlightsParamMode "mode"
#define kRawHighlightsParamClip "clip"
#define kRawHighlightsParamClipOverride "clipOverride"

NATRON_NAMESPACE_ENTER

enum RawHighlightsModeEnum {
    eRawHighlightsModeClip = 0,
    eRawHighlightsModeUnclipped,
    eRawHighlightsModeOpposed,
    eRawHighlightsModeLCh
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
 * a CFA, as in darktable.
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
     * a CFA, which is Inpaint Opposed.
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

    virtual ImageMetadata deriveOutputMetadata(double time,
                                               ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    KnobChoiceWPtr _mode;
    KnobDoubleWPtr _clip;
    KnobBoolWPtr _clipOverride;
    RawNodeSupport::OwnedMessage _message;

    // The chrominance of Inpaint Opposed, for the source and settings its key names.
    std::mutex _chromaMutex;
    bool _chromaValid;
    U64 _chromaKey;
    std::array<float, 3> _chroma;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Raw_RawHighlights_h
