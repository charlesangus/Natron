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

#ifndef Engine_Nodes_Filter_NeuralDenoise_h
#define Engine_Nodes_Filter_NeuralDenoise_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "Engine/Ai/ModelKnob.h"
#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/OCIO/ModelColourspace.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

#define PLUGINID_NATRON_NEURALDENOISE "NeuralDenoise"
#define PLUGIN_MAJOR_NATRON_NEURALDENOISE 1

#define kNeuralDenoiseParamStrength "strength"
#define kNeuralDenoiseParamTileSize "tileSize"

#define kNeuralDenoiseInputSource 0
#define kNeuralDenoiseInputMask 1

NATRON_NAMESPACE_ENTER

inline constexpr InputSpec kNeuralDenoiseInputs[] = {
    InputSpec { .label = "Source" },
    InputSpec { .label = "Mask", .role = InputRole::Mask, .optional = true },
};

inline constexpr NodeSpec kNeuralDenoiseNodeSpec = {
    .id = PLUGINID_NATRON_NEURALDENOISE,
    .label = "NeuralDenoise",
    .description = "Removes noise with a neural network, darktable's neural restore denoise models run through ONNX "
                   "Runtime.\n"
                   "The image is converted from the working space into Model Colourspace, the model runs over it in "
                   "overlapping tiles, and its output is converted back. Pick the space the model was trained in: "
                   "sRGB Encoded Rec.709 for the sRGB models (NIND UNet, NAFNet-SIDD), Linear Rec.2020 for RawNIND "
                   "linear. Nothing else is done to the values; the sRGB models expect them roughly within 0 to 1.\n"
                   "Strength mixes the result with the input. Each frame is denoised on its own, so the result may "
                   "flicker over time.\n"
                   "The models are downloaded on demand; see Preferences > AI Models.",
    .grouping = PLUGIN_GROUP_FILTER,
    .majorVersion = PLUGIN_MAJOR_NATRON_NEURALDENOISE,
    .minorVersion = 0,
    .icon = NATRON_IMAGES_PATH "NativeNodes/NeuralDenoise.png",
    .inputs = kNeuralDenoiseInputs,
};

inline constexpr ImagePipelineSpec kNeuralDenoisePipeline = {
    .stages = { Stage::MaskMix, Stage::PassThroughUnprocessed },
    .layers = LayerPolicy::color(true, true, true, false),
    .op = OpKind::Custom,
};

/**
 * @brief Neural denoising. A CPU node, as every AI node is (Engine/Nodes/README.md, "AI nodes").
 **/
class NeuralDenoise
    : public StagedImageEffect {
public:
    static EffectInstance* BuildEffect(NodePtr node)
    {
        return new NeuralDenoise(node);
    }

    explicit NeuralDenoise(NodePtr node);

    virtual ~NeuralDenoise();

    // The downloader the next node created uses; null for ModelDownloader::process(). For tests.
    static ModelDownloader*& downloaderForTesting();

    // The pixels a tile shares with its neighbours, which the Source is also grown by.
    static constexpr int kOverlap = 64;
    // The tile edge when Tile Size is 0 and the model takes any size.
    static constexpr int kDefaultTile = 512;

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

    // The Source over the window grown by the tile overlap.
    virtual void getRegionsOfInterest(double time,
                                      const RenderScale& scale,
                                      const RectD& outputRoD,
                                      const RectD& renderWindow,
                                      ViewIdx view,
                                      RoIMap* ret) OVERRIDE FINAL;

    // An identity at a strength of 0.
    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The space the selected model was trained in, from its task.
    std::optional<AiColourSpace> selectedModelSpace() const;

    void postError(const std::string& message);
    void clearError();

    ModelDownloader* _downloader;
    std::unique_ptr<ModelKnob> _model;
    ModelColourspaceKnobs _colourspace;
    KnobDoubleWPtr _strength;
    KnobIntWPtr _tileSize;
    // The error of the node's own it last posted, rather than one of the model knob's.
    std::mutex _errorMutex;
    std::string _postedError;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_NeuralDenoise_h
