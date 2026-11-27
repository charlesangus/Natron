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

#include "SceneNodeAuthoring.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/ContentHasher.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeColorSampler.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/Nodes/Metadata/EffectMetadata.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/NonTrappingMath.h"
#include "Engine/Nodes/Scene/LiveTextureStore.h"
#include "Engine/Nodes/Scene/PrimSelector.h"
#include "Engine/Nodes/Scene/SceneProjectionCameras.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDProject3D.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"
#include "Engine/ViewerInstance.h"

#include <pxr/usd/usd/collectionAPI.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/tokens.h>

#include <QDir>
#include <QString>
#include <QStringList>

NATRON_NAMESPACE_ENTER

std::string
SceneNodeAuthoring::getAnchoredFilePath(const KnobFilePtr& knob)
{
    if (!knob) {
        return std::string();
    }
    const std::string path = knob->getResolvedPath();
    const QString qpath = QString::fromStdString(path);
    // A URI an asset resolver handles is kept as it is.
    const int colon = qpath.indexOf(QLatin1Char(':'));
    const int slash = qpath.indexOf(QLatin1Char('/'));
    const bool uri = (colon > 1) && ((slash < 0) || (colon < slash));
    if (path.empty() || uri || !QDir::isRelativePath(qpath)) {
        return path;
    }

    return QDir::cleanPath(QDir::current().absoluteFilePath(qpath)).toStdString();
}

void
setSceneKnobSecret(const KnobIPtr& knob,
                   bool secret)
{
    if (!knob) {
        return;
    }
    knob->setSecret(secret);
    knob->setAllDimensionsEnabled(!secret);
}

void
SceneTransformKnobs::create(KnobHolder* holder,
                            const KnobPagePtr& page)
{
    KnobDoublePtr translateKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Translate"), 3);
    translateKnob->setName(kSceneParamTranslate);
    translateKnob->setHintToolTip(EffectInstance::tr("The prim's position in its parent's space, in scene units."));
    for (int d = 0; d < 3; ++d) {
        translateKnob->setDefaultValue(0., d);
    }
    page->addKnob(translateKnob);
    translate = translateKnob;

    KnobDoublePtr rotateKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Rotate"), 3);
    rotateKnob->setName(kSceneParamRotate);
    rotateKnob->setHintToolTip(EffectInstance::tr("Rotations about the X, Y and Z axes, in degrees, applied in the rotate order."));
    for (int d = 0; d < 3; ++d) {
        rotateKnob->setDefaultValue(0., d);
    }
    page->addKnob(rotateKnob);
    rotate = rotateKnob;

    KnobDoublePtr scaleKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Scale"), 3);
    scaleKnob->setName(kSceneParamScale);
    scaleKnob->setHintToolTip(EffectInstance::tr("Scale along the X, Y and Z axes, multiplied by the uniform scale."));
    for (int d = 0; d < 3; ++d) {
        scaleKnob->setDefaultValue(1., d);
    }
    page->addKnob(scaleKnob);
    scale = scaleKnob;

    KnobDoublePtr uniformScaleKnob = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr("Uniform Scale"));
    uniformScaleKnob->setName(kSceneParamUniformScale);
    uniformScaleKnob->setHintToolTip(EffectInstance::tr("Scale along every axis, multiplied into the scale."));
    uniformScaleKnob->setDefaultValue(1.);
    page->addKnob(uniformScaleKnob);
    uniformScale = uniformScaleKnob;

    KnobChoicePtr rotateOrderKnob = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr("Rotate Order"));
    rotateOrderKnob->setName(kSceneParamRotateOrder);
    rotateOrderKnob->setHintToolTip(EffectInstance::tr("The order the three rotations are applied in, as USD names its rotate ops: "
                                                       "XYZ turns about X first, then Y, then Z."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("XYZ", "XYZ", ""));
        options.push_back(ChoiceOption("XZY", "XZY", ""));
        options.push_back(ChoiceOption("YXZ", "YXZ", ""));
        options.push_back(ChoiceOption("YZX", "YZX", ""));
        options.push_back(ChoiceOption("ZXY", "ZXY", ""));
        options.push_back(ChoiceOption("ZYX", "ZYX", ""));
        rotateOrderKnob->populateChoices(options);
    }
    rotateOrderKnob->setDefaultValue((int)eSceneRotateOrderXYZ);
    rotateOrderKnob->setAnimationEnabled(false);
    page->addKnob(rotateOrderKnob);
    rotateOrder = rotateOrderKnob;
} // SceneTransformKnobs::create

void
SceneTransformKnobs::setSecret(bool secret) const
{
    setSceneKnobSecret(translate.lock(), secret);
    setSceneKnobSecret(rotate.lock(), secret);
    setSceneKnobSecret(scale.lock(), secret);
    setSceneKnobSecret(uniformScale.lock(), secret);
    setSceneKnobSecret(rotateOrder.lock(), secret);
}

KnobStringPtr
createScenePrimPathKnob(KnobHolder* holder,
                        const KnobPagePtr& page,
                        const std::string& defaultPath,
                        const QString& toolTip)
{
    KnobStringPtr knob = AppManager::createKnob<KnobString>(holder, EffectInstance::tr("Prim Path"));

    knob->setName(kSceneParamPrimPath);
    knob->setPrimPathHint(KnobString::ePrimPathHintPath);
    knob->setHintToolTip(toolTip);
    knob->setAnimationEnabled(false);
    knob->setDefaultValue(defaultPath);
    page->addKnob(knob);

    return knob;
}

void
SceneCameraPathKnobs::create(KnobHolder* holder,
                             const KnobPagePtr& page,
                             const char* menuName,
                             const char* pathName,
                             const QString& menuToolTip,
                             const QString& pathToolTip)
{
    KnobChoicePtr menuKnob = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr("Camera"));
    menuKnob->setName(menuName);
    menuKnob->setHintToolTip(menuToolTip);
    menuKnob->setAnimationEnabled(false);
    menuKnob->setNotifiesOnReselect(true);
    // Rebuilt from the path, which alone decides the render.
    menuKnob->setIsPersistent(false);
    menuKnob->setEvaluateOnChange(false);
    page->addKnob(menuKnob);
    menu = menuKnob;

    KnobStringPtr pathKnob = AppManager::createKnob<KnobString>(holder, EffectInstance::tr("Camera Path"));
    pathKnob->setName(pathName);
    pathKnob->setHintToolTip(pathToolTip);
    pathKnob->setAnimationEnabled(false);
    page->addKnob(pathKnob);
    path = pathKnob;
}

std::string
SceneCameraPathKnobs::getPath() const
{
    KnobStringPtr pathKnob = path.lock();

    return pathKnob ? pathKnob->getValue() : std::string();
}

void
SceneCameraPathKnobs::refreshMenu(const std::vector<std::string>& cameras,
                                  bool readDownstream,
                                  std::size_t unsearched,
                                  bool noViewer) const
{
    KnobChoicePtr menuKnob = menu.lock();
    if (!menuKnob) {
        return;
    }
    const std::string chosen = getPath();
    std::vector<ChoiceOption> options;
    int chosenIndex = -1;
    for (const std::string& camera : cameras) {
        if (camera == chosen) {
            chosenIndex = (int)options.size();
        }
        options.push_back(ChoiceOption(camera, camera, ""));
    }
    if (!chosen.empty() && (chosenIndex < 0)) {
        chosenIndex = (int)options.size();
        if (readDownstream) {
            options.push_back(ChoiceOption(chosen, chosen,
                                           EffectInstance::tr("Read from the scene the projection renders in, which has this camera once it is merged in.").toStdString()));
        } else {
            options.push_back(ChoiceOption(chosen,
                                           EffectInstance::tr("%1 (missing)").arg(QString::fromStdString(chosen)).toStdString(),
                                           EffectInstance::tr("No camera upstream has this path yet: nothing projects until one does.").toStdString()));
        }
    }
    if (unsearched > 0) {
        options.push_back(makeSceneUnsearchedOption(unsearched, noViewer));
    }
    // Repopulating an unchanged menu would move the node's hash and come back here.
    const std::vector<ChoiceOption> current = menuKnob->getEntries_mt_safe();
    bool same = current.size() == options.size();
    for (std::size_t i = 0; same && (i < options.size()); ++i) {
        same = (current[i].id == options[i].id) && (current[i].label == options[i].label);
    }
    if (!same) {
        menuKnob->populateChoices(options);
    }
    if ((chosenIndex >= 0) && (menuKnob->getValue() != chosenIndex)) {
        menuKnob->setValue(chosenIndex);
    }
}

bool
SceneCameraPathKnobs::knobChanged(KnobI* k,
                                  ValueChangedReasonEnum reason,
                                  bool* menuStale) const
{
    KnobChoicePtr menuKnob = menu.lock();
    KnobStringPtr pathKnob = path.lock();
    *menuStale = false;
    if (!menuKnob || !pathKnob) {
        return false;
    }
    if (k == menuKnob.get()) {
        const int index = menuKnob->getValue();
        if (((reason == eValueChangedReasonUserEdited) || (reason == eValueChangedReasonNatronGuiEdited))
            && (index >= 0) && (index < menuKnob->getNumEntries())) {
            const ChoiceOption picked = menuKnob->getEntry(index);
            // A script can still set a disabled note row, which names no camera.
            if (!picked.disabled) {
                pathKnob->setValue(picked.id);
            }
        }

        return true;
    }
    if (k == pathKnob.get()) {
        *menuStale = true;

        return true;
    }

    return false;
}

ChoiceOption
makeSceneUnsearchedOption(std::size_t unsearched,
                          bool noViewer)
{
    const QString tip = noViewer ? EffectInstance::tr("No viewer shows this node, so nothing inside a payload was searched. Show the node in a viewer and load these branches there to list what they hold.")
                                 : EffectInstance::tr("The viewer showing this node leaves these branches unloaded. Load them there to list what they hold.");

    ChoiceOption option(kSceneMenuUnsearchedId,
                        EffectInstance::tr("%1 unloaded branches not searched").arg((qulonglong)unsearched).toStdString(),
                        tip.toStdString());
    option.disabled = true;

    return option;
}

bool
isSceneUnsearchedOption(const std::string& id)
{
    return id == kSceneMenuUnsearchedId;
}

SceneMenuJob::SceneMenuJob(bool noViewer)
    : _noViewer(noViewer)
    , _abortInfo(AbortableRenderInfo::create(true, 0))
{
}

void
SceneMenuJob::cancel()
{
    _abortInfo->setAborted();
}

bool
SceneMenuJob::isCancelled() const
{
    return _abortInfo->isAborted();
}

void
SceneMenuJob::wait() const
{
    std::unique_lock<std::mutex> l(_lock);

    _done.wait(l, [this]() { return _finished; });
}

bool
SceneMenuJob::isDone() const
{
    std::lock_guard<std::mutex> l(_lock);

    return _finished;
}

SceneMenuJob::Result
SceneMenuJob::getResult() const
{
    std::lock_guard<std::mutex> l(_lock);

    return _result;
}

void
SceneMenuJob::finish(const Result& result)
{
    {
        std::lock_guard<std::mutex> l(_lock);
        _result = result;
        _finished = true;
    }
    _done.notify_all();
}

struct SceneWorker::Implementation {
    std::mutex lock;
    std::condition_variable wake;
    bool stopping = false;
    bool started = false;
    bool pending = false;
    Task pendingRun;
    Task pendingDropped;

    void loop();
};

namespace {
// The threads of every worker, for joinAll(). Never destroyed: a thread joinAll() gave up on may
// still reach it while the process exits.
struct SceneWorkerThreads {
    std::mutex lock;
    std::condition_variable ended;
    std::size_t running = 0;
    bool shutDown = false;
    std::vector<std::weak_ptr<SceneWorker::Implementation>> workers;
};

SceneWorkerThreads&
sceneWorkerThreads()
{
    static SceneWorkerThreads* threads = new SceneWorkerThreads;

    return *threads;
}

// Releases the Python GIL while the calling thread waits, if it holds it: a job may need the GIL
// (expressions) to finish.
class PythonUnlockedWhileJoining {
public:
    PythonUnlockedWhileJoining()
        : _state(nullptr)
    {
        if (Py_IsInitialized() && PyGILState_Check()) {
            _state = PyEval_SaveThread();
        }
    }

    ~PythonUnlockedWhileJoining()
    {
        if (_state) {
            PyEval_RestoreThread(_state);
        }
    }

