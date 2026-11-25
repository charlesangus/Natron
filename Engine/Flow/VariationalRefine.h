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

#ifndef Engine_Flow_VariationalRefine_h
#define Engine_Flow_VariationalRefine_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/Flow/FlowField.h"
#include "Engine/Flow/FlowPyramid.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The variational refinement of Kroeger et al. 2016: a brightness-constancy term, a
 * gradient-constancy term and a smoothness term, all under a robust penaliser, each data term
 * normalised by its gradient (Zimmer et al. 2011). The weights are the paper's, which it states for
 * 8-bit code values, so image values of about 0..1 are scaled up to those.
 **/
struct RefineParams {
    int fixedPointIterations = 5;
    int sorIterations = 5;
    float alpha = 20.f;
    float gamma = 10.f;
    float delta = 5.f;
    float omega = 1.6f;
};

/**
 * @brief Improves flow in place at one pyramid level, solving by fixed-point iterations around the
 * warp of the incoming flow, each with red-black SOR, so the result does not depend on threading.
 *
 * weight, when given, scales both data terms per pixel: where it is 0 only the smoothness term acts.
 * Pixels warping outside the frame are treated the same way.
 *
 * Returns false if aborted, leaving flow as it came in.
 **/
bool refineFlow(const FlowPyramidLevel& from,
                const FlowPyramidLevel& to,
                const FlowPlane* weight,
                const RefineParams& params,
                const FlowContext& context,
                FlowField& flow);

NATRON_NAMESPACE_EXIT

#endif // Engine_Flow_VariationalRefine_h
