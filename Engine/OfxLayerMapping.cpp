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

#include "OfxLayerMapping.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include <nuke/fnOfxExtensions.h>
#include <ofxImageEffect.h>
#include <ofxNatron.h>

NATRON_NAMESPACE_ENTER

namespace OfxLayerMapping {

namespace {

    // The plane and channels labels a plug-in may put in a custom-plane string are skipped: a
    // layer is its name and its channels.
    bool
    decodeCustomPlane(const std::string& encoded,
                      std::string* layerID,
                      std::vector<std::string>* channels)
    {
        const std::size_t nameMarker = encoded.find(kNatronOfxImageComponentsPlaneName);
        if (nameMarker == std::string::npos) {
            return false;
        }
        const std::size_t nameStart = nameMarker + std::strlen(kNatronOfxImageComponentsPlaneName);

        const std::size_t channelLen = std::strlen(kNatronOfxImageComponentsPlaneChannel);
        std::size_t channel = encoded.find(kNatronOfxImageComponentsPlaneChannel, nameStart);
        if (channel == std::string::npos) {
            return false;
        }

        std::size_t nameEnd = channel;
        const std::size_t layerLabel = encoded.find(kNatronOfxImageComponentsPlaneLabel, nameStart);
        if (layerLabel < nameEnd) {
            nameEnd = layerLabel;
        }
        const std::size_t channelsLabel = encoded.find(kNatronOfxImageComponentsPlaneChannelsLabel, nameStart);
        if (channelsLabel < nameEnd) {
            nameEnd = channelsLabel;
        }
        *layerID = encoded.substr(nameStart, nameEnd - nameStart);
        if (layerID->empty()) {
            return false;
        }

        channels->clear();
        while (channel != std::string::npos) {
            if (channels->size() >= 4) {
                return false;
            }
            const std::size_t start = channel + channelLen;
            const std::size_t next = encoded.find(kNatronOfxImageComponentsPlaneChannel, start);
            channels->push_back(encoded.substr(start, (next == std::string::npos) ? std::string::npos : next - start));
            channel = next;
        }

        return true;
    }

    std::string
    encodeCustomPlane(const ImageLayerDesc& layer)
    {
        std::string ret = kNatronOfxImageComponentsPlaneName + layer.getLayerID();
        const std::vector<std::string>& channels = layer.getChannels();
        for (std::size_t i = 0; i < channels.size(); ++i) {
            ret += kNatronOfxImageComponentsPlaneChannel;
            ret += channels[i];
        }

        return ret;
    }

    ImageLayerDesc
    customPlaneToLayer(const std::string& encoded)
    {
        std::string layerID;
        std::vector<std::string> channels;
        if (!decodeCustomPlane(encoded, &layerID, &channels)) {
            return ImageLayerDesc::getNoneComponents();
        }

        return ImageLayerDesc(layerID, channels);
    }

    bool
    isBuiltinPlugInPlaneID(const std::string& planeID)
    {
        const std::vector<std::string>& ids = builtinPlugInPlaneIDs();

        return std::find(ids.begin(), ids.end(), planeID) != ids.end();
    }

    std::string
    upperCase(std::string s)
    {
        for (std::size_t i = 0; i < s.size(); ++i) {
            s[i] = (char)std::toupper((unsigned char)s[i]);
        }

        return s;
    }

    std::string
    lowerCase(std::string s)
    {
        for (std::size_t i = 0; i < s.size(); ++i) {
            s[i] = (char)std::tolower((unsigned char)s[i]);
        }

        return s;
    }

} // anonymous namespace

ImageLayerDesc
planeToLayer(const std::string& ofxPlane)
{
    if (ofxPlane == kFnOfxImagePlaneColour) {
        return ImageLayerDesc::getRGBAComponents();
    } else if (ofxPlane == kFnOfxImagePlaneBackwardMotionVector) {
        return ImageLayerDesc::getBackwardMotionComponents();
    } else if (ofxPlane == kFnOfxImagePlaneForwardMotionVector) {
        return ImageLayerDesc::getForwardMotionComponents();
    } else if (ofxPlane == kFnOfxImagePlaneStereoDisparityLeft) {
        return ImageLayerDesc::getDisparityLeftComponents();
    } else if (ofxPlane == kFnOfxImagePlaneStereoDisparityRight) {
        return ImageLayerDesc::getDisparityRightComponents();
    }

    return customPlaneToLayer(ofxPlane);
}

std::string
layerToPlane(const ImageLayerDesc& layer)
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
    }

    return encodeCustomPlane(layer);
}

bool
isColourPlane(const std::string& ofxPlane)
{
    return ofxPlane == kFnOfxImagePlaneColour;
}

std::string
colourPlane()
{
    return kFnOfxImagePlaneColour;
}

