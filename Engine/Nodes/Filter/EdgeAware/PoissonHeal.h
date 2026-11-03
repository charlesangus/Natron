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

#include <cstdint>
#include <vector>

#include "Engine/Nodes/Filter/EdgeAware/EdgeAwareCommon.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief darktable's dt_heal() (src/common/heal.c, after GIMP's healing tool and Georgiev's
 * healing brush): seamless cloning by solving the Laplace equation. The solution and its
 * convergence depend on the whole buffer, so pass the whole frame (EdgeAwareCommon.h).
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

/**
 * @brief poissonHeal()'s equation solved to convergence: conjugate gradients preconditioned by a
 * multigrid V-cycle (2x2 aggregation with the Galerkin operator, red/black Gauss-Seidel
 * smoothing), stopped once the residual has fallen by kMultigridTolerance against the boundary
 * terms or after maxCycles cycles. Unlike darktable's capped SOR, large masks converge.
 *
 * guess, when not null (width x height x 4), is a starting dest - src inside the mask, such as a
 * previous solve's, which a capped draft solve refines. The work is split by masked pixel count,
 * and the result does not depend on how parallel orders it. Returns the cycles run.
 **/
int poissonHealMultigrid(const float* src,
                         float* dest,
                         const float* mask,
                         int width,
                         int height,
                         int maxCycles,
                         const ParallelFor& parallel,
                         const float* guess = nullptr);

/// The residual reduction, against the boundary terms, at which poissonHealMultigrid() stops.
const double kPoissonHealMultigridTolerance = 1e-5;

/**
 * @brief One grid of a HealGrid: cells of the level are cellOffset + (y + 1) * stride + x + 1, a
 * one-cell zero border around nx x ny, and its active cells are listed at three ranges of
 * HealGrid::cells: all of them, then the two checkerboard colours, (x + y) even being colour 0.
 **/
struct HealGridLevel {
    int nx = 0;
    int ny = 0;
    std::uint32_t cellOffset = 0;
    std::uint32_t stride = 0;
    std::uint32_t listOffset[3] = { 0, 0, 0 };
    std::uint32_t listCount[3] = { 0, 0, 0 };
};

/**
 * @brief The multigrid hierarchy poissonHealMultigrid() solves with for a mask, flattened for a
 * device: the levels finest first, four floats a cell (the diagonal, its inverse, the coupling
 * to the east cell and to the south cell) and the lists of each level's active cells by their
 * index within the level.
 **/
struct HealGrid {
    std::vector<HealGridLevel> levels;
    std::size_t totalCells = 0;
    std::vector<float> coefficients;
    std::vector<std::uint32_t> cells;
};

void buildHealGrid(const float* mask,
                   int width,
                   int height,
                   HealGrid* grid);

/**
 * @brief The L2 norm over the masked pixels and the four channels of the Laplacian of dest - src,
 * with the neighbours poissonHeal() uses: 0 for an exact heal.
 **/
double poissonHealResidual(const float* src,
                           const float* dest,
                           const float* mask,
                           int width,
                           int height);
} // namespace EdgeAware

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_EdgeAware_PoissonHeal_h
