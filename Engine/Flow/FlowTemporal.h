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

#ifndef Engine_Flow_FlowTemporal_h
#define Engine_Flow_FlowTemporal_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/Flow/FlowField.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief An adjacent frame's vectors of the same direction as the field being smoothed, and the flow
 * from the field's frame to that adjacent frame, which carries them over.
 **/
struct FlowTemporalNeighbour {
    const FlowField* field = nullptr;
    const FlowField* through = nullptr;
};

/**
 * @brief Blends a field with its neighbours carried to its frame, weighted per pixel by confidence:
 * a neighbour's sample counts by its own confidence, the confidence of the flow carrying it, and
 * strength, while the field's own sample counts by its confidence. At strength 1 three confident
 * fields count equally, and at strength 0 out is current. The confidence itself stays current's, as
 * the blend gives no new measurement of whether the frames agree. out must not be an input.
 **/
void flowSmoothTemporal(const FlowField& current,
                        const FlowTemporalNeighbour* neighbours,
                        int neighbourCount,
                        float strength,
                        const FlowContext& context,
                        FlowField& out);

NATRON_NAMESPACE_EXIT

#endif // Engine_Flow_FlowTemporal_h
