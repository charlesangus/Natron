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
#include <list>
#include <map>
#include <mutex>
#include <thread>
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
#include "Engine/Nodes/NonTrappingMath.h"
#include "Engine/Nodes/Scene/PrimSelector.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDProject3D.h"
#include "Engine/Project.h"
#include "Engine/ViewerInstance.h"

#include <pxr/usd/usd/collectionAPI.h>
#include <pxr/usd/usd/primRange.h>
#include <pxr/usd/usd/tokens.h>

#include <QDir>
#include <QString>

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
             std::string* errorMessage)
{
    *matches = PatternMatches();
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
        if (!SceneOps::selectPrims(stage, pattern, &selected, errorMessage)) {
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
    for (std::size_t i = 1; i < recorded.windows.size(); ++i) {
        if ((recorded.windows[i].format != recorded.windows[0].format) || (recorded.windows[i].window != recorded.windows[0].window)) {
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
                    const PXR_NS::UsdShadeShader surface = PXR_NS::UsdShadeMaterial(prim).ComputeSurfaceSource();
                    const PXR_NS::UsdShadeInput space = isProjectionShader(surface) ? surface.GetInput(PXR_NS::TfToken(kUSDProject3DInputProjectionSpace)) : PXR_NS::UsdShadeInput();
                    int spaceValue = (int)USDProject3D::eProjectionSpaceWorld;
                    if (space && space.Get(&spaceValue) && (spaceValue == (int)USDProject3D::eProjectionSpaceGeometry)) {
                        graft->geometryProjectionShader = surface.GetPath().ReplacePrefix(sourceRoot, graftedRoot).GetString();
                        const PXR_NS::UsdAttribute hold = surface.GetPrim().GetAttribute(PXR_NS::TfToken(kUSDProject3DAttrFrameHold));
                        graft->projectionHeld = hold && hold.Get(&graft->projectionHoldFrame);
                    }
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
        facts.appendString(graft->geometryProjectionShader);
        facts.appendBool(graft->projectionHeld);
        facts.appendDouble(graft->projectionHoldFrame);
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
        && collection.CreateIncludesRel().SetTargets(binding.includes);
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
authorProjectionGeometryFrame(const PXR_NS::UsdStageRefPtr& stage,
                              const SceneLayerEdit& edit,
                              const SceneMaterialGraft& graft,
                              bool animated,
                              const std::function<PXR_NS::GfMatrix4d(double)>& frameAt)
{
    if (graft.geometryProjectionShader.empty()) {
        return true;
    }
    const PXR_NS::UsdPrim shader = stage->OverridePrim(PXR_NS::SdfPath(graft.geometryProjectionShader));
    const PXR_NS::TfToken name(PXR_NS::UsdShadeTokens->inputs.GetString() + kUSDProject3DInputGeometryFrame);
    const PXR_NS::UsdAttribute attribute = shader ? shader.CreateAttribute(name, PXR_NS::SdfValueTypeNames->Matrix4d) : PXR_NS::UsdAttribute();
    if (graft.projectionHeld) {
        return attribute && attribute.Set(frameAt(graft.projectionHoldFrame));
    }

    return authorValue(attribute, edit, animated, frameAt);
}

bool
authorMaterialBinding(const PXR_NS::UsdPrim& prim,
                      const std::string& materialPath,
                      std::string* errorMessage)
{
    if (materialPath.empty()) {
        return true;
    }
    const PXR_NS::UsdShadeMaterialBindingAPI binding = prim ? PXR_NS::UsdShadeMaterialBindingAPI::Apply(prim) : PXR_NS::UsdShadeMaterialBindingAPI();
    const PXR_NS::UsdRelationship rel = binding ? prim.CreateRelationship(PXR_NS::UsdShadeTokens->materialBinding, false) : PXR_NS::UsdRelationship();
    if (!rel || !rel.SetTargets({ PXR_NS::SdfPath(materialPath) })) {
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

bool
getLiveTextureSource(const EffectInstance& consumer,
                     int inputNb,
                     const std::string& layer,
                     LiveTextureSource* source)
{
    const EffectInstancePtr connected = consumer.getInput(inputNb);
    NodePtr node = connected ? connected->getNode() : NodePtr();
    const double time = consumer.getCurrentTime();
    // A disabled node renders as its preferred input, so the texture shows what the render would.
    while (node && node->isNodeDisabled(time)) {
        node = node->getPreferredInputNode();
    }
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

    return true;
}

void
appendLiveTextureSource(const LiveTextureSource& source,
                        ContentHasher* h)
{
    h->appendString(source.nodeName);
    h->appendString(source.layer);
    h->appendU64(source.contentHash);
    h->appendBool(source.varying);
}

LiveTextureSource
recordLiveTexture(const SceneLayerEdit& edit,
                  const LiveTextureSource& source)
{
    LiveTextureSource recorded = source;

    recorded.firstFrame = edit.firstFrame;
    recorded.lastFrame = edit.lastFrame;
    recorded.windows.clear();
    const NodePtr node = source.node.lock();
    const std::vector<double> frames = source.varying ? getSampleFrames(edit) : std::vector<double>(1, recorded.getSourceFrame(edit.firstFrame));
    recorded.windows.reserve(frames.size());
    for (double frame : frames) {
        LiveTextureWindow window;
        if (!node || !getNodeImageWindow(node, frame, ViewIdx(0), &window.format, &window.window)) {
            window = LiveTextureWindow();
        }
        recorded.windows.push_back(window);
    }
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
