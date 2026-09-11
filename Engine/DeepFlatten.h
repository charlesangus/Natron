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

#ifndef Engine_DeepFlatten_h
#define Engine_DeepFlatten_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

#include "Global/Enums.h"
#include "Global/GlobalDefines.h"

#include "Engine/DeepPixelOps.h"
#include "Engine/EngineFwd.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Turning deep samples into flat values: the two operations the Viewer's implicit deep
 * flatten and the explicit DeepToImage node both need, with no cache, no thread-local storage and
 * no EffectInstance dependency, so either caller can reach them.
 **/
namespace DeepFlatten {
/**
 * @brief Composites every sample of every pixel of roi in src front-to-back into dst, which must
 * be a float Image whose component count equals channelOrder's size; channelOrder[c] names the
 * DeepImage channel that lands in dst's c-th component, and alphaChannelIndex says which of them
 * is alpha. Pixels of roi that src does not cover, or that hold no samples, are written as zero.
 *
 * src's samples are tidied first unless it already declares itself tidy, because
 * DeepPixelOps::flattenFrontToBack() silently gives a wrong answer on overlapping samples.
 * scratch and work are the caller-owned reusable storage that tidying needs (see DeepPixelOps.h);
 * nothing is allocated per pixel.
 *
 * Returns eStatusFailed if dst's depth or component count disagrees with channelOrder, or if src
 * lacks one of the named channels. A src with no "Z" channel at all holds no samples anywhere, so
 * that is not an error: roi is zeroed and eStatusOK returned.
 **/
StatusEnum flattenToImage(const DeepImage& src,
                          const RectI& roi,
                          const std::vector<std::string>& channelOrder,
                          int alphaChannelIndex,
                          DeepPixelScratch* scratch,
                          DeepTidyWorkspace* work,
                          const ImagePtr& dst) WARN_UNUSED_RETURN;

/**
 * @brief Copies out the samples src holds at pixel (x, y), in the order they are stored, together
 * with the names of the channels each sample's values correspond to -- a DeepSample's channels are
 * unnamed, and a probe of a file carrying AOVs is useless without them. "Z" and "ZBack" are not
 * among them: they are the sample's own depths.
 *
 * The samples are returned exactly as src stores them, deliberately *not* tidied: tidying splits
 * and merges samples, so a probe that tidied would show the user values, and a sample count, that
 * the source does not contain.
 *
 * Returns false if (x, y) is outside src's bounds or src carries no depths at all.
 **/
bool getSamplesAtPixel(const DeepImage& src,
                       int x,
                       int y,
                       std::vector<std::string>* channelNames,
                       std::vector<DeepSample>* samples) WARN_UNUSED_RETURN;
} // namespace DeepFlatten

NATRON_NAMESPACE_EXIT

#endif // Engine_DeepFlatten_h
