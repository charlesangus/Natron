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

#include <ofxNatron.h>

#include <cassert>
#include <cstring>
#include <sstream>
#include <stdexcept>

NATRON_NAMESPACE_ENTER

static const char* rgbaComps[4] = { "R", "G", "B", "A" };
static const char* rgbComps[3] = { "R", "G", "B" };
static const char* alphaComps[1] = { "A" };
static const char* motionComps[2] = { "U", "V" };
static const char* disparityComps[2] = { "X", "Y" };
static const char* xyComps[2] = { "X", "Y" };

ImageLayerDesc::ImageLayerDesc()
    : _layerID("none")

    , _layerLabel("none")
    , _channels()
    , _channelsLabel("none")
{
}

ImageLayerDesc::ImageLayerDesc(const std::string& layerID,
                               const std::string& layerLabel,
                               const std::string& channelsLabel,
                               const std::vector<std::string>& channels)
    : _layerID(layerID)
    , _layerLabel(layerLabel)
    , _channels(channels)
    , _channelsLabel(channelsLabel)
{
    if (layerLabel.empty()) {
        // Layer label is the ID if empty
        _layerLabel = _layerID;
    }
    if (channelsLabel.empty()) {
        // Channels label is the concatenation of all channels
        for (std::size_t i = 0; i < channels.size(); ++i) {
            _channelsLabel.append(channels[i]);
        }
    }
}

ImageLayerDesc::ImageLayerDesc(const std::string& layerName,
                               const std::string& layerLabel,
                               const std::string& channelsLabel,
                               const char** channels,
                               int count)
    : _layerID(layerName)
    , _layerLabel(layerLabel)
    , _channels()
    , _channelsLabel(channelsLabel)
{
    _channels.resize(count);
    for (int i = 0; i < count; ++i) {
        _channels[i] = channels[i];
    }

    if (layerLabel.empty()) {
        // Layer label is the ID if empty
        _layerLabel = _layerID;
    }
    if (channelsLabel.empty()) {
        // Channels label is the concatenation of all channels
        for (std::size_t i = 0; i < _channels.size(); ++i) {
            _channelsLabel.append(channels[i]);
        }
    }
}

ImageLayerDesc::ImageLayerDesc(const ImageLayerDesc& other)
{
    *this = other;
}

ImageLayerDesc&
ImageLayerDesc::operator=(const ImageLayerDesc& other)
{
    _layerID = other._layerID;
    _layerLabel = other._layerLabel;
    _channels = other._channels;
    _channelsLabel = other._channelsLabel;
    return *this;
}

ImageLayerDesc::~ImageLayerDesc()
{
}

bool
ImageLayerDesc::isColorLayer(const std::string& layerID)
{
    return layerID == kNatronColorLayerID;
}

bool
ImageLayerDesc::isColorLayer() const
{
    return ImageLayerDesc::isColorLayer(_layerID);
}

bool
ImageLayerDesc::operator==(const ImageLayerDesc& other) const
{
    if (_channels.size() != other._channels.size()) {
        return false;
    }
    return _layerID == other._layerID;
}

