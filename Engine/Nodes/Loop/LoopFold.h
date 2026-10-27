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

#ifndef Engine_Nodes_Loop_LoopFold_h
#define Engine_Nodes_Loop_LoopFold_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>

NATRON_NAMESPACE_ENTER

/**
 * @brief The per-pixel arithmetic of LoopEnd's Accumulate. The accumulator is float, the depth
 * LoopEnd stores between fold steps, and every path folds through these functions, so a fold
 * chain split into tasks and a fold run in one render give the same bits.
 **/
namespace LoopFold {

enum OperationEnum {
    eOperationMean = 0,
    eOperationSum,
    eOperationMin,
    eOperationMax,
    eOperationOver,
    eOperationUnder
};

/**
 * @brief The accumulator before the first iteration is folded in.
 **/
float initialValue(OperationEnum operation) WARN_UNUSED_RETURN;

/**
 * @brief Folds pixels x1..x2-1 of one row of an iteration into acc, which holds nComps channels
 * per pixel starting at x1. src holds srcComps channels per pixel starting at srcX1 and covers
 * srcX1..srcX2-1; it may be null. Pixels and channels the iteration lacks read 0. alphaIndex is
 * the alpha channel in both layouts, or -1 when there is none, which Over and Under then read as 0.
 **/
void foldRow(OperationEnum operation,
             const float* src,
             int srcComps,
             int srcX1,
             int srcX2,
             float* acc,
             int nComps,
             int alphaIndex,
             int x1,
             int x2);

/**
 * @brief Turns n accumulated values into the output once all count iterations are in.
 **/
void finishRow(OperationEnum operation,
               int count,
               float* acc,
               std::size_t n);

} // namespace LoopFold

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Loop_LoopFold_h
