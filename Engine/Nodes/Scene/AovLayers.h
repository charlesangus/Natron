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

#ifndef Engine_Nodes_Scene_AovLayers_h
#define Engine_Nodes_Scene_AovLayers_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <string>
#include <vector>

#include "Engine/ImageLayerDesc.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The Natron layer a Hydra AOV renders to: its name and channels. The layers of the AOVs
 * every render has (depth, primId, normal) keep the names they have always had.
 **/
namespace AovLayers {
enum KindEnum {
    eKindColor,
    eKindVector,
    eKindUv,
    eKindScalar
};

struct Layout {
    KindEnum kind = eKindColor;
    int channels = 3;
};

ImageLayerDesc depthLayer();
ImageLayerDesc primIdLayer();
ImageLayerDesc normalLayer();

// The authored layer when there is one, else the AOV name without its first namespace.
std::string layerName(const std::string& aovName,
                      const std::string& authoredLayer);

// The layout a RenderVar dataType ("color3f", "texCoord2f", "float") gives; false for one that names no layout.
bool layoutFromDataType(const std::string& dataType,
                        Layout* layout) WARN_UNUSED_RETURN;

// The layout of an AOV the renderer, or every delegate, is known to render; false for any other.
bool layoutFromName(const std::string& rendererId,
                    const std::string& aovName,
                    Layout* layout) WARN_UNUSED_RETURN;

/**
 * @brief The channels of a layout: R,G,B,A for colour, X,Y,Z,W for vectors, U,V,W for texture
 * coordinates, and for a scalar Z when it is a depth, id when it is an id, else Y.
 **/
std::vector<std::string> channelNames(const std::string& aovName,
                                      const Layout& layout);

/**
 * @brief The layout of an AOV: the dataType's, else the name's, else colour with as many
 * channels as formatChannels, else RGB (the RenderVar schema's color3f). A known formatChannels
 * (above 0) that differs from the dataType's or the name's count wins, with *warning set.
 **/
Layout layout(const std::string& rendererId,
              const std::string& aovName,
              const std::string& dataType,
              int formatChannels,
              std::string* warning);

ImageLayerDesc layer(const std::string& rendererId,
                     const std::string& aovName,
                     const std::string& dataType,
                     const std::string& authoredLayer,
                     int formatChannels,
                     std::string* warning);

/**
 * @brief The channel count of the HdFormat the renderer last returned the AOV in, 0 when it has
 * not rendered it in this process. The layers a node declares before rendering use it, so that
 * a delegate's format wins over a dataType from the second declaration on.
 **/
int formatChannels(const std::string& rendererId,
                   const std::string& aovName);

void rememberFormatChannels(const std::string& rendererId,
                            const std::string& aovName,
                            int channels);
} // namespace AovLayers

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_AovLayers_h