    PythonUnlockedWhileJoining(const PythonUnlockedWhileJoining&) = delete;
    PythonUnlockedWhileJoining& operator=(const PythonUnlockedWhileJoining&) = delete;

private:
    PyThreadState* _state;
};

void
callDropped(const SceneWorker::Task& dropped)
{
    if (!dropped) {
        return;
    }
    try {
        dropped();
    } catch (...) {
    }
}

// Sets imp stopping and returns its waiting job's dropped callback, for the caller to call unlocked.
SceneWorker::Task
stopWorker(SceneWorker::Implementation* imp)
{
    SceneWorker::Task dropped;
    {
        std::lock_guard<std::mutex> l(imp->lock);
        imp->stopping = true;
        if (imp->pending) {
            dropped.swap(imp->pendingDropped);
            imp->pendingRun = SceneWorker::Task();
            imp->pending = false;
        }
    }
    imp->wake.notify_one();

    return dropped;
}
} // anonymous namespace

void
SceneWorker::Implementation::loop()
{
    for (;;) {
        Task run;
        {
            std::unique_lock<std::mutex> l(lock);
            wake.wait(l, [this]() { return stopping || pending; });
            if (stopping) {
                break;
            }
            run.swap(pendingRun);
            pendingDropped = Task();
            pending = false;
        }
        try {
            run();
        } catch (...) {
        }
    }
    SceneWorkerThreads& threads = sceneWorkerThreads();
    {
        std::lock_guard<std::mutex> l(threads.lock);
        --threads.running;
    }
    threads.ended.notify_all();
}

SceneWorker::SceneWorker()
    : _imp(std::make_shared<Implementation>())
{
}

SceneWorker::~SceneWorker()
{
    stop();
}

void
SceneWorker::post(const Task& run,
                  const Task& dropped)
{
    Task replaced;
    bool start = false;
    {
        std::lock_guard<std::mutex> l(_imp->lock);
        if (_imp->stopping) {
            replaced = dropped;
        } else {
            if (_imp->pending) {
                replaced.swap(_imp->pendingDropped);
            }
            _imp->pendingRun = run;
            _imp->pendingDropped = dropped;
            _imp->pending = true;
            start = !_imp->started;
            _imp->started = true;
        }
    }
    if (start) {
        SceneWorkerThreads& threads = sceneWorkerThreads();
        bool refused = false;
        {
            std::lock_guard<std::mutex> l(threads.lock);
            refused = threads.shutDown;
            if (!refused) {
                ++threads.running;
                threads.workers.erase(std::remove_if(threads.workers.begin(), threads.workers.end(), [](const std::weak_ptr<Implementation>& worker) { return worker.expired(); }), threads.workers.end());
                threads.workers.push_back(_imp);
            }
        }
        if (refused) {
            callDropped(stopWorker(_imp.get()));
        } else {
            std::shared_ptr<Implementation> imp = _imp;
            std::thread([imp]() { imp->loop(); }).detach();
        }
    }
    _imp->wake.notify_one();
    callDropped(replaced);
}

void
SceneWorker::stop()
{
    callDropped(stopWorker(_imp.get()));
}

bool
SceneWorker::joinAll(int timeoutMs)
{
    SceneWorkerThreads& threads = sceneWorkerThreads();
    std::vector<std::shared_ptr<Implementation>> workers;
    {
        std::lock_guard<std::mutex> l(threads.lock);
        threads.shutDown = true;
        for (const std::weak_ptr<Implementation>& weak : threads.workers) {
            if (const std::shared_ptr<Implementation> worker = weak.lock()) {
                workers.push_back(worker);
            }
        }
        threads.workers.clear();
    }
    for (const std::shared_ptr<Implementation>& worker : workers) {
        callDropped(stopWorker(worker.get()));
    }
    workers.clear();
    PythonUnlockedWhileJoining unlocked;
    std::unique_lock<std::mutex> l(threads.lock);

    return threads.ended.wait_for(l, std::chrono::milliseconds(timeoutMs), [&threads]() { return threads.running == 0; });
}

struct SceneMenuRefresh::State {
    std::mutex lock;
    std::shared_ptr<SceneMenuJob> current;
    SceneWorker worker;
    const U64 noViewerId = SceneEvaluationContext::makeId();
    const U64 mappedId = SceneEvaluationContext::makeId();
    // The rest is main thread only.
    RerunFunc rerun;
    std::weak_ptr<EffectInstance> effect;
    // What the latest refresh searched on, and its job.
    bool searched = false;
    SceneLoadSetPtr searchedLoadSet;
    SceneLoadReachPtr searchedReach;
    bool searchedNoViewer = false;
    std::shared_ptr<SceneMenuJob> latest;
    // Set while followLoadChanges() reruns the refresh.
    bool followingLoad = false;

    ~State()
    {
        EffectInstance::retireSceneContextId(noViewerId);
        EffectInstance::retireSceneContextId(mappedId);
    }
};

namespace {
std::mutex menuRefreshRegistryLock;
std::vector<std::weak_ptr<SceneMenuRefresh::State>> menuRefreshRegistry;
std::atomic<bool> followLoadChangesPending(false);

// Whether a refresh searching on loadSet, with no viewer when noViewer, may find other entries than the last one did.
bool
loadChangeReachesSearch(const SceneMenuRefresh::State& state,
                        const SceneLoadSetPtr& loadSet,
                        bool noViewer)
{
    if (noViewer != state.searchedNoViewer) {
        return true;
    }
    const bool sameLoadSet = state.searchedLoadSet ? (loadSet && (*state.searchedLoadSet == *loadSet)) : !loadSet;
    if (sameLoadSet) {
        return false;
    }
    // A search still running, or cut short, has not gathered its whole reach.
    if (!state.searchedReach || !state.latest || !state.latest->isDone() || state.latest->isCancelled()) {
        return true;
    }

    return SceneOps::loadChangeReaches(state.searchedLoadSet.get(), loadSet.get(), state.searchedReach->getPrefixes());
}
} // anonymous namespace

SceneMenuRefresh::SceneMenuRefresh()
    : _state(std::make_shared<State>())
{
    std::lock_guard<std::mutex> l(menuRefreshRegistryLock);

    menuRefreshRegistry.erase(std::remove_if(menuRefreshRegistry.begin(), menuRefreshRegistry.end(), [](const std::weak_ptr<State>& state) { return state.expired(); }), menuRefreshRegistry.end());
    menuRefreshRegistry.push_back(_state);
}

void
SceneMenuRefresh::setRerun(const RerunFunc& rerun)
{
    _state->rerun = rerun;
}

SceneMenuRefresh::~SceneMenuRefresh()
{
    {
        std::lock_guard<std::mutex> l(_state->lock);
        if (_state->current) {
            _state->current->cancel();
        }
    }
    _state->worker.stop();
}

SceneEvaluationContext
SceneMenuRefresh::getContext(EffectInstance* effect,
                             bool* noViewer) const
{
    SceneEvaluationContext context;
    SceneEvaluationContext viewerContext;

    bool ownSet = false;
    *noViewer = !SceneNodeAuthoring::getShowingViewerContext(effect, &context, &viewerContext, &ownSet);
    if (*noViewer) {
        context = SceneEvaluationContext(_state->noViewerId, std::make_shared<const SceneLoadSet>(SceneLoadSet::none()));
    } else if (ownSet || !context.hasSameLoadSet(viewerContext)) {
        // A set mapped through the graph must not share the memos the viewer's own id keys.
        context.id = _state->mappedId;
    }
    context.reach = std::make_shared<SceneLoadReach>();

    return context;
}

U64
SceneMenuRefresh::getMappedContextId() const
{
    return _state->mappedId;
}

std::shared_ptr<SceneMenuJob>
SceneMenuRefresh::start(EffectInstance* effect,
                        const SceneEvaluationContext& context,
                        bool noViewer,
                        const WorkFunc& work,
                        const ApplyFunc& apply)
{
    if (_state->followingLoad && _state->searched && !loadChangeReachesSearch(*_state, context.loadSet, noViewer)) {
        return std::shared_ptr<SceneMenuJob>();
    }
    std::shared_ptr<SceneMenuJob> job = std::make_shared<SceneMenuJob>(noViewer);
    _state->effect = effect ? effect->weak_from_this() : std::weak_ptr<EffectInstance>();
    _state->searched = true;
    _state->searchedLoadSet = context.loadSet;
    _state->searchedReach = context.reach;
    _state->searchedNoViewer = noViewer;
    _state->latest = job;
    {
        std::lock_guard<std::mutex> l(_state->lock);
        if (_state->current) {
            _state->current->cancel();
        }
        _state->current = job;
    }
    std::weak_ptr<State> weakState = _state;
    std::weak_ptr<EffectInstance> weakEffect = effect ? effect->weak_from_this() : std::weak_ptr<EffectInstance>();
    _state->worker.post([job, work, apply, weakState, weakEffect]() {
        SceneMenuJob::Result result;
        if (!job->isCancelled()) {
            try {
                work(*job, &result);
            } catch (...) {
                result = SceneMenuJob::Result();
            }
        }
        job->finish(result);
        if (job->isCancelled() || !QCoreApplication::instance()) {
            return;
        }
        QMetaObject::invokeMethod(QCoreApplication::instance(), [job, apply, weakState, weakEffect]() {
            const std::shared_ptr<State> state = weakState.lock();
            const EffectInstancePtr effect = weakEffect.lock();
            if (state && effect && effect->getNode()) {
                apply(job);
            } }, Qt::QueuedConnection); }, [job]() { job->finish(SceneMenuJob::Result()); });

    return job;
}

bool
SceneMenuRefresh::take(const std::shared_ptr<SceneMenuJob>& job)
{
    if (!job || !job->isDone() || job->isCancelled()) {
        return false;
    }
    std::lock_guard<std::mutex> l(_state->lock);
    if (_state->current != job) {
        return false;
    }
    _state->current.reset();

    return true;
}

std::vector<std::shared_ptr<SceneMenuJob>>
SceneMenuRefresh::followLoadChanges()
{
    followLoadChangesPending = false;
    std::vector<std::shared_ptr<State>> states;
    {
        std::lock_guard<std::mutex> l(menuRefreshRegistryLock);
        for (const std::weak_ptr<State>& weak : menuRefreshRegistry) {
            if (const std::shared_ptr<State> state = weak.lock()) {
                states.push_back(state);
            }
        }
    }
    std::vector<std::shared_ptr<SceneMenuJob>> jobs;
    for (const std::shared_ptr<State>& state : states) {
        // A refresh never run has nothing to follow; one whose node is gone has no one to rerun it.
        if (!state->searched || !state->rerun || state->effect.expired()) {
            continue;
        }
        state->followingLoad = true;
        std::shared_ptr<SceneMenuJob> job;
        try {
            job = state->rerun();
        } catch (...) {
            job.reset();
        }
        state->followingLoad = false;
        if (job) {
            jobs.push_back(job);
        }
    }

    return jobs;
}

void
SceneMenuRefresh::scheduleFollowLoadChanges()
{
    if (!QCoreApplication::instance() || followLoadChangesPending.exchange(true)) {
        return;
    }
    QMetaObject::invokeMethod(QCoreApplication::instance(), []() { followLoadChanges(); }, Qt::QueuedConnection);
}

std::string
getSceneNodeDefaultPrimPath(const EffectInstance* effect,
                            const std::string& fallbackName)
{
    const std::string scriptName = effect ? effect->getScriptName_mt_safe() : std::string();

    return "/" + (scriptName.empty() ? fallbackName : scriptName);
}

void
followSceneNodeRename(const KnobStringPtr& knob,
                      const std::string& newDefaultPath)
{
    if (!knob) {
        return;
    }
    const std::string oldDefault = knob->getDefaultValue(0);
    if (oldDefault == newDefaultPath) {
        return;
    }
    if (knob->getValue() == oldDefault) {
        knob->setDefaultValue(newDefaultPath);
    } else {
        knob->setDefaultValueWithoutApplying(newDefaultPath);
    }
}

namespace {
// Evaluation contexts a pattern's matches are kept for at once.
const std::size_t kPatternCacheEntries = 4;
} // anonymous namespace

struct PrimPatternCache::State {
    struct Entry {
        U64 context = 0;
        ScenePayload upstream;
        std::string pattern;
        U64 filterKey = 0;
        SceneLoadSetPtr loadSet;
        // SceneOps::getLoadReach() of the pattern's prefixes on the stage the matches came from.
        std::vector<std::string> loadReach;
        SceneNodeAuthoring::PatternMatches matches;
    };

