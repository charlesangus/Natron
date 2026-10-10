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
#include <functional>
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
 * @brief Drops the cached headers of `paths` alone, so that the next readHeader of each parses the
 * file again. Thread-safe.
 **/
void evictHeaders(const std::vector<std::string>& paths);

/**
 * @brief Calls `hook` with the path of each header readHeader parsed, after parsing it and before
 * caching it. An empty function removes the hook.
 **/
void setHeaderParsedHookForTests(const std::function<void(const std::string&)>& hook);

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
 *
 * Only the parts and channels of `view` are listed, as viewParts gives them; a part name's view
 * suffix is not part of a layer name, so the layers are the same in every view.
 **/
void fileLayers(const Header& header, std::vector<FileLayer>* layers, const std::string& view = std::string());

/**
 * @brief The views a file names, in file order and without repeats: the `multiView` list of a
 * single-part file, then the `view` attribute of each part. The first is the default view. Empty
 * for a file that names none.
 **/
std::vector<std::string> viewNames(const Header& header);

/**
 * @brief The view of the file whose pixels `view` reads, compared without regard to case: `view`
 * itself if the file has it, else the file's default view. Empty for a file that names no view.
 **/
std::string resolveView(const Header& header, const std::string& view);

/**
 * @brief The channels of one subimage that belong to a view: the file index of each, and its name
 * with the view taken out (the OpenEXR multi-view convention puts the view before the last
 * component of the name, and leaves the default view unprefixed). `part` is the subimage's name
 * in a multi-part file, without the view that the name may end or begin with.
 **/
struct PartChannels {
    int subimage;
    std::string part;
    std::vector<std::string> names;
    std::vector<int> index;

    PartChannels()
        : subimage(0)
        , part()
        , names()
        , index()
    {
    }
};

/**
 * @brief Whether a part named `part` holds the colour plane rather than a layer named after it,
 * compared without regard to case. Writers name such a part after the plane ("Color" for
 * Natron's, "rgba" for others), so its bare channels are the colour plane's.
 **/
bool isColourPartName(const std::string& part);

/**
 * @brief The subimages of `header` that hold channels of `view` (resolved by resolveView), in
 * file order. A subimage or channel that names no view belongs to every view; so does everything
 * in a file that names none, which gives every subimage whole.
 **/
void viewParts(const Header& header, const std::string& view, std::vector<PartChannels>* parts);

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

/**
 * @brief The file extensions the native Read accepts: every extension in OIIO's runtime
 * "extension_list" except those of a format in kExcludedOiioFormats, lower case, sorted. An
 * extension that an excluded format shares with a kept one stays. Computed once. Thread-safe.
 **/
const std::vector<std::string>& readableExtensions();

/**
 * @brief The OIIO formats whose extensions the native Read does not accept.
 **/
extern const char* const kExcludedOiioFormats[5];

/**
 * @brief The name of the kept OIIO format that claims `extension` (case-insensitive, with or
 * without a leading dot), or an empty string if no kept format does.
 **/
std::string formatNameForExtension(const std::string& extension);

/**
 * @brief Whether the extension of `path` is one the native Read accepts.
 **/
bool isReadablePath(const std::string& path);
} // namespace OiioReadSupport

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_IO_OiioReadSupport_h
