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

#ifndef Engine_Flow_FlowEstimate_h
#define Engine_Flow_FlowEstimate_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

#include "Engine/Ai/InferenceSession.h"
#include "Engine/Ai/OpticalFlow.h"
#include "Engine/Flow/FlowField.h"
#include "Engine/Flow/FlowPyramid.h"

NATRON_NAMESPACE_ENTER

enum FlowMethod {
    eFlowMethodDIS = 0,
    eFlowMethodTVL1,
    eFlowMethodNeural
};

/**
 * @brief The optical-flow model eFlowMethodNeural runs: an open session and how frames are fed to it.
 **/
struct FlowNeuralModel {
    std::shared_ptr<InferenceSession> session;
    OpticalFlowSpec spec;
};

/**
 * @brief One frame of a sequence. matte, when not null, is a 0..1 plane of the image's size, sample
 * (x, y) at matte[y * matteRowElements + x * matteStride]. Non-finite samples count as 0, and those
 * outside 0..1 are clamped.
 **/
struct FlowFrame {
    FlowImageView image;
    const float* matte = nullptr;
    int matteStride = 1;
    std::ptrdiff_t matteRowElements = 0;
};

struct FlowEstimateParams {
    FlowMethod method = eFlowMethodDIS;
    FlowQuality quality = eFlowQualityMedium;
    FlowChannels channels = eFlowChannelsLuminance;
    /// The round-trip error, in pixels, at which the confidence falls to 1/e.
    float confidenceSigma = 2.f;
    /// Required by eFlowMethodNeural. The frames' values are handed to it as they are: they must
    /// already be in the colour space the model wants. channels does not apply to it, and quality
    /// sets its iterations (neuralIterationsForQuality()).
    std::shared_ptr<const FlowNeuralModel> neural;
};

enum FlowEstimateStatus {
    eFlowEstimateOK = 0,
    eFlowEstimateAborted,
    /// A neighbour's size or matched-channel count differs from the current frame's: see
    /// FlowEstimateResult::mismatchOffset. The solvers can only compare frames that agree on both.
    eFlowEstimateFormatMismatch,
    /// The neural model failed: see FlowEstimateResult::error.
    eFlowEstimateModelError
};

/**
 * @brief The current frame's motion in both directions, each with its confidence. A direction with
 * no neighbour frame has zero vectors and confidence 0.
 **/
struct FlowEstimateResult {
    /// Current frame to the next.
    FlowField forward;
    /// Current frame to the previous.
    FlowField backward;
    /// The frame, relative to the current one, that caused eFlowEstimateFormatMismatch.
    int mismatchOffset = 0;
    /// Why eFlowEstimateModelError.
    std::string error;
};

/// The refinement iterations eFlowMethodNeural runs at each quality.
int neuralIterationsForQuality(FlowQuality quality);

/**
 * @brief Estimates the current frame's forward and backward flow, as the single entry point over the
 * methods. previous and next may be null where the sequence has no such frame.
 *
 * Each direction is solved both ways between its two frames, for the confidence's round trip, so a
 * call costs up to four solves. A matte on a frame doubles the solves of the pairs it is part of:
 * one pass with the data term weighted by the matte and one by its inverse, composited by the matte.
 * A matte that is 0 everywhere or 1 everywhere separates nothing and gives the result of no matte.
 **/
FlowEstimateStatus estimateFlow(const FlowFrame* previous,
                                const FlowFrame& current,
                                const FlowFrame* next,
                                const FlowEstimateParams& params,
                                const FlowContext& context,
                                FlowEstimateResult& result);

/**
 * @brief estimateFlow() with temporal smoothing. frames holds t-2, t-1, t, t+1 and t+2; only frames[2]
 * is required, and a null frame is one the sequence does not have. Each adjacent pair is solved both
 * ways, so a call costs up to eight solves. The current frame's vectors are then blended with those of
 * t-1 and t+1 of the same direction, by flowSmoothTemporal() with the given strength. A direction with
 * no neighbour frame stays at zero vectors and confidence 0.
 *
 * The pairs are solved in sequence order, keeping only the pyramids of the pair being solved and the
 * fields the two outputs read. releaseFrame, when set, is called on the calling thread with a frame's
 * index in frames as soon as its pixels and matte will not be read again, so the caller can free them;
 * the FlowFrame itself must stay valid for the whole call.
 **/
FlowEstimateStatus estimateFlowSmoothed(const FlowFrame* const frames[5],
                                        float smoothingStrength,
                                        const FlowEstimateParams& params,
                                        const FlowContext& context,
                                        FlowEstimateResult& result,
                                        const std::function<void(int)>& releaseFrame = std::function<void(int)>());

NATRON_NAMESPACE_EXIT

#endif // Engine_Flow_FlowEstimate_h
