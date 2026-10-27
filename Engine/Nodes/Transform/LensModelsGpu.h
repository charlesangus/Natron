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

#ifndef Engine_Nodes_Transform_LensModelsGpu_h
#define Engine_Nodes_Transform_LensModelsGpu_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cmath>
#include <cstdint>

#include "Engine/Nodes/KnobReader.h"
#include "Engine/Nodes/Transform/LensModels.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A LensModels::Model as Engine/Gpu/Kernels/lensmodels.slang reads it, word for word. A
 * double is carried as a float pair, hi and lo, whose sum the kernel computes with. The affines
 * are m00 m01 m10 m11 tx ty, in the order toCore, fromCore, toCoreInverse, fromCoreInverse.
 **/
struct LensModelGpu {
    uint32_t valid = 0;
    uint32_t type = 0;
    uint32_t core = 0;
    uint32_t reserved = 0;
    float affines[4][6][2] = {};
    float c[LensModels::kMaxCoeffs][2] = {};
};

static_assert(sizeof(LensModelGpu) == 100 * 4, "lensmodels.slang reads 100 words");

inline void
setLensModelGpuDouble(double v,
                      float out[2])
{
    out[0] = knobValueAsFloat(v);
    // Past the float range the high word is infinite and there is no remainder; subtracting an
    // infinite high word would raise the invalid exception for an infinite v.
    out[1] = std::isinf(out[0]) ? 0.f : (float)(v - (double)out[0]);
}

inline LensModelGpu
packLensModelGpu(const LensModels::Model& model)
{
    LensModelGpu packed;

    packed.valid = model.valid ? 1u : 0u;
    packed.type = (uint32_t)model.type;
    packed.core = (uint32_t)model.core;
    const LensModels::Affine* const affines[4] = { &model.toCore, &model.fromCore, &model.toCoreInverse, &model.fromCoreInverse };
    for (int i = 0; i < 4; ++i) {
        const LensModels::Affine& a = *affines[i];
        const double words[6] = { a.m00, a.m01, a.m10, a.m11, a.tx, a.ty };
        for (int k = 0; k < 6; ++k) {
            setLensModelGpuDouble(words[k], packed.affines[i][k]);
        }
    }
    for (int i = 0; i < LensModels::kMaxCoeffs; ++i) {
        setLensModelGpuDouble(model.c[i], packed.c[i]);
    }

    return packed;
}

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Transform_LensModelsGpu_h
