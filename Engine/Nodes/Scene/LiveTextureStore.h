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
#include <functional>
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

// The URI scheme live textures are served under: natron://<node>/<layer>?v=<hash>.
#define kLiveTextureScheme "natron"

NATRON_NAMESPACE_ENTER

// What a live texture that exists but cannot be served shows in every renderer, bright so that it is
// seen: linear RGBA.
constexpr float kLiveTextureMissingColour[4] = { 1.f, 0.f, 1.f, 1.f };

/**
 * @brief One rendered live texture, immutable once stored: width x height RGBA float pixels, rows
 * bottom-up as Natron renders them, after a small header, as LiveTextureFile.h lays them out. The colour is not premultiplied, as USD
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

    // The file render delegates read this from, with its mip chain; empty while in plain memory.
    const std::string& getFilePath() const
    {
        return _filePath;
    }

    /**
     * @brief Wraps bytes mapped from filePath, read-only, whose owner removes the file when the last
     * reference to them goes. The chain's lower levels follow the pixels in the same bytes.
     **/
    static std::shared_ptr<const LiveTextureBuffer> fromFile(const std::shared_ptr<const char>& bytes,
                                                             std::size_t byteSize,
                                                             int width,
                                                             int height,
                                                             const std::string& filePath);

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
    std::string _filePath;
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

    // The frame whose image the texture shows at frame: USD holds the sample at or before it.
    double getSourceFrame(double frame) const;

    std::string getUri(double frame) const;

    // The window recorded for frame, null when none was.
    LiveTextureWindow getWindow(double frame) const;

    static std::string makeUri(const std::string& nodeName,
                               const std::string& layer,
                               U64 version);
};

typedef std::vector<LiveTextureSource> LiveTextureSourceList;

/**
 * @brief The rendered live textures by URI, which the natron:// resolver serves. Filled only by
 * renders (prepare()), never by the resolver, and bounded by a byte budget that drops the least
 * recently used, except the textures of a prepare() under way and of the last one finished, which
 * stay even over budget. Cleared on cache purge and Free GPU Memory: a texture Storm needs again is
 * rendered again by the consumer that needs it. A texture is always of the project's first view:
 * neither its URI nor the layers authoring it are per view. MT-safe.
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
     * A texture that cannot be rendered, there or elsewhere, stays missing, which every renderer
     * shows as kLiveTextureMissingColour, and its consumer gets a warning. Returns how many it could not render.
     *
     * Once aborted, called on the calling thread only, returns true, the texture being rendered is
     * stopped and prepare() returns without the rest: what it did not finish is not a failure and
     * warns no one, and a later prepare() renders it.
     **/
    int prepare(const LiveTextureSourceList& sources,
                double frame,
                const std::function<bool()>& aborted = std::function<bool()>());

    /**
     * @brief The texture uri as a file a render delegate can read, with its mip chain, written the
     * first time a delegate asks so a session that only uses Storm never touches shared memory. From
     * then on the store holds the file's mapping in place of its own copy. Holding the result keeps
     * the file past eviction and clear(), which is what lets a renderer close and reopen it
     * mid-render. Blocks only callers asking for the same uri while the file is written. Null when
     * there is no such file, with the reason in getFailure() and the consumers warned; a texture that
     * did not fit is not tried again until it is rendered again.
     *
     * A file that does not fit because shared memory is full is a failure of its own, getNoRoom(),
     * not a missing texture: the files already held stay, so a render under way keeps the image it
     * reads, and the caller fails its render. prepare() and insert() of the uri clear it, which is
     * what lets a later render try again.
     **/
    LiveTextureBufferConstPtr exportToSharedMemory(const std::string& uri);

    /**
     * @brief While one lives, exportToSharedMemory() on this thread gives up once aborted returns
     * true, whether it is waiting for another caller's export or writing the file: it returns null,
     * leaving no file and recording no failure. Delegates filter their materials on the thread that
     * renders, so a render sets one up for as long as it can be cancelled. aborted is called from
     * TBB's threads too.
     **/
    class ExportAbortScope {
    public:
        explicit ExportAbortScope(const std::function<bool()>& aborted);
        ~ExportAbortScope();

        ExportAbortScope(const ExportAbortScope&) = delete;
        ExportAbortScope& operator=(const ExportAbortScope&) = delete;

    private:
        std::function<bool()> _outer;
    };

    // The name of the node whose image a file from exportToSharedMemory() holds, empty for any other.
    std::string getSourceOfFile(const std::string& path) const;

    // Why uri's image could not be exported for lack of room in shared memory, naming the texture
    // and the space needed and free; empty if it could.
    std::string getNoRoom(const std::string& uri) const;

    // The same for any texture the latest prepare() calls asked for.
    std::string getAnyNoRoom() const;

    // The reason uri could not be rendered or exported, empty if it was not tried or succeeded.
    std::string getFailure(const std::string& uri) const;

private:
    LiveTextureStore();

    void evictLocked();
    void recordFailureLocked(const std::string& uri,
                             const std::string& failure);
    void addConsumerLocked(const std::string& uri,
                           const NodeWPtr& consumer);
    std::vector<LiveTextureBufferConstPtr> releaseUnusedFilesLocked(const std::string& keptUri,
                                                                    std::size_t bytes);

    struct Entry {
        LiveTextureBufferConstPtr buffer;
        std::list<std::string>::iterator use;
        // Whom to warn if a delegate's export fails, long after prepare() returned.
        std::vector<NodeWPtr> consumers;
    };

    mutable std::mutex _mutex;
    std::condition_variable _rendered;
    std::map<std::string, Entry> _entries;
    // Most recently used first.
    std::list<std::string> _uses;
    std::set<std::string> _rendering;
    std::set<std::string> _exporting;
    // Files alive through a delegate's reference after their entry left the store: a texture
    // rendered again must not write a second file under the same URI.
    std::map<std::string, std::weak_ptr<const LiveTextureBuffer>> _exported;
    std::map<std::string, std::string> _failures;
    std::map<std::string, std::string> _noRoom;
    // The URIs of the prepare() calls under way, with how many hold each.
    std::map<std::string, int> _pinned;
    std::set<std::string> _lastPrepared;
    std::size_t _byteSize = 0;
    std::size_t _byteBudget;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_LiveTextureStore_h