    mutable std::mutex mutex;
    // Most recently used first.
    std::list<Entry> entries;
    U64 queryCount = 0;
};

PrimPatternCache::PrimPatternCache()
    : _state(new State)
{
}

PrimPatternCache::~PrimPatternCache()
{
}

void
PrimPatternCache::invalidate()
{
    std::lock_guard<std::mutex> lock(_state->mutex);

    _state->entries.clear();
}

U64
PrimPatternCache::getQueryCount() const
{
    std::lock_guard<std::mutex> lock(_state->mutex);

    return _state->queryCount;
}

PrimPatternCache::State&
PrimPatternCache::getState()
{
    return *_state;
}

namespace {
bool
borrowFor(const ScenePayload& upstream,
          const SceneEvaluationContext& context,
          SceneQueryPool::Lease* lease,
          std::string* errorMessage)
{
    if (!lease->isValid()) {
        *lease = SceneQueryPool::instance().borrow(upstream, context.loadSet, errorMessage);
    }
    if (!lease->getStage()) {
        if (errorMessage->empty()) {
            *errorMessage = EffectInstance::tr("USD could not compose the input scene.").toStdString();
        }

        return false;
    }

    return true;
}

bool
isUnloadedPayload(const PXR_NS::UsdPrim& prim)
{
    return prim.HasAuthoredPayloads() && !prim.IsLoaded();
}
} // anonymous namespace

namespace SceneNodeAuthoring {
bool
matchPattern(PrimPatternCache* cache,
             const ScenePayload& upstream,
             const std::string& pattern,
             const SceneEvaluationContext& context,
             U64 filterKey,
             const PatternFilter& filter,
             SceneQueryPool::Lease* lease,
             PatternMatches* matches,
             std::string* errorMessage,
             bool traverseInstanceProxies)
{
    *matches = PatternMatches();
    if (traverseInstanceProxies) {
        // The cache tells the two searches apart by the key alone.
        ContentHasher key;
        key.appendU64(filterKey);
        key.appendBool(true);
        filterKey = key.finalize();
    }
    PrimSelector::Reach reach;
    if (!PrimSelector::getReach(pattern, &reach, errorMessage)) {
        return false;
    }
    if (upstream.isEmpty() || PrimSelector::isBlank(pattern)) {
        return true;
    }
    PrimPatternCache::State& state = cache->getState();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        for (std::list<PrimPatternCache::State::Entry>::iterator it = state.entries.begin(); it != state.entries.end(); ++it) {
            if ((it->pattern != pattern) || (it->filterKey != filterKey) || !it->upstream.isSameStack(upstream)
                || (it->upstream.getStackHash() != upstream.getStackHash())) {
                continue;
            }
            const bool sameLoadSet = it->loadSet ? (context.loadSet && (*it->loadSet == *context.loadSet)) : !context.loadSet;
            // Literal paths in a partial load set match by what is loaded above them.
            if (!sameLoadSet && (reach.literalOnly || SceneOps::loadChangeReaches(it->loadSet.get(), context.loadSet.get(), it->loadReach))) {
                continue;
            }
            *matches = it->matches;
            if (context.reach) {
                context.reach->add(it->loadReach);
            }
            PrimPatternCache::State::Entry entry = *it;
            entry.context = context.id;
            entry.upstream = upstream;
            entry.loadSet = context.loadSet;
            state.entries.erase(it);
            for (std::list<PrimPatternCache::State::Entry>::iterator own = state.entries.begin(); own != state.entries.end(); ++own) {
                if (own->context == context.id) {
                    state.entries.erase(own);
                    break;
                }
            }
            state.entries.push_front(entry);

            return true;
        }
        ++state.queryCount;
    }

    if (!borrowFor(upstream, context, lease, errorMessage)) {
        if (context.reach) {
            context.reach->add(reach.prefixes);
        }

        return false;
    }
    const PXR_NS::UsdStageRefPtr stage = lease->getStage();
    const std::vector<std::string> loadReach = SceneOps::getLoadReach(stage, reach.prefixes);
    if (context.reach) {
        context.reach->add(loadReach);
    }
    const bool partial = !context.includesEverything();
    std::vector<PXR_NS::SdfPath> found;
    if (partial && reach.literalOnly) {
        for (const std::string& literal : reach.literals) {
            const PXR_NS::SdfPath path(literal);
            const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(path);
            if (prim) {
                if (prim.IsActive() && prim.IsDefined() && !prim.IsAbstract() && (!filter || filter(prim))) {
                    found.push_back(path);
                }
                continue;
            }
            if (filter) {
                continue;
            }
            for (PXR_NS::SdfPath ancestor = path.GetParentPath(); !ancestor.IsEmpty() && !ancestor.IsAbsoluteRootPath(); ancestor = ancestor.GetParentPath()) {
                const PXR_NS::UsdPrim above = stage->GetPrimAtPath(ancestor);
                if (above) {
                    if (above.IsActive() && isUnloadedPayload(above)) {
                        found.push_back(path);
                    }
                    break;
                }
            }
        }
        std::sort(found.begin(), found.end());
        found.erase(std::unique(found.begin(), found.end()), found.end());
    } else {
        std::vector<PXR_NS::SdfPath> selected;
        if (!SceneOps::selectPrims(stage, pattern, &selected, errorMessage, traverseInstanceProxies)) {
            return false;
        }
        for (const PXR_NS::SdfPath& path : selected) {
            if (!filter || filter(stage->GetPrimAtPath(path))) {
                found.push_back(path);
            }
        }
    }
    matches->paths = found;
    if (partial && found.empty()) {
        matches->unsearched = SceneOps::countUnloadedPayloads(stage, reach.prefixes);
    }

    std::lock_guard<std::mutex> lock(state.mutex);
    for (std::list<PrimPatternCache::State::Entry>::iterator it = state.entries.begin(); it != state.entries.end(); ++it) {
        if (it->context == context.id) {
            state.entries.erase(it);
            break;
        }
    }
    PrimPatternCache::State::Entry entry;
    entry.context = context.id;
    entry.upstream = upstream;
    entry.pattern = pattern;
    entry.filterKey = filterKey;
    entry.loadSet = context.loadSet;
    entry.loadReach = loadReach;
    entry.matches = *matches;
    state.entries.push_front(entry);
    if (state.entries.size() > kPatternCacheEntries) {
        state.entries.pop_back();
    }

    return true;
} // matchPattern

std::string
describeNoMatch(const std::string& pattern,
                const PatternMatches& matches,
                const std::string& consequence)
{
    if (matches.unsearched > 0) {
        return EffectInstance::tr("The prim pattern \"%1\" found 0 matches, %2 unloaded branches not searched; %3.")
            .arg(QString::fromStdString(pattern))
            .arg((qulonglong)matches.unsearched)
            .arg(QString::fromStdString(consequence))
            .toStdString();
    }

    return EffectInstance::tr("The prim pattern \"%1\" matches no prim; %2.")
        .arg(QString::fromStdString(pattern))
        .arg(QString::fromStdString(consequence))
        .toStdString();
}

double
getTimeCodesPerSecond(const SceneLayerEdit& edit)
{
    if (edit.upstream.isEmpty()) {
        return (edit.framesPerSecond > 0.) ? edit.framesPerSecond : edit.upstream.getMetadata().timeCodesPerSecond;
    }

    return edit.upstream.getMetadata().timeCodesPerSecond;
}

double
getMetersPerUnit(const SceneLayerEdit& edit)
{
    return (edit.metersPerUnit > 0.) ? edit.metersPerUnit : SceneMetadata().metersPerUnit;
}

bool
liveTextureWindowVaries(const LiveTextureSource& recorded)
{
    if (!recorded.windows) {
        return false;
    }
    const std::vector<LiveTextureWindow>& windows = *recorded.windows;
    for (std::size_t i = 1; i < windows.size(); ++i) {
        if ((windows[i].format != windows[0].format) || (windows[i].window != windows[0].window)) {
            return true;
        }
    }

    return false;
}

bool
authorUnlitSurfaceInputs(PXR_NS::UsdShadeShader surface,
                         PXR_NS::UsdShadeInput* emissive)
{
    // No diffuse, and the specular workflow with a black specular colour, which, unlike the
    // metallic workflow's dielectric, reflects nothing: what is left is the emission alone.
    const PXR_NS::UsdShadeInput diffuse = surface.CreateInput(PXR_NS::TfToken("diffuseColor"), PXR_NS::SdfValueTypeNames->Color3f);
    const PXR_NS::UsdShadeInput metallic = surface.CreateInput(PXR_NS::TfToken("metallic"), PXR_NS::SdfValueTypeNames->Float);
    const PXR_NS::UsdShadeInput roughness = surface.CreateInput(PXR_NS::TfToken("roughness"), PXR_NS::SdfValueTypeNames->Float);
    const PXR_NS::UsdShadeInput specularWorkflow = surface.CreateInput(PXR_NS::TfToken("useSpecularWorkflow"), PXR_NS::SdfValueTypeNames->Int);
    const PXR_NS::UsdShadeInput specularColor = surface.CreateInput(PXR_NS::TfToken("specularColor"), PXR_NS::SdfValueTypeNames->Color3f);
    *emissive = surface.CreateInput(PXR_NS::TfToken("emissiveColor"), PXR_NS::SdfValueTypeNames->Color3f);

    return diffuse && diffuse.Set(PXR_NS::GfVec3f(0.f))
        && metallic && metallic.Set(0.f)
        && roughness && roughness.Set(1.f)
        && specularWorkflow && specularWorkflow.Set(1)
        && specularColor && specularColor.Set(PXR_NS::GfVec3f(0.f))
        && *emissive;
}

void
appendUpstreamMetadata(const ScenePayload& upstream,
                       ContentHasher* h)
{
    const SceneMetadata& metadata = upstream.getMetadata();

    h->appendBool(upstream.isEmpty());
    h->appendI64((std::int64_t)metadata.upAxis);
    h->appendDouble(metadata.metersPerUnit);
    h->appendString(metadata.defaultPrim);
    h->appendDouble(metadata.timeCodesPerSecond);
    h->appendString(metadata.renderSettingsPrimPath);
}

void
authorLayerMetadata(const SceneLayerEdit& edit)
{
    const PXR_NS::SdfPath& root = PXR_NS::SdfPath::AbsoluteRootPath();

    edit.layer->SetTimeCodesPerSecond(getTimeCodesPerSecond(edit));
    edit.layer->SetField(root, PXR_NS::UsdGeomTokens->upAxis,
                         (edit.upAxis == SceneMetadata::eUpAxisZ) ? PXR_NS::UsdGeomTokens->z : PXR_NS::UsdGeomTokens->y);
    edit.layer->SetField(root, PXR_NS::UsdGeomTokens->metersPerUnit, getMetersPerUnit(edit));
}

PXR_NS::UsdStageRefPtr
openLayerStage(const SceneLayerEdit& edit,
               std::string* errorMessage)
{
    if (!edit.layer) {
        *errorMessage = EffectInstance::tr("No layer to author.").toStdString();

        return PXR_NS::UsdStageRefPtr();
    }
    authorLayerMetadata(edit);
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(edit.layer, PXR_NS::SdfLayerHandle(), PXR_NS::UsdStage::LoadNone);
    if (!stage) {
        *errorMessage = EffectInstance::tr("USD could not open a stage on the node's layer.").toStdString();
    }

    return stage;
}

namespace {
    bool
    getPrimPathFrom(const std::string& value,
                    PXR_NS::SdfPath* path,
                    std::string* errorMessage)
    {
        if (PXR_NS::SdfPath::IsValidPathString(value)) {
            const PXR_NS::SdfPath candidate(value);
            if (candidate.IsAbsolutePath() && candidate.IsPrimPath()) {
                *path = candidate;

                return true;
            }
        }
        *errorMessage = EffectInstance::tr("\"%1\" is not an absolute prim path such as /World/Prim.").arg(QString::fromStdString(value)).toStdString();

        return false;
    }
} // anonymous namespace

bool
getPrimPath(const KnobStringPtr& knob,
            PXR_NS::SdfPath* path,
            std::string* errorMessage)
{
    return getPrimPathFrom(knob ? knob->getValue() : std::string(), path, errorMessage);
}

