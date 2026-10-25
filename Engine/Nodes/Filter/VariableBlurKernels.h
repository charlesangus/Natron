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

#ifndef Engine_Nodes_Filter_VariableBlurKernels_h
#define Engine_Nodes_Filter_VariableBlurKernels_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>

NATRON_NAMESPACE_ENTER

/**
 * @brief The variable-width box of the variable-size blurs, over a summed-area table.
 *
 * A box of radius r at pixel x covers [x - r, x + 1 + r) in pixel-edge coordinates, so r = 0 is
 * the pixel itself and a fractional r covers the end pixels in part: the image is read as
 * constant over each pixel, and the box is its mean over that interval, the end pixels weighed by
 * the part of them it covers. In two dimensions the box is the product of the two, which the
 * summed-area table gives in O(1) by bilinear lookups: the table of a piecewise-constant image is
 * its integral, bilinear between the pixel corners. Outside the table's image reads as zero, and
 * the box is always divided by its full area, so it darkens towards the edges as Blur does.
 *
 * A table holds (width + 1) x (height + 1) doubles, row 0 and column 0 zero, entry (i, j) the sum
 * of the pixels left of column i and below row j; double precision with a tile-local origin keeps
 * the box differences exact to well below a float's resolution at any tile size.
 **/
namespace VariableBlurKernels {
/**
 * @brief The radius whose box, applied three times, has the variance of a Gaussian of standard
 * deviation sigma: 3 v(r) = sigma^2, with v(r) the variance of the box with fractional ends
 * above. Exact, and 0 for a sigma of 0 or less.
 **/
double boxRadius(double sigma) WARN_UNUSED_RETURN;

/**
 * @brief The variance of the box of radius r, fractional ends included.
 **/
double boxVariance(double r) WARN_UNUSED_RETURN;

/**
 * @brief Row j + 1 of a table, satRow, from row j of the image, src: the running sums of src
 * along the row, satRow[0] being 0, a NaN or an infinity counting as 0. Only the row sums;
 * accumulateColumn() completes the table.
 **/
void prefixRow(const float* src,
               int width,
               double* satRow);

/**
 * @brief Turns column `column` of a table whose rows hold prefixRow()'s sums into the table's
 * column, by adding each entry the one below it. stride is the row length, width + 1.
 **/
void accumulateColumn(double* sat,
                      std::size_t stride,
                      int height,
                      int column);

/**
 * @brief Row y of one box pass: dst[x] is the box of radius rx[x] horizontally and ry[x]
 * vertically around pixel (x, y) of the table's image, and src[x] exactly where both are 0.
 * rx, ry, src and dst point at row y.
 **/
void boxRow(const double* sat,
            int width,
            int height,
            int y,
            const float* rx,
            const float* ry,
            const float* src,
            float* dst);

/**
 * @brief The whole variable blur of one width x height plane, single-threaded: `iterations`
 * box passes with the per-pixel radii rx and ry (width x height each), from src into dst. The
 * reference the node's threaded passes and the GPU kernels are tested against.
 **/
void iteratedBox(const float* src,
                 int width,
                 int height,
                 const float* rx,
                 const float* ry,
                 int iterations,
                 float* dst);
} // namespace VariableBlurKernels

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Filter_VariableBlurKernels_h
