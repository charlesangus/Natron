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

#include <cctype>

#include "Engine/ImageLayerDesc.h"
#include "Engine/OfxLayerMapping.h"

NATRON_NAMESPACE_ENTER

namespace OfxMultiplaneChoice {

namespace {

    // ofxsMultiPlane.h's kMultiPlanePlaneParamOptionNone.
    const char* const kPluginPlaneOptionNone = "none";

    // ofxsMultiPlane.h's kMultiPlaneChannelParamOption0 and kMultiPlaneChannelParamOption1.
    const char* const kPluginChannelOption0 = "0";
    const char* const kPluginChannelOption1 = "1";

    // Channel names start with a letter and hold letters, digits and underscores. A fixed
    // choice option such as "1.5" is not a plane.channel encoding.
    bool
    isChannelSuffix(const std::string& s)
    {
        if (s.empty() || !std::isalpha((unsigned char)s[0])) {
            return false;
        }
        for (std::string::size_type i = 1; i < s.size(); ++i) {
            if (!std::isalnum((unsigned char)s[i]) && (s[i] != '_')) {
                return false;
            }
        }

        return true;
    }

    // Recognizes prefix as one of the built-in plug-in plane IDs, with or without a "<clip>." prepended
    // (clip names never contain a dot themselves). Returns false for anything else, including a
    // custom layer ID, which the caller falls back to handling generically.
    bool
    matchesKnownPluginPlaneID(const std::string& prefix, std::string* clipPrefix)
    {
        const std::vector<std::string>& ids = OfxLayerMapping::builtinPlugInPlaneIDs();

        for (std::size_t i = 0; i < ids.size(); ++i) {
            const std::string& knownID = ids[i];
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
    const std::string::size_type dot = value.rfind('.');
    if (dot == std::string::npos) {
        return value;
    }

    return OfxLayerMapping::channelValueToPlugInOption(value.substr(0, dot), value.substr(dot + 1));
}

std::string
noneChannelOption(const std::string& paramName)
{
    // Distortion.cpp multiplies the output alpha by "channelA" (and divides UV by it when
    // unpremultiplying), and Premult.cpp treats "unPremultByChannel" at 1 as an identity.
    if ((paramName == "channelA") || (paramName == "unPremultByChannel")) {
        return kPluginChannelOption1;
    }

    return kPluginChannelOption0;
}

std::string
pluginOptionToChannelValue(const std::string& optionID)
{
    const std::string::size_type dot = optionID.rfind('.');
    if (dot == std::string::npos) {
        return std::string();
    }

    return OfxLayerMapping::plugInOptionToChannelValue(optionID.substr(0, dot), optionID.substr(dot + 1));
}

std::string
layerValueToPluginPlaneOption(const std::string& value)
{
    if (value.empty()) {
        return kPluginPlaneOptionNone;
    }

    return OfxLayerMapping::layerIDToPlugInPlaneID(value);
}

std::string
pluginPlaneOptionToLayerValue(const std::string& optionID)
{
    if (optionID == kPluginPlaneOptionNone) {
        return std::string();
    }

    return OfxLayerMapping::plugInPlaneIDToLayerID(optionID);
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

        if ((entry == kPluginChannelOption0) || (entry == kPluginChannelOption1)) {
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
