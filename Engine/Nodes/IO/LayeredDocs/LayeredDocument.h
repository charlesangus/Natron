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

#include <cstddef>
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
 * @brief Pixels of a layered document: a rectangle of the canvas, whose rows run downwards from
 * the canvas's top, and how to decode it.
 *
 * `load` fills `pixels` with all `width * height` pixels of the rectangle, interleaved in
 * `channels` order, top row first, as `type`, with alpha associated (premultiplied), the way
 * OIIO hands out every other file. It may be called from any thread, at most once per document
 * unless it fails.
 **/
struct Plane {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    std::vector<std::string> channels;
    OIIO::TypeDesc type = OIIO::TypeDesc::UINT8;
    std::function<bool(std::vector<unsigned char>* pixels, std::string* error)> load;
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

    // The stored composite; one without `load` is rebuilt by flattening the layers.
    Plane composite;

    std::vector<Layer> layers;

    // Layers kept as structure only, such as empty ones or kinds that hold no raster pixels, each
    // as "<group path>/<name> (<reason>)".
    std::vector<std::string> skipped;

    /**
     * @brief Crops every layer to the canvas and moves those left empty to `skipped`, gives each
     * remaining layer its ID (sanitisedLayerIds) and, when there is no stored composite, makes one
     * that flattens the visible layers bottom to top with the Normal mode, at the deepest of their
     * depths. Returns false and fills `error` if the canvas is empty.
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
     * canvas, its channels and type, `oiio:subimagename` set to the layer ID for a layer, and the
     * document's metadata (describe) on the composite.
     **/
    OIIO::ImageSpec specOf(int subimage) const;

    /**
     * @brief Rows [y1, y2) of the data window of `subimage`, in canvas rows counted down from the
     * top, interleaved as its spec gives them. Thread-safe.
     **/
    bool readRows(int subimage,
                  int y1,
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

private:
    struct Cache {
        std::mutex mutex;
        bool loaded = false;
        std::vector<unsigned char> pixels;
    };

    bool pixelsOf(int subimage,
                  const std::vector<unsigned char>** pixels,
                  std::string* error) const;

    bool flatten(std::vector<unsigned char>* pixels,
                 std::string* error) const;

    const Plane& planeOf(int subimage) const;

    bool _flattened = false;
    int _compositeX = 0;
    int _compositeY = 0;
    int _compositeWidth = 0;
    int _compositeHeight = 0;
    mutable std::vector<std::unique_ptr<Cache>> _caches;
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
