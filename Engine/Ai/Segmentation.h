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

#ifndef Engine_Ai_Segmentation_h
#define Engine_Ai_Segmentation_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Engine/Ai/InferenceSession.h"
#include "Engine/Ai/ModelPackage.h"
#include "Engine/Ai/Tensor.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief One prompt of an interactive segmentation, in pixels of the encoded image: x to the right,
 * y down, pixel j covering [j - 0.5, j + 0.5).
 **/
struct SegmentationPoint {
    enum Label {
        eLabelNegative = 0,
        eLabelPositive = 1,
        // A box is sent as its two corners, which only SAM models understand.
        eLabelBoxTopLeft = 2,
        eLabelBoxBottomRight = 3,
    };

    float x = 0.f;
    float y = 0.f;
    int label = eLabelPositive;
};

// A box prompt, in the coordinates of SegmentationPoint.
struct SegmentationBox {
    float x1 = 0.f;
    float y1 = 0.f;
    float x2 = 0.f;
    float y2 = 0.f;
};

/**
 * @brief What an encoding belongs to: the node and state of the image, its frame and the model.
 **/
struct SegmentationKey {
    uint64_t imageHash = 0;
    double frame = 0.;
    std::string modelId;

    bool operator==(const SegmentationKey& other) const
    {
        return (imageHash == other.imageHash) && (frame == other.frame) && (modelId == other.modelId);
    }
};

/**
 * @brief Click-to-segment with a two-graph model of darktable-ai's "mask" task (SegNext or SAM 2.1):
 * the heavy encoder runs once per image and its outputs are kept, then the light decoder turns each
 * set of prompts into a mask. Ported from darktable's src/common/ai/segmentation.c.
 *
 * The encoder runs on the device Preferences choose, the decoder always on the CPU with graph
 * optimisation disabled, as darktable does. Every method may be called from any thread; calls are
 * serialised.
 **/
class Segmenter {
public:
    enum class Arch {
        // Multi-mask with predicted IoU, logit output, low-resolution mask feedback, box prompts.
        eSam,
        // One probability mask, full-resolution mask feedback, points only.
        eSegNext,
    };

    // How many encodings stay cached; past it the least recently used is dropped.
    static constexpr std::size_t kCacheCapacity = 4;

    /**
     * @brief Opens both graphs, the encoder with encoderOptions (usually
     * AiSessionOptions::fromPreferences()), the decoder on the CPU. config gives the architecture
     * ("sam2", "sam" or "segnext") and the input_size and prev_mask_size attributes. A null loader
     * means OrtLoader::process(). Throws AiError when a graph cannot be opened, or does not have the
     * inputs and outputs of a segmentation decoder, or the architecture is not a segmentation one.
     **/
    Segmenter(const std::string& encoderPath,
              const std::string& decoderPath,
              const ModelConfig& config,
              const AiSessionOptions& encoderOptions,
              const std::shared_ptr<const OrtLoader>& loader = std::shared_ptr<const OrtLoader>());

    ~Segmenter();

    Segmenter(const Segmenter&) = delete;
    Segmenter& operator=(const Segmenter&) = delete;

    // The architecture config.json names, or throws AiError.
    static Arch archFromConfig(const ModelConfig& config);

    Arch arch() const
    {
        return _arch;
    }

    bool supportsBoxes() const
    {
        return _arch == Arch::eSam;
    }

    // The longest side the encoder sees.
    int inputSize() const
    {
        return _inputSize;
    }

    /**
     * @brief Makes key's encoding the current one, running the encoder on rgb8 (packed RGB, width x
     * height, top row first, display-referred sRGB) only when the cache does not hold it. Returns
     * whether the cache held it. Throws what InferenceSession::run() throws.
     **/
    bool encode(const uint8_t* rgb8,
                int width,
                int height,
                const SegmentationKey& key,
                const AiAbortPredicate& abort = AiAbortPredicate());

    // Makes key's encoding current when cached, counting a hit. Returns whether it was.
    bool select(const SegmentationKey& key);

    bool isCached(const SegmentationKey& key) const;

    // Whether an encoding is current, and its key and size.
    bool hasEncoding() const;
    SegmentationKey currentKey() const;
    int encodedWidth() const;
    int encodedHeight() const;

    /**
     * @brief One decoder pass over the current encoding. Returns a probability mask, encodedWidth()
     * x encodedHeight(), top row first. The decoder is given the previous pass's mask, so successive
     * calls refine; resetRefinement() starts afresh. Boxes on a model without box support, or no
     * current encoding, throw AiError.
     **/
    std::vector<float> decode(const std::vector<SegmentationPoint>& points,
                              const std::vector<SegmentationBox>& boxes = std::vector<SegmentationBox>());

    /**
     * @brief The object mask the prompts select, as darktable's object tool computes it: up to
     * passes decoder passes, each after the first adding an interior point of the previous mask
     * (and, for SAM, its bounding box), stopping once the mask settles. Areas no positive prompt
     * falls in are cleared, unless none does, when the largest is kept. Starts from a fresh
     * refinement state.
     **/
    std::vector<float> segment(const std::vector<SegmentationPoint>& points,
                               const std::vector<SegmentationBox>& boxes,
                               float threshold,
                               int passes = 2);

    void resetRefinement();

    // Runs one throwaway decode so that ONNX Runtime's first-run costs land on the calling thread.
    void warmUpDecoder();

    // Counters for tests and diagnostics.
    std::size_t encoderRuns() const;
    std::size_t cacheHits() const;

    /**
     * @brief Keeps the areas of mask above threshold that a positive point falls in, each area
     * with its soft edge down to a floor; the largest area when no positive point is in one.
     **/
    static void keepSeedAreas(std::vector<float>& mask,
                              int width,
                              int height,
                              float threshold,
                              const std::vector<SegmentationPoint>& points);

private:
    struct Encoding {
        SegmentationKey key;
        // Encoder outputs by name.
        std::map<std::string, Tensor> outputs;
        int width = 0;
        int height = 0;
        // input size / longest side.
        float scale = 1.f;
        // The encoded image, the guide of the joint-bilateral mask upsampling.
        std::vector<uint8_t> rgb;
    };

    Tensor preprocess(const uint8_t* rgb8, int width, int height, float* scale) const;
    void makeCurrent(std::list<std::shared_ptr<Encoding>>::iterator it);
    std::vector<float> decodeLocked(const std::vector<SegmentationPoint>& points);

    const Arch _arch;
    std::shared_ptr<InferenceSession> _encoder;
    std::shared_ptr<InferenceSession> _decoder;
    int _inputSize = 1024;
    // Decoder input name fed by each encoder output name.
    std::vector<std::pair<std::string, std::string>> _routing;
    // Decoder prompt inputs, in darktable's positional order.
    std::string _coordsInput, _labelsInput, _maskInput, _hasMaskInput;
    int _prevMaskDim = 256;

    mutable std::mutex _mutex;
    // Most recently used first.
    std::list<std::shared_ptr<Encoding>> _cache;
    std::shared_ptr<Encoding> _current;
    std::vector<float> _prevMask;
    bool _hasPrevMask = false;
    std::size_t _encoderRuns = 0;
    std::size_t _cacheHits = 0;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Ai_Segmentation_h
