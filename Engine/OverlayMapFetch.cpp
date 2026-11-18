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

#include "OverlayMapFetch.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <list>
#include <map>
#include <tuple>

#include <QtConcurrentRun> // QtCore on Qt4, QtConcurrent on Qt5

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RenderScale.h"
#include "Engine/TLSHolder.h"
#include "Engine/TimeLine.h"

NATRON_NAMESPACE_ENTER

namespace OverlayMapFetch {
namespace {
    const double kMaxMapSize = 256.;
    const unsigned kMaxMipmapLevel = 20;
    const std::size_t kCacheCapacity = 32;
    // Failures are notified, so retrying at once would redraw and render in a loop.
    const std::chrono::milliseconds kFailedRetryDelay(2000);

    struct Key {
        const EffectInstance* owner;
        int inputNb;
        U64 inputHash;
        double time;
        int view;
        std::string channels;

        bool operator<(const Key& o) const
        {
            return std::tie(owner, inputNb, inputHash, time, view, channels) < std::tie(o.owner, o.inputNb, o.inputHash, o.time, o.view, o.channels);
        }
    };

    struct Job {
        Key key;
        const AppInstance* app = 0;
        const EffectInstance* inputRaw = 0;
        EffectInstanceWPtr owner;
        EffectInstanceWPtr input;
        double time = 0.;
        ViewIdx view;
        std::vector<Channel> channels;
        AbortableRenderInfoPtr abortInfo;
    };

    typedef std::shared_ptr<Job> JobPtr;

    enum EntryStateEnum {
        eEntryStateQueued,
        eEntryStateRunning,
        eEntryStateDone,
        eEntryStateFailed,
    };

    struct Entry {
        EntryStateEnum state = eEntryStateQueued;
        MapImageConstPtr image;
        const AppInstance* app = 0;
        const EffectInstance* input = 0;
        AbortableRenderInfoPtr abortInfo;
        std::chrono::steady_clock::time_point failedAt;
        U64 lastUse = 0;
    };

    // One input of one owner renders at most one map at a time; of the requests made meanwhile
    // only the latest waits, so scrubbing does not queue a render per frame.
    struct Slot {
        JobPtr running;
        JobPtr queued;
    };

    typedef std::pair<const EffectInstance*, int> SlotKey;

    struct Cache {
        std::mutex mutex;
        std::condition_variable idle;
        std::map<Key, Entry> entries;
        std::map<SlotKey, Slot> slots;
        U64 clock = 0;
    };

    Cache&
    cache()
    {
        static Cache instance;

        return instance;
    }

    std::string
    describeChannels(const std::vector<Channel>& channels)
    {
        std::string ret;

        for (const Channel& c : channels) {
            ret += c.layer.getLayerID();
            ret += '.';
            ret += std::to_string(c.index);
            ret += ';';
        }

        return ret;
    }

    bool
    isFinished(const Entry& e)
    {
        return (e.state == eEntryStateDone) || (e.state == eEntryStateFailed);
    }

    // Drops the least recently used finished entries past the capacity. Called with the mutex held.
    void
    evict(Cache& c)
    {
        while (c.entries.size() > kCacheCapacity) {
            std::map<Key, Entry>::iterator oldest = c.entries.end();
            for (std::map<Key, Entry>::iterator it = c.entries.begin(); it != c.entries.end(); ++it) {
                if (isFinished(it->second) && ((oldest == c.entries.end()) || (it->second.lastUse < oldest->second.lastUse))) {
                    oldest = it;
                }
            }
            if (oldest == c.entries.end()) {
                return;
            }
            c.entries.erase(oldest);
        }
    }

    // Erases the entry of key only if it is still the one job created.
    void
    eraseEntryOf(Cache& c,
                 const Job& job)
    {
        std::map<Key, Entry>::iterator found = c.entries.find(job.key);

        if ((found != c.entries.end()) && (found->second.abortInfo == job.abortInfo)) {
            c.entries.erase(found);
        }
    }

    typedef std::function<bool(const EffectInstance* owner, const EffectInstance* input, const AppInstance* app)> Matcher;

