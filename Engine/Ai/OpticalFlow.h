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

#ifndef Engine_Ai_OpticalFlow_h
#define Engine_Ai_OpticalFlow_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>

#include "Engine/Ai/InferenceSession.h"
#include "Engine/Flow/FlowField.h"

NATRON_NAMESPACE_ENTER

struct OpticalFlowSpec {
    // The longest edge, in pixels, a frame is run at. Larger frames are downscaled to it and the
    // vectors scaled back up. 0 never downscales.
    int maxInferenceSize = 1024;
    // The model's stride: the run size is rounded up to a multiple of it.
    int multipleOf = 8;
    // The smallest edge the model accepts. Smaller frames are padded up to it.
    int minSize = 128;
    // What the wrapped pixel value 1 becomes at the model's input. RAFT-class models take 0..255.
    float inputScale = 255.f;
    // The refinement iterations fed to a model that has an `iterations` input; a model without one
    // ignores it.
    int iterations = 4;
};

// The name of a flow model's optional third input: a float32 [1] tensor holding the iteration count.
#define kOpticalFlowIterationsInput "iterations"

/**
 * @brief A frame of three colour planes of equal size, already in the colour space the model wants.
 * Values outside 0 to 1, negatives and highlights, are allowed.
 **/
struct OpticalFlowFrame {
    FlowPlane channel[3];

    int width() const { return channel[0].width; }
    int height() const { return channel[0].height; }
};

/**
 * @brief The range wrapper applied to pixels on their way to the model: the identity from -1 to 1,
 * and a logarithmic tail beyond, so highlights keep their ordering instead of clipping. Odd and
 * strictly increasing, so opticalFlowUnwrapRange() inverts it.
 **/
float opticalFlowWrapRange(float value);
float opticalFlowUnwrapRange(float wrapped);

/**
 * @brief The size a frame of width x height is run at: the frame itself, or downscaled to the
 * spec's maxInferenceSize on the long edge, then padded up to the spec's multiple and minimum.
 **/
struct OpticalFlowGeometry {
    int inferenceWidth = 0;
    int inferenceHeight = 0;
    int paddedWidth = 0;
    int paddedHeight = 0;
};
OpticalFlowGeometry opticalFlowGeometry(int width,
                                        int height,
                                        const OpticalFlowSpec& spec);

/**
 * @brief The motion from `from` to `to`, at full size and in full-size pixels: a vector at (x, y)
 * points to where the pixel's content lies in `to`. The result's u and v are set, its confidence
 * is not. The session's model takes two NCHW float inputs, the first frame then the second, and
 * optionally kOpticalFlowIterationsInput, which gets spec.iterations. It returns the flow,
 * [1, 2, H, W] with the horizontal component first, as its first output. The frames must have the
 * same size. Throws AiAborted when abort fires and AiError for any other
 * failure, including an input or output not shaped as above.
 **/
FlowField estimateOpticalFlow(const std::shared_ptr<InferenceSession>& session,
                              const OpticalFlowFrame& from,
                              const OpticalFlowFrame& to,
                              const OpticalFlowSpec& spec,
                              const AiAbortPredicate& abort = AiAbortPredicate());

/**
 * @brief Both directions with the frames padded and resized once: forward is the motion from a to
 * b, and backward from b to a. Two runs.
 **/
struct OpticalFlowPair {
    FlowField forward;
    FlowField backward;
};
OpticalFlowPair estimateOpticalFlowPair(const std::shared_ptr<InferenceSession>& session,
                                        const OpticalFlowFrame& a,
                                        const OpticalFlowFrame& b,
                                        const OpticalFlowSpec& spec,
                                        const AiAbortPredicate& abort = AiAbortPredicate());

NATRON_NAMESPACE_EXIT

#endif // Engine_Ai_OpticalFlow_h
