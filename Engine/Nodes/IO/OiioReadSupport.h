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

#ifndef Engine_Nodes_IO_OiioReadSupport_h
#define Engine_Nodes_IO_OiioReadSupport_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <OpenImageIO/imageio.h>

#include "Engine/EngineFwd.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_ENTER

namespace OiioReadSupport {
struct Header {
    std::vector<OIIO::ImageSpec> subimages;
};

/**
 * @brief The headers of every subimage of `path`, cached on (path, modification time, size).
 * A file that changed on disk is parsed again. Returns null and fills `error` on failure.
 * Thread-safe.
 **/
std::shared_ptr<const Header> readHeader(const std::string& path, std::string* error);

void clearHeaderCache();

/**
 * @brief The data window of `spec` in Natron's canvas, whose rows run upwards from the bottom of
 * the display window while the file's run downwards from its top.
 **/
RectI dataWindowOf(const OIIO::ImageSpec& spec);

/**
 * @brief A layer of a file other than its colour: the layer, the subimage that holds it, and the
 * index in that subimage of each of the layer's channels, in the layer's order.
 **/
struct FileLayer {
    ImageLayerDesc desc;
    int subimage;
    std::vector<int> channels;

    FileLayer()
        : desc()
        , subimage(0)
        , channels()
    {
    }
};

/**
 * @brief The layers of a file other than its colour, grouped by LayerRegistry::groupChannelNames
 * one subimage at a time. In a file of several parts, a channel that names no layer belongs to
 * the layer its part is named after; a part with no name of its own adds no layer name. A layer
 * that LayerRegistry::validate refuses for a file, such as one over the channel cap, is left out,
 * as is a layer whose ID an earlier one already has.
 **/
void fileLayers(const Header& header, std::vector<FileLayer>* layers);

/**
 * @brief Decodes channels [chbegin, chend) of `subimage` over `window` as 32-bit floats, as
 * OIIO converts them and with no colour transform.
 *
 * `window` is in Natron's canvas (bottom-up, see dataWindowOf) and must lie inside the subimage's
 * data window. The pixels are interleaved; row 0 of `dst` is the window's bottom row and
 * `rowStride` is the distance between rows in floats (at least window.width() * (chend - chbegin)).
 * `dst` must hold (window.height() - 1) * rowStride + window.width() * (chend - chbegin) floats.
 *
 * Only the scanlines, or tiles, that intersect `window` are read. Every call opens its own
 * ImageInput, so concurrent calls never share one. Thread-safe.
 **/
bool decode(const std::string& path,
            int subimage,
            int chbegin,
            int chend,
            const RectI& window,
            float* dst,
            std::size_t rowStride,
            std::string* error);

/**
 * @brief The attributes of `spec`, a subimage header of the file at `path`, as metadata keys.
 *
 * A key is the prefix of the attribute's source followed by its name, whose OIIO prefix is
 * dropped: `Exif:` and `GPS:` attributes give `exif/`, `dpx:` `dpx/`, `cineon:` `cin/`,
 * `tiff:` `tiff/`, `openexr:` `exr/`, and `oiio:` `oiio/` (so `oiio:ColorSpace` is
 * `oiio/ColorSpace`). An attribute with no prefix of its own gives `exr/` in an OpenEXR file and
 * `exif/` in any other, and one with any other prefix is dropped. The SMPTE timecode and keycode give the standard `ofx/timecode` and
 * `ofx/edgecode`. Integers give ints (wider ones doubles), floats and rationals doubles,
 * strings strings, and arrays and aggregates vectors.
 **/
ImageMetadata attributeMetadata(const std::string& path,
                                const OIIO::ImageSpec& spec);
} // namespace OiioReadSupport

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_IO_OiioReadSupport_h