    // Aborts the running fetches match selects, drops their queued ones and their cached maps.
    void
    abortMatching(const Matcher& match,
                  bool wait)
    {
        Cache& c = cache();
        std::unique_lock<std::mutex> lock(c.mutex);

        for (std::map<SlotKey, Slot>::iterator it = c.slots.begin(); it != c.slots.end(); ++it) {
            Slot& slot = it->second;
            if (slot.queued && match(slot.queued->key.owner, slot.queued->inputRaw, slot.queued->app)) {
                eraseEntryOf(c, *slot.queued);
                slot.queued.reset();
            }
            if (slot.running && match(slot.running->key.owner, slot.running->inputRaw, slot.running->app)) {
                slot.running->abortInfo->setAborted();
            }
        }
        for (std::map<Key, Entry>::iterator it = c.entries.begin(); it != c.entries.end();) {
            if (match(it->first.owner, it->second.input, it->second.app)) {
                if ((it->second.state == eEntryStateRunning) && it->second.abortInfo) {
                    it->second.abortInfo->setAborted();
                }
                it = c.entries.erase(it);
            } else {
                ++it;
            }
        }
        if (!wait) {
            return;
        }
        c.idle.wait(lock, [&c, &match]() {
            for (std::map<SlotKey, Slot>::const_iterator it = c.slots.begin(); it != c.slots.end(); ++it) {
                const JobPtr& running = it->second.running;
                if (running && match(running->key.owner, running->inputRaw, running->app)) {
                    return false;
                }
            }

            return true;
        });
    }

    void runJob(const JobPtr& job);

    void
    start(const JobPtr& job)
    {
        (void)QtConcurrent::run([job]() { runJob(job); });
    }

