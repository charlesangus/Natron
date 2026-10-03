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

#ifndef Engine_DeepLayers_h
#define Engine_DeepLayers_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <bitset>
#include <list>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Maps a deep stream's flat channel names to storage-level layers and back, with no
 * EffectInstance dependency. Z, ZBack and Zback are each sample's depths, never layers.
 **/
namespace DeepLayers {
/**
 * @brief Colour bits R=0, G=1, B=2, A=3 present in names, with bit 3 always set: deep alpha is
 * structural, so a stream is never presented without it.
 **/
std::bitset<4> colorBits(const std::vector<std::string>& names);
std::bitset<4> colorBits(const DeepImage& image);

/**
 * @brief Groups names into layers. The colour storage comes first and is always present, the
 * narrowest of Alpha or RGBA covering the R/G/B bits found plus A. Every other name joins the
 * layer before its last dot, or is a one-channel layer named after itself, in input order. A
 * layer's channels are canonical: R, G, B, A when present, then the rest sorted, so a node that
 * re-derives its input's layers reports them identically.
 **/
void groupDeepChannels(const std::vector<std::string>& names, std::list<ImageLayerDesc>* layers);

/**
 * @brief The flat channel name of layer's channel at index: bare R/G/B/A by bit for the colour
 * storage, the channel itself for a bare one-channel layer, "layerID.channel" otherwise. Empty
 * when index is out of range.
 **/
std::string channelName(const ImageLayerDesc& layer, int index);

/**
 * @brief The inverse of channelName() over layers. Returns false, leaving the outputs untouched,
 * when no layer maps to name.
 **/
bool findChannel(const std::string& name, const std::list<ImageLayerDesc>& layers, ImageLayerDesc* layer, int* index);
} // namespace DeepLayers

NATRON_NAMESPACE_EXIT

#endif // Engine_DeepLayers_h
