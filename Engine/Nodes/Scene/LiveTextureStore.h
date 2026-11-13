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

#ifndef Engine_Nodes_Scene_LiveTextureStore_h
#define Engine_Nodes_Scene_LiveTextureStore_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/GlobalDefines.h"
#include "Global/Macros.h"

#include <condition_variable>
#include <cstddef>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

// Free of USD: the store is filled by Natron renders, which know nothing of pxr.

// The URI scheme live textures are served under: natron://<node>/<layer>?v=<hash>, and for a UDIM
// set natron://<node>/<layer>.<UDIM>?v=<hash>, whose tiles are natron://<node>/<layer>.1001?v=<hash>
// and so on. USD and Storm find a set's tiles by putting each tile number from 1001 to 1100 in
// place of the token and resolving the result, so the query after it reaches every tile URI.
#define kLiveTextureScheme "natron"
#define kLiveTextureUdimToken "<UDIM>"

NATRON_NAMESPACE_ENTER

/**
 * @brief One rendered live texture, immutable once stored: width x height RGBA float pixels, rows
 * bottom-up as Natron renders them, after a small header. The colour is not premultiplied, as USD
 * textures are not: where an RGBA image's alpha is 0, within kTransparentFillBand texels of one
 * with an alpha, the colour is that of the nearest texels with an alpha, so filtering across its
 * edges blends in no black; farther out it is as rendered. The header and the pixels are one
 * buffer, which is what the natron:// asset serves without a copy.
 **/
class LiveTextureBuffer {
public:
    /*
     * Bilinear filtering at mip level k reaches 2^(k+1) - 1 texels past the last one with an alpha,
     * so 8 keeps levels 0 to 2, up to 4x minification, free of black. Beyond that an edge is under
     * a pixel on screen, and a wider band would smear colour across uncovered areas that show when
     * the alpha is not used.
     */
    static constexpr int kTransparentFillBand = 8;

    // Null when the size is not positive or does not fit in memory.
    static std::shared_ptr<LiveTextureBuffer> create(int width, int height);

    int getWidth() const
    {
        return _width;
    }

    int getHeight() const
    {
        return _height;
    }

    // Row y, 0 being the bottom one, of 4 * width floats.
    float* getRow(int y);
    const float* getRow(int y) const;

    // The header and pixels, as the asset serves them.
    const std::shared_ptr<const char>& getBytes() const
    {
        return _bytes;
    }

    std::size_t getByteSize() const
    {
        return _byteSize;
    }

    /**
     * @brief Reads back what getBytes() holds: false, leaving the outputs untouched, for bytes
     * that are not a live texture or are truncated.
     **/
    static bool parse(const char* bytes,
                      std::size_t size,
                      int* width,
                      int* height,
                      const float** pixels);

private:
    LiveTextureBuffer() = default;

    std::shared_ptr<const char> _bytes;
    std::size_t _byteSize = 0;
    float* _pixels = nullptr;
    int _width = 0;
    int _height = 0;
};

typedef std::shared_ptr<const LiveTextureBuffer> LiveTextureBufferConstPtr;

/**
 * @brief Where a live texture lies, in the full-scale pixel coordinates of its source's image: the
 * texture holds window, and texture coordinates 0 to 1 span format, so the overscan around the
 * format is read at coordinates outside 0 to 1. Null when not known.
 **/
struct LiveTextureWindow {
    RectI format;
    RectI window;

    bool isNull() const
    {
        return format.isNull() || window.isNull();
    }

    /**
     * @brief The scale and translation that take a texture coordinate over format to one over
     * window: (1, 1) and (0, 0) when null.
     **/
    void getUvTransform(double scale[2],
                        double translation[2]) const;
};

/**
 * @brief An image input a scene layer authors as a natron:// texture: which node's image, at
 * which frames, under which URI. Recorded in the layer's payload entry, so consumers find what
 * they must render before Hydra reads the stage, without traversing it.
 **/
struct LiveTextureSource {
    NodeWPtr node;
    // The node whose layer authors the texture, which warns when it cannot be rendered.
    NodeWPtr consumer;
    // The source's fully qualified script name and the layer read, as the URI names them.
    std::string nodeName;
    std::string layer;
    U64 contentHash = 0;
    // Whether the image changes over time; a static one has one URI at every frame.
    bool varying = false;
    // The frames the layer authors samples at, as SceneNodeAuthoring::getSampleFrames does.
    double firstFrame = 0.;
    double lastFrame = 0.;
    // Filled by SceneNodeAuthoring::recordLiveTexture: one window per frame from firstFrame when
    // varying, else one. A texture is rendered over the window its layer authored coordinates for.
    std::vector<LiveTextureWindow> windows;
    // The UDIM tiles the source declares (kNatronMetadataKeyUdimTiles), sorted and without
    // duplicates; empty for a single texture. Each tile is rendered over the format the source has
    // under that tile, without the overscan, and windows are not read.
    std::vector<int> tiles;
    // Filled by SceneNodeAuthoring::recordLiveTexture for a varying UDIM source: the tiles declared
    // at each frame from firstFrame, as windows are, empty where a frame declares none or does not
    // parse. Otherwise empty, and tiles holds the set at every frame.
    std::vector<std::vector<int>> frameTiles;

    // The last tile USD and Storm look for: tiles above it are rendered but never read.
    static constexpr int kLastUsdUdimTile = 1100;

    // The frame whose image the texture shows at frame: USD holds the sample at or before it.
    double getSourceFrame(double frame) const;

