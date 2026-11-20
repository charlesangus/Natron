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

#include "SceneNodeStageSource.h"

#include <algorithm>
#include <deque>
#include <map>
#include <set>
#include <string>

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Nodes/Scene/SceneTeardown.h"
#include "Engine/Nodes/Scene/SceneWriteBarrier.h"
#include "Engine/TLSHolder.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

namespace {
bool
evaluateNode(const NodeWPtr& weakNode,
             const SceneEvaluationContext& context,
             double time,
             const AbortableRenderInfoPtr& abortInfo,
             ScenePayload* payload,
             std::string* error)
{
    NodePtr node = weakNode.lock();
    EffectInstancePtr effect = node ? node->getEffectInstance() : EffectInstancePtr();
    bool ok = false;

    if (effect && node->isActivated() && effect->producesSceneData()) {
        ok = (effect->evaluateScene(EffectInstance::EvaluateSceneArgs(time, ViewIdx(0), context, abortInfo), payload, error) == EffectInstance::eRenderRoIRetCodeOk);
    } else {
        *error = "The node puts out no scene.";
    }
    node.reset();
    effect.reset();
    appPTR->getAppTLS()->cleanupTLSForThread();

    return ok;
}

struct Reaper {
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::shared_ptr<SceneNodeStageSource>> queue;
    bool busy = false;
    std::thread thread;

    void run()
    {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            changed.wait(lock, [this]() { return !queue.empty(); });
            std::shared_ptr<SceneNodeStageSource> source = queue.front();
            queue.pop_front();
            busy = true;
            lock.unlock();
            source->stop();
            // Not destroyed here: joinReaped() waits for this loop, and the main thread for
            // joinReaped(), but only for the worker to stop, not for its stage to close.
            SceneTeardown::release(std::move(source));
            lock.lock();
            busy = false;
            changed.notify_all();
        }
    }
};

// Never destroyed: a source may still be handed over while statics are torn down.
Reaper&
reaper()
{
    static Reaper* instance = new Reaper;

    return *instance;
}

struct LiveSources {
    std::mutex mutex;
    std::condition_variable changed;
    std::set<SceneNodeStageSource*> sources;
};

// Never destroyed, as reaper().
LiveSources&
liveSources()
{
    static LiveSources* instance = new LiveSources;

    return *instance;
}
} // namespace

SceneNodeStageSource::SceneNodeStageSource(const NodePtr& node,
                                           QObject* context,
                                           const SyncedFunc& synced,
                                           bool loadPayloads)
    : _node(node)
    , _context(context)
    , _abortInfo(AbortableRenderInfo::create(true, 0))
    , _synced(synced)
    , _loadPayloads(loadPayloads)
    , _contextId(SceneEvaluationContext::makeId())
    , _loadSet(loadPayloads ? SceneLoadSetPtr() : std::make_shared<const SceneLoadSet>(SceneLoadSet::none()))
{
    {
        LiveSources& live = liveSources();
        std::lock_guard<std::mutex> lock(live.mutex);
        live.sources.insert(this);
    }
    _worker = std::thread([this]() { run(); });
}

SceneNodeStageSource::~SceneNodeStageSource()
{
    stop();
    {
        LiveSources& live = liveSources();
        std::lock_guard<std::mutex> lock(live.mutex);
        live.sources.erase(this);
    }
    liveSources().changed.notify_all();
    EffectInstance::retireSceneContextId(_contextId);
    std::unique_ptr<SceneStage> stage;
    {
        SceneWriteBarrier::ReadScope readScope;
        std::lock_guard<std::mutex> lock(_stageMutex);
        stage = std::move(_stage);
    }
    // Whoever holds the source last may be the main thread, from a timer or a model swap.
    if (stage) {
        SceneTeardown::release(std::move(stage));
    }
}

void
SceneNodeStageSource::requestStop()
{
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        _quit = true;
    }
    {
        std::lock_guard<std::mutex> lock(_contextMutex);
        _context = nullptr;
    }
    if (!_stopRequested.exchange(true)) {
        _abortInfo->setAborted();
    }
    _queueChanged.notify_all();
}

bool
SceneNodeStageSource::isFinished() const
{
    return _finished;
}

void
SceneNodeStageSource::stop()
{
    requestStop();
    if (_worker.joinable()) {
        _worker.join();
    }
}

void
SceneNodeStageSource::reap(const std::vector<std::shared_ptr<SceneNodeStageSource>>& sources)
{
    Reaper& r = reaper();
    {
        std::lock_guard<std::mutex> lock(r.mutex);
        for (const std::shared_ptr<SceneNodeStageSource>& source : sources) {
            if (source) {
                source->requestStop();
                r.queue.push_back(source);
            }
        }
        if (!r.thread.joinable()) {
            r.thread = std::thread([&r]() { r.run(); });
        }
    }
    r.changed.notify_all();
}

void
SceneNodeStageSource::joinReaped()
{
    Reaper& r = reaper();
    std::unique_lock<std::mutex> lock(r.mutex);

    r.changed.wait(lock, [&r]() { return r.queue.empty() && !r.busy; });
}