void
componentsToLayers(const std::string& ofxComponents,
                   ImageLayerDesc* layer,
                   ImageLayerDesc* pairedLayer)
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
        *layer = customPlaneToLayer(ofxComponents);
    }
}

std::string
layerToComponents(const ImageLayerDesc& layer)
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
    } else if ((layer == ImageLayerDesc::getBackwardMotionComponents()) || (layer == ImageLayerDesc::getForwardMotionComponents())) {
        return kFnOfxImageComponentMotionVectors;
    } else if ((layer == ImageLayerDesc::getDisparityLeftComponents()) || (layer == ImageLayerDesc::getDisparityRightComponents())) {
        return kFnOfxImageComponentStereoDisparity;
    }

    return encodeCustomPlane(layer);
}

std::string
componentsTypeToComponents(ImageComponentsTypeEnum componentsType,
                           int nComps)
{
    switch (componentsType) {
    case eImageComponentsTypeMotion:
        return kFnOfxImageComponentMotionVectors;
    case eImageComponentsTypeDisparity:
        return kFnOfxImageComponentStereoDisparity;
    case eImageComponentsTypeColor:
    case eImageComponentsTypeNone:
        break;
    }

    return layerToComponents(ImageLayerDesc::mapNCompsToColorLayer(nComps));
}

ImageComponentsTypeEnum
componentsToComponentsType(const std::string& ofxComponents)
{
    if ((ofxComponents == kOfxImageComponentRGBA) || (ofxComponents == kOfxImageComponentRGB) || (ofxComponents == kOfxImageComponentAlpha) || (ofxComponents == kNatronOfxImageComponentXY)) {
        return eImageComponentsTypeColor;
    } else if (ofxComponents == kFnOfxImageComponentMotionVectors) {
        return eImageComponentsTypeMotion;
    } else if (ofxComponents == kFnOfxImageComponentStereoDisparity) {
        return eImageComponentsTypeDisparity;
    }

    return eImageComponentsTypeNone;
}

const std::vector<std::string>&
builtinPlugInPlaneIDs()
{
    static const std::vector<std::string> ids = {
        kFnOfxImagePlaneColour,
        kFnOfxImagePlaneBackwardMotionVector,
        kFnOfxImagePlaneForwardMotionVector,
        kFnOfxImagePlaneStereoDisparityLeft,
        kFnOfxImagePlaneStereoDisparityRight
    };

    return ids;
}

std::string
layerIDToPlugInPlaneID(const std::string& layerID)
{
    if (ImageLayerDesc::isColorViewID(layerID)) {
        return kFnOfxImagePlaneColour;
    } else if (layerID == kNatronBackwardMotionVectorsLayerID) {
        return kFnOfxImagePlaneBackwardMotionVector;
    } else if (layerID == kNatronForwardMotionVectorsLayerID) {
        return kFnOfxImagePlaneForwardMotionVector;
    } else if (layerID == kNatronDisparityLeftLayerID) {
        return kFnOfxImagePlaneStereoDisparityLeft;
    } else if (layerID == kNatronDisparityRightLayerID) {
        return kFnOfxImagePlaneStereoDisparityRight;
    }

    return layerID;
}

std::string
plugInPlaneIDToLayerID(const std::string& planeID)
{
    if (planeID == kFnOfxImagePlaneColour) {
        return kNatronColorViewRGBA;
    } else if (planeID == kFnOfxImagePlaneBackwardMotionVector) {
        return kNatronBackwardMotionVectorsLayerID;
    } else if (planeID == kFnOfxImagePlaneForwardMotionVector) {
        return kNatronForwardMotionVectorsLayerID;
    } else if (planeID == kFnOfxImagePlaneStereoDisparityLeft) {
        return kNatronDisparityLeftLayerID;
    } else if (planeID == kFnOfxImagePlaneStereoDisparityRight) {
        return kNatronDisparityRightLayerID;
    }

    return planeID;
}

std::string
channelValueToPlugInOption(const std::string& layerID,
                           const std::string& channel)
{
    const std::string planeID = layerIDToPlugInPlaneID(layerID);
    if (isBuiltinPlugInPlaneID(planeID)) {
        return planeID + "." + upperCase(channel);
    }

    return planeID + "." + channel;
}

std::string
plugInOptionToChannelValue(const std::string& planeID,
                           const std::string& channel)
{
    if (planeID == kFnOfxImagePlaneColour) {
        if ((channel == "X") || (channel == "Y")) {
            return std::string(kNatronColorViewXY) + "." + lowerCase(channel);
        }

        return std::string(kNatronColorViewRGBA) + "." + channel;
    }
    if (isBuiltinPlugInPlaneID(planeID)) {
        return plugInPlaneIDToLayerID(planeID) + "." + lowerCase(channel);
    }

    return planeID + "." + channel;
}

} // namespace OfxLayerMapping

NATRON_NAMESPACE_EXIT
