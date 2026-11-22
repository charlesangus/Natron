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

#include "ImageLayerDesc.h"

#include <algorithm>
#include <cassert>

NATRON_NAMESPACE_ENTER

static const char* rgbaComps[4] = { "R", "G", "B", "A" };
static const char* rgbComps[3] = { "R", "G", "B" };
static const char* alphaComps[1] = { "A" };
static const char* motionComps[2] = { "u", "v" };
static const char* disparityComps[2] = { "x", "y" };
static const char* xyComps[2] = { "x", "y" };

ImageLayerDesc::ImageLayerDesc()
    : _layerID("none")
    , _channels()
    , _colorStorage(false)
{
}

ImageLayerDesc::ImageLayerDesc(const std::string& layerID,
                               const std::vector<std::string>& channels)
    : _layerID(layerID)
    , _channels(channels)
    , _colorStorage(layerID == kNatronColorLayerID)
{
}

ImageLayerDesc::ImageLayerDesc(const std::string& layerID,
                               const char** channels,
                               int count)
    : _layerID(layerID)
    , _channels(channels, channels + count)
    , _colorStorage(layerID == kNatronColorLayerID)
{
}

ImageLayerDesc::ImageLayerDesc(ColorViewTag,
                               const std::string& viewID,
                               const std::vector<std::string>& channels)
    : _layerID(viewID)
    , _channels(channels)
    , _colorStorage(false)
{
}

ImageLayerDesc::ImageLayerDesc(const ImageLayerDesc& other)
{
    *this = other;
}

ImageLayerDesc&
ImageLayerDesc::operator=(const ImageLayerDesc& other)
{
    _layerID = other._layerID;
    _channels = other._channels;
    _colorStorage = other._colorStorage;
    return *this;
}

ImageLayerDesc::~ImageLayerDesc()
{
}

bool
ImageLayerDesc::isColorLayer(const std::string& layerID)
{
    return ImageLayerDesc::isColorViewID(layerID);
}

bool
ImageLayerDesc::isColorLayer() const
{
    return _colorStorage || ImageLayerDesc::isColorViewID(_layerID);
}

bool
ImageLayerDesc::isColorStorage() const
{
    return _colorStorage;
}

bool
ImageLayerDesc::operator==(const ImageLayerDesc& other) const
{
    if (_channels.size() != other._channels.size()) {
        return false;
    }
    return (_layerID == other._layerID) && (_colorStorage == other._colorStorage);
}

bool
ImageLayerDesc::operator<(const ImageLayerDesc& other) const
{
    if (_layerID != other._layerID) {
        return _layerID < other._layerID;
    }
    return _colorStorage < other._colorStorage;
}

int
ImageLayerDesc::getNumComponents() const
{
    return (int)_channels.size();
}

const std::string&
ImageLayerDesc::getLayerID() const
{
    return _layerID;
}

std::string
ImageLayerDesc::getUserFacingLabel() const
{
    if (!_colorStorage) {
        return _layerID;
    }
    const ImageLayerDesc& view = ImageLayerDesc::colorViewForNComps(getNumComponents());
    if (view) {
        return view.getLayerID();
    }

    return kNatronColorViewRGBA;
}

const std::vector<std::string>&
ImageLayerDesc::getChannels() const
{
    return _channels;
}

const ImageLayerDesc&
ImageLayerDesc::getNoneComponents()
{
    static const ImageLayerDesc comp;
    return comp;
}

const ImageLayerDesc&
ImageLayerDesc::getRGBAComponents()
{
    static const ImageLayerDesc comp(kNatronColorLayerID, rgbaComps, 4);

    return comp;
}

const ImageLayerDesc&
ImageLayerDesc::getRGBComponents()
{
    static const ImageLayerDesc comp(kNatronColorLayerID, rgbComps, 3);

    return comp;
}

const ImageLayerDesc&
ImageLayerDesc::getXYComponents()
{
    static const ImageLayerDesc comp(kNatronColorLayerID, xyComps, 2);

    return comp;
}

const ImageLayerDesc&
ImageLayerDesc::getAlphaComponents()
{
    static const ImageLayerDesc comp(kNatronColorLayerID, alphaComps, 1);

    return comp;
}

const ImageLayerDesc&
ImageLayerDesc::getBackwardMotionComponents()
{
    static const ImageLayerDesc comp(kNatronBackwardMotionVectorsLayerID, motionComps, 2);

    return comp;
}

