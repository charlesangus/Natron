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

#include "OfxMultiplaneChoice.h"

#include "Engine/ImageLayerDesc.h"

NATRON_NAMESPACE_ENTER

namespace OfxMultiplaneChoice {

namespace {

    // ofxsMultiPlane.cpp's kMultiPlanePlaneParamOptionNone.
    const char* const kPluginPlaneOptionNone = "none";

    bool
    isChannelSuffix(const std::string& s)
    {
        if (s.empty()) {
            return false;
        }
        for (std::string::size_type i = 0; i < s.size(); ++i) {
            if ((s[i] < 'A') || (s[i] > 'Z')) {
                return false;
            }
        }

        return true;
    }

    // The built-in plane IDs ofxsMultiPlane.cpp's getHardCodedPlanes() knows about: every one of
    // them is reverse-DNS-style and so may itself contain dots, unlike a custom (dynamically
    // discovered) layer ID.
    const char* const*
    knownPluginPlaneIDs(std::size_t* count)
    {
        static const char* const ids[] = {
            kNatronColorLayerID,
            kNatronBackwardMotionVectorsLayerID,
            kNatronForwardMotionVectorsLayerID,
            kNatronDisparityLeftLayerID,
            kNatronDisparityRightLayerID
        };

        *count = sizeof(ids) / sizeof(ids[0]);

        return ids;
    }

    // Recognizes prefix as one of knownPluginPlaneIDs(), with or without a "<clip>." prepended
    // (clip names never contain a dot themselves). Returns false for anything else, including a
    // custom layer ID, which the caller falls back to handling generically.
    bool
    matchesKnownPluginPlaneID(const std::string& prefix, std::string* clipPrefix)
    {
        std::size_t count = 0;
        const char* const* ids = knownPluginPlaneIDs(&count);

        for (std::size_t i = 0; i < count; ++i) {
            const std::string knownID(ids[i]);
            if (prefix == knownID) {
                clipPrefix->clear();

                return true;
            }
            const std::string suffix = "." + knownID;
            if ((prefix.size() > suffix.size()) && (prefix.compare(prefix.size() - suffix.size(), suffix.size(), suffix) == 0)) {
                const std::string candidateClip = prefix.substr(0, prefix.size() - suffix.size());
                if (candidateClip.find('.') == std::string::npos) {
                    *clipPrefix = candidateClip;

                    return true;
                }
            }
        }

        return false;
    }

} // anonymous namespace

std::string
channelValueToPluginOption(const std::string& value)
{
    if (value.empty() || (value == "0") || (value == "1")) {
        return value;
    }

    const std::string::size_type dot = value.rfind('.');
    if (dot == std::string::npos) {
        return value;
    }

    const std::string layerID = value.substr(0, dot);
    const std::string channelName = value.substr(dot + 1);

    if (ImageLayerDesc::isColorViewID(layerID) || (layerID == kNatronColorLayerID)) {
        // Every colour view shares the one storage plane the plugin actually sees.
        return std::string(kNatronColorLayerID) + "." + channelName;
    }

    return value;
}

std::string
pluginOptionToChannelValue(const std::string& optionID)
{
    if (optionID.empty() || (optionID == "0") || (optionID == "1")) {
        return optionID;
    }

    const std::string::size_type dot = optionID.rfind('.');
    if (dot == std::string::npos) {
        return optionID;
    }

    const std::string planeID = optionID.substr(0, dot);
    const std::string channelName = optionID.substr(dot + 1);

    if ((planeID == kNatronColorLayerID) || (planeID == kNatronColorStorageLabel)) {
        return std::string(kNatronColorViewRGBA) + "." + channelName;
    }

    return optionID;
}

std::string
layerValueToPluginPlaneOption(const std::string& value)
{
    if (value.empty()) {
        return kPluginPlaneOptionNone;
    }

    if (ImageLayerDesc::isColorViewID(value) || (value == kNatronColorLayerID)) {
        return kNatronColorLayerID;
    }

    return value;
}

std::string
pluginPlaneOptionToLayerValue(const std::string& optionID)
{
    if (optionID == kPluginPlaneOptionNone) {
        return std::string();
    }

    if ((optionID == kNatronColorLayerID) || (optionID == kNatronColorStorageLabel)) {
        return kNatronColorViewRGBA;
    }

    return optionID;
}

bool
isMultiplaneChannelChoice(const std::vector<std::string>& entries,
                          std::string* clipName)
{
    if (clipName) {
        clipName->clear();
    }

    bool found = false;
    std::string commonClip;
    bool commonClipSet = false;

    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::string& entry = entries[i];

        if ((entry == "0") || (entry == "1")) {
            found = true;
            continue;
        }

        const std::string::size_type lastDot = entry.rfind('.');
        if ((lastDot == std::string::npos) || (lastDot + 1 >= entry.size())) {
            continue;
        }
        const std::string channel = entry.substr(lastDot + 1);
        if (!isChannelSuffix(channel)) {
            continue;
        }
        const std::string prefix = entry.substr(0, lastDot);

        std::string entryClip;
        if (!matchesKnownPluginPlaneID(prefix, &entryClip)) {
            // A custom layer ID never contains a dot itself, so at most one more dot ahead of
            // it can be a clip name; more than that is not a plane.channel encoding at all.
            const std::string::size_type firstDot = prefix.find('.');
            if (firstDot == std::string::npos) {
                entryClip.clear();
            } else if (prefix.find('.', firstDot + 1) == std::string::npos) {
                entryClip = prefix.substr(0, firstDot);
            } else {
                continue;
            }
        }

        found = true;
        if (!commonClipSet) {
            commonClip = entryClip;
            commonClipSet = true;
        }
    }

    if (found && clipName) {
        *clipName = commonClip;
    }

    return found;
}

} // namespace OfxMultiplaneChoice

NATRON_NAMESPACE_EXIT