bool
getPrimPath(const KnobStringPtr& knob,
            double time,
            ViewIdx view,
            PXR_NS::SdfPath* path,
            std::string* errorMessage)
{
    return getPrimPathFrom(knob ? knob->getValueAtTime(time, 0, view) : std::string(), path, errorMessage);
}

bool
isAnimated(const KnobIPtr& knob)
{
    if (!knob) {
        return false;
    }
    for (int d = 0; d < knob->getDimension(); ++d) {
        if (knob->isAnimated(d) || !knob->getExpression(d).empty()) {
            return true;
        }
    }

    return false;
}

std::vector<double>
getSampleFrames(const SceneLayerEdit& edit)
{
    std::vector<double> frames;
    const double first = std::ceil(edit.firstFrame);
    const double last = std::max(first, std::floor(edit.lastFrame));

    frames.reserve((std::size_t)(last - first) + 1);
    for (double frame = first; frame <= last; frame += 1.) {
        frames.push_back(frame);
    }

    return frames;
}

PXR_NS::UsdTimeCode
frameToTimeCode(const SceneLayerEdit& edit,
                double frame)
{
    if (edit.framesPerSecond <= 0.) {
        return PXR_NS::UsdTimeCode(frame);
    }

    return PXR_NS::UsdTimeCode(frame * getTimeCodesPerSecond(edit) / edit.framesPerSecond);
}

bool
authorTransform(const SceneTransformKnobs& knobs,
                const PXR_NS::UsdGeomXformable& xformable,
                const SceneLayerEdit& edit,
                std::string* errorMessage)
{
    static const PXR_NS::UsdGeomXformOp::Type kRotateTypes[] = {
        PXR_NS::UsdGeomXformOp::TypeRotateXYZ,
        PXR_NS::UsdGeomXformOp::TypeRotateXZY,
        PXR_NS::UsdGeomXformOp::TypeRotateYXZ,
        PXR_NS::UsdGeomXformOp::TypeRotateYZX,
        PXR_NS::UsdGeomXformOp::TypeRotateZXY,
        PXR_NS::UsdGeomXformOp::TypeRotateZYX,
    };
    KnobDoublePtr translate = knobs.translate.lock();
    KnobDoublePtr rotate = knobs.rotate.lock();
    KnobDoublePtr scale = knobs.scale.lock();
    KnobDoublePtr uniformScale = knobs.uniformScale.lock();
    KnobChoicePtr rotateOrder = knobs.rotateOrder.lock();

    if (!xformable || !translate || !rotate || !scale || !uniformScale || !rotateOrder) {
        *errorMessage = EffectInstance::tr("The prim cannot hold a transform.").toStdString();

        return false;
    }
    int order = rotateOrder->getValue();
    if ((order < 0) || (order >= (int)(sizeof(kRotateTypes) / sizeof(kRotateTypes[0])))) {
        order = (int)eSceneRotateOrderXYZ;
    }

    const PXR_NS::UsdGeomXformOp translateOp = xformable.AddTranslateOp(PXR_NS::UsdGeomXformOp::PrecisionDouble);
    const PXR_NS::UsdGeomXformOp rotateOp = xformable.AddXformOp(kRotateTypes[order], PXR_NS::UsdGeomXformOp::PrecisionFloat);
    const PXR_NS::UsdGeomXformOp scaleOp = xformable.AddScaleOp(PXR_NS::UsdGeomXformOp::PrecisionFloat);
    const bool authored = translateOp && rotateOp && scaleOp
        && authorValue(translateOp.GetAttr(), edit, isAnimated(translate), [&translate](double frame) {
                              return PXR_NS::GfVec3d(translate->getValueAtTime(frame, 0), translate->getValueAtTime(frame, 1), translate->getValueAtTime(frame, 2));
                          })
        && authorValue(rotateOp.GetAttr(), edit, isAnimated(rotate), [&rotate](double frame) {
                              return PXR_NS::GfVec3f(knobValueAsFloat(rotate->getValueAtTime(frame, 0)), knobValueAsFloat(rotate->getValueAtTime(frame, 1)), knobValueAsFloat(rotate->getValueAtTime(frame, 2)));
                          })
        && authorValue(scaleOp.GetAttr(), edit, isAnimated(scale) || isAnimated(uniformScale), [&scale, &uniformScale](double frame) {
                              const double uniform = uniformScale->getValueAtTime(frame, 0);

                              return PXR_NS::GfVec3f(knobValueAsFloat(productOrInfinity(scale->getValueAtTime(frame, 0), uniform)), knobValueAsFloat(productOrInfinity(scale->getValueAtTime(frame, 1), uniform)), knobValueAsFloat(productOrInfinity(scale->getValueAtTime(frame, 2), uniform)));
                          });
    if (!authored) {
        *errorMessage = EffectInstance::tr("USD could not author the transform of %1.").arg(QString::fromStdString(xformable.GetPath().GetString())).toStdString();

        return false;
    }

    return true;
} // authorTransform

namespace {
    bool
    graftMaterial(const ScenePayload& material,
                  const SceneReparentArgs& args,
                  SceneGraftMemo* memo,
                  SceneMaterialGraft* graft,
                  std::string* errorMessage)
    {
        *graft = SceneMaterialGraft();
        if (!material.isEmpty()) {
            std::vector<SceneRootGraft> grafts;
            if (!SceneOps::reparentRoots(args, memo, &graft->payload, &grafts, &graft->unmappedLinks, errorMessage)) {
                return false;
            }

            // Only the roots grafted are searched for the material, so a load under one of them
            // is all that can change what is found.
            const SceneEvaluationContext& materialContext = args.inputContexts.empty() ? SceneEvaluationContext::current() : args.inputContexts.front();
            if (materialContext.reach) {
                std::vector<std::string> rootPaths;
                for (const SceneRootGraft& root : grafts) {
                    rootPaths.push_back(root.sourcePath);
                }
                materialContext.reach->add(rootPaths);
            }
            SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(material, materialContext.loadSet, errorMessage);
            const PXR_NS::UsdStageRefPtr stage = lease.getStage();
            if (!stage) {
                if (errorMessage->empty()) {
                    *errorMessage = EffectInstance::tr("USD could not compose the material input.").toStdString();
                }

                return false;
            }
            for (const SceneRootGraft& root : grafts) {
                const PXR_NS::SdfPath sourceRoot(root.sourcePath);
                const PXR_NS::SdfPath graftedRoot(root.graftedPath);
                const PXR_NS::UsdPrim rootPrim = stage->GetPrimAtPath(sourceRoot);
                for (const PXR_NS::UsdPrim& prim : PXR_NS::UsdPrimRange(rootPrim)) {
                    if (!prim.IsA<PXR_NS::UsdShadeMaterial>()) {
                        continue;
                    }
                    graft->materialPath = prim.GetPath().ReplacePrefix(sourceRoot, graftedRoot).GetString();
                    readGeometryProjection(prim, &graft->projection);
                    break;
                }
                if (!graft->materialPath.empty()) {
                    break;
                }
            }
        }

        ContentHasher facts;
        appendUpstreamMetadata(graft->payload, &facts);
        facts.appendString(graft->materialPath);
        facts.appendString(graft->projection.framePrimvar);
        facts.appendBool(graft->projection.held);
        facts.appendDouble(graft->projection.holdFrame);
        graft->factsDigest = facts.finalize();

        return true;
    }
} // anonymous namespace

bool
graftMaterialInput(const ScenePayload& material,
                   const SceneEvaluationContext& materialContext,
                   const std::string& primPath,
                   const std::string& layerTag,
                   SceneGraftMemo* memo,
                   SceneMaterialGraft* graft,
                   std::string* errorMessage)
{
    SceneReparentArgs args;

    args.inputs.push_back(material);
    args.destinationPath = primPath;
    args.layerTag = layerTag;
    args.inputContexts.push_back(materialContext);

    return graftMaterial(material, args, memo, graft, errorMessage);
}

SceneEvaluationContext
getMaterialInputContext(const SceneEvaluationContext& ctx,
                        const std::string& primPath)
{
    return SceneEvaluationContext(ctx.id, SceneOps::mapGraftLoadSet(ctx.loadSet, primPath, std::string(), false));
}

void
mapMaterialInputReach(SceneGraftMemo* memo,
                      const std::string& primPath,
                      const std::vector<std::string>& in,
                      std::vector<std::string>* out)
{
    SceneOps::mapGraftReach(memo, 0, primPath, std::string(), in, out);
}

bool
graftSharedMaterial(const ScenePayload& material,
                    const SceneEvaluationContext& materialContext,
                    const std::string& scope,
                    const std::string& layerTag,
                    SceneGraftMemo* memo,
                    SceneMaterialGraft* graft,
                    std::string* errorMessage)
{
    if (scope.empty()) {
        *graft = SceneMaterialGraft();
        *errorMessage = EffectInstance::tr("The project's Materials Scope is off, so there is no scope to place the material in.").toStdString();

        return false;
    }
    SceneReparentArgs args;
    args.inputs.push_back(material);
    args.destinationPath = "/" + scope;
    args.destinationType = "Scope";
    args.ancestorType = "Scope";
    args.passThroughRoot = scope;
    args.layerTag = layerTag;
    args.inputContexts.push_back(materialContext);

    return graftMaterial(material, args, memo, graft, errorMessage);
}

namespace {
    bool
    reachesTarget(const EffectInstance* consumer,
                  const EffectInstance* target,
                  std::map<const EffectInstance*, bool>* memo)
    {
        if (consumer == target) {
            return true;
        }
        const std::map<const EffectInstance*, bool>::const_iterator known = memo->find(consumer);
        if (known != memo->end()) {
            return known->second;
        }
        bool reaches = false;
        const int nInputs = consumer->getNInputs();
        for (int i = 0; (i < nInputs) && !reaches; ++i) {
            const EffectInstancePtr input = consumer->getInput(i);
            reaches = input && input->producesSceneData() && reachesTarget(input.get(), target, memo);
        }
        (*memo)[consumer] = reaches;

        return reaches;
    }

    // Unites into *united the sets target would evaluate on through every route from consumer.
    void
    mapThroughRoutes(const EffectInstance* consumer,
                     const EffectInstance* target,
                     const SceneEvaluationContext& ctx,
                     double time,
                     ViewIdx view,
                     std::map<const EffectInstance*, bool>* memo,
                     bool* hasRoute,
                     SceneLoadSetPtr* united,
                     U64* firstRouteId,
                     SceneEvaluationContext* firstRoute)
    {
        if (consumer == target) {
            if (!*hasRoute) {
                *firstRouteId = ctx.id;
                *firstRoute = ctx;
            }
            *united = *hasRoute ? SceneLoadSet::unite(*united, ctx.loadSet) : ctx.loadSet;
            *hasRoute = true;

            return;
        }
        // As evaluateScene() does, a node passing a scene through evaluates only that one, on ctx.
        double identityTime = time;
        ViewIdx identityView = view;
        int identityInputNb = -1;
        if (consumer->getSceneIdentity(time, view, &identityTime, &identityView, &identityInputNb)) {
            if (identityInputNb == -2) {
                if (identityTime != time) {
                    mapThroughRoutes(consumer, target, ctx, identityTime, identityView, memo, hasRoute, united, firstRouteId, firstRoute);
                }

                return;
            }
            const EffectInstancePtr source = consumer->getIdentitySource(identityInputNb);
            if (source && source->producesSceneData() && reachesTarget(source.get(), target, memo)) {
                mapThroughRoutes(source.get(), target, ctx, identityTime, identityView, memo, hasRoute, united, firstRouteId, firstRoute);
            }

            return;
        }
        const int nInputs = consumer->getNInputs();
        for (int i = 0; i < nInputs; ++i) {
            const EffectInstancePtr input = consumer->getInput(i);
            if (!input || !input->producesSceneData() || !reachesTarget(input.get(), target, memo)) {
                continue;
            }
            SceneEvaluationContext inputContext;
            try {
                inputContext = consumer->getEvaluatedSceneInputContext(i, ctx, time, view);
            } catch (...) {
                // A mapping that fails is one nothing is known about, which is every payload.
                inputContext = SceneEvaluationContext(ctx.id, SceneLoadSetPtr());
            }
            mapThroughRoutes(input.get(), target, inputContext, time, view, memo, hasRoute, united, firstRouteId, firstRoute);
        }
    }
} // anonymous namespace