const ImageLayerDesc&
ImageLayerDesc::getForwardMotionComponents()
{
    static const ImageLayerDesc comp(kNatronForwardMotionVectorsLayerID, motionComps, 2);

    return comp;
}

const ImageLayerDesc&
ImageLayerDesc::getDisparityLeftComponents()
{
    static const ImageLayerDesc comp(kNatronDisparityLeftLayerID, disparityComps, 2);

    return comp;
}

const ImageLayerDesc&
ImageLayerDesc::getDisparityRightComponents()
{
    static const ImageLayerDesc comp(kNatronDisparityRightLayerID, disparityComps, 2);

    return comp;
}

bool
ImageLayerDesc::isColorViewID(const std::string& layerID)
{
    return layerID == kNatronColorViewRGBA || layerID == kNatronColorViewRGB || layerID == kNatronColorViewAlpha || layerID == kNatronColorViewXY;
}

const ImageLayerDesc&
ImageLayerDesc::getColorView(const std::string& viewID)
{
    if (viewID == kNatronColorViewRGBA) {
        static const ImageLayerDesc view(ColorViewTag(), kNatronColorViewRGBA, ImageLayerDesc::getRGBAComponents().getChannels());
        return view;
    } else if (viewID == kNatronColorViewRGB) {
        static const ImageLayerDesc view(ColorViewTag(), kNatronColorViewRGB, ImageLayerDesc::getRGBComponents().getChannels());
        return view;
    } else if (viewID == kNatronColorViewAlpha) {
        static const ImageLayerDesc view(ColorViewTag(), kNatronColorViewAlpha, ImageLayerDesc::getAlphaComponents().getChannels());
        return view;
    } else if (viewID == kNatronColorViewXY) {
        static const ImageLayerDesc view(ColorViewTag(), kNatronColorViewXY, ImageLayerDesc::getXYComponents().getChannels());
        return view;
    }

    return ImageLayerDesc::getNoneComponents();
}

/**
 * @brief The bit desc's channel channelIndex occupies (mirrors ResolvedLayer::channelBit in
 * Engine/KnobChannelSet.h, duplicated here rather than shared because KnobChannelSet.h already
 * includes this header): a single-channel plane is an alpha plane, so its one channel is bit 3.
 **/
static int
colorChannelBit(const ImageLayerDesc& desc,
                int channelIndex)
{
    return desc.getNumComponents() == 1 ? 3 : channelIndex;
}

static std::bitset<4>
allColorChannelBits(const ImageLayerDesc& desc)
{
    std::bitset<4> bits;
    const int count = std::min(desc.getNumComponents(), 4);

    for (int c = 0; c < count; ++c) {
        bits.set(colorChannelBit(desc, c));
    }

    return bits;
}

static std::bitset<4>
namedColorChannelBits(const ImageLayerDesc& desc,
                      const std::vector<std::string>& names)
{
    std::bitset<4> bits;
    const std::vector<std::string>& channels = desc.getChannels();
    const int count = std::min((int)channels.size(), 4);

    for (int c = 0; c < count; ++c) {
        if (std::find(names.begin(), names.end(), channels[c]) != names.end()) {
            bits.set(colorChannelBit(desc, c));
        }
    }

    return bits;
}

std::bitset<4>
ImageLayerDesc::colorStorageBits(const ImageLayerDesc& storage)
{
    return allColorChannelBits(storage);
}

ImageLayerDesc
ImageLayerDesc::narrowestColorStorageCovering(const std::bitset<4>& bits)
{
    if (bits.none()) {
        return ImageLayerDesc::getNoneComponents();
    }
    for (int nComps = 1; nComps <= 4; ++nComps) {
        const ImageLayerDesc& storage = ImageLayerDesc::mapNCompsToColorLayer(nComps);
        if ((bits & ~colorStorageBits(storage)).none()) {
            return storage;
        }
    }

    return ImageLayerDesc::getNoneComponents();
}

std::bitset<4>
ImageLayerDesc::colorViewMask(const std::string& viewID)
{
    return allColorChannelBits(ImageLayerDesc::getColorView(viewID));
}

int
ImageLayerDesc::colorViewChannelBit(const std::string& viewID,
                                    int index)
{
    if (index < 0) {
        return -1;
    }

    const std::bitset<4> mask = ImageLayerDesc::colorViewMask(viewID);
    int seen = -1;

    for (int b = 0; b < 4; ++b) {
        if (mask.test(b)) {
            ++seen;
            if (seen == index) {
                return b;
            }
        }
    }

    return -1;
}

