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

#ifndef Engine_Nodes_IO_LayeredDocs_LayeredDocument_h
#define Engine_Nodes_IO_LayeredDocs_LayeredDocument_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <OpenImageIO/imageio.h>

NATRON_NAMESPACE_ENTER

namespace LayeredDocs {
/**
 * @brief The prefix of every metadata key a layered document publishes.
 **/
#define kLayeredMetadataPrefix "layered/"

/**
 * @brief The int attribute, 1, on the header of every layer subimage: the layer is a picture in the
 * same colour encoding as the composite, so a reader converts its colour the way it converts the
 * composite's. Its ':' keeps it out of the published metadata.
 **/
#define kLayeredColourLayerAttribute "natron:colourLayer"

/**
 * @brief A rectangle of a plane in the plane's own pixels: columns [x1, x2) and rows [y1, y2),
 * rows counted down from the plane's top.
 **/
struct Region {
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;

    int width() const
    {
        return x2 - x1;
    }

    int height() const
    {
        return y2 - y1;
    }
};

/**
 * @brief Pixels of a layered document: a rectangle of the canvas, whose rows run downwards from
 * the canvas's top, and how to decode it.
 *
 * Pixels are interleaved in `channels` order, top row first, as `type`, with alpha associated
 * (premultiplied), the way OIIO hands out every other file. A reader sets one of the decoders:
 *
 * - `loadRegion` decodes only `region`, writing its `region.width() * region.height()` pixels
 *   contiguously to `dst`. Reads go straight to it, so that a read of a few rows of a huge plane
 *   decodes only those. It may be called from any thread, any number of times.
 * - `load`, for a format that can only decode a plane whole, fills `pixels` with all
 *   `width * height` pixels. The document keeps the result, so it is called at most once unless it
 *   fails, and may be called from any thread.
 *
 * `tileWidth` and `tileHeight`, when a `loadRegion` reader sets both, are the size of the blocks
 * it decodes in; the plane is then read as OIIO tiles of that size rather than as scanlines, so
 * that a read of a few columns does not decode whole rows.
 **/
struct Plane {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    std::vector<std::string> channels;
    OIIO::TypeDesc type = OIIO::TypeDesc::UINT8;
    std::function<bool(std::vector<unsigned char>* pixels, std::string* error)> load;
    std::function<bool(const Region& region, unsigned char* dst, std::string* error)> loadRegion;
    int tileWidth = 0;
    int tileHeight = 0;

    bool decodable() const
    {
        return bool(load) || bool(loadRegion);
    }

    bool tiled() const
    {
        return bool(loadRegion) && tileWidth > 0 && tileHeight > 0;
    }
};

/**
 * @brief Adds `rows` to the count of stored rows the readers have decoded: a reader counts each
 * row of each stored channel or tile row it decompresses or copies out of the file, and a plane
 * decoded whole counts all of its rows. Lets a test see how much of a file a read decoded.
 * Thread-safe.
 **/
void countDecodedRows(std::uint64_t rows);

/**
 * @brief The count countDecodedRows keeps, since the process started.
 **/
std::uint64_t decodedRows();

/**
 * @brief A file opened once for reading at offsets, by any number of threads at once.
 **/
class File {
public:
    explicit File(const std::string& path);
    ~File();
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    bool isOpen() const
    {
        return _fd >= 0;
    }

    std::uint64_t size() const
    {
        return _size;
    }

    /**
     * @brief Reads exactly `count` bytes at `offset` into `dst`. False if the file ends first or
     * cannot be read.
     **/
    bool readAt(std::uint64_t offset,
                void* dst,
                std::size_t count) const;

private:
    int _fd = -1;
    std::uint64_t _size = 0;
};

/**
 * @brief A raster layer. `plane` always has the channels R, G, B and A, so that a mask folds into
 * the alpha and a Gray layer fills R, G and B.
 *
 * `blendMode` is the file's own name for the mode. `groups` are the names of the enclosing
 * groups, outermost first; `groupVisible` and `groupOpacity` are what they contribute when the
 * document is flattened.
 **/
struct Layer {
    std::string name;
    std::vector<std::string> groups;
    std::string blendMode;
    double opacity = 1.;
    bool visible = true;
    bool clipping = false;
    bool groupVisible = true;
    double groupOpacity = 1.;
    Plane plane;

    // Set by LayeredDocument::finalise.
    std::string id;
    int windowX = 0;
    int windowY = 0;
    int windowWidth = 0;
    int windowHeight = 0;
};

/**
 * @brief A layered document read by a format reader: the canvas, the composite and the raster
 * layers, bottom to top. A reader fills the public fields and then calls finalise(), after which
 * the document is read-only and safe to share between threads.
 *
 * Subimage 0 is the composite; subimage k > 0 is the k-th layer of `layers`.
 **/
class LayeredDocument {
public:
    LayeredDocument() = default;
    LayeredDocument(const LayeredDocument&) = delete;
    LayeredDocument& operator=(const LayeredDocument&) = delete;

    int width = 0;
    int height = 0;