bool
mapContextDownToNode(const EffectInstance* consumer,
                     const EffectInstance* target,
                     const SceneEvaluationContext& ctx,
                     double time,
                     ViewIdx view,
                     SceneEvaluationContext* mapped,
                     bool* ownSet)
{
    std::map<const EffectInstance*, bool> memo;
    if (!consumer || !target || !reachesTarget(consumer, target, &memo)) {
        return false;
    }
    bool hasRoute = false;
    SceneLoadSetPtr united;
    U64 firstRouteId = ctx.id;
    SceneEvaluationContext firstRoute;
    mapThroughRoutes(consumer, target, ctx, time, view, &memo, &hasRoute, &united, &firstRouteId, &firstRoute);
    *mapped = SceneEvaluationContext(firstRouteId, united);
    if (ownSet) {
        *ownSet = hasRoute && !mapped->hasSameLoadSet(firstRoute);
    }

    return hasRoute;
}

bool
getShowingViewerContext(EffectInstance* effect,
                        SceneEvaluationContext* context,
                        SceneEvaluationContext* viewerContext,
                        bool* ownSet)
{
    const NodePtr node = effect ? effect->getNode() : NodePtr();
    if (!node) {
        return false;
    }
    std::list<ViewerInstance*> downstream;
    Node::getViewersDownstreamOf(std::list<NodePtr>(1, node), &downstream);
    if (downstream.empty()) {
        return false;
    }
    std::vector<ViewerInstance*> candidates;
    const AppInstancePtr app = effect->getApp();
    const std::list<NodePtr> recent = app ? app->getViewerNodesByRecentActivity() : std::list<NodePtr>();
    for (const NodePtr& viewerNode : recent) {
        for (ViewerInstance* viewer : downstream) {
            if ((viewer->getNode() == viewerNode) && (std::find(candidates.begin(), candidates.end(), viewer) == candidates.end())) {
                candidates.push_back(viewer);
            }
        }
    }
    for (ViewerInstance* viewer : downstream) {
        if (std::find(candidates.begin(), candidates.end(), viewer) == candidates.end()) {
            candidates.push_back(viewer);
        }
    }
    const double time = effect->getCurrentTime();
    for (ViewerInstance* viewer : candidates) {
        const SceneEvaluationContext shown = viewer->getSceneEvaluationContext();
        if (mapContextDownToNode(viewer, effect, shown, time, ViewIdx(0), context, ownSet)) {
            if (viewerContext) {
                *viewerContext = shown;
            }

            return true;
        }
    }

    return false;
}

bool
borrowMenuScene(const EffectInstanceWPtr& input,
                double time,
                const SceneEvaluationContext& context,
                const SceneMenuJob& job,
                ScenePayload* scene,
                SceneQueryPool::Lease* lease)
{
    if (job.isCancelled()) {
        return false;
    }
    EffectInstancePtr effect = input.lock();
    if (!effect) {
        return false;
    }
    std::string error;
    const EffectInstance::RenderRoIRetCode code = effect->evaluateScene(EffectInstance::EvaluateSceneArgs(time, ViewIdx(0), context, job.getAbortInfo()), scene, &error);
    std::shared_ptr<const EffectInstance> held = std::move(effect);
    releaseOnMainThread(&held);
    if ((code != EffectInstance::eRenderRoIRetCodeOk) || scene->isEmpty() || job.isCancelled()) {
        return false;
    }
    *lease = SceneQueryPool::instance().borrow(*scene, context.loadSet, &error);

    return lease->isValid();
}

void
releaseOnMainThread(std::shared_ptr<const EffectInstance>* effect)
{
    QCoreApplication* app = QCoreApplication::instance();
    if (!*effect || !app || (QThread::currentThread() == app->thread())) {
        effect->reset();

        return;
    }
    // A copy left on this thread would drop the last reference here, off the main thread.
    auto release = [held = std::move(*effect)]() mutable { held.reset(); };
    QMetaObject::invokeMethod(app, std::move(release), Qt::QueuedConnection);
}

std::size_t
countMenuUnsearched(const PXR_NS::UsdStageRefPtr& stage,
                    const SceneEvaluationContext& context)
{
    if (!stage || context.includesEverything()) {
        return 0;
    }

    return SceneOps::countUnloadedPayloads(stage, std::vector<std::string>(1, std::string("/")));
}

SceneEvaluationContext
getCameraInputContext(const SceneEvaluationContext& ctx,
                      const std::string& cameraPath)
{
    if (!PXR_NS::SdfPath::IsValidPathString(cameraPath) || !PXR_NS::SdfPath(cameraPath).IsAbsolutePath() || !PXR_NS::SdfPath(cameraPath).IsPrimPath()) {
        return ctx;
    }

    return SceneEvaluationContext(ctx.id, SceneLoadSet::withPaths(std::make_shared<const SceneLoadSet>(SceneLoadSet::none()), std::vector<std::string>(1, cameraPath)));
}

SceneEvaluationContext
getCameraMenuContext(const SceneEvaluationContext& ctx,
                     const std::string& cameraPath)
{
    if (!PXR_NS::SdfPath::IsValidPathString(cameraPath) || !PXR_NS::SdfPath(cameraPath).IsAbsolutePath() || !PXR_NS::SdfPath(cameraPath).IsPrimPath()) {
        return ctx;
    }

    return SceneEvaluationContext(ctx.id, SceneLoadSet::withPaths(ctx.loadSet, std::vector<std::string>(1, cameraPath)), ctx.reach);
}

SceneEvaluationContext
getSharedMaterialContext(const SceneEvaluationContext& ctx,
                         const std::string& scope)
{
    if (scope.empty()) {
        return ctx;
    }

    return SceneEvaluationContext(ctx.id, SceneOps::mapGraftLoadSet(ctx.loadSet, "/" + scope, scope, false));
}

void
mapSharedMaterialReach(SceneGraftMemo* memo,
                       const std::string& scope,
                       const std::vector<std::string>& in,
                       std::vector<std::string>* out)
{
    if (scope.empty()) {
        *out = in;

        return;
    }
    SceneOps::mapGraftReach(memo, 0, "/" + scope, scope, in, out);
}

std::vector<PXR_NS::TfToken>
getCollectionBindingNames(const PXR_NS::UsdPrim& prim)
{
    std::vector<PXR_NS::TfToken> names;

    if (!prim) {
        return names;
    }
    for (const PXR_NS::UsdProperty& property : prim.GetAuthoredPropertiesInNamespace(PXR_NS::UsdShadeTokens->materialBindingCollection.GetString())) {
        if (property.Is<PXR_NS::UsdRelationship>()) {
            names.push_back(property.GetName());
        }
    }

    return names;
}

bool
authorCollectionBinding(const PXR_NS::UsdPrim& root,
                        const SceneCollectionBinding& binding,
                        std::string* errorMessage)
{
    const QString rootPath = QString::fromStdString(root ? root.GetPath().GetString() : std::string());

    if (!PXR_NS::SdfPath::IsValidIdentifier(binding.bindingName)) {
        *errorMessage = EffectInstance::tr("\"%1\" is not a valid binding name.").arg(QString::fromStdString(binding.bindingName)).toStdString();

        return false;
    }
    if (!root || !PXR_NS::SdfPath::IsValidPathString(binding.materialPath) || !PXR_NS::SdfPath(binding.materialPath).IsAbsolutePath()) {
        *errorMessage = EffectInstance::tr("USD could not bind %1 to the material at \"%2\".").arg(rootPath).arg(QString::fromStdString(binding.materialPath)).toStdString();

        return false;
    }
    const PXR_NS::TfToken name(binding.bindingName);
    const PXR_NS::UsdCollectionAPI collection = PXR_NS::UsdCollectionAPI::Apply(root, name);
    const PXR_NS::UsdShadeMaterialBindingAPI bindingAPI = PXR_NS::UsdShadeMaterialBindingAPI::Apply(root);
    bool authored = collection && bindingAPI
        && collection.CreateExpansionRuleAttr(PXR_NS::VtValue(PXR_NS::UsdTokens->expandPrims))
        && collection.CreateIncludesRel().SetTargets(binding.includes)
        && (binding.excludes.empty() || collection.CreateExcludesRel().SetTargets(binding.excludes));
    PXR_NS::TfToken relName;
    if (authored) {
        relName = bindingAPI.GetCollectionBindingRel(name, binding.purpose).GetName();
        const PXR_NS::UsdRelationship rel = root.CreateRelationship(relName, false);
        authored = rel && rel.SetTargets({ collection.GetCollectionPath(), PXR_NS::SdfPath(binding.materialPath) })
            && PXR_NS::UsdShadeMaterialBindingAPI::SetMaterialBindingStrength(rel, binding.strength);
    }
    if (authored) {
        PXR_NS::TfTokenVector order(1, relName);
        for (const PXR_NS::TfToken& existing : binding.existingBindings) {
            if (existing != relName) {
                order.push_back(existing);
            }
        }
        root.SetPropertyOrder(order);
    }
    if (!authored) {
        *errorMessage = EffectInstance::tr("USD could not author the material binding %1 on %2.").arg(QString::fromStdString(binding.bindingName)).arg(rootPath).toStdString();

        return false;
    }

    return true;
} // authorCollectionBinding

bool
isProjectionShader(const PXR_NS::UsdShadeShader& shader)
{
    PXR_NS::SdfAssetPath asset;

    if (!shader || !shader.GetSourceAsset(&asset, PXR_NS::TfToken("glslfx"))) {
        return false;
    }
    const std::string& path = asset.GetAssetPath();
    const std::string file(kUSDProject3DShaderFile);
    if ((path.size() < file.size()) || (path.compare(path.size() - file.size(), file.size(), file) != 0)) {
        return false;
    }

    return (path.size() == file.size()) || (path[path.size() - file.size() - 1] == '/');
}

bool
readGeometryProjection(const PXR_NS::UsdPrim& material,
                       SceneGeometryProjection* projection)
{
    *projection = SceneGeometryProjection();
    if (!material || !material.IsA<PXR_NS::UsdShadeMaterial>()) {
        return false;
    }
    const PXR_NS::UsdShadeShader surface = PXR_NS::UsdShadeMaterial(material).ComputeSurfaceSource();
    if (!isProjectionShader(surface)) {
        return false;
    }
    const PXR_NS::UsdShadeInput space = surface.GetInput(PXR_NS::TfToken(kUSDProject3DInputProjectionSpace));
    int spaceValue = (int)USDProject3D::eProjectionSpaceWorld;
    if (!space || !space.Get(&spaceValue) || (spaceValue != (int)USDProject3D::eProjectionSpaceGeometry)) {
        return false;
    }
    const PXR_NS::UsdShadeInput frame = surface.GetInput(PXR_NS::TfToken(kUSDProject3DInputGeometryFrame));
    PXR_NS::UsdShadeConnectableAPI reader;
    PXR_NS::TfToken readerOutput;
    PXR_NS::UsdShadeAttributeType readerOutputType;
    if (!frame || !frame.GetConnectedSource(&reader, &readerOutput, &readerOutputType)) {
        return false;
    }
    const PXR_NS::UsdShadeInput varname = reader.GetInput(PXR_NS::TfToken("varname"));
    std::string framePrimvar;
    if (!varname || !varname.Get(&framePrimvar) || framePrimvar.empty()) {
        return false;
    }
    projection->framePrimvar = framePrimvar;
    const PXR_NS::UsdAttribute hold = surface.GetPrim().GetAttribute(PXR_NS::TfToken(kUSDProject3DAttrFrameHold));
    projection->held = hold && hold.Get(&projection->holdFrame);
    const PXR_NS::UsdShadeInput reference = surface.GetInput(PXR_NS::TfToken(kUSDProject3DInputReferencePoints));
    PXR_NS::UsdShadeConnectableAPI referenceReader;
    if (reference && reference.GetConnectedSource(&referenceReader, &readerOutput, &readerOutputType)) {
        const PXR_NS::UsdShadeInput referenceName = referenceReader.GetInput(PXR_NS::TfToken("varname"));
        if (referenceName) {
            referenceName.Get(&projection->referencePrimvar);
        }
    }

    return true;
}

