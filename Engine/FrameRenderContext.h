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

#ifndef Engine_FrameRenderContext_h
#define Engine_FrameRenderContext_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <cstddef>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "Global/GlobalDefines.h"

#include "Engine/DeviceImage.h"
#include "Engine/EngineFwd.h"
#ifdef NATRON_HAVE_GPU
#include "Engine/GpuContext.h"
#endif
#include "Engine/ImageLayerDesc.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The images the tasks of a frame rendered, each kept until every task consuming it has released it.
 **/
class FrameStore {
public:
    struct TaskKey {
        NodePtr node;
        double time;
        ViewIdx view;
        unsigned int mipmapLevel;

        bool operator==(const TaskKey& other) const;
    };

    struct TaskKeyHash {
        std::size_t operator()(const TaskKey& key) const;
    };

    struct Entry {
        // Guards layers and deviceLayers, which a RAM find extends and an eviction empties after the entry is stored.
        std::mutex lock;

        // The images downloaded from deviceLayers join the ones stored in RAM.
        std::map<ImageLayerDesc, ImagePtr> layers;
        std::map<ImageLayerDesc, DeviceImagePtr> deviceLayers;

        // Intersection of the bounds of all layers.
        RectI bounds;

        // A cached image grows to the union of every RoI rendered under its key, while only this part is known to be
        // rendered by the task that stored it.
        RectI renderedRoI;
        std::atomic<int> consumersLeft;

        // The images of layers when the entry was stored, which are the ones counted in _imageRefs.
        std::vector<const Image*> countedImages;

        // Guarded by lock.
        std::size_t deviceBytes;
        std::size_t downloadedBytes;
        bool retired;

        Entry()
            : lock()
            , layers()
            , deviceLayers()
            , bounds()
            , renderedRoI()
            , consumersLeft(0)
            , countedImages()
            , deviceBytes(0)
            , downloadedBytes(0)
            , retired(false)
        {
        }
    };

    FrameStore();

    ~FrameStore();

    FrameStore(const FrameStore&) = delete;
    FrameStore& operator=(const FrameStore&) = delete;

    /**
     * @brief Stores the layers rendered for key over renderedRoI until it has been released consumers times,
     * replacing any entry already stored for key. Nothing is stored when consumers is not positive.
     **/
    void put(const TaskKey& key, std::map<ImageLayerDesc, ImagePtr> layers, const RectI& renderedRoI, int consumers);

    /**
     * @brief Like put(), for layers that live in device memory. A consumer reading RAM images gets them downloaded
     * on the first find.
     **/
    void putDevice(const TaskKey& key, std::map<ImageLayerDesc, DeviceImagePtr> deviceLayers, const RectI& renderedRoI, int consumers);

    /**
     * @brief Appends to out the image of each needed layer, in order, if all of them are stored for key and both
     * their bounds and the RoI rendered for key contain pixelRoI. Returns false and leaves out untouched otherwise.
     * A layer stored only in device memory is downloaded once and kept in RAM for the later finds.
     **/
    bool find(const TaskKey& key, const std::list<ImageLayerDesc>& needed, const RectI& pixelRoI, std::list<ImagePtr>* out) const;

    /**
     * @brief Like find() for device images; never uploads, so layers stored only in RAM are not found.
     **/
    bool findDevice(const TaskKey& key, const std::list<ImageLayerDesc>& needed, const RectI& pixelRoI, std::list<DeviceImagePtr>* out) const;

    /**
     * @brief Moves the device layers of key to RAM and frees their device memory, leaving the consumer count alone.
     * Returns the device bytes freed.
     **/
    std::size_t evictToRam(const TaskKey& key);

    /**
     * @brief Called once by each consumer of key when done reading it; the entry is erased by the last one.
     * Images a consumer still holds stay alive.
     **/
    void release(const TaskKey& key);

    /**
     * @brief The bytes of the stored images the image cache does not already account for, each image counted once
     * however many keys it is stored under.
     **/
    std::size_t bytesInFlight() const;

    /**
     * @brief The RAM bytes held, including the copies downloaded from device images; what bytesInFlight() returns.
     **/
    std::size_t hostBytesInFlight() const;

    /**
     * @brief The device memory held by the stored device images.
     **/
    std::size_t deviceBytesInFlight() const;

    /**
     * @brief How many device images were downloaded to RAM by find() or evictToRam().
     **/
    int downloadCount() const;

    void clear();

private:
    typedef std::unordered_map<TaskKey, std::shared_ptr<Entry>, TaskKeyHash> EntryMap;

    struct ImageRefs {
        int refs = 0;

        // The size counted when the image was first stored, as an image may grow while it is stored.
        std::size_t bytes = 0;
    };

    typedef std::unordered_map<const Image*, ImageRefs> ImageRefsMap;

    void putEntry(const TaskKey& key, const std::shared_ptr<Entry>& entry);

    void addImageRefsLocked(const Entry& entry);

    void removeImageRefsLocked(Entry& entry);

    void addDownloadedBytesLocked(Entry& entry, std::size_t bytes) const;

