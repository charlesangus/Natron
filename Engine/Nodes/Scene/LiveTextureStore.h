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
#include "Engine/ViewIdx.h"

// Free of USD: the store is filled by Natron renders, which know nothing of pxr.

// The URI scheme live textures are served under: natron://<node>/<layer>?v=<hash>.
#define kLiveTextureScheme "natron"

NATRON_NAMESPACE_ENTER

/**
 * @brief One rendered live texture, immutable once stored: width x height RGBA float pixels, rows
 * bottom-up as Natron renders them, after a small header. The header and the pixels are one
 * buffer, which is what the natron:// asset serves without a copy.
 **/
class LiveTextureBuffer {
public:
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

    // The frame whose image the texture shows at frame: USD holds the sample at or before it.
    double getSourceFrame(double frame) const;

    std::string getUri(double frame) const;

    static std::string makeUri(const std::string& nodeName,
                               const std::string& layer,
                               U64 version);
};

typedef std::vector<LiveTextureSource> LiveTextureSourceList;

/**
 * @brief The rendered live textures by URI, which the natron:// resolver serves. Filled only by
 * renders (prepare()), never by the resolver, and bounded by a byte budget that drops the least
 * recently used. Cleared on cache purge and Free GPU Memory: a texture Storm needs again is
 * rendered again by the consumer that needs it. MT-safe.
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
     * keeps its render state. Does nothing on the main thread, which must never wait on a render. A
     * texture that cannot be rendered stays missing, which Storm shows as its fallback, and its
     * consumer gets a warning. Returns how many it could not render.
     **/
    int prepare(const LiveTextureSourceList& sources,
                double frame,
                ViewIdx view);

    // The reason uri could not be rendered, empty if it was not tried or succeeded.
    std::string getFailure(const std::string& uri) const;

private:
    LiveTextureStore();

    void evictLocked();

    struct Entry {
        LiveTextureBufferConstPtr buffer;
        std::list<std::string>::iterator use;
    };

    mutable std::mutex _mutex;
    std::condition_variable _rendered;
    std::map<std::string, Entry> _entries;
    // Most recently used first.
    std::list<std::string> _uses;
    std::set<std::string> _rendering;
    std::map<std::string, std::string> _failures;
    std::size_t _byteSize = 0;
    std::size_t _byteBudget;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_LiveTextureStore_h
