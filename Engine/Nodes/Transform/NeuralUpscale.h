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

#ifndef Engine_Nodes_Transform_NeuralUpscale_h
#define Engine_Nodes_Transform_NeuralUpscale_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "Engine/Ai/ModelKnob.h"
#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/OCIO/ModelColourspace.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#define PLUGINID_NATRON_NEURALUPSCALE "NeuralUpscale"
#define PLUGIN_MAJOR_NATRON_NEURALUPSCALE 1

#define kNeuralUpscaleParamScale "scale"
#define kNeuralUpscaleParamScale2x "2x"
#define kNeuralUpscaleParamScale4x "4x"
#define kNeuralUpscaleParamAlphaFilter "alphaFilter"
#define kNeuralUpscaleParamAlphaFilterCubic "cubic"
#define kNeuralUpscaleParamAlphaFilterLanczos "lanczos"
#define kNeuralUpscaleParamTileSize "tileSize"

// The output metadata key holding the factor, 2 or 4.
#define kNeuralUpscaleMetadataFactor "upscale/factor"

#define kNeuralUpscaleInputSource 0

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kNeuralUpscaleInputs[] = {
    InputSpec { .label = "Source" },
};

inline constexpr NodeSpec kNeuralUpscaleNodeSpec = {
    .id = PLUGINID_NATRON_NEURALUPSCALE,
    .label = "NeuralUpscale",
    .description = "Enlarges the image 2 or 4 times with a neural network, darktable's neural restore upscale "
                   "models run through ONNX Runtime.\n"
                   "The output format and region of definition are the Source's times Scale, with the same pixel "
                   "aspect ratio. The RGB of the selected layer is converted from the working space into Model "
                   "Colourspace, the model runs over it in overlapping tiles, and its output is converted back. The "
                   "models were trained on sRGB, and nothing else is done to the values: they expect them roughly "
                   "within 0 to 1. Pick a log space to keep highlights above 1 within the model's range. Alpha, and "
                   "any channel left unchecked, is enlarged with Alpha Filter. Only the selected layer is output.\n"
                   "The node never concatenates with transforms. Each frame is upscaled on its own, so the result "
                   "may flicker over time.\n"
                   "The models are downloaded on demand; see Preferences > AI Models.",
    .grouping = PLUGIN_GROUP_TRANSFORM,
    .majorVersion = PLUGIN_MAJOR_NATRON_NEURALUPSCALE,
    .minorVersion = 0,
    .icon = NATRON_IMAGES_PATH "NativeNodes/NeuralUpscale.png",
    .inputs = kNeuralUpscaleInputs,
};

// PassThroughUnprocessed only tells the host the node writes every channel itself: renderOp()
// never runs the stages, as the output does not share the Source's pixel grid.
inline constexpr ImagePipelineSpec kNeuralUpscalePipeline = {
    .stages = { Stage::PassThroughUnprocessed },
    .layers = LayerPolicy::color(true, true, true, false),
    .op = OpKind::Custom,
};

/**
 * @brief Neural upscaling by 2 or 4. A CPU node, as every AI node is (Engine/Nodes/README.md,
 * "AI nodes").
 **/
class NeuralUpscale
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new NeuralUpscale(node);
    }

    explicit NeuralUpscale(NodePtr node);

    virtual ~NeuralUpscale();

    // The downloader the next node created uses; null for ModelDownloader::process(). For tests.
    static ModelDownloader*& downloaderForTesting();

    // darktable's tile overlap for upscaling, in Source pixels, which the Source is also grown by.
    static constexpr int kOverlap = 16;
    // The tile edge when Tile Size is 0 and the model takes any size.
    static constexpr int kDefaultTile = 512;
    // darktable's "large output" threshold, in megapixels.
    static constexpr double kLargeOutputMegapixels = 60.;

    enum AlphaFilterEnum {
        eAlphaFilterCubic = 0,
        eAlphaFilterLanczos,
    };

    /**
     * @brief The warning for an output of outputPixels with nComps float channels: at least
     * kLargeOutputMegapixels, or more bytes than cacheBytes when that is not 0. Empty when neither.
     **/
    static std::string largeOutputWarning(const RectI& outputPixels,
                                          int nComps,
                                          U64 cacheBytes);

    /**
     * @brief The Lanczos-3 kernel, sinc(t) sinc(t / 3) within 3 pixels and 0 beyond.
     **/
    static double lanczos3(double t);

    virtual const NodeSpec* nodeSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual const ImagePipelineSpec& imagePipelineSpec() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual std::string getInputHint(int inputNb) const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual void addAcceptedComponents(int inputNb, std::list<ImageLayerDesc>* comps) OVERRIDE FINAL;

    ModelKnob* modelKnob() const
    {
        return _model.get();
    }

    const ModelColourspaceKnobs& colourspaceKnobs() const
    {
        return _colourspace;
    }

    // 2 or 4, from Scale.
    int factor() const;

private:
    virtual void initializeKnobs() OVERRIDE FINAL;

    virtual void onKnobsLoaded() OVERRIDE FINAL;

    virtual void onEffectCreated(bool mayCreateFileDialog,
                                 const CreateNodeArgs& args) OVERRIDE FINAL;

    virtual bool knobChanged(KnobI* k,
                             ValueChangedReasonEnum reason,
                             ViewSpec view,
                             double time,
                             bool originatedFromMainThread) OVERRIDE FINAL;

    // The Source's format times the factor; the pixel aspect ratio is the Source's.
    virtual StatusEnum getPreferredMetadataAt(double time,
                                              ViewIdx view,
                                              NodeMetadata& metadata) OVERRIDE FINAL;

    // The factor under kNeuralUpscaleMetadataFactor.
    virtual ImageMetadata deriveOutputMetadata(double time,
                                               ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The Source's region of definition scaled about the origin.
    virtual StatusEnum getRegionOfDefinition(U64 hash,
                                             double time,
                                             const RenderScale& scale,
                                             ViewIdx view,
                                             RectD* rod) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The window divided by the factor, grown by the tile overlap.
    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    // The Source's other layers are not output: they would keep the Source's size.
    virtual void filterPassThroughLayers(double time,
                                         ViewIdx view,
                                         std::list<ImageLayerDesc>* layers) OVERRIDE FINAL;

    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The registry entry of the selected model, when it lists it.
    const AiModelEntry* selectedModel() const;

    // The space the selected model was trained in, from its task.
    std::optional<AiColourSpace> selectedModelSpace() const;

    // Selects, for a model of the other factor, its sibling in the same package, else the
    // default model for Scale.
    void syncModelToScale();

    // Sets Scale to the selected model's factor.
    void syncScaleToModel();

    void warnLargeOutput(const RectI& outputPixels,
                         int nComps);

    void postError(const std::string& message);
    void clearError();

    ModelDownloader* _downloader;
    std::unique_ptr<ModelKnob> _model;
    ModelColourspaceKnobs _colourspace;
    KnobChoiceWPtr _scale;
    KnobChoiceWPtr _alphaFilter;
    KnobIntWPtr _tileSize;
    // The error of the node's own it last posted, rather than one of the model knob's.
    std::mutex _errorMutex;
    std::string _postedError;
    // The output size last warned about, so each size is warned about once.
    std::mutex _warnMutex;
    RectI _warnedOutput;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_NeuralUpscale_h
