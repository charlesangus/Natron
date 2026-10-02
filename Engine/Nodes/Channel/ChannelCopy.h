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

#ifndef Engine_Nodes_Channel_ChannelCopy_h
#define Engine_Nodes_Channel_ChannelCopy_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <bitset>
#include <list>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Colour-plane lookup and channel copying shared by the Channel nodes that rebuild the
 * colour storage plane from their input's (RemoveLayers, AddLayers).
 **/
namespace ChannelCopy {

/**
 * @brief The colour storage plane of `layers`, or NULL when they carry none.
 **/
const ImageLayerDesc* findColorStorage(const std::list<ImageLayerDesc>& layers);

/**
 * @brief The colour storage layout the metadata of input `inputNb` declare, or the None layout
 * when that input is not connected. Metadata do not vary with time, so neither does this,
 * whereas the input's present layers may.
 **/
ImageLayerDesc metadataColorStorage(const EffectInstance& effect, int inputNb);

/**
 * @brief For each channel of the colour plane `plane`, the index of the channel of `source`
 * on the same colour bit when `copiedBits` holds that bit, otherwise -1.
 **/
std::vector<int> mapColorChannels(const ImageLayerDesc& plane, const Image& source, const std::bitset<4>& copiedBits);

/**
 * @brief Writes each channel c of `dst` over `roi` from channel srcChannels[c] of `src`, and 0
 * where that index is -1 or missing or `src` is NULL. `src` may have another depth than `dst`,
 * since the Channel nodes fetch their input without mapping it to their clip preferences.
 * Returns eStatusFailed for an unsupported depth, and stops early when `effect` is aborted.
 **/
StatusEnum copyChannels(const Image::ReadAccess* src,
                        ImageBitDepthEnum srcDepth,
                        Image* dst,
                        const RectI& roi,
                        const std::vector<int>& srcChannels,
                        const EffectInstance& effect);

} // namespace ChannelCopy

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Channel_ChannelCopy_h