int
ImageLayerDesc::colorViewChannelIndex(const std::string& viewID,
                                      int bit)
{
    if (bit < 0 || bit > 3) {
        return -1;
    }

    const std::bitset<4> mask = ImageLayerDesc::colorViewMask(viewID);
    if (!mask.test(bit)) {
        return -1;
    }

    int index = 0;
    for (int b = 0; b < bit; ++b) {
        if (mask.test(b)) {
            ++index;
        }
    }

    return index;
}

void
ImageLayerDesc::presentColorViews(const ImageLayerDesc& storage,
                                  std::vector<std::string>* views)
{
    views->clear();
    if (!storage.isColorLayer()) {
        return;
    }

    views->push_back(kNatronColorViewRGBA);
    views->push_back(kNatronColorViewRGB);
    views->push_back(kNatronColorViewAlpha);
    if (storage.getNumComponents() == 2) {
        views->push_back(kNatronColorViewXY);
    }
}

void
ImageLayerDesc::resolveColorView(const std::string& viewID,
                                 const ImageLayerDesc& storage,
                                 const std::vector<std::string>& rowChannels,
                                 std::bitset<4>* bits,
                                 std::bitset<4>* zeroBits)
{
    const ImageLayerDesc& view = ImageLayerDesc::getColorView(viewID);
    const std::bitset<4> mask = allColorChannelBits(view);
    const std::bitset<4> rowBits = rowChannels.empty() ? mask : namedColorChannelBits(view, rowChannels);
    const std::bitset<4> selected = mask & rowBits;

    if (bits) {
        *bits = selected;
    }
    if (zeroBits) {
        *zeroBits = selected & ~ImageLayerDesc::colorStorageBits(storage);
    }
}

const ImageLayerDesc&
ImageLayerDesc::colorViewForNComps(int nComps)
{
    switch (nComps) {
    case 1:
        return ImageLayerDesc::getColorView(kNatronColorViewAlpha);
    case 2:
        return ImageLayerDesc::getColorView(kNatronColorViewXY);
    case 3:
        return ImageLayerDesc::getColorView(kNatronColorViewRGB);
    case 4:
        return ImageLayerDesc::getColorView(kNatronColorViewRGBA);
    default:
        return ImageLayerDesc::getNoneComponents();
    }
}

void
ImageLayerDesc::expandColorViews(std::list<ImageLayerDesc>* storageList)
{
    if (!storageList) {
        return;
    }

    std::list<ImageLayerDesc>::iterator it = storageList->begin();
    while (it != storageList->end()) {
        if (!it->isColorStorage()) {
            ++it;
            continue;
        }

        std::vector<std::string> views;
        ImageLayerDesc::presentColorViews(*it, &views);

        std::list<ImageLayerDesc>::iterator insertPos = storageList->erase(it);
        for (std::vector<std::string>::const_iterator v = views.begin(); v != views.end(); ++v) {
            insertPos = storageList->insert(insertPos, ImageLayerDesc::getColorView(*v));
            ++insertPos;
        }
        it = insertPos;
    }
}

void
ImageLayerDesc::collapseColorToLayoutView(std::list<ImageLayerDesc>* storageList)
{
    if (!storageList) {
        return;
    }

    for (std::list<ImageLayerDesc>::iterator it = storageList->begin(); it != storageList->end(); ++it) {
        if (!it->isColorStorage()) {
            continue;
        }
        const ImageLayerDesc& view = ImageLayerDesc::colorViewForNComps(it->getNumComponents());
        *it = view ? view : ImageLayerDesc::getColorView(kNatronColorViewRGBA);
    }
}

ChoiceOption
ImageLayerDesc::getChannelOption(int channelIndex) const
{
    if (channelIndex < 0 || channelIndex >= (int)_channels.size()) {
        assert(false);
        return ChoiceOption("", "", "");
    }
    const std::string optionID = _layerID + "." + _channels[channelIndex];

    return ChoiceOption(optionID, optionID, "");
}

ChoiceOption
ImageLayerDesc::getLayerOption() const
{
    return ChoiceOption(_layerID, _layerID, "");
}

const ImageLayerDesc&
ImageLayerDesc::mapNCompsToColorLayer(int nComps)
{
    switch (nComps) {
    case 1:
        return ImageLayerDesc::getAlphaComponents();
    case 2:
        return ImageLayerDesc::getXYComponents();
    case 3:
        return ImageLayerDesc::getRGBComponents();
    case 4:
        return ImageLayerDesc::getRGBAComponents();
    default:
        return ImageLayerDesc::getNoneComponents();
    }
}

NATRON_NAMESPACE_EXIT
