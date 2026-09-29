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

#ifndef NATRON_ENGINE_OFXMULTIPLANECHOICE_H
#define NATRON_ENGINE_OFXMULTIPLANECHOICE_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Translates between Natron's own layer/channel selection values (as read/written by
 * KnobLayerSelect/KnobChannelSelect: a layer ID, "<layerID>.<channelName>", the constants "0"
 * and "1", or empty for None) and the option IDs openfx-supportext's ofxsMultiPlane.cpp encodes
 * into an OFX plugin's own plane/channel choice params -- Premult/Unpremult's "inputPlane" and
 * Premult's own "unPremultByChannel", IDistort's/STMap's "channelU"/"channelV"/"channelA" --
 * that Natron's own layer/channel knobs are meant to replace.
 *
 * The plugin never sees Natron's colour views: every colour view (rgba/rgb/alpha/xy) shares the
 * single colour storage plane on the plugin side, so every colour option carries the storage
 * plane ID (kNatronColorLayerID). The reverse direction folds that ID, and the retired "Color"
 * label, back to the rgba view: it never hands the storage ID back to a caller.
 **/
namespace OfxMultiplaneChoice {

/**
 * @brief A KnobChannelSelect value ("<layerID>.<channelName>", "0", "1", or empty) to the
 * option ID ofxsMultiPlane.cpp's ImagePlaneDesc::getChannelOption() would produce for it.
 **/
std::string channelValueToPluginOption(const std::string& value);

/**
 * @brief The inverse of channelValueToPluginOption(): a plugin channel option ID to a
 * KnobChannelSelect value. Accepts both the storage plane ID and the retired "Color" label for
 * the colour channels it is handed, but never returns the storage ID itself: colour channels
 * come back as rgba.<C>.
 **/
std::string pluginOptionToChannelValue(const std::string& optionID);

/**
 * @brief A KnobLayerSelect layer value (a layer ID, or empty for None) to the option ID
 * ofxsMultiPlane.cpp's ImagePlaneDesc::getPlaneOption() would produce for it.
 **/
std::string layerValueToPluginPlaneOption(const std::string& value);

/**
 * @brief The inverse of layerValueToPluginPlaneOption(): a plugin plane option ID to a
 * KnobLayerSelect layer value. Accepts both the storage plane ID and the retired "Color" label,
 * but never returns the storage ID itself: the colour plane comes back as rgba.
 **/
std::string pluginPlaneOptionToLayerValue(const std::string& optionID);

/**
 * @brief Whether entries is the option list of a multiplane channel choice, as
 * ofxsMultiPlane.cpp's addInputChannelOptionsRGBA()/buildChannelMenus() build it (plane.channel
 * entries, e.g. "uk.co.thefoundry.OfxImagePlaneColour.R" or "diffuse.R", optionally with a
 * "0"/"1" constant, and, when the choice depends on more than one clip, a "<clip>." prefix) --
 * as opposed to a plain fixed channel choice such as ofxsMaskMix.h's "unPremultByChannel"
 * (option IDs "r", "g", "b", "a": no plane, no constants). When true and a clip prefix was
 * found on at least one recognised entry, *clipName is set to it; otherwise *clipName is
 * cleared.
 **/
bool isMultiplaneChannelChoice(const std::vector<std::string>& entries, std::string* clipName);

} // namespace OfxMultiplaneChoice

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_OFXMULTIPLANECHOICE_H
