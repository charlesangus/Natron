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

#ifndef Engine_Flow_TvL1Flow_h
#define Engine_Flow_TvL1Flow_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <vector>

#include "Engine/Flow/FlowField.h"
#include "Engine/Flow/FlowPyramid.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Coarse-to-fine TV-L1 by the duality scheme (Zach, Pock and Bischof 2007), with the median
 * filter between warps of Wedel et al. 2009. lambda, theta and tau are the values the literature
 * states for 8-bit code values, so image values of about 0..1 are scaled up to those. The flow is
 * computed down to finestLevel of the pyramid and upsampled from there.
 **/
struct TvL1Params {
    int finestLevel = 0;
    int warps = 5;
    int iterations = 30;
    float lambda = 0.15f;
    float theta = 0.3f;
    float tau = 0.25f;
};

TvL1Params tvL1ParamsForQuality(FlowQuality quality);

/**
 * @brief The flow from one frame's pyramid to another's, of the same size and depth.
 *
 * weights, when given, is a per-level pyramid of 0..1 data weights over the from frame, scaling
 * lambda per pixel: where it is 0 only the total variation term acts.
 *
 * Returns false if aborted, leaving flow unusable. The confidence is left at 0.
 **/
bool computeTvL1Flow(const FlowPyramid& from,
                     const FlowPyramid& to,
                     const TvL1Params& params,
                     const std::vector<FlowPlane>* weights,
                     const FlowContext& context,
                     FlowField& flow);

NATRON_NAMESPACE_EXIT

#endif // Engine_Flow_TvL1Flow_h