    // The stored composite; one with no decoder is rebuilt by flattening the layers.
    Plane composite;

    std::vector<Layer> layers;

    // Layers kept as structure only, such as empty ones or kinds that hold no raster pixels, each
    // as "<group path>/<name> (<reason>)".
    std::vector<std::string> skipped;

    // The OIIO colour space name every subimage publishes as `oiio:ColorSpace`, when the format
    // records how its values are encoded; empty otherwise.
    std::string colorSpace;

    // The bytes the reader keeps for the document besides the planes it caches, such as the
    // archive it reads members from. May be called from any thread.
    std::function<std::size_t()> readerBytes;

    /**
     * @brief Crops every layer to the canvas and moves those left empty to `skipped`, gives each
     * remaining layer its ID (sanitisedLayerIds) and, when there is no stored composite, makes one
     * that flattens the visible layers bottom to top with the Normal mode, at the deepest of their
     * depths, read in 64 by 64 tiles. Clipping is not applied: a clipped layer is composited as if
     * it were not. Returns false and fills `error` if the canvas is empty.
     **/
    bool finalise(std::string* error);

    bool flattened() const
    {
        return _flattened;
    }

    int subimageCount() const
    {
        return 1 + (int)layers.size();
    }

    /**
     * @brief The header of `subimage`: its data window within the full window, which is the
     * canvas, its channels and type, a tile size (that of a tiled plane whose stored tiles line
     * up with the data window, or 64 by 64 for a flattened composite), `oiio:ColorSpace` set to
     * `colorSpace` when that is known, `oiio:subimagename` set to
     * the layer ID and kLayeredColourLayerAttribute for a layer, and the document's metadata
     * (describe) on the composite.
     **/
    OIIO::ImageSpec specOf(int subimage) const;

    /**
     * @brief Columns [x1, x2) and rows [y1, y2) of the data window of `subimage`, in canvas pixels
     * with rows counted down from the top, interleaved as its spec gives them, row after row with
     * no padding. Only the part asked for is decoded when the reader can window its planes.
     * Thread-safe.
     **/
    bool readRegion(int subimage,
                    int x1,
                    int y1,
                    int x2,
                    int y2,
                    void* dst,
                    std::string* error) const;

    /**
     * @brief Adds the document's metadata to `spec` as attributes under kLayeredMetadataPrefix:
     * `layers` (the layer IDs bottom to top), `skipped`, `composite` ("stored" or "flattened"),
     * and per layer `<id>/name`, `<id>/blendMode`, `<id>/opacity`, `<id>/visible`,
     * `<id>/clipping` and `<id>/group` (the group names joined by "/").
     **/
    void describe(OIIO::ImageSpec* spec) const;

    /**
     * @brief The bytes the document holds: the planes decoded whole and kept, and readerBytes.
     * Thread-safe.
     **/
    std::size_t residentBytes() const;

private:
    struct Cache {
        std::mutex mutex;
        bool loaded = false;
        std::vector<unsigned char> pixels;
    };

    bool pixelsOf(int subimage,
                  const std::vector<unsigned char>** pixels,
                  std::string* error) const;

    bool flatten(int x1,
                 int y1,
                 int x2,
                 int y2,
                 void* dst,
                 std::string* error) const;

    const Plane& planeOf(int subimage) const;

    void windowOf(int subimage,
                  int* x,
                  int* y,
                  int* w,
                  int* h) const;

    bool _flattened = false;
    int _compositeX = 0;
    int _compositeY = 0;
    int _compositeWidth = 0;
    int _compositeHeight = 0;
    mutable std::vector<std::unique_ptr<Cache>> _caches;
    mutable std::atomic<std::size_t> _cachedBytes { 0 };
};

/**
 * @brief Natron layer IDs for layers at `paths` (each the enclosing group names, outermost first,
 * then the layer's name), in order. A path's names are joined by "_", and every character but
 * ASCII letters, digits and "_" becomes "_", so that LayerRegistry::validate accepts the ID. An ID
 * that is empty, names the colour plane or another reserved or built-in layer, or repeats an
 * earlier one gets the first free suffix of "_2", "_3"...
 **/
std::vector<std::string> sanitisedLayerIds(const std::vector<std::vector<std::string>>& paths);

/**
 * @brief `count` pixels of `names` channels (R, G, B, A, or Y for gray) as R, G, B, A floats: a
 * gray channel fills R, G and B, and a missing alpha is opaque.
 **/
void toRgba(const float* src,
            std::size_t count,
            const std::vector<std::string>& names,
            float* dst);

/**
 * @brief The values of `src`, `count` of `from`, as `to`.
 **/
bool convertValues(OIIO::TypeDesc from,
                   const void* src,
                   OIIO::TypeDesc to,
                   void* dst,
                   std::size_t count);

/**
 * @brief The deeper of two stored types: 8 < 16 bit < half < float.
 **/
OIIO::TypeDesc deeperType(OIIO::TypeDesc a,
                          OIIO::TypeDesc b);
} // namespace LayeredDocs

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_IO_LayeredDocs_LayeredDocument_h
