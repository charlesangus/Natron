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

#include "AovLayers.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <mutex>
#include <utility>

#include "Engine/Nodes/Scene/CyclesAovs.h"
#include "Engine/Nodes/Scene/MoonRayAovs.h"

NATRON_NAMESPACE_ENTER

namespace {
std::mutex gFormatsMutex;
std::map<std::pair<std::string, std::string>, int> gFormats;

std::string
withoutNamespace(const std::string& aovName)
{
    const std::string::size_type colon = aovName.find(':');

    return ((colon == std::string::npos) || (colon + 1 >= aovName.size())) ? aovName : aovName.substr(colon + 1);
}

bool
isDepthName(const std::string& name)
{
    return (name == "depth") || (name == "cameraDepth") || (name == "Z");
}

bool
isIdName(const std::string& name)
{
    return (name == "primId") || (name == "instanceId") || (name == "elementId");
}
} // anon namespace

namespace AovLayers {
ImageLayerDesc
depthLayer()
{
    return ImageLayerDesc("depth", std::vector<std::string>(1, "Z"));
}

ImageLayerDesc
primIdLayer()
{
    return ImageLayerDesc("primId", std::vector<std::string>(1, "id"));
}

ImageLayerDesc
normalLayer()
{
    static const char* const channels[] = { "x", "y", "z" };

    return ImageLayerDesc("normal", std::vector<std::string>(channels, channels + 3));
}

std::string
layerName(const std::string& aovName,
          const std::string& authoredLayer)
{
    if (!authoredLayer.empty()) {
        return authoredLayer;
    }
    // A colon cannot appear in a layer name, and "cycles:lightgroup:key" keeps one after the namespace.
    std::string name = withoutNamespace(aovName);
    std::replace(name.begin(), name.end(), ':', '_');

    return name;
}

bool
layoutFromDataType(const std::string& dataType,
                   Layout* layout)
{
    // The base name, its component count and a precision letter: "texCoord2f", "float", "half4".
    std::string base = dataType;
    if (!base.empty() && ((base.back() == 'f') || (base.back() == 'h') || (base.back() == 'd')) && (base.size() > 1) && std::isdigit((unsigned char)base[base.size() - 2])) {
        base.pop_back();
    }
    // The 64 of int64 and uint64 is their width, not a component count.
    if ((base == "int64") || (base == "uint64")) {
        layout->kind = eKindScalar;
        layout->channels = 1;

        return true;
    }
    std::string::size_type digits = base.size();
    while ((digits > 0) && std::isdigit((unsigned char)base[digits - 1])) {
        --digits;
    }
    const int count = (digits < base.size()) ? std::atoi(base.c_str() + digits) : 1;
    base = base.substr(0, digits);
    if ((count < 1) || (count > 4)) {
        return false;
    }
    if (base == "color") {
        if (count < 3) {
            return false;
        }
        layout->kind = eKindColor;
    } else if ((base == "normal") || (base == "vector") || (base == "point")) {
        if (count != 3) {
            return false;
        }
        layout->kind = eKindVector;
    } else if (base == "texCoord") {
        if ((count < 2) || (count > 3)) {
            return false;
        }
        layout->kind = eKindUv;
    } else if ((base == "float") || (base == "half") || (base == "double") || (base == "int") || (base == "uint")) {
        if (count == 1) {
            layout->kind = eKindScalar;
        } else if (((base == "float") || (base == "half") || (base == "double")) && (count >= 3)) {
            // Delegates give colour passes as float3 and float4 as often as color3f.
            layout->kind = eKindColor;
        } else {
            layout->kind = eKindVector;
        }
    } else {
        return false;
    }
    layout->channels = count;

    return true;
}

bool
layoutFromName(const std::string& rendererId,
               const std::string& aovName,
               Layout* layout)
{
    if (MoonRayAovs::isMoonRay(rendererId) && (aovName == MoonRayAovs::depthAov().GetString())) {
        *layout = Layout { eKindScalar, 1 };

        return true;
    }
    if (CyclesAovs::isCycles(rendererId) && CyclesAovs::passLayout(aovName, layout)) {
        return true;
    }
    if (aovName.compare(0, 4, "lpe:") == 0) {
        *layout = Layout { eKindColor, 3 };

        return true;
    }
    static const std::map<std::string, Layout> table = {
        { "color", { eKindColor, 4 } },
        { "depth", { eKindScalar, 1 } },
        { "cameraDepth", { eKindScalar, 1 } },
        { "primId", { eKindScalar, 1 } },
        { "instanceId", { eKindScalar, 1 } },
        { "elementId", { eKindScalar, 1 } },
        { "mist", { eKindScalar, 1 } },
        { "normal", { eKindVector, 3 } },
        { "Neye", { eKindVector, 3 } },
        { "N", { eKindVector, 3 } },
        { "P", { eKindVector, 3 } },
        { "Peye", { eKindVector, 3 } },
        { "position", { eKindVector, 3 } },
        { "st", { eKindUv, 3 } },
        { "uv", { eKindUv, 3 } },
    };
    std::map<std::string, Layout>::const_iterator found = table.find(aovName);
    if (found == table.end()) {
        found = table.find(withoutNamespace(aovName));
    }
    if (found == table.end()) {
        return false;
    }
    *layout = found->second;

    return true;
}

std::vector<std::string>
channelNames(const std::string& aovName,
             const Layout& layout)
{
    static const char* const color[] = { "R", "G", "B", "A" };
    static const char* const vector[] = { "X", "Y", "Z", "W" };
    static const char* const uv[] = { "U", "V", "W" };
    std::vector<std::string> out;

    if ((layout.kind == eKindScalar) || (layout.channels <= 1)) {
        const std::string bare = withoutNamespace(aovName);
        if (isDepthName(bare)) {
            out.push_back("Z");
        } else if (isIdName(bare)) {
            out.push_back("id");
        } else {
            out.push_back("Y");
        }

        return out;
    }
    const int count = std::min(layout.channels, (layout.kind == eKindUv) ? 3 : 4);
    for (int i = 0; i < count; ++i) {
        const char* const* names = (layout.kind == eKindColor) ? color : uv;
        if (layout.kind == eKindVector) {
            names = vector;
        }
        out.push_back(names[i]);
    }

    return out;
}

Layout
layout(const std::string& rendererId,
       const std::string& aovName,
       const std::string& dataType,
       int formatChannels,
       std::string* warning)
{
    Layout out;
    const bool known = (!dataType.empty() && layoutFromDataType(dataType, &out)) || layoutFromName(rendererId, aovName, &out);

    if (formatChannels <= 0) {
        return out;
    }
    if (!known) {
        out.kind = (formatChannels == 1) ? eKindScalar : eKindColor;
    } else if ((out.kind == eKindUv) && (out.channels == 2) && (formatChannels == 3)) {
        // Cycles hands texCoord2f back as three channels; the layer keeps U and V.
        return out;
    } else if ((formatChannels != out.channels) && warning) {
        *warning = "The AOV " + aovName + (dataType.empty() ? std::string() : " of dataType " + dataType) + " has " + std::to_string(out.channels) + " channels, but the renderer gives it " + std::to_string(formatChannels) + ": its layer has the renderer's " + std::to_string(formatChannels) + ".";
    }
    if (formatChannels != out.channels) {
        out.channels = formatChannels;
        if ((formatChannels == 1) && (out.kind != eKindScalar)) {
            out.kind = eKindScalar;
        } else if ((formatChannels > 3) && (out.kind == eKindUv)) {
            out.kind = eKindVector;
        }
    }

    return out;
}

ImageLayerDesc
layer(const std::string& rendererId,
      const std::string& aovName,
      const std::string& dataType,
      const std::string& authoredLayer,
      int formatChannels,
      std::string* warning)
{
    const std::string name = layerName(aovName, authoredLayer);

    return ImageLayerDesc(name, channelNames(aovName, layout(rendererId, aovName, dataType, formatChannels, warning)));
}

int
formatChannels(const std::string& rendererId,
               const std::string& aovName)
{
    std::lock_guard<std::mutex> l(gFormatsMutex);
    std::map<std::pair<std::string, std::string>, int>::const_iterator found = gFormats.find(std::make_pair(rendererId, aovName));

    return (found == gFormats.end()) ? 0 : found->second;
}

void
rememberFormatChannels(const std::string& rendererId,
                       const std::string& aovName,
                       int channels)
{
    std::lock_guard<std::mutex> l(gFormatsMutex);

    gFormats[std::make_pair(rendererId, aovName)] = channels;
}
} // namespace AovLayers

NATRON_NAMESPACE_EXIT