bool
authorProjectionFrame(const PXR_NS::UsdPrim& prim,
                      const SceneLayerEdit& edit,
                      const SceneGeometryProjection& projection,
                      bool animated,
                      const std::function<PXR_NS::GfMatrix4d(double)>& frameAt)
{
    if (projection.framePrimvar.empty()) {
        return true;
    }
    const PXR_NS::UsdGeomPrimvarsAPI primvars(prim);
    std::vector<PXR_NS::UsdAttribute> attributes(1, primvars.CreatePrimvar(PXR_NS::TfToken(projection.framePrimvar), PXR_NS::SdfValueTypeNames->Matrix4d, PXR_NS::UsdGeomTokens->constant).GetAttr());
    for (int column = 0; column < 3; ++column) {
        const PXR_NS::TfToken name(SceneProjectionCameras::frameColumnPrimvar(projection.framePrimvar, column));
        attributes.push_back(primvars.CreatePrimvar(name, PXR_NS::SdfValueTypeNames->Float4Array, PXR_NS::UsdGeomTokens->constant).GetAttr());
    }
    for (int part = 0; part < 4; ++part) {
        const PXR_NS::TfToken name(SceneProjectionCameras::moonRayFramePrimvar(projection.framePrimvar, part));
        attributes.push_back(primvars.CreatePrimvar(name, PXR_NS::SdfValueTypeNames->Float3, PXR_NS::UsdGeomTokens->constant).GetAttr());
    }
    const auto setFrame = [&attributes](const PXR_NS::GfMatrix4d& frame, PXR_NS::UsdTimeCode time) {
        bool ok = attributes[0] && attributes[0].Set(frame, time);
        for (int column = 0; ok && (column < 3); ++column) {
            ok = attributes[column + 1] && attributes[column + 1].Set(SceneProjectionCameras::frameColumn(frame, column), time);
        }
        for (int part = 0; ok && (part < 4); ++part) {
            ok = attributes[part + 4] && attributes[part + 4].Set(SceneProjectionCameras::moonRayFramePart(frame, part), time);
        }

        return ok;
    };
    if (projection.held) {
        return setFrame(frameAt(projection.holdFrame), PXR_NS::UsdTimeCode::Default());
    }
    if (!animated) {
        return setFrame(frameAt(edit.firstFrame), PXR_NS::UsdTimeCode::Default());
    }
    for (double frame : getSampleFrames(edit)) {
        if (!setFrame(frameAt(frame), frameToTimeCode(edit, frame))) {
            return false;
        }
    }

    return true;
}

namespace {
    // The samples inside the range miss a value held into it from a sample outside, so its ends
    // are read too.
    std::vector<double>
    getTimesOverRange(const PXR_NS::UsdAttribute& attribute,
                      const SceneReferenceContext& context)
    {
        std::vector<double> times;
        attribute.GetTimeSamplesInInterval(PXR_NS::GfInterval(context.firstTime, context.lastTime), &times);
        times.push_back(context.firstTime);
        times.push_back(context.lastTime);

        return times;
    }

    // By value: a cache can re-sample unchanged arrays, and USD then reports them as time-varying.
    template <typename ArrayType>
    bool
    holdsItsValueOverRange(const PXR_NS::UsdAttribute& attribute,
                           const SceneReferenceContext& context)
    {
        if (!attribute || !attribute.HasValue()) {
            return true;
        }
        ArrayType reference;
        attribute.Get(&reference, context.holdTime);
        for (const double time : getTimesOverRange(attribute, context)) {
            ArrayType value;
            if (!attribute.Get(&value, time) || (value != reference)) {
                return false;
            }
        }

        return true;
    }

    bool
    topologyHoldsOverRange(const PXR_NS::UsdPrim& prim,
                           const PXR_NS::UsdAttribute& pointsAttribute,
                           std::size_t pointCount,
                           const SceneReferenceContext& context)
    {
        if (prim.IsA<PXR_NS::UsdGeomMesh>()) {
            const PXR_NS::UsdGeomMesh mesh(prim);
            if (!holdsItsValueOverRange<PXR_NS::VtIntArray>(mesh.GetFaceVertexCountsAttr(), context)
                || !holdsItsValueOverRange<PXR_NS::VtIntArray>(mesh.GetFaceVertexIndicesAttr(), context)) {
                return false;
            }
        } else if (prim.IsA<PXR_NS::UsdGeomCurves>()) {
            if (!holdsItsValueOverRange<PXR_NS::VtIntArray>(PXR_NS::UsdGeomCurves(prim).GetCurveVertexCountsAttr(), context)) {
                return false;
            }
        } else if (prim.IsA<PXR_NS::UsdGeomPoints>() && !holdsItsValueOverRange<PXR_NS::VtInt64Array>(PXR_NS::UsdGeomPoints(prim).GetIdsAttr(), context)) {
            return false;
        }
        // Unchanged indices can still come with more or fewer unused points, and Storm drops a
        // vertex primvar whose size differs from the points'.
        const std::vector<double> times = getTimesOverRange(pointsAttribute, context);
        PXR_NS::VtVec3fArray value;
        for (const double time : times) {
            if (!pointsAttribute.Get(&value, time) || (value.size() != pointCount)) {
                return false;
            }
        }

        return true;
    }

    bool
    isBoundToSkin(const PXR_NS::UsdPrim& prim)
    {
        const PXR_NS::UsdSkelBindingAPI binding(prim);
        if (!binding.GetInheritedSkeleton()) {
            return false;
        }
        // A skeleton inherited from far up reaches props with no weights too, which it does not move.
        const PXR_NS::UsdGeomPrimvar indices = PXR_NS::UsdGeomPrimvarsAPI(prim).FindPrimvarWithInheritance(PXR_NS::TfToken("skel:jointIndices"));
        const PXR_NS::UsdAttribute shapes = binding.GetBlendShapesAttr();

        return (indices && indices.HasValue()) || (shapes && shapes.HasValue());
    }

    bool
    applyBlendShapes(const PXR_NS::UsdSkelSkinningQuery& skinning,
                     const PXR_NS::UsdSkelSkeletonQuery& skeleton,
                     double time,
                     PXR_NS::VtVec3fArray* points)
    {
        const PXR_NS::UsdSkelAnimQuery& animation = skeleton.GetAnimQuery();
        PXR_NS::VtFloatArray animationWeights;
        if (!animation || !animation.ComputeBlendShapeWeights(&animationWeights, PXR_NS::UsdTimeCode(time))) {
            return true;
        }
        PXR_NS::VtFloatArray weights;
        const PXR_NS::UsdSkelAnimMapperRefPtr& mapper = skinning.GetBlendShapeMapper();
        if (!mapper || !mapper->Remap(animationWeights, &weights)) {
            return false;
        }
        const PXR_NS::UsdSkelBlendShapeQuery shapes { PXR_NS::UsdSkelBindingAPI(skinning.GetPrim()) };
        PXR_NS::VtFloatArray subShapeWeights;
        PXR_NS::VtUIntArray shapeIndices;
        PXR_NS::VtUIntArray subShapeIndices;
        if (!shapes.ComputeSubShapeWeights(weights, &subShapeWeights, &shapeIndices, &subShapeIndices)) {
            return false;
        }

        return shapes.ComputeDeformedPoints(subShapeWeights, shapeIndices, subShapeIndices,
                                            shapes.ComputeBlendShapePointIndices(), shapes.ComputeSubShapePointOffsets(), *points);
    }

    // Joint-skinned points come out in the skeleton's space: the prim's own transform plays no part, as
    // in Storm. *space gets the prim whose local to world carries them on.
    bool
    computeSkinnedPoints(const PXR_NS::UsdPrim& prim,
                         SceneReferenceContext* context,
                         PXR_NS::VtVec3fArray* points,
                         PXR_NS::UsdPrim* space)
    {
        const PXR_NS::UsdSkelRoot root = PXR_NS::UsdSkelRoot::Find(prim);
        if (!root) {
            return false;
        }
        if (context->populatedRoots.insert(root.GetPath()).second && !context->skinning.Populate(root, PXR_NS::UsdTraverseInstanceProxies())) {
            return false;
        }
        const PXR_NS::UsdSkelSkinningQuery skinning = context->skinning.GetSkinningQuery(prim);
        const PXR_NS::UsdSkelSkeleton bound = PXR_NS::UsdSkelBindingAPI(prim).GetInheritedSkeleton();
        const PXR_NS::UsdSkelSkeletonQuery skeleton = context->skinning.GetSkelQuery(bound);
        if (!skinning || !skeleton) {
            return false;
        }
        if (skinning.HasBlendShapes() && !applyBlendShapes(skinning, skeleton, context->holdTime, points)) {
            return false;
        }
        if (!skinning.HasJointInfluences()) {
            *space = prim;

            return true;
        }
        PXR_NS::VtMatrix4dArray joints;
        if (!skeleton.ComputeSkinningTransforms(&joints, PXR_NS::UsdTimeCode(context->holdTime))
            || !skinning.ComputeSkinnedPoints(joints, points, PXR_NS::UsdTimeCode(context->holdTime))) {
            return false;
        }
        *space = bound.GetPrim();

        return true;
    }
} // anonymous namespace

SceneReferencePoints
readReferencePoints(const PXR_NS::UsdPrim& prim,
                    SceneReferenceContext* context,
                    PXR_NS::UsdGeomXformCache* xforms)
{
    SceneReferencePoints result;
    if (!prim.IsA<PXR_NS::UsdGeomPointBased>()) {
        return result;
    }
    const PXR_NS::UsdAttribute pointsAttribute = PXR_NS::UsdGeomPointBased(prim).GetPointsAttr();
    const bool skinned = isBoundToSkin(prim);
    if (!skinned && !pointsAttribute.ValueMightBeTimeVarying()) {
        return result;
    }
    PXR_NS::VtVec3fArray points;
    if (!pointsAttribute.Get(&points, context->holdTime) || points.empty()) {
        return result;
    }
    if (!topologyHoldsOverRange(prim, pointsAttribute, points.size(), *context)) {
        result.status = SceneReferencePoints::eStatusTopologyChanges;

        return result;
    }
    PXR_NS::UsdPrim space = prim;
    if (skinned && !computeSkinnedPoints(prim, context, &points, &space)) {
        result.status = SceneReferencePoints::eStatusCannotSkin;

        return result;
    }
    const PXR_NS::GfMatrix4d toWorld = xforms->GetLocalToWorldTransform(space);
    for (PXR_NS::GfVec3f& point : points) {
        point = PXR_NS::GfVec3f(toWorld.Transform(PXR_NS::GfVec3d(point)));
    }
    result.status = SceneReferencePoints::eStatusRead;
    result.points = points;

    return result;
}

bool
authorReferencePoints(const PXR_NS::UsdPrim& prim,
                      const SceneGeometryProjection& projection,
                      const PXR_NS::VtVec3fArray& points)
{
    const PXR_NS::UsdGeomPrimvarsAPI primvars(prim);
    const PXR_NS::UsdGeomPrimvar primvar = primvars.CreatePrimvar(PXR_NS::TfToken(projection.referencePrimvar), PXR_NS::SdfValueTypeNames->Point3fArray, PXR_NS::UsdGeomTokens->vertex);
    // A one-element array, not a float: Cycles copies a primvar by its array size, so a scalar
    // reaches its shaders as zero.
    const PXR_NS::UsdGeomPrimvar present = primvars.CreatePrimvar(PXR_NS::TfToken(SceneProjectionCameras::cyclesReferencePresentPrimvar(projection.referencePrimvar)), PXR_NS::SdfValueTypeNames->FloatArray, PXR_NS::UsdGeomTokens->constant);

    return primvar && primvar.Set(points) && present && present.Set(PXR_NS::VtFloatArray(1, 1.f));
}

bool
authorMaterialBinding(const PXR_NS::UsdPrim& prim,
                      const std::string& materialPath,
                      std::string* errorMessage,
                      const PXR_NS::TfToken& purpose,
                      const PXR_NS::TfToken& strength)
{
    if (materialPath.empty()) {
        return true;
    }
    const PXR_NS::UsdShadeMaterialBindingAPI binding = prim ? PXR_NS::UsdShadeMaterialBindingAPI::Apply(prim) : PXR_NS::UsdShadeMaterialBindingAPI();
    const bool allPurpose = purpose.IsEmpty() || (purpose == PXR_NS::UsdShadeTokens->allPurpose);
    const PXR_NS::TfToken relName = allPurpose ? PXR_NS::UsdShadeTokens->materialBinding : PXR_NS::TfToken(PXR_NS::UsdShadeTokens->materialBinding.GetString() + ":" + purpose.GetString());
    const PXR_NS::UsdRelationship rel = binding ? prim.CreateRelationship(relName, false) : PXR_NS::UsdRelationship();
    if (!rel || !rel.SetTargets({ PXR_NS::SdfPath(materialPath) }) || (!strength.IsEmpty() && !PXR_NS::UsdShadeMaterialBindingAPI::SetMaterialBindingStrength(rel, strength))) {
        *errorMessage = EffectInstance::tr("USD could not bind %1 to the material at %2.").arg(QString::fromStdString(prim ? prim.GetPath().GetString() : std::string())).arg(QString::fromStdString(materialPath)).toStdString();

        return false;
    }

    return true;
}