    mutable std::mutex _entriesMutex;
    EntryMap _entries;

    // An identity task stores the image of its input under its own key too.
    ImageRefsMap _imageRefs;
    std::atomic<std::size_t> _bytesInFlight;
    mutable std::atomic<std::size_t> _downloadedBytes;
    mutable std::atomic<std::size_t> _deviceBytes;
    mutable std::atomic<int> _downloads;
};

/**
 * @brief Everything the tasks rendering one frame share: the frame args of every node the frame reaches, the
 * request pass result and the images passed from task to task. A thread running a task installs it with
 * AppTLS::FrameContextScope, so that each effect it touches gets its frame args in O(1) without the whole graph
 * being installed on that thread.
 **/
class FrameRenderContext {
    FrameRenderContext();

public:
    /**
     * @brief Takes the same arguments as the ParallelRenderArgsSetter walking upstream of treeRoot, and builds the
     * same frame args with ParallelRenderArgsSetter::buildArgsMap().
     **/
    static FrameRenderContextPtr create(double time,
                                        ViewIdx view,
                                        bool isRenderUserInteraction,
                                        bool isSequential,
                                        const AbortableRenderInfoPtr& abortInfo,
                                        const NodePtr& treeRoot,
                                        int textureIndex,
                                        const TimeLine* timeline,
                                        const NodePtr& activeRotoPaintNode,
                                        bool isAnalysis,
                                        bool draftMode,
                                        const RenderStatsPtr& stats);

    /**
     * @brief Shares the frame args setter installed on the calling thread instead of building new ones, so that the
     * tasks and the thread that ran the request pass see the same hashes and requests.
     **/
    static FrameRenderContextPtr createFromSetter(const ParallelRenderArgsSetter& setter,
                                                  const AbortableRenderInfoPtr& abortInfo,
                                                  const RenderStatsPtr& stats,
                                                  double time,
                                                  ViewIdx view);

    ~FrameRenderContext();

    FrameRenderContext(const FrameRenderContext&) = delete;
    FrameRenderContext& operator=(const FrameRenderContext&) = delete;

    /**
     * @brief Points the frame args of every node of request at its node request, like
     * ParallelRenderArgsSetter::updateNodesRequest(). Must be called before any task thread uses this context.
     **/
    void setRequest(const std::shared_ptr<FrameRequestMap>& request);

    const std::shared_ptr<FrameRequestMap>& getRequest() const
    {
        return _request;
    }

    const std::map<NodePtr, ParallelRenderArgsPtr>& getArgsMap() const
    {
        return _argsMap;
    }

    ParallelRenderArgsPtr getArgs(const NodePtr& node) const;

    /**
     * @brief The frame args of the effect owning holder, or null if this frame does not reach it.
     **/
    ParallelRenderArgsPtr getArgsForHolder(const TLSHolderBase* holder) const;

    /**
     * @brief Whether node belongs to the internal tree of a RotoPaint node this frame reaches.
     **/
    bool isRotoPaintTreeNode(const Node* node) const;

    const AbortableRenderInfoPtr& getAbortInfo() const
    {
        return _abortInfo;
    }

    const RenderStatsPtr& getStats() const
    {
        return _stats;
    }

    double getTime() const
    {
        return _time;
    }

    ViewIdx getView() const
    {
        return _view;
    }

    FrameStore& getStore() const
    {
        return _store;
    }

#ifdef NATRON_HAVE_GPU
    /**
     * @brief Whether the consumer of the frame can take its root planes in device memory. Thread safe.
     **/
    void setWantsRootOnDevice(bool wants);

    bool wantsRootOnDevice() const;

    /**
     * @brief Hands the root planes to the consumer in device memory, replacing any set before. They are kept apart
     * from the store, which is cleared before the frame's future finishes. Thread safe.
     **/
    void setRootDevicePlanes(std::map<ImageLayerDesc, GpuDeviceImagePtr> planes);

    /**
     * @brief Returns the planes setRootDevicePlanes() set and leaves none. Thread safe.
     **/
    std::map<ImageLayerDesc, GpuDeviceImagePtr> takeRootDevicePlanes();
#endif

private:
    void indexArgs();

    std::map<NodePtr, ParallelRenderArgsPtr> _argsMap;

    // Effects share the holder of their render clones, so a clone finds the args of its node.
    std::unordered_map<const TLSHolderBase*, ParallelRenderArgsPtr> _argsByHolder;
    std::unordered_set<const Node*> _rotoPaintTreeNodes;
    std::shared_ptr<FrameRequestMap> _request;
    AbortableRenderInfoPtr _abortInfo;
    RenderStatsPtr _stats;
    double _time;
    ViewIdx _view;
    mutable FrameStore _store;
#ifdef NATRON_HAVE_GPU
    std::atomic<bool> _wantsRootOnDevice;
    std::mutex _rootDeviceMutex;
    std::map<ImageLayerDesc, GpuDeviceImagePtr> _rootDevicePlanes;
#endif
};

NATRON_NAMESPACE_EXIT

#endif // Engine_FrameRenderContext_h
