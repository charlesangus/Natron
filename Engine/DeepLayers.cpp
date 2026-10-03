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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "DeepLayers.h"

#include <cstddef>
#include <map>

#include "Engine/DeepImage.h"
#include "Engine/LayerRegistry.h"

NATRON_NAMESPACE_ENTER

namespace {
bool
isDepthChannel(const std::string& name)
{
    return name == "Z" || name == "ZBack" || name == "Zback";
}

int
bareColorBit(const std::string& name)
{
    if (name == "R") {
        return 0;
    }
    if (name == "G") {
        return 1;
    }
    if (name == "B") {
        return 2;
    }
    if (name == "A") {
        return 3;
    }

    return -1;
}

// Marks a bare I/Y so LayerRegistry::groupChannelNames, which folds them into colour, makes each
// its own layer instead.
const char kBareMarker = '\x01';
} // namespace

std::bitset<4>
DeepLayers::colorBits(const std::vector<std::string>& names)
{
    std::bitset<4> bits;

    bits.set(3);
    for (std::size_t i = 0; i < names.size(); ++i) {
        const int bit = bareColorBit(names[i]);
        if (bit >= 0) {
            bits.set(bit);
        }
    }

    return bits;
}

std::bitset<4>
DeepLayers::colorBits(const DeepImage& image)
{
    std::vector<std::string> names;
    const std::map<std::string, DeepChannelBuffer>& channels = image.getChannels();

    for (std::map<std::string, DeepChannelBuffer>::const_iterator it = channels.begin(); it != channels.end(); ++it) {
        names.push_back(it->first);
    }

    return colorBits(names);
}

void
DeepLayers::groupDeepChannels(const std::vector<std::string>& names,
                              std::list<ImageLayerDesc>* layers)
{
    if (!layers) {
        return;
    }
    layers->clear();
    layers->push_back(ImageLayerDesc::narrowestColorStorageCovering(colorBits(names)));

    std::vector<std::string> rest;
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::string& name = names[i];
        if (isDepthChannel(name) || bareColorBit(name) >= 0) {
            continue;
        }
        if (name == "I" || name == "Y") {
            rest.push_back(std::string(1, kBareMarker) + name + "." + name);
        } else {
            rest.push_back(name);
        }
    }

    std::vector<ImageLayerDesc> grouped;
    LayerRegistry::groupChannelNames(rest, &grouped);
    for (std::size_t i = 0; i < grouped.size(); ++i) {
        const ImageLayerDesc& desc = grouped[i];
        const std::string& id = desc.getLayerID();
        if (!id.empty() && id[0] == kBareMarker) {
            const std::string bare = id.substr(1);
            layers->push_back(ImageLayerDesc(bare, bare, "", std::vector<std::string>(1, bare)));
        } else {
            layers->push_back(desc);
        }
    }
}

std::string
DeepLayers::channelName(const ImageLayerDesc& layer,
                        int index)
{
    if (index < 0 || index >= layer.getNumComponents()) {
        return std::string();
    }

    const std::vector<std::string>& channels = layer.getChannels();
    if (layer.getLayerID() == kNatronColorLayerID) {
        static const char* const kBitNames[4] = { "R", "G", "B", "A" };
        const int bit = layer.getNumComponents() == 1 ? 3 : index;

        return bit < 4 ? std::string(kBitNames[bit]) : channels[index];
    }
    if (layer.getNumComponents() == 1 && channels[0] == layer.getLayerID()) {
        return channels[0];
    }

    return layer.getLayerID() + "." + channels[index];
}

bool
DeepLayers::findChannel(const std::string& name,
                        const std::list<ImageLayerDesc>& layers,
                        ImageLayerDesc* layer,
                        int* index)
{
    const bool bareColor = bareColorBit(name) >= 0;

    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        if ((it->getLayerID() == kNatronColorLayerID) != bareColor) {
            continue;
        }
        for (int c = 0; c < it->getNumComponents(); ++c) {
            if (channelName(*it, c) == name) {
                if (layer) {
                    *layer = *it;
                }
                if (index) {
                    *index = c;
                }

                return true;
            }
        }
    }

    return false;
}

NATRON_NAMESPACE_EXIT