bool
ImageLayerDesc::operator<(const ImageLayerDesc& other) const
{
    return _layerID < other._layerID;
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

const std::string&
ImageLayerDesc::getLayerLabel() const
{
    return _layerLabel;
}

const std::string&
ImageLayerDesc::getChannelsLabel() const
{
    return _channelsLabel;
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
    static const ImageLayerDesc comp(kNatronColorLayerID, kNatronColorLayerLabel, "", rgbaComps, 4);

    return comp;
}

const ImageLayerDesc&
ImageLayerDesc::getRGBComponents()
{
    static const ImageLayerDesc comp(kNatronColorLayerID, kNatronColorLayerLabel, "", rgbComps, 3);

    return comp;
}

const ImageLayerDesc&
ImageLayerDesc::getXYComponents()
{
    static const ImageLayerDesc comp(kNatronColorLayerID, kNatronColorLayerLabel, "XY", xyComps, 2);

    return comp;
}

const ImageLayerDesc&
ImageLayerDesc::getAlphaComponents()
{
    static const ImageLayerDesc comp(kNatronColorLayerID, kNatronColorLayerLabel, "Alpha", alphaComps, 1);

    return comp;
}

const ImageLayerDesc&
ImageLayerDesc::getBackwardMotionComponents()
{
    static const ImageLayerDesc comp(kNatronBackwardMotionVectorsLayerID, kNatronBackwardMotionVectorsLayerLabel, kNatronMotionComponentsLabel, motionComps, 2);

    return comp;
}

const ImageLayerDesc&
ImageLayerDesc::getForwardMotionComponents()
{
    static const ImageLayerDesc comp(kNatronForwardMotionVectorsLayerID, kNatronForwardMotionVectorsLayerLabel, kNatronMotionComponentsLabel, motionComps, 2);

    return comp;
}

const ImageLayerDesc&
ImageLayerDesc::getDisparityLeftComponents()
{
    static const ImageLayerDesc comp(kNatronDisparityLeftLayerID, kNatronDisparityLeftLayerLabel, kNatronDisparityComponentsLabel, disparityComps, 2);

    return comp;
}

const ImageLayerDesc&
ImageLayerDesc::getDisparityRightComponents()
{
    static const ImageLayerDesc comp(kNatronDisparityRightLayerID, kNatronDisparityRightLayerLabel, kNatronDisparityComponentsLabel, disparityComps, 2);

    return comp;
}

ChoiceOption
ImageLayerDesc::getChannelOption(int channelIndex) const
{
    if (channelIndex < 0 || channelIndex >= (int)_channels.size()) {
        assert(false);
        return ChoiceOption("", "", "");
    }
    std::string optionID, optionLabel;
    optionLabel += _layerLabel;
    optionID += _layerID;
    if (!optionLabel.empty()) {
        optionLabel += '.';
    }
    if (!optionID.empty()) {
        optionID += '.';
    }

    // For the option label, append the name of the channel
    optionLabel += _channels[channelIndex];
    optionID += _channels[channelIndex];

    return ChoiceOption(optionID, optionLabel, "");
}

ChoiceOption
ImageLayerDesc::getLayerOption() const
{
    std::string optionLabel = _layerLabel + "." + _channelsLabel;

    // The option ID is always the name of the layer, this ensures for the Color layer that even if the components type changes, the choice stays
    // the same in the parameter.
    return ChoiceOption(_layerID, optionLabel, "");
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

static bool
extractOFXEncodedCustomLayer(const std::string& comp, std::string* layerName, std::string* layerLabel, std::string* channelsLabel, std::vector<std::string>* channels)
{

    // Find the layer unique identifier
    const std::size_t foundLayerLen = std::strlen(kNatronOfxImageComponentsPlaneName);
    std::size_t foundLayer = comp.find(kNatronOfxImageComponentsPlaneName);
    if (foundLayer == std::string::npos) {
        return false;
    }

    const std::size_t layerNameStartIdx = foundLayer + foundLayerLen;

    // Find the optional layer label
    // If layerLabelStartIdx = 0, there is no layer label.
    std::size_t layerLabelStartIdx = 0;

    const std::size_t foundLayerLabelLen = std::strlen(kNatronOfxImageComponentsPlaneLabel);
    std::size_t foundLayerLabel = comp.find(kNatronOfxImageComponentsPlaneLabel, layerNameStartIdx);
    if (foundLayerLabel != std::string::npos) {
        layerLabelStartIdx = foundLayerLabel + foundLayerLabelLen;
    }

    // Find the optional channels label
    // If channelsLabelStartIdx = 0, there's no channels label.
    std::size_t channelsLabelStartIdx = 0;

    const std::size_t foundChannelsLabelLen = std::strlen(kNatronOfxImageComponentsPlaneChannelsLabel);

    // If there was a layer label before, pick from there otherwise pick from the name
    std::size_t findChannelsLabelStart = layerLabelStartIdx > 0 ? layerLabelStartIdx : layerNameStartIdx;
    std::size_t foundChannelsLabel = comp.find(kNatronOfxImageComponentsPlaneChannelsLabel, findChannelsLabelStart);
    if (foundChannelsLabel != std::string::npos) {
        channelsLabelStartIdx = foundChannelsLabel + foundChannelsLabelLen;
    }

    // Find the first channel
    // If there was a channels label before, find from there, otherwise if there was a layer label before
    // find from there, otherwise find from the name.
    std::size_t findChannelStart = 0;
    if (channelsLabelStartIdx > 0) {
        findChannelStart = channelsLabelStartIdx;
    } else if (layerLabelStartIdx > 0) {
        findChannelStart = layerLabelStartIdx;
    } else {
        findChannelStart = layerNameStartIdx;
    }

    const std::size_t foundChannelLen = std::strlen(kNatronOfxImageComponentsPlaneChannel);
    std::size_t foundChannel = comp.find(kNatronOfxImageComponentsPlaneChannel, findChannelStart);
    if (foundChannel == std::string::npos) {
        // There needs to be at least one channel.
        return false;
    }

    // Extract channels label
    if (channelsLabelStartIdx > 0) {
        *channelsLabel = comp.substr(channelsLabelStartIdx, foundChannel - channelsLabelStartIdx);
    }

    // Extract layer label
    if (layerLabelStartIdx > 0) {
        std::size_t endIndex = (foundChannelsLabel != std::string::npos) ? foundChannelsLabel : foundChannel;
        *layerLabel = comp.substr(layerLabelStartIdx, endIndex - layerLabelStartIdx);
    }

    // Extract layer name
    {
        std::size_t endIndex;
        if (foundLayerLabel != std::string::npos) {
            // There is a layer label
            endIndex = foundLayerLabel;
        } else if (foundChannelsLabel != std::string::npos) {
            // There is no layer label but a channels label
            endIndex = foundChannelsLabel;
        } else {
            // No layer label and no channels label
            endIndex = foundChannel;
        }
        *layerName = comp.substr(layerNameStartIdx, endIndex - layerNameStartIdx);
    }

    while (foundChannel != std::string::npos) {
        if (channels->size() >= 4) {
            // A layer must have between 1 and 4 channels.
            return false;
        }
        findChannelStart = foundChannel + foundChannelLen;
        std::size_t nextChannel = comp.find(kNatronOfxImageComponentsPlaneChannel, findChannelStart);
        std::string chan = comp.substr(findChannelStart, nextChannel - findChannelStart);
        channels->push_back(chan);
        foundChannel = nextChannel;
    }

    return true;
} // extractOFXEncodedCustomLayer

static ImageLayerDesc
ofxCustomCompToNatronComp(const std::string& comp)
{
    std::string layerID, layerLabel, channelsLabel;
    std::vector<std::string> channels;
    if (!extractOFXEncodedCustomLayer(comp, &layerID, &layerLabel, &channelsLabel, &channels)) {
        return ImageLayerDesc::getNoneComponents();
    }

    return ImageLayerDesc(layerID, layerLabel, channelsLabel, channels);
}

ImageLayerDesc
ImageLayerDesc::mapOFXPlaneStringToLayer(const std::string& ofxPlane)
{
    assert(ofxPlane != kFnOfxImagePlaneColour);
    if (ofxPlane == kFnOfxImagePlaneBackwardMotionVector) {
        return ImageLayerDesc::getBackwardMotionComponents();
    } else if (ofxPlane == kFnOfxImagePlaneForwardMotionVector) {
        return ImageLayerDesc::getForwardMotionComponents();
    } else if (ofxPlane == kFnOfxImagePlaneStereoDisparityLeft) {
        return ImageLayerDesc::getDisparityLeftComponents();
    } else if (ofxPlane == kFnOfxImagePlaneStereoDisparityRight) {
        return ImageLayerDesc::getDisparityRightComponents();
    } else {
        return ofxCustomCompToNatronComp(ofxPlane);
    }
}

void
ImageLayerDesc::mapOFXComponentsTypeStringToLayers(const std::string& ofxComponents, ImageLayerDesc* layer, ImageLayerDesc* pairedLayer)
{
    if (ofxComponents == kOfxImageComponentRGBA) {
        *layer = ImageLayerDesc::getRGBAComponents();
    } else if (ofxComponents == kOfxImageComponentAlpha) {
        *layer = ImageLayerDesc::getAlphaComponents();
    } else if (ofxComponents == kOfxImageComponentRGB) {
        *layer = ImageLayerDesc::getRGBComponents();
    } else if (ofxComponents == kNatronOfxImageComponentXY) {
        *layer = ImageLayerDesc::getXYComponents();
    } else if (ofxComponents == kOfxImageComponentNone) {
        *layer = ImageLayerDesc::getNoneComponents();
    } else if (ofxComponents == kFnOfxImageComponentMotionVectors) {
        *layer = ImageLayerDesc::getBackwardMotionComponents();
        *pairedLayer = ImageLayerDesc::getForwardMotionComponents();
    } else if (ofxComponents == kFnOfxImageComponentStereoDisparity) {
        *layer = ImageLayerDesc::getDisparityLeftComponents();
        *pairedLayer = ImageLayerDesc::getDisparityRightComponents();
    } else {
        *layer = ofxCustomCompToNatronComp(ofxComponents);
    }

} // mapOFXComponentsTypeStringToLayers

static std::string
natronCustomCompToOfxComp(const ImageLayerDesc& comp)
{
    std::stringstream ss;
    const std::vector<std::string>& channels = comp.getChannels();
    const std::string& layerID = comp.getLayerID();
    const std::string& layerLabel = comp.getLayerLabel();
    const std::string& channelsLabel = comp.getChannelsLabel();
    ss << kNatronOfxImageComponentsPlaneName << layerID;
    if (!layerLabel.empty()) {
        ss << kNatronOfxImageComponentsPlaneLabel << layerLabel;
    }
    if (!channelsLabel.empty()) {
        ss << kNatronOfxImageComponentsPlaneChannelsLabel << channelsLabel;
    }
    for (std::size_t i = 0; i < channels.size(); ++i) {
        ss << kNatronOfxImageComponentsPlaneChannel << channels[i];
    }

    return ss.str();
} // natronCustomCompToOfxComp

std::string
ImageLayerDesc::mapLayerToOFXPlaneString(const ImageLayerDesc& layer)
{
    if (layer.isColorLayer()) {
        return kFnOfxImagePlaneColour;
    } else if (layer == ImageLayerDesc::getBackwardMotionComponents()) {
        return kFnOfxImagePlaneBackwardMotionVector;
    } else if (layer == ImageLayerDesc::getForwardMotionComponents()) {
        return kFnOfxImagePlaneForwardMotionVector;
    } else if (layer == ImageLayerDesc::getDisparityLeftComponents()) {
        return kFnOfxImagePlaneStereoDisparityLeft;
    } else if (layer == ImageLayerDesc::getDisparityRightComponents()) {
        return kFnOfxImagePlaneStereoDisparityRight;
    } else {
        return natronCustomCompToOfxComp(layer);
    }
}

std::string
ImageLayerDesc::mapLayerToOFXComponentsTypeString(const ImageLayerDesc& layer)
{
    if (layer == ImageLayerDesc::getNoneComponents()) {
        return kOfxImageComponentNone;
    } else if (layer == ImageLayerDesc::getAlphaComponents()) {
        return kOfxImageComponentAlpha;
    } else if (layer == ImageLayerDesc::getRGBComponents()) {
        return kOfxImageComponentRGB;
    } else if (layer == ImageLayerDesc::getRGBAComponents()) {
        return kOfxImageComponentRGBA;
    } else if (layer == ImageLayerDesc::getXYComponents()) {
        return kNatronOfxImageComponentXY;
    } else if (layer == ImageLayerDesc::getBackwardMotionComponents() || layer == ImageLayerDesc::getForwardMotionComponents()) {
        return kFnOfxImageComponentMotionVectors;
    } else if (layer == ImageLayerDesc::getDisparityLeftComponents() || layer == ImageLayerDesc::getDisparityRightComponents()) {
        return kFnOfxImageComponentStereoDisparity;
    } else {
        return natronCustomCompToOfxComp(layer);
    }
}
NATRON_NAMESPACE_EXIT
