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

#ifndef Engine_Nodes_Raw_RawSuperpixel_h
#define Engine_Nodes_Raw_RawSuperpixel_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>

#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Raw/RawKernels.h"
#include "Engine/Nodes/Raw/RawNodeSupport.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Raw Demosaic's render below full resolution with the Zoomed-Out Demosaic preference at
 * Fast: each output pixel at mipmap level N covers a 2^N x 2^N footprint of full-resolution
 * photosites, clipped to the source's region of definition. Over a Bayer mosaic a 2x2 footprint
 * is one superpixel (red, the mean of the two greens, blue), and a larger one the box filter of
 * its superpixels. demosaic_superpixel.slang computes the same values in the same order.
 **/
namespace RawSuperpixel {
enum ModeEnum : std::uint32_t {
    // Each colour the mean of its photosites in the footprint. A colour the footprint lacks, as a
    // 2x2 X-Trans footprint can, takes the mean over the footprint grown by the least number of
    // photosites, up to the CFA period, that finds it.
    eModeSuperpixel = 0,
    // The mean of the first channel in every output channel: photosite monochrome, or data
    // without a CFA of one channel.
    eModeBoxMono = 1,
    // The mean of each of the first three channels: data without a CFA of three channels or more.
    eModeBoxRgb = 2,
    // Each colour the sum of its photosites over the number of photosites in the footprint: the
    // box filter of photosite colour's output.
    eModePhotositeColor = 3
};

/**
 * @brief The full-resolution photosites a render of window at mipmapLevel reads: the footprint
 * of window grown by period, the farthest a missing colour is looked for, clipped to rod.
 **/
RectI sourceRegion(const RectI& window,
                   unsigned int mipmapLevel,
                   int period,
                   const RectI& rod);

/**
 * @brief Fills out, allocated over the window at mipmapLevel with 3 components, from source,
 * which holds sourceRegion() of that window: in eModeSuperpixel and eModePhotositeColor one
 * channel of photosites, in eModeBoxMono at least one channel and in eModeBoxRgb at least three.
 **/
void render(const RawNodeSupport::TopDownBuffer& source,
            const RawLayout& layout,
            const RectI& rod,
            unsigned int mipmapLevel,
            ModeEnum mode,
            RawNodeSupport::TopDownBuffer* out,
            const RawKernels::ParallelFor& parallel);
} // namespace RawSuperpixel

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Raw_RawSuperpixel_h