    bool isUdim() const
    {
        return !tiles.empty();
    }

    // The texture's URI at frame; a UDIM set's holds kLiveTextureUdimToken where its tiles' differ.
    std::string getUri(double frame) const;

    // The URI of tile of a UDIM set at frame: getUri(frame) with the tile number for the token.
    std::string getTileUri(double frame,
                           int tile) const;

    // The tiles of a UDIM set at frame: those recorded for it in frameTiles, else tiles.
    const std::vector<int>& getTiles(double frame) const;

    // The tiles at frame above kLastUsdUdimTile, which nothing reading the set through USD sees.
    std::vector<int> getTilesBeyondUsd(double frame) const;

    // The warning naming getTilesBeyondUsd() for the set at frame, empty when there are none.
    std::string getTilesBeyondUsdWarning(double frame) const;

    // The window recorded for frame, null when none was.
    LiveTextureWindow getWindow(double frame) const;

    static std::string makeUri(const std::string& nodeName,
                               const std::string& layer,
                               U64 version);

private:
    U64 getVersion(double frame) const;

    // The index of frame in windows and frameTiles: 0 for a source that does not vary.
    double getRecordedIndex(double frame) const;
};

typedef std::vector<LiveTextureSource> LiveTextureSourceList;

/**
 * @brief The rendered live textures by URI, which the natron:// resolver serves. Filled only by
 * renders (prepare()), never by the resolver, and bounded by a byte budget that drops the least
 * recently used, except the textures of a prepare() under way and of the last one finished, which
 * stay even over budget. Cleared on cache purge and Free GPU Memory: a texture Storm needs again is
 * rendered again by the consumer that needs it. A texture is always of the project's first view:
 * neither its URI nor the layers authoring it are per view. One buffer may be stored under several
 * URIs, the same UDIM tile under successive versions of its set, and counts against the budget
 * once. MT-safe.
 **/
class LiveTextureStore {
public:
    static LiveTextureStore& instance();

    void insert(const std::string& uri,
                const LiveTextureBufferConstPtr& buffer);

    // Null when uri is not stored. Marks it used.
    LiveTextureBufferConstPtr find(const std::string& uri);

    bool contains(const std::string& uri) const;

    void clear();

    void setByteBudget(std::size_t bytes);
    std::size_t getByteBudget() const;
    std::size_t getByteSize() const;

    /**
     * @brief Renders the texture of every source in sources at frame that the store lacks, and
     * waits for them: each on a thread of its own, so a caller in the middle of a render (USDRender)
     * keeps its render state. Renders nothing on the main thread, which must never wait on a render.
     * A texture that cannot be rendered, there or elsewhere, stays missing, which Storm shows as its
     * fallback, and its consumer gets a warning.
     *
     * A UDIM source has a texture per tile, each rendered under that tile's render context
     * (RenderContextName::udimTile()). A tile whose udimTileVersion() at the source frame is held
     * under an earlier URI of its set is stored under the new one too, without rendering: an edit
     * that changes some tiles renders only those. A tile that cannot be rendered is stored as
     * magenta, so the set still finds it and shows what is missing, and is tried again on the next
     * call; its consumer gets a warning naming the tile. A stand-in whose tile's format is not
     * known takes another tile's format, else the source's outside any tile. Tiles above
     * LiveTextureSource::kLastUsdUdimTile are rendered and warned about. A texture that fails is
     * tried once per call, however many sources name it.
     *
     * Returns how many textures, counting each tile, it could not render.
     **/
    int prepare(const LiveTextureSourceList& sources,
                double frame);

    /**
     * @brief The reason uri could not be rendered, empty if it was not tried or succeeded. For the
     * URI of a UDIM set, its tiles' reasons.
     **/
    std::string getFailure(const std::string& uri) const;

    /**
     * @brief How many times prepare() has rendered each tile, by tile number; 0 stands for a
     * texture that is not a UDIM tile. Tiles reused from an earlier URI or already stored count
     * nothing.
     **/
    std::map<int, std::size_t> getRenderCounts() const;

private:
    LiveTextureStore();

    struct Entry {
        LiveTextureBufferConstPtr buffer;
        std::list<std::string>::iterator use;
        // Stands in for a texture that could not be rendered.
        bool fallback = false;
    };

    void evictLocked();
    void insertLocked(const std::string& uri,
                      const LiveTextureBufferConstPtr& buffer,
                      bool fallback);
    void eraseLocked(std::map<std::string, Entry>::iterator entry);
    void setFailureLocked(const std::string& uri,
                          const std::string& failure);

    mutable std::mutex _mutex;
    std::condition_variable _rendered;
    std::map<std::string, Entry> _entries;
    // Most recently used first.
    std::list<std::string> _uses;
    std::set<std::string> _rendering;
    std::map<std::string, std::string> _failures;
    std::map<int, std::size_t> _renderCounts;
    // The URIs of the prepare() calls under way, with how many hold each.
    std::map<std::string, int> _pinned;
    std::set<std::string> _lastPrepared;
    // How many URIs hold each buffer, which counts in _byteSize once.
    std::map<const LiveTextureBuffer*, int> _bufferUses;
    // The rendered UDIM tiles by what they show (source, layer, tile, frame and tile version), for
    // as long as a URI holds them.
    std::map<U64, std::weak_ptr<const LiveTextureBuffer>> _tiles;
    std::size_t _byteSize = 0;
    std::size_t _byteBudget;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_LiveTextureStore_h