void
SceneNodeStageSource::stopAll()
{
    LiveSources& live = liveSources();
    std::unique_lock<std::mutex> lock(live.mutex);

    for (SceneNodeStageSource* source : live.sources) {
        source->requestStop();
    }
    live.changed.wait(lock, [&live]() {
        for (SceneNodeStageSource* source : live.sources) {
            if (!source->isFinished()) {
                return false;
            }
        }

        return true;
    });
}

void
SceneNodeStageSource::postToContext(const std::function<void()>& call)
{
    // Held across the post, so that requestStop(), on context's thread, returns only once no
    // post can still name it.
    std::lock_guard<std::mutex> lock(_contextMutex);

    if (_context) {
        QMetaObject::invokeMethod(_context, call, Qt::QueuedConnection);
    }
}

void
SceneNodeStageSource::readStage(const std::function<void(const SceneStage* stage)>& read) const
{
    SceneWriteBarrier::ReadScope readScope;
    std::lock_guard<std::mutex> lock(_stageMutex);

    read((_shown && _stage && _stage->isValid()) ? _stage.get() : nullptr);
}

bool
SceneNodeStageSource::tryReadStage(const std::function<void(const SceneStage* stage)>& read) const
{
    SceneWriteBarrier::ReadScope readScope;
    std::unique_lock<std::mutex> lock(_stageMutex, std::try_to_lock);

    if (!lock.owns_lock()) {
        return false;
    }
    read((_shown && _stage && _stage->isValid()) ? _stage.get() : nullptr);

    return true;
}

U64
SceneNodeStageSource::getEditCount() const
{
    return _editCount;
}

NodePtr
SceneNodeStageSource::getNode() const
{
    return _node.lock();
}

void
SceneNodeStageSource::request(double time)
{
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        _pending = true;
        _pendingTime = time;
    }
    _queueChanged.notify_one();
}

U64
SceneNodeStageSource::getComposeCount() const
{
    return _composeCount;
}

void
SceneNodeStageSource::setLoadsChanged(const std::function<void()>& loadsChanged)
{
    _loadsChanged = loadsChanged;
}

void
SceneNodeStageSource::requestLoad(const QStringList& paths,
                                  bool withDescendants)
{
    LoadRequest request;

    request.paths = paths;
    request.withDescendants = withDescendants;
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        _loadRequests.push_back(request);
    }
    _queueChanged.notify_one();
}

void
SceneNodeStageSource::requestUnload(const QStringList& paths)
{
    LoadRequest request;

    request.paths = paths;
    request.load = false;
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        _loadRequests.push_back(request);
    }
    _queueChanged.notify_one();
}

void
SceneNodeStageSource::requestLoadRules(const SceneLoadRules& rules)
{
    std::vector<LoadRequest> requests;

    for (const SceneDrawLoadRequest& draw : sceneLoadRequestsFor(rules)) {
        LoadRequest load;
        load.paths << QString::fromStdString(draw.path);
        load.withDescendants = draw.withDescendants;
        load.excluded = draw.excluded;
        requests.push_back(load);
    }
    if (requests.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        _loadRequests.insert(_loadRequests.end(), requests.begin(), requests.end());
    }
    _queueChanged.notify_one();
}

