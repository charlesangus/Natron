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

#ifndef Engine_Nodes_Filter_EdgeAware_PoissonHeal_h
#define Engine_Nodes_Filter_EdgeAware_PoissonHeal_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/Nodes/Filter/EdgeAware/EdgeAwareCommon.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief darktable's dt_heal() (src/common/heal.c, after GIMP's healing tool and Georgiev's
 * healing brush): seamless cloning by solving the Laplace equation.
 **/
namespace EdgeAware {
/// The default iteration cap of darktable's retouch module.
const int kPoissonHealDefaultIterations = 2000;

/**
 * @brief In place on dest (width x height x 4): inside the mask, dest - src is replaced by the
 * harmonic function matching it at the mask's edge, and src added back, so src's texture takes
 * dest's surrounding colour. A pixel is in the mask where mask (width x height, one float per
 * pixel) is non-zero and finite.
 *
 * Red/black Gauss-Seidel with darktable's over-relaxation factor for the mask's size, stopped at
 * maxIter iterations or once the update falls below (0.1 / 255)^2. Pixels on the image border
 * have fewer neighbours, as in darktable. Outside the mask the result is src + (dest - src). A
 * sample where src or dest is non-finite counts as 0 difference and keeps its dest value.
 **/
void poissonHeal(const float* src,
                 float* dest,
                 const float* mask,
                 int width,
                 int height,
                 int maxIter,
                 const ParallelFor& parallel);
} // namespace EdgeAware

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_EdgeAware_PoissonHeal_h
