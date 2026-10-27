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

#include <array>
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

#define PLUGINID_NATRON_NEURALDENOISE "NeuralDenoise"
#define PLUGIN_MAJOR_NATRON_NEURALDENOISE 1

#define kNeuralDenoiseParamStrength "strength"
#define kNeuralDenoiseParamPreserveWideGamut "preserveWideGamut"
#define kNeuralDenoiseParamShadowBoost "shadowBoost"
#define kNeuralDenoiseParamShadowBoostAuto "auto"
#define kNeuralDenoiseParamShadowBoostOn "on"
#define kNeuralDenoiseParamShadowBoostOff "off"
#define kNeuralDenoiseParamDetailRecovery "detailRecovery"
// Followed by the band, 1 (finest) to 5.
#define kNeuralDenoiseParamDetailBand "detailBand"
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
                   "Strength works as darktable's: for the sRGB models, lowering it brings luminance texture back "
                   "through a wavelet filter that keeps the noise out; for RawNIND linear it mixes the result with the "
                   "input. Preserve Wide Gamut and Shadow Boost are darktable's too. Each frame is denoised on its own, "
                   "so the result may flicker over time.\n"
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
    // Preserve Wide Gamut's neighbourhood: the 5x5 window around an out-of-gamut pixel.
    static constexpr int kGamutRadius = 2;
    // The wavelet bands of Strength's detail recovery, finest first, and their default thresholds
    // as multiples of the residual's standard deviation.
    static constexpr int kDetailBands = 5;
    static constexpr std::array<double, kDetailBands> kDetailBandDefaults = { 0.25, 0.15, 0.05, 0.02, 0.01 };

    enum ShadowBoostEnum {
        eShadowBoostAuto = 0,
        eShadowBoostOn,
        eShadowBoostOff,
    };

    /**
     * @brief Whether a frame has enough deep shadow for Auto Shadow Boost: count packed RGB
     * samples, of which at least 10% have a Rec.709 luminance under 0.005.
     **/
    static bool hasDeepShadows(const float* rgb,
                               std::size_t count);

    /**
     * @brief Strength's detail: the luminance of original minus denoised, both packed RGB of
     * width by height, filtered by darktable's wavelet denoise with each band's threshold
     * bandMultipliers times the residual's standard deviation. Non-finite residuals count as 0.
     **/
    static std::vector<float> filteredLuminanceResidual(const float* original,
                                                        const float* denoised,
                                                        int width,
                                                        int height,
                                                        const std::array<double, kDetailBands>& bandMultipliers);

    /**
     * @brief Preserve Wide Gamut: each pixel of denoised whose inGamut is 0 takes original's
     * colour, shifted by the mean luminance of the denoised in-gamut pixels in the 5x5 window
     * around it less its own. Packed RGB of width by height.
     **/
    static void passThroughOutOfGamut(const float* original,
                                      const std::vector<unsigned char>& inGamut,
                                      float* denoised,
                                      int width,
                                      int height);

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

    // Drops the whole frame kept for Strength's detail recovery, as Clear Caches does.
    virtual void purgeCaches() OVERRIDE FINAL;

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

    // An identity at a strength of 0 for a linear model, whose strength is a mix.
    virtual bool isOpIdentity(double time,
                              const RenderScale& scale,
                              const RectI& roi,
                              ViewIdx view) OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual StatusEnum renderOp(const RenderActionArgs& args,
                                const StagePlan& plan) OVERRIDE FINAL WARN_UNUSED_RETURN;

    // The task of the selected model, when the registry lists it.
    std::optional<AiModelTask> selectedModelTask() const;

    // The space the selected model was trained in, from its task.
    std::optional<AiColourSpace> selectedModelSpace() const;

    // Enables the knobs only the sRGB models use.
    void refreshKnobsForModel();

    // Whether a render at time needs the whole Source: for detail recovery, or to measure Auto
    // Shadow Boost.
    bool needsWholeSource(double time,
                          ViewIdx view);

    // Whether the model's config.json declares darktable's shadow_boost attribute.
    static bool declaresShadowBoost(const ModelConfig& config);

    struct WholeFrame {
        U64 key = 0;
        RectI frame;
        // Packed RGB over frame, rows from frame.y1, in the working space.
        std::vector<float> denoised;
        // One value per pixel, as denoised.
        std::vector<float> detail;
    };

    void postError(const std::string& message);
    void clearError();

    ModelDownloader* _downloader;
    std::unique_ptr<ModelKnob> _model;
    ModelColourspaceKnobs _colourspace;
    KnobDoubleWPtr _strength;
    KnobBoolWPtr _preserveWideGamut;
    KnobChoiceWPtr _shadowBoost;
    KnobGroupWPtr _detailRecovery;
    std::array<KnobDoubleWPtr, kDetailBands> _detailBands;
    KnobIntWPtr _tileSize;
    std::mutex _frameMutex;
    std::shared_ptr<const WholeFrame> _frame;
    // The error of the node's own it last posted, rather than one of the model knob's.
    std::mutex _errorMutex;
    std::string _postedError;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_NeuralDenoise_h