void
SceneNodeStageSource::serviceLoads(const std::vector<LoadRequest>& requests)
{
    // Only this worker creates the stage, so reading the pointer needs no lock.
    if (!_stage || (requests.empty() && !_stage->hasPendingLoads())) {
        return;
    }
    const auto toSdfPaths = [](const QStringList& strings) {
        PXR_NS::SdfPathVector paths;
        for (const QString& path : strings) {
            const std::string pathString = path.toStdString();
            if (PXR_NS::SdfPath::IsValidPathString(pathString)) {
                paths.push_back(PXR_NS::SdfPath(pathString));
            }
        }

        return paths;
    };
    // A batch skips a path whose prim the stage does not compose yet, as one inside a payload the
    // same batch loads: each request waits for a pass after those of the loads above it.
    std::set<PXR_NS::SdfPath> loadPaths;
    for (const LoadRequest& request : requests) {
        if (request.load) {
            for (const PXR_NS::SdfPath& path : toSdfPaths(request.paths)) {
                loadPaths.insert(path);
            }
        }
    }
    std::map<std::size_t, std::vector<std::size_t>> passes;
    for (std::size_t i = 0; i < requests.size(); ++i) {
        std::size_t depth = 0;
        for (const PXR_NS::SdfPath& path : toSdfPaths(requests[i].paths)) {
            std::size_t above = 0;
            for (PXR_NS::SdfPath ancestor = path.GetParentPath(); !ancestor.IsEmpty(); ancestor = ancestor.GetParentPath()) {
                above += loadPaths.count(ancestor);
            }
            depth = std::max(depth, above);
        }
        passes[depth].push_back(i);
    }
    if (passes.empty()) {
        passes.emplace(0, std::vector<std::size_t>());
    }
    const SceneStage::LockedSection locked = [this](const std::function<void()>& section) {
        SceneWriteBarrier::ReadScope readScope;
        std::lock_guard<std::mutex> lock(_stageMutex);
        section();
    };
    const SceneStage::LockedSection committing = [this](const std::function<void()>& section) {
        SceneWriteBarrier::ReadScope readScope;
        std::lock_guard<std::mutex> lock(_stageMutex);
        section();
        _loadSet = _stage->getLoadSet();
    };
    // The node is evaluated again for the set the batch leaves, so that its pattern nodes match
    // within the payloads just loaded, and stop matching within those just unloaded.
    const SceneStage::LoadPreparer prepare = [this](const SceneLoadSetPtr& loadSet) -> std::function<void()> {
        ScenePayload payload;
        std::string error;
        if (!evaluateNode(_node, SceneEvaluationContext(_contextId, loadSet), _time, _abortInfo, &payload, &error) || _stopRequested) {
            return std::function<void()>();
        }

        return [this, payload]() {
            _stage->sync(payload);
            _shown = !payload.isEmpty();
            _editCount = _stage->getEditCount();
        };
    };
    bool serviced = false;
    for (const std::pair<const std::size_t, std::vector<std::size_t>>& pass : passes) {
        for (std::size_t i : pass.second) {
            const LoadRequest& request = requests[i];
            const PXR_NS::SdfPathVector paths = toSdfPaths(request.paths);
            if (request.load) {
                PXR_NS::SdfPathVector excluded;
                for (const std::string& path : request.excluded) {
                    if (PXR_NS::SdfPath::IsValidPathString(path)) {
                        excluded.push_back(PXR_NS::SdfPath(path));
                    }
                }
                _stage->requestLoad(paths, request.withDescendants, excluded);
            } else {
                _stage->requestUnload(paths);
            }
        }
        if (_stage->serviceLoads(locked, committing, [this]() { return _stopRequested.load(); }, prepare)) {
            serviced = true;
        }
        if (_stopRequested) {
            return;
        }
    }
    // A batch that asked for nothing loadable is still answered, so that whoever asked is not left waiting.
    if (_stopRequested || (!serviced && requests.empty())) {
        return;
    }
    std::weak_ptr<SceneNodeStageSource> self = weak_from_this();
    postToContext([self]() {
        std::shared_ptr<SceneNodeStageSource> source = self.lock();
        if (source && !source->_stopRequested && source->_loadsChanged) {
            source->_loadsChanged();
        }
    });
}

void
SceneNodeStageSource::run()
{
    for (;;) {
        double time = 0.;
        bool evaluate = false;
        std::vector<LoadRequest> loads;
        {
            std::unique_lock<std::mutex> lock(_queueMutex);
            _queueChanged.wait(lock, [this]() { return _quit || _pending || _loadsReady || !_loadRequests.empty(); });
            if (_quit) {
                _finished = true;
                lock.unlock();
                // Taken so that stopAll(), which reads _finished under it, cannot miss the wake.
                {
                    std::lock_guard<std::mutex> liveLock(liveSources().mutex);
                }
                liveSources().changed.notify_all();

                return;
            }
            evaluate = _pending;
            _pending = false;
            _loadsReady = false;
            time = _pendingTime;
            loads.swap(_loadRequests);
        }
        if (!evaluate) {
            serviceLoads(loads);
            continue;
        }
        _time = time;
        ScenePayload payload;
        std::string error;
        const bool ok = evaluateNode(_node, SceneEvaluationContext(_contextId, _loadSet), time, _abortInfo, &payload, &error);
        if (_stopRequested) {
            continue;
        }
        {
            SceneWriteBarrier::ReadScope readScope;
            std::lock_guard<std::mutex> lock(_stageMutex);
            if (!_stage) {
                _stage.reset(new SceneStage(_loadPayloads ? PXR_NS::UsdStage::LoadAll : PXR_NS::UsdStage::LoadNone));
                const std::weak_ptr<SceneNodeStageSource> self = weak_from_this();
                _stage->setLoadsWake([self]() {
                    const std::shared_ptr<SceneNodeStageSource> source = self.lock();
                    if (!source) {
                        return;
                    }
                    {
                        std::lock_guard<std::mutex> lock(source->_queueMutex);
                        source->_loadsReady = true;
                    }
                    source->_queueChanged.notify_one();
                });
            }
            _stage->sync(ok ? payload : ScenePayload());
            _shown = ok && !payload.isEmpty();
            _editCount = _stage->getEditCount();
        }
        ++_composeCount;
        std::weak_ptr<SceneNodeStageSource> self = weak_from_this();
        const QString qerror = QString::fromStdString(error);
        postToContext([self, ok, qerror]() {
            std::shared_ptr<SceneNodeStageSource> source = self.lock();
            if (source && !source->_stopRequested && source->_synced) {
                source->_synced(ok, qerror);
            }
        });
        // Asked for against the composition just synced.
        serviceLoads(loads);
    }
}

NATRON_NAMESPACE_EXIT
