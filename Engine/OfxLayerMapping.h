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

#ifndef NATRON_ENGINE_OFXLAYERMAPPING_H
#define NATRON_ENGINE_OFXLAYERMAPPING_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/NodeMetadata.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The one translation between Natron layers and the strings OFX carries: the plane
 * strings of the multi-plane suites (kFnOfxImagePlane*), the clip components strings
 * (kOfxImageComponent*, kFnOfxImageComponent*) and the custom-plane encoding of ofxNatron.h.
 * Natron's own layer names never appear on the plugin side and OFX strings never leave the
 * OFX host.
 **/
namespace OfxLayerMapping {

/**
 * @brief The colour plane string maps to the RGBA colour storage; an unknown or malformed string
 * to the None layer.
 **/
ImageLayerDesc planeToLayer(const std::string& ofxPlane);

std::string layerToPlane(const ImageLayerDesc& layer);

bool isColourPlane(const std::string& ofxPlane);

std::string colourPlane();

/**
 * @brief The motion vector and disparity components strings name a pair of layers: *layer gets
 * the backward or left one and *pairedLayer the forward or right one. Any other string leaves
 * *pairedLayer untouched.
 **/
void componentsToLayers(const std::string& ofxComponents, ImageLayerDesc* layer, ImageLayerDesc* pairedLayer);

std::string layerToComponents(const ImageLayerDesc& layer);

/**
 * @brief nComps picks the colour components string when componentsType is colour or None.
 **/
std::string componentsTypeToComponents(ImageComponentsTypeEnum componentsType, int nComps);

/**
 * @brief The RGBA, RGB, Alpha and XY components strings are colour; None and a custom-plane
 * string are None.
 **/
ImageComponentsTypeEnum componentsToComponentsType(const std::string& ofxComponents);

/**
 * @brief The plugin-side IDs of the built-in planes, as ofxsMultiPlane.cpp's option IDs carry
 * them. Unlike a custom layer name, each contains dots.
 **/
const std::vector<std::string>& builtinPlugInPlaneIDs();

/**
 * @brief A layer name to the plane ID a plug-in's own plane choice uses: every colour view names
 * the colour plane. Any other name passes through.
 **/
std::string layerIDToPlugInPlaneID(const std::string& layerID);

/**
 * @brief The inverse of layerIDToPlugInPlaneID(); the colour plane comes back as the rgba view.
 **/
std::string plugInPlaneIDToLayerID(const std::string& planeID);

/**
 * @brief A "<layer>.<channel>" value to the "<plane>.<channel>" option a plug-in's channel choice
 * uses. The built-in planes carry upper-case channel names on the plug-in side.
 **/
std::string channelValueToPlugInOption(const std::string& layerID, const std::string& channel);

/**
 * @brief The inverse of channelValueToPlugInOption(): an X or Y channel of the colour plane comes
 * back on the xy view, any other colour channel on the rgba view.
 **/
std::string plugInOptionToChannelValue(const std::string& planeID, const std::string& channel);

} // namespace OfxLayerMapping

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_OFXLAYERMAPPING_H