    MapImageConstPtr
    render(const EffectInstancePtr& input,
           const NodePtr& requester,
           const AbortableRenderInfoPtr& abortInfo,
           double time,
           ViewIdx view,
           const std::vector<Channel>& channels)
    {
        std::shared_ptr<MapImage> image = std::make_shared<MapImage>();

        image->workerThread = std::this_thread::get_id();
        image->nChannels = (int)channels.size();
        NodePtr inputNode = input->getNode();
        AppInstancePtr app = input->getApp();
        if (!inputNode || !app) {
            return MapImageConstPtr();
        }
        const NodeMetadataConstPtr metadata = input->getMetadata(time, view);
        const double par = (metadata && (metadata->getPixelAspectRatio(-1) > 0.)) ? metadata->getPixelAspectRatio(-1) : 1.;
        RectD rod;
        bool isProjectFormat = false;
        const StatusEnum stat = input->getRegionOfDefinition_public(input->getHash(), time, RenderScale::identity, view, &rod, &isProjectFormat);
        if ((stat == eStatusFailed) || rod.isNull()) {
            return MapImageConstPtr();
        }
        if (rod.isInfinite() && metadata) {
            const RectI format = metadata->getOutputFormat();
            rod = RectD(format.x1 * par, format.y1, format.x2 * par, format.y2);
        }
        if (rod.isNull() || rod.isInfinite()) {
            return MapImageConstPtr();
        }
        const double longest = std::max(rod.width() / par, rod.height());
        unsigned level = 0;
        while ((level < kMaxMipmapLevel) && (longest / (double)(1u << level) > kMaxMapSize)) {
            ++level;
        }
        const RectI window = rod.toPixelEnclosing(level, par);
        image->canonical = rod;
        image->bounds = window;
        image->mipmapLevel = level;
        image->par = par;

        std::list<ImageLayerDesc> layers;
        for (const Channel& c : channels) {
            if (std::find(layers.begin(), layers.end(), c.layer) == layers.end()) {
                layers.push_back(c.layer);
            }
        }
        std::map<ImageLayerDesc, ImagePtr> planes;
        {
            ParallelRenderArgsSetter frameRenderArgs(time,
                                                     view,
                                                     true /*isRenderUserInteraction*/,
                                                     false /*isSequential*/,
                                                     abortInfo,
                                                     requester,
                                                     0 /*textureIndex*/,
                                                     app->getTimeLine().get(),
                                                     NodePtr(),
                                                     true /*isAnalysis*/,
                                                     false /*draftMode*/,
                                                     RenderStatsPtr());
            FrameRequestMap request;
            if (EffectInstance::computeRequestPass(time, view, level, rod, inputNode, request) == eStatusFailed) {
                appPTR->getAppTLS()->cleanupTLSForThread();

                return MapImageConstPtr();
            }
            frameRenderArgs.updateNodesRequest(request);
            EffectInstance::RenderRoIArgs args(time,
                                               RenderScale::fromMipmapLevel(level),
                                               level,
                                               view,
                                               false /*byPassCache*/,
                                               window,
                                               RectD(),
                                               layers,
                                               eImageBitDepthFloat,
                                               false /*calledFromGetImage*/,
                                               0 /*caller*/,
                                               eStorageModeRAM,
                                               time);
            EffectInstance::RenderRoIRetCode code = EffectInstance::eRenderRoIRetCodeFailed;
            // A failed render action throws rather than returning a code.
            try {
                code = input->renderRoI(args, &planes);
            } catch (const std::exception&) {
                code = EffectInstance::eRenderRoIRetCodeFailed;
            }
            if (code != EffectInstance::eRenderRoIRetCodeOk) {
                planes.clear();
            }
        }
        appPTR->getAppTLS()->cleanupTLSForThread();
        if (planes.empty()) {
            return MapImageConstPtr();
        }

        const int width = window.width();
        const int height = window.height();
        image->pixels.assign((std::size_t)width * (std::size_t)height * (std::size_t)std::max(1, image->nChannels), 0.f);
        for (std::size_t c = 0; c < channels.size(); ++c) {
            std::map<ImageLayerDesc, ImagePtr>::const_iterator found = planes.find(channels[c].layer);
            if ((found == planes.end()) || !found->second || (found->second->getBitDepth() != eImageBitDepthFloat)) {
                continue;
            }
            const ImagePtr& plane = found->second;
            const int nComps = (int)plane->getComponentsCount();
            const int index = channels[c].index;
            if ((index < 0) || (index >= nComps)) {
                continue;
            }
            const RectI covered = window.intersect(plane->getBounds());
            if (covered.isNull()) {
                continue;
            }
            Image::ReadAccess access = plane->getReadRights();
            for (int y = covered.y1; y < covered.y2; ++y) {
                const float* row = reinterpret_cast<const float*>(access.pixelAt(covered.x1, y));
                if (!row) {
                    continue;
                }
                float* dst = &image->pixels[((std::size_t)(y - window.y1) * width + (covered.x1 - window.x1)) * image->nChannels + c];
                for (int x = covered.x1; x < covered.x2; ++x, row += nComps, dst += image->nChannels) {
                    *dst = row[index];
                }
            }
        }

        return image;
    } // render