int
getRotateOrderAxis(SceneRotateOrderEnum rotateOrder,
                   int position)
{
    static const char* const kOrders[] = { "XYZ", "XZY", "YXZ", "YZX", "ZXY", "ZYX" };
    const int order = ((int)rotateOrder >= 0 && (int)rotateOrder < 6) ? (int)rotateOrder : 0;

    return kOrders[order][std::min(std::max(position, 0), 2)] - 'X';
}

PXR_NS::GfMatrix4d
composeRotationMatrix(const PXR_NS::GfVec3d& rotate,
                      SceneRotateOrderEnum rotateOrder)
{
    // With row vectors the leftmost factor applies first, as the order's first axis does.
    PXR_NS::GfMatrix4d rotation(1.);
    for (int i = 0; i < 3; ++i) {
        const int axis = getRotateOrderAxis(rotateOrder, i);
        PXR_NS::GfVec3d direction(0.);
        direction[axis] = 1.;
        PXR_NS::GfMatrix4d factor(1.);
        factor.SetRotate(PXR_NS::GfRotation(direction, rotate[axis]));
        rotation = rotation * factor;
    }

    return rotation;
}

PXR_NS::GfVec3d
decomposeRotationMatrix(const PXR_NS::GfMatrix4d& rotation,
                        SceneRotateOrderEnum rotateOrder,
                        const PXR_NS::GfVec3d& hint)
{
    const int i = getRotateOrderAxis(rotateOrder, 0);
    const int j = getRotateOrderAxis(rotateOrder, 1);
    const int k = getRotateOrderAxis(rotateOrder, 2);
    // In column-vector terms the rotation is Rk Rj Ri, and r(a, b) is its element at row a,
    // column b: the transpose of Gf's row-vector matrix.
    const auto r = [&rotation](int a, int b) {
        return rotation[b][a];
    };
    // +1 when the order is a cyclic permutation of XYZ.
    const double s = ((j - i + 3) % 3 == 1) ? 1. : -1.;
    const double sinB = std::min(1., std::max(-1., -s * r(k, i)));
    double a, b, c;
    b = std::asin(sinB);
    if (std::abs(r(k, i)) < 1. - 1e-10) {
        a = std::atan2(s * r(k, j), r(k, k));
        c = std::atan2(s * r(j, i), r(i, i));
    } else {
        // Gimbal lock: only the sum or difference of the outer angles is defined.
        c = 0.;
        a = std::atan2(-s * r(j, k), r(j, j));
    }
    const double toDegrees = 180. / M_PI;
    // Two sets of angles give each rotation: (a, b, c) and (a + 180, 180 - b, c + 180).
    PXR_NS::GfVec3d candidates[2];
    candidates[0][i] = a * toDegrees;
    candidates[0][j] = b * toDegrees;
    candidates[0][k] = c * toDegrees;
    candidates[1][i] = a * toDegrees + 180.;
    candidates[1][j] = 180. - b * toDegrees;
    candidates[1][k] = c * toDegrees + 180.;
    PXR_NS::GfVec3d best;
    double bestDistance = -1.;
    for (PXR_NS::GfVec3d& candidate : candidates) {
        double distance = 0.;
        for (int axis = 0; axis < 3; ++axis) {
            candidate[axis] += 360. * std::round((hint[axis] - candidate[axis]) / 360.);
            distance += std::abs(candidate[axis] - hint[axis]);
        }
        if ((bestDistance < 0.) || (distance < bestDistance)) {
            best = candidate;
            bestDistance = distance;
        }
    }

    return best;
} // decomposeRotationMatrix

PXR_NS::GfMatrix4d
composeTransformMatrix(const PXR_NS::GfVec3d& translate,
                       const PXR_NS::GfVec3d& rotate,
                       const PXR_NS::GfVec3d& scale,
                       const PXR_NS::GfVec3d& pivot,
                       SceneRotateOrderEnum rotateOrder)
{
    PXR_NS::GfMatrix4d toPivot(1.);
    toPivot.SetTranslate(-pivot);
    PXR_NS::GfMatrix4d scaling(1.);
    scaling.SetScale(scale);
    PXR_NS::GfMatrix4d fromPivot(1.);
    fromPivot.SetTranslate(pivot + translate);

    return toPivot * scaling * composeRotationMatrix(rotate, rotateOrder) * fromPivot;
}

bool
isTransformAnimated(const SceneTransformKnobs& knobs,
                    const KnobDoublePtr& pivot)
{
    return isAnimated(knobs.translate.lock()) || isAnimated(knobs.rotate.lock()) || isAnimated(knobs.scale.lock())
        || isAnimated(knobs.uniformScale.lock()) || isAnimated(pivot);
}

PXR_NS::GfMatrix4d
getTransformMatrixAt(const SceneTransformKnobs& knobs,
                     const KnobDoublePtr& pivot,
                     double frame)
{
    KnobDoublePtr translate = knobs.translate.lock();
    KnobDoublePtr rotate = knobs.rotate.lock();
    KnobDoublePtr scale = knobs.scale.lock();
    KnobDoublePtr uniformScale = knobs.uniformScale.lock();
    KnobChoicePtr rotateOrder = knobs.rotateOrder.lock();

    if (!translate || !rotate || !scale || !uniformScale || !rotateOrder || !pivot) {
        return PXR_NS::GfMatrix4d(1.);
    }
    const double uniform = uniformScale->getValueAtTime(frame, 0);

    return composeTransformMatrix(PXR_NS::GfVec3d(translate->getValueAtTime(frame, 0), translate->getValueAtTime(frame, 1), translate->getValueAtTime(frame, 2)),
                                  PXR_NS::GfVec3d(rotate->getValueAtTime(frame, 0), rotate->getValueAtTime(frame, 1), rotate->getValueAtTime(frame, 2)),
                                  PXR_NS::GfVec3d(scale->getValueAtTime(frame, 0) * uniform, scale->getValueAtTime(frame, 1) * uniform, scale->getValueAtTime(frame, 2) * uniform),
                                  PXR_NS::GfVec3d(pivot->getValueAtTime(frame, 0), pivot->getValueAtTime(frame, 1), pivot->getValueAtTime(frame, 2)),
                                  (SceneRotateOrderEnum)rotateOrder->getValue());
}

namespace {
    /**
     * The tiles effect declares at frame (kNatronMetadataKeyUdimTiles), none when it declares none.
     * False, with no tiles and the reason in error, when its declaration does not parse.
     **/
    bool
    readDeclaredTiles(const EffectInstancePtr& effect,
                      double frame,
                      std::vector<int>* tiles,
                      std::string* error)
    {
        tiles->clear();
        const ImageMetadata metadata = getEffectOutputMetadata(effect, frame, ViewIdx(0));
        const ImageMetadata::Value* declared = metadata.find(kNatronMetadataKeyUdimTiles);
        if (!declared || parseUdimTiles(*declared, tiles, error)) {
            return true;
        }
        tiles->clear();
        if (error->empty()) {
            *error = "the declaration is malformed";
        }

        return false;
    }

    // "frame 2" or "frames 2, 4-6": frames, ascending, with runs of consecutive ones joined.
    QString
    describeFrames(const std::vector<int>& frames)
    {
        QStringList runs;

        for (std::size_t i = 0; i < frames.size();) {
            std::size_t end = i + 1;
            while ((end < frames.size()) && (frames[end] == frames[end - 1] + 1)) {
                ++end;
            }
            runs.push_back((end - i == 1) ? QString::number(frames[i]) : QString::fromUtf8("%1-%2").arg(frames[i]).arg(frames[end - 1]));
            i = end;
        }

        return ((frames.size() == 1) ? EffectInstance::tr("frame %1") : EffectInstance::tr("frames %1")).arg(runs.join(QString::fromUtf8(", ")));
    }

    // What a varying source declares over its frames, as LiveTextureSource holds it.
    struct DeclaredTiles {
        // Null when no frame declares tiles.
        std::shared_ptr<const std::vector<std::vector<int>>> frameTiles;
        std::vector<int> tiles;
        std::string missingTilesWarning;
        // Why a declaration does not parse, when no frame declares tiles.
        std::string error;
    };

    /**
     * The tiles effect, named nodeName, declares at each frame of [firstFrame, lastFrame], the frames
     * as LiveTextureSource::getSourceFrame() holds them. A UDIM set when any frame declares tiles.
     **/
    DeclaredTiles
    readDeclaredTilesOverFrames(const EffectInstancePtr& effect,
                                const std::string& nodeName,
                                U64 contentHash,
                                double firstFrame,
                                double lastFrame)
    {
        // Every evaluation of a consumer reads the whole range, which only an edit of the source, and
        // so its hash, or of the range changes.
        typedef std::tuple<const EffectInstance*, std::string, U64, double, double> Key;
        static std::mutex cacheMutex;
        static std::map<Key, DeclaredTiles> cache;
        const Key key(effect.get(), nodeName, contentHash, firstFrame, lastFrame);
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            const std::map<Key, DeclaredTiles>::const_iterator found = cache.find(key);
            if (found != cache.end()) {
                return found->second;
            }
        }

        DeclaredTiles result;
        const double first = std::ceil(firstFrame);
        const double last = std::max(first, std::floor(lastFrame));
        std::shared_ptr<std::vector<std::vector<int>>> frameTiles = std::make_shared<std::vector<std::vector<int>>>();
        frameTiles->reserve((std::size_t)(last - first) + 1);
        std::set<int> every;
        std::vector<int> undeclared;
        std::vector<int> malformed;
        std::string firstError;
        for (double frame = first; frame <= last; frame += 1.) {
            std::vector<int> tiles;
            std::string error;
            if (!readDeclaredTiles(effect, frame, &tiles, &error)) {
                malformed.push_back((int)frame);
                if (firstError.empty()) {
                    firstError = error;
                }
            } else if (tiles.empty()) {
                undeclared.push_back((int)frame);
            }
            every.insert(tiles.begin(), tiles.end());
            frameTiles->push_back(std::move(tiles));
        }
        result.tiles.assign(every.begin(), every.end());
        if (result.tiles.empty()) {
            if (!malformed.empty()) {
                result.error = EffectInstance::tr("at %1, %2").arg(describeFrames(std::vector<int>(1, malformed[0]))).arg(QString::fromStdString(firstError)).toStdString();
            }
        } else {
            result.frameTiles = frameTiles;
            QStringList warnings;
            if (!undeclared.empty()) {
                warnings.push_back(EffectInstance::tr("%1 declares no UDIM tiles at %2, where its live texture shows nothing.")
                                       .arg(QString::fromStdString(nodeName))
                                       .arg(describeFrames(undeclared)));
            }
            if (!malformed.empty()) {
                warnings.push_back(EffectInstance::tr("The UDIM tiles %1 declares at %2 do not parse (%3), so its live texture shows nothing there.")
                                       .arg(QString::fromStdString(nodeName))
                                       .arg(describeFrames(malformed))
                                       .arg(QString::fromStdString(firstError)));
            }
            result.missingTilesWarning = warnings.join(QLatin1Char(' ')).toStdString();
        }
        {
            std::lock_guard<std::mutex> lock(cacheMutex);
            if (cache.size() >= 64) {
                cache.clear();
            }
            cache[key] = result;
        }

        return result;
    } // readDeclaredTilesOverFrames
} // anonymous namespace

namespace {
    // The knob's value: one "<input>\t<source name>\n" per input that lost its source.
    std::map<int, std::string>
    parseDeletedTextureSources(const std::string& value)
    {
        std::map<int, std::string> sources;
        std::istringstream lines(value);

        for (std::string line; std::getline(lines, line);) {
            const std::size_t tab = line.find('\t');
            if ((tab != std::string::npos) && (tab + 1 < line.size())) {
                sources[std::atoi(line.c_str())] = line.substr(tab + 1);
            }
        }

        return sources;
    }

