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

#ifndef Engine_Nodes_Keyer_ChromaKeyer_h
#define Engine_Nodes_Keyer_ChromaKeyer_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"

#define PLUGINID_NATRON_CHROMAKEYER "net.sf.openfx.ChromaKeyerPlugin"
#define PLUGIN_MAJOR_NATRON_CHROMAKEYER 3

#define kChromaKeyerParamKeyColor "keyColor"
#define kChromaKeyerParamAcceptanceAngle "acceptanceAngle"
#define kChromaKeyerParamSuppressionAngle "suppressionAngle"
#define kChromaKeyerParamKeyLift "keyLift"
#define kChromaKeyerParamKeyGain "keyGain"
#define kChromaKeyerParamShow "show"
#define kChromaKeyerParamSourceAlpha "sourceAlphaHandling"

#define kChromaKeyerInputSource 0
#define kChromaKeyerInputBg 1
#define kChromaKeyerInputMask 2

NATRON_NAMESPACE_ENTER

struct ChromaKeyerSettings;

/**
 * @brief Chroma keyer after Keith Jack's "Video Demystified", in the scene-linear working space:
 * the key colour and the foreground are taken to Y'PbPr with the working space's luma weights,
 * the chrominance is rotated so the key colour lies on one axis, and the distance along that
 * axis inside the acceptance angle gives the background key. The key lift and gain, and the
 * foreground suppression (within the suppression angle) follow before the result is output as
 * the source, the premultiplied or unpremultiplied foreground, or the composite over Bg. The
 * output is always RGBA, with the foreground key as alpha. Nothing is clamped, so foreground
 * values above 1 or below 0 come through, and so can a key outside [0, 1].
 *
 * The host's mask and mix stage limits the result; with "Inside Mask" source alpha, the Source's
 * own alpha marks areas that are definitely foreground. An RGB source has no alpha: "Normal"
 * composites with an alpha of one there, and "Inside Mask" marks nothing.
 *
 * It is a Custom op: its CPU render runs through the stages with its own row, which also reads
 * Bg, and its GPU render is one pixelwrap entry reading Bg after its parameter block.
 **/
class ChromaKeyer
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new ChromaKeyer(node);
    }

    explicit ChromaKeyer(NodePtr node);

    virtual ~ChromaKeyer();

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual std::string getInputHint(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

private:
    // Never an identity: the output is always RGBA with the key as alpha. Mix 0 is the stages'.
    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // Accepts an RGBA plane in float whose stages the GPU runs, with or without Bg.
    virtual bool canRenderOpOnGpu(const GpuQuery& query) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool getOpGpuInputLayer(int inputNb,
                                    const ImageLayerDesc& plane,
                                    double time,
                                    ViewIdx view,
                                    ImageLayerDesc* layer) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOpGpu(const GpuRenderArgs& args) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual StatusEnum getPreferredMetadataAt(double time,
                                              ViewIdx view,
                                              NodeMetadata& metadata) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The knob values and working-space weights renderOp() and renderOpGpu() key with.
    void readSettings(double time, ViewIdx view, ChromaKeyerSettings* settings) const;

    // The layer renderOp() fetches the Source or Bg in, false when it fetches nothing.
    bool resolveColorInputLayer(int inputNb, double time, ViewIdx view, ImageLayerDesc* layer) const WARN_UNUSED_RETURN;

    // Whether the Source's layer has an alpha of its own.
    bool sourceHasAlpha(double time, ViewIdx view) const WARN_UNUSED_RETURN;

    KnobColorWPtr _keyColor;
    KnobDoubleWPtr _acceptanceAngle;
    KnobDoubleWPtr _suppressionAngle;
    KnobDoubleWPtr _keyLift;
    KnobDoubleWPtr _keyGain;
    KnobChoiceWPtr _show;
    KnobChoiceWPtr _sourceAlpha;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Keyer_ChromaKeyer_h