    void
    runJob(const JobPtr& job)
    {
        MapImageConstPtr image;
        {
            const EffectInstancePtr input = job->input.lock();
            const EffectInstancePtr owner = job->owner.lock();
            const NodePtr requester = owner ? owner->getNode() : NodePtr();
            if (input && requester && requester->isActivated() && !job->abortInfo->isAborted()) {
                image = render(input, requester, job->abortInfo, job->time, job->view, job->channels);
            }
        }
        const bool aborted = job->abortInfo->isAborted();
        Cache& c = cache();
        {
            std::lock_guard<std::mutex> lock(c.mutex);
            std::map<Key, Entry>::iterator found = c.entries.find(job->key);
            if ((found != c.entries.end()) && (found->second.abortInfo == job->abortInfo)) {
                if (aborted) {
                    c.entries.erase(found);
                } else {
                    found->second.image = image;
                    found->second.state = image ? eEntryStateDone : eEntryStateFailed;
                    found->second.failedAt = std::chrono::steady_clock::now();
                }
            }
        }
        if (!aborted) {
            const EffectInstancePtr owner = job->owner.lock();
            const NodePtr node = owner ? owner->getNode() : NodePtr();
            if (node && node->isActivated()) {
                node->notifyOverlayMapChanged();
            }
        }
        // The slot is released only after the notification, so a waiter sees every listener told.
        JobPtr next;
        {
            std::lock_guard<std::mutex> lock(c.mutex);
            std::map<SlotKey, Slot>::iterator slot = c.slots.find(SlotKey(job->key.owner, job->key.inputNb));
            if ((slot != c.slots.end()) && (slot->second.running == job)) {
                next = slot->second.queued;
                slot->second.queued.reset();
                slot->second.running = next;
                if (next) {
                    std::map<Key, Entry>::iterator found = c.entries.find(next->key);
                    if ((found != c.entries.end()) && (found->second.abortInfo == next->abortInfo)) {
                        found->second.state = eEntryStateRunning;
                    }
                } else {
                    c.slots.erase(slot);
                }
            }
            evict(c);
        }
        c.idle.notify_all();
        if (next) {
            start(next);
        }
    } // runJob
} // anon namespace

const float*
MapImage::pixel(int x,
                int y) const
{
    if (empty() || (x < bounds.x1) || (x >= bounds.x2) || (y < bounds.y1) || (y >= bounds.y2)) {
        return 0;
    }

    return &pixels[((std::size_t)(y - bounds.y1) * bounds.width() + (x - bounds.x1)) * nChannels];
}

Point
MapImage::pixelCentre(int x,
                      int y) const
{
    const double scale = (double)(1u << mipmapLevel);
    Point p;

    p.x = (x + 0.5) * scale * par;
    p.y = (y + 0.5) * scale;

    return p;
}

bool
MapImage::sample(double x,
                 double y,
                 float* values,
                 bool stretchEdges) const
{
    if (empty() || !std::isfinite(x) || !std::isfinite(y)) {
        return false;
    }
    if (stretchEdges) {
        x = std::min(canonical.x2, std::max(canonical.x1, x));
        y = std::min(canonical.y2, std::max(canonical.y1, y));
    } else if ((x < canonical.x1) || (x > canonical.x2) || (y < canonical.y1) || (y > canonical.y2)) {
        return false;
    }
    const double scale = (double)(1u << mipmapLevel);
    const double px = x / (scale * par) - 0.5;
    const double py = y / scale - 0.5;
    const double clampedX = std::min((double)(bounds.x2 - 1), std::max((double)bounds.x1, px));
    const double clampedY = std::min((double)(bounds.y2 - 1), std::max((double)bounds.y1, py));
    const int x0 = (int)std::floor(clampedX);
    const int y0 = (int)std::floor(clampedY);
    const int x1 = std::min(x0 + 1, bounds.x2 - 1);
    const int y1 = std::min(y0 + 1, bounds.y2 - 1);
    const double fx = clampedX - x0;
    const double fy = clampedY - y0;
    const float* p00 = pixel(x0, y0);
    const float* p10 = pixel(x1, y0);
    const float* p01 = pixel(x0, y1);
    const float* p11 = pixel(x1, y1);
    if (!p00 || !p10 || !p01 || !p11) {
        return false;
    }
    for (int c = 0; c < nChannels; ++c) {
        const double bottom = p00[c] + fx * (p10[c] - p00[c]);
        const double top = p01[c] + fx * (p11[c] - p01[c]);
        values[c] = (float)(bottom + fy * (top - bottom));
    }

    return true;
}

std::shared_ptr<const OverlayPointMap::SeedGrid>
MapImage::seedGrid(U64 key,
                   const std::function<std::shared_ptr<const OverlayPointMap::SeedGrid>()>& build) const
{
    std::lock_guard<std::mutex> lock(_seedMutex);

    if (!_seed || (_seedKey != key)) {
        _seed = build();
        _seedKey = key;
    }

    return _seed;
}

MapImageConstPtr
fetch(const EffectInstancePtr& owner,
      int inputNb,
      double time,
      ViewIdx view,
      const std::vector<Channel>& channels,
      OverlayPointMap::Result* result)
{
    const EffectInstancePtr input = owner ? owner->getInput(inputNb) : EffectInstancePtr();
    const NodePtr ownerNode = owner ? owner->getNode() : NodePtr();
    const AppInstancePtr app = owner ? owner->getApp() : AppInstancePtr();

    if (!input || !ownerNode || !ownerNode->isActivated() || !app || !app->getProject() || app->getProject()->isProjectClosing()) {
        *result = OverlayPointMap::eUnmappable;

        return MapImageConstPtr();
    }
    Key key;
    key.owner = owner.get();
    key.inputNb = inputNb;
    key.inputHash = input->getHash();
    key.time = time;
    key.view = (int)view;
    key.channels = describeChannels(channels);

    Cache& c = cache();
    JobPtr launch;
    {
        std::lock_guard<std::mutex> lock(c.mutex);
        std::map<Key, Entry>::iterator found = c.entries.find(key);
        if (found != c.entries.end()) {
            Entry& entry = found->second;
            entry.lastUse = ++c.clock;
            switch (entry.state) {
            case eEntryStateDone:
                *result = OverlayPointMap::eMapped;

                return entry.image;
            case eEntryStateFailed:
                if (std::chrono::steady_clock::now() - entry.failedAt < kFailedRetryDelay) {
                    *result = OverlayPointMap::eUnmappable;

                    return MapImageConstPtr();
                }
                c.entries.erase(found);
                break;
            case eEntryStateQueued:
            case eEntryStateRunning:
                *result = OverlayPointMap::ePending;

                return MapImageConstPtr();
            }
        }

        JobPtr job = std::make_shared<Job>();
        job->key = key;
        job->app = app.get();
        job->inputRaw = input.get();
        job->owner = owner;
        job->input = input;
        job->time = time;
        job->view = view;
        job->channels = channels;
        job->abortInfo = AbortableRenderInfo::create(true, 0);

        Entry& entry = c.entries[key];
        entry.app = job->app;
        entry.input = job->inputRaw;
        entry.abortInfo = job->abortInfo;
        entry.lastUse = ++c.clock;
        Slot& slot = c.slots[SlotKey(key.owner, inputNb)];
        if (slot.running) {
            if (slot.queued) {
                eraseEntryOf(c, *slot.queued);
            }
            slot.queued = job;
            entry.state = eEntryStateQueued;
        } else {
            slot.running = job;
            entry.state = eEntryStateRunning;
            launch = job;
        }
        evict(c);
    }
    *result = OverlayPointMap::ePending;
    if (launch) {
        start(launch);
    }

    return MapImageConstPtr();
} // fetch

void
abortFetches(const EffectInstance* effect,
             bool wait)
{
    if (!effect) {
        return;
    }
    const Matcher match = [effect](const EffectInstance* owner, const EffectInstance* input, const AppInstance*) {
        return (owner == effect) || (input == effect);
    };

    abortMatching(match, wait);
}

void
abortAppFetches(const AppInstance* app,
                bool wait)
{
    const Matcher match = [app](const EffectInstance*, const EffectInstance*, const AppInstance* jobApp) {
        return jobApp == app;
    };

    abortMatching(match, wait);
}

void
abortAllFetches(bool wait)
{
    const Matcher match = [](const EffectInstance*, const EffectInstance*, const AppInstance*) {
        return true;
    };

    abortMatching(match, wait);
}

bool
hasFetches(const EffectInstance* effect)
{
    Cache& c = cache();
    std::lock_guard<std::mutex> lock(c.mutex);

    for (std::map<SlotKey, Slot>::const_iterator it = c.slots.begin(); it != c.slots.end(); ++it) {
        const JobPtr& running = it->second.running;
        if (running && ((running->key.owner == effect) || (running->inputRaw == effect))) {
            return true;
        }
    }

    return false;
}

bool
waitForFetches(int timeoutMs)
{
    Cache& c = cache();
    std::unique_lock<std::mutex> lock(c.mutex);

    return c.idle.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&c]() { return c.slots.empty(); });
}

void
clearCache()
{
    Cache& c = cache();
    std::lock_guard<std::mutex> lock(c.mutex);

    for (std::map<Key, Entry>::iterator it = c.entries.begin(); it != c.entries.end();) {
        if (isFinished(it->second)) {
            it = c.entries.erase(it);
        } else {
            ++it;
        }
    }
}
} // namespace OverlayMapFetch

NATRON_NAMESPACE_EXIT