    void
    setDeletedTextureSources(const KnobStringPtr& knob,
                             const std::map<int, std::string>& sources)
    {
        std::string value;

        for (const std::pair<const int, std::string>& source : sources) {
            value += std::to_string(source.first) + "\t" + source.second + "\n";
        }
        if (value != knob->getValue()) {
            knob->setValue(value);
        }
    }
} // anonymous namespace

void
createDeletedTextureSourcesKnob(KnobHolder* holder,
                                const KnobPagePtr& page)
{
    KnobStringPtr knob = AppManager::createKnob<KnobString>(holder, EffectInstance::tr("Deleted Texture Sources"));

    knob->setName(kSceneParamDeletedTextureSources);
    knob->setHintToolTip(EffectInstance::tr("The image inputs whose node was deleted, which show the missing-texture colour until something is connected to them."));
    knob->setAnimationEnabled(false);
    knob->setSecretByDefault(true);
    page->addKnob(knob);
}

void
recordDeletedTextureSource(const EffectInstance& consumer,
                           int inputNb,
                           const std::string& sourceName)
{
    const KnobStringPtr knob = consumer.getKnobByNameAndType<KnobString>(kSceneParamDeletedTextureSources);

    if (knob) {
        std::map<int, std::string> sources = parseDeletedTextureSources(knob->getValue());
        sources[inputNb] = sourceName;
        setDeletedTextureSources(knob, sources);
    }
}

void
forgetDeletedTextureSourceIfConnected(const EffectInstance& consumer,
                                      int inputNb)
{
    const KnobStringPtr knob = consumer.getKnobByNameAndType<KnobString>(kSceneParamDeletedTextureSources);
    const NodePtr node = consumer.getNode();

    // The real input: a group's Input node counts as connected even with nothing outside the group.
    if (knob && node && node->getRealInput(inputNb)) {
        std::map<int, std::string> sources = parseDeletedTextureSources(knob->getValue());
        sources.erase(inputNb);
        setDeletedTextureSources(knob, sources);
    }
}

bool
getLiveTextureSource(const EffectInstance& consumer,
                     int inputNb,
                     const std::string& layer,
                     LiveTextureSource* source,
                     std::string* udimError)
{
    const EffectInstancePtr connected = consumer.getInput(inputNb);
    const double time = consumer.getCurrentTime();
    source->tiles.clear();
    source->frameTiles.reset();
    source->missingTilesWarning.clear();
    if (udimError) {
        udimError->clear();
    }
    if (!connected) {
        const KnobStringPtr knob = consumer.getKnobByNameAndType<KnobString>(kSceneParamDeletedTextureSources);
        const std::map<int, std::string> deleted = knob ? parseDeletedTextureSources(knob->getValue()) : std::map<int, std::string>();
        const std::map<int, std::string>::const_iterator found = deleted.find(inputNb);
        if (found == deleted.end()) {
            return false;
        }
        source->node.reset();
        source->consumer = consumer.getNode();
        source->nodeName = found->second;
        source->layer = layer;
        source->contentHash = 0;
        source->varying = false;
        const AppInstancePtr app = consumer.getApp();
        if (app && app->getProject()) {
            app->getProject()->getFrameRange(&source->firstFrame, &source->lastFrame);
        }

        return true;
    }
    // A disabled node renders as its preferred input, so the texture shows what the render would.
    const NodePtr node = resolveDisabledNode(connected->getNode(), time);
    const EffectInstancePtr input = node ? node->getEffectInstance() : EffectInstancePtr();
    if (!input) {
        return false;
    }
    source->node = node;
    source->consumer = consumer.getNode();
    source->nodeName = node->getFullyQualifiedName();
    source->layer = layer;
    source->contentHash = input->getHash();
    source->varying = input->isFrameVaryingOrAnimated_Recursive();
    const AppInstancePtr app = consumer.getApp();
    if (app && app->getProject()) {
        app->getProject()->getFrameRange(&source->firstFrame, &source->lastFrame);
    }
    if (udimError) {
        if (source->varying) {
            const DeclaredTiles declared = readDeclaredTilesOverFrames(input, source->nodeName, source->contentHash, source->firstFrame, source->lastFrame);
            source->tiles = declared.tiles;
            source->frameTiles = declared.frameTiles;
            source->missingTilesWarning = declared.missingTilesWarning;
            *udimError = declared.error;
        } else {
            readDeclaredTiles(input, source->getSourceFrame(time), &source->tiles, udimError);
        }
    }

    return true;
}

std::string
getLiveTextureFailure(const LiveTextureSource& source,
                      double time)
{
    const NodePtr node = source.node.lock();

    if (!source.nodeName.empty() && (!node || !node->isActivated())) {
        return "the node " + source.nodeName + " that feeds it no longer exists";
    }

    return LiveTextureStore::instance().getFailure(source.getUri(time));
}

void
appendLiveTextureSource(const LiveTextureSource& source,
                        ContentHasher* h)
{
    h->appendString(source.nodeName);
    h->appendString(source.layer);
    h->appendU64(source.contentHash);
    h->appendBool(source.varying);
    h->appendU64(source.tiles.size());
    for (int tile : source.tiles) {
        h->appendI64(tile);
    }
}

std::vector<std::string>
getLiveTextureWarnings(const LiveTextureSource& source,
                       double time)
{
    std::vector<std::string> warnings;
    const double frame = source.getSourceFrame(time);
    const std::string uri = source.getUri(frame);
    const std::string failure = getLiveTextureFailure(source, frame);

    if (!failure.empty()) {
        if (source.isUdim()) {
            const std::vector<int> tiles = LiveTextureStore::instance().getFailedTiles(uri);
            QString numbers;
            for (int tile : tiles) {
                numbers += (numbers.isEmpty() ? QString() : QString::fromLatin1(", ")) + QString::number(tile);
            }
            if (tiles.size() == 1) {
                warnings.push_back(EffectInstance::tr("The UDIM tile %1 of the live texture %2 shows magenta: %3.")
                                       .arg(numbers)
                                       .arg(QString::fromStdString(uri))
                                       .arg(QString::fromStdString(failure))
                                       .toStdString());
            } else if (!tiles.empty()) {
                warnings.push_back(EffectInstance::tr("The UDIM tiles %1 of the live texture %2 show magenta: %3.")
                                       .arg(numbers)
                                       .arg(QString::fromStdString(uri))
                                       .arg(QString::fromStdString(failure))
                                       .toStdString());
            } else {
                warnings.push_back(EffectInstance::tr("The UDIM tiles of the live texture %1 that could not be rendered show magenta: %2.")
                                       .arg(QString::fromStdString(uri))
                                       .arg(QString::fromStdString(failure))
                                       .toStdString());
            }
        } else {
            warnings.push_back(EffectInstance::tr("The live texture %1 shows magenta, the missing-texture colour: %2.")
                                   .arg(QString::fromStdString(uri))
                                   .arg(QString::fromStdString(failure))
                                   .toStdString());
        }
    }
    const std::string beyond = source.getTilesBeyondUsdWarning(frame);
    if (!beyond.empty()) {
        warnings.push_back(beyond);
    }
    if (source.isUdim() && !source.missingTilesWarning.empty()) {
        warnings.push_back(source.missingTilesWarning);
    }

    return warnings;
}

LiveTextureSource
recordLiveTexture(const SceneLayerEdit& edit,
                  const LiveTextureSource& source)
{
    LiveTextureSource recorded = source;

    recorded.firstFrame = edit.firstFrame;
    recorded.lastFrame = edit.lastFrame;
    recorded.windows.reset();
    const NodePtr node = source.node.lock();
    if (recorded.isUdim()) {
        const EffectInstancePtr effect = node ? node->getEffectInstance() : EffectInstancePtr();
        // frameTiles is laid out from the source's first frame, which is the project's, as edit's
        // normally is too.
        if (source.varying && effect && ((edit.firstFrame != source.firstFrame) || (edit.lastFrame != source.lastFrame))) {
            const DeclaredTiles declared = readDeclaredTilesOverFrames(effect, source.nodeName, source.contentHash, edit.firstFrame, edit.lastFrame);
            recorded.tiles = declared.tiles;
            recorded.frameTiles = declared.frameTiles;
            recorded.missingTilesWarning = declared.missingTilesWarning;
        }
        if (edit.liveTextures) {
            edit.liveTextures->push_back(recorded);
        }

        return recorded;
    }
    const std::vector<double> frames = source.varying ? getSampleFrames(edit) : std::vector<double>(1, recorded.getSourceFrame(edit.firstFrame));
    std::shared_ptr<std::vector<LiveTextureWindow>> windows = std::make_shared<std::vector<LiveTextureWindow>>();
    windows->reserve(frames.size());
    for (double frame : frames) {
        LiveTextureWindow window;
        if (!node || !getNodeImageWindow(node, frame, ViewIdx(0), &window.format, &window.window)) {
            window = LiveTextureWindow();
        }
        windows->push_back(window);
    }
    recorded.windows = windows;
    if (edit.liveTextures) {
        edit.liveTextures->push_back(recorded);
    }

    return recorded;
}

bool
connectLiveTextureCoordinates(const PXR_NS::UsdStageRefPtr& stage,
                              const SceneLayerEdit& edit,
                              const PXR_NS::SdfPath& transformPath,
                              const LiveTextureSource& recorded,
                              const PXR_NS::UsdShadeOutput& coordinates,
                              const PXR_NS::UsdShadeInput& st)
{
    // A UDIM set's tiles are each their own format and are picked by the coordinates' integer part,
    // which a transform would shift.
    if (recorded.isUdim()) {
        return coordinates && st && st.ConnectToSource(coordinates);
    }
    PXR_NS::UsdShadeShader transform = PXR_NS::UsdShadeShader::Define(stage, transformPath);

    if (!transform || !transform.CreateIdAttr(PXR_NS::VtValue(PXR_NS::TfToken("UsdTransform2d")))) {
        return false;
    }
    const PXR_NS::UsdShadeInput in = transform.CreateInput(PXR_NS::TfToken("in"), PXR_NS::SdfValueTypeNames->Float2);
    const PXR_NS::UsdShadeInput scale = transform.CreateInput(PXR_NS::TfToken("scale"), PXR_NS::SdfValueTypeNames->Float2);
    const PXR_NS::UsdShadeInput translation = transform.CreateInput(PXR_NS::TfToken("translation"), PXR_NS::SdfValueTypeNames->Float2);
    const PXR_NS::UsdShadeOutput result = transform.CreateOutput(PXR_NS::TfToken("result"), PXR_NS::SdfValueTypeNames->Float2);
    if (!in || !scale || !translation || !result || !coordinates || !st) {
        return false;
    }

    const bool windowVaries = liveTextureWindowVaries(recorded);
    const auto scaleAt = [&recorded](double frame) {
        double s[2];
        double t[2];
        recorded.getWindow(frame).getUvTransform(s, t);

        return PXR_NS::GfVec2f((float)s[0], (float)s[1]);
    };
    const auto translationAt = [&recorded](double frame) {
        double s[2];
        double t[2];
        recorded.getWindow(frame).getUvTransform(s, t);

        return PXR_NS::GfVec2f((float)t[0], (float)t[1]);
    };

    return in.ConnectToSource(coordinates)
        && authorValue(scale.GetAttr(), edit, windowVaries, scaleAt)
        && authorValue(translation.GetAttr(), edit, windowVaries, translationAt)
        && st.ConnectToSource(result);
} // connectLiveTextureCoordinates

bool
connectOpacityToAlpha(PXR_NS::UsdShadeShader surface,
                      PXR_NS::UsdShadeShader texture)
{
    const PXR_NS::UsdShadeOutput alpha = texture.CreateOutput(PXR_NS::TfToken("a"), PXR_NS::SdfValueTypeNames->Float);
    PXR_NS::UsdShadeInput opacity = surface.CreateInput(PXR_NS::TfToken("opacity"), PXR_NS::SdfValueTypeNames->Float);
    const PXR_NS::UsdShadeInput opacityMode = surface.CreateInput(PXR_NS::TfToken("opacityMode"), PXR_NS::SdfValueTypeNames->Token);

    return alpha && opacity && opacityMode
        && opacity.ConnectToSource(alpha)
        && opacityMode.Set(PXR_NS::TfToken("presence"));
}

} // namespace SceneNodeAuthoring

NATRON_NAMESPACE_EXIT
