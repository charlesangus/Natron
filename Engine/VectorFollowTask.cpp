/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
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

#include "VectorFollowTask.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <mutex>

#include <QtCore/QTimer>

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/IsolatedJob.h"
#include "Engine/Node.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/VectorFollowBake.h"
#include "Engine/VectorSampler.h"

NATRON_NAMESPACE_ENTER

namespace {

enum PhaseEnum {
    ePhaseCompute,
    ePhaseReduce
};

class PhaseArgs
    : public GenericThreadStartArgs {
public:
    PhaseArgs(PhaseEnum phase_,
              int generation_)
        : GenericThreadStartArgs()
        , phase(phase_)
        , generation(generation_)
    {
    }

    const PhaseEnum phase;
    const int generation;
};

struct PendingFrame {
    int shape = 0;
    VectorFollowFrame frame;
};

} // anonymous namespace

struct VectorFollowTask::Imp {
    NodeWPtr host;
    VectorFollowParams params;
    std::unique_ptr<VectorFollowRun> run;
    std::unique_ptr<VectorSampler> sampler;
    std::unique_ptr<IsolatedJob> job;
    std::atomic<bool> aborted { false };
    // A waited abort ends the follow with this phase's signal still queued; the next follow must not take it for its own.
    int generation = 0;
    bool sourceLost = false;
    // Cancelling is only seen by asking, and one frame of an upstream render or the reduction can take long.
    QTimer progressPoll;
    std::mutex framesMutex;
    std::deque<PendingFrame> frames;
    int framesTotal = 1;
    int framesDone = 0;

    std::function<bool()> abortCheck()
    {
        return [this]() {
            return aborted.load();
        };
    }
};

VectorFollowTask::VectorFollowTask(const NodePtr& host)
    : GenericSchedulerThread()
    , _imp(new Imp)
{
    _imp->host = host;
    QObject::connect(this, SIGNAL(framesReady()), this, SLOT(onFramesReady()), Qt::QueuedConnection);
    QObject::connect(this, SIGNAL(phaseDone(int, int)), this, SLOT(onPhaseDone(int, int)), Qt::QueuedConnection);
    _imp->progressPoll.setInterval(100);
    QObject::connect(&_imp->progressPoll, SIGNAL(timeout()), this, SLOT(onProgressPoll()));
    setThreadName("VectorFollowTask");
}

VectorFollowTask::~VectorFollowTask()
{
    abortFollow(true);
    quitThread(false);
    waitForThreadToQuit_enforce_blocking();
}

GenericSchedulerThread::TaskQueueBehaviorEnum
VectorFollowTask::tasksQueueBehaviour() const
{
    return eTaskQueueBehaviorProcessInOrder;
}

bool
VectorFollowTask::startFollow(const VectorFollowParams& params,
                              std::string* error)
{
    if (isFollowing()) {
        *error = "A follow is already running on this node.";

        return false;
    }
    NodePtr host = _imp->host.lock();
    if (!host) {
        return false;
    }
    if (host->getEffectInstance()->isReadByIsolatedJob()) {
        *error = "The shapes cannot change while a render or a track reads them.";

        return false;
    }
    RotoCurveContextPtr context = host->getCurveContext();
    std::unique_ptr<VectorFollowRun> run(new VectorFollowRun(context, params));
    if (!run->prepare(error)) {
        return false;
    }

    _imp->params = params;
    _imp->run = std::move(run);
    _imp->sampler.reset(new VectorSampler(params.source));
    _imp->job.reset(new IsolatedJob(params.source, "vector follow"));
    _imp->aborted = false;
    _imp->sourceLost = false;
    _imp->framesDone = 0;
    _imp->framesTotal = std::max(1, (int)params.items.size() * (params.last - params.first + 1));
    ++_imp->generation;
    QObject::connect(params.source.get(), SIGNAL(deactivated(bool)), this, SLOT(onSourceDeactivated()), Qt::UniqueConnection);
    host->clearPersistentMessage(false);
    host->getApp()->progressStart(host, tr("Following vectors...").toStdString(), "");
    _imp->progressPoll.start();
    startTask(std::make_shared<PhaseArgs>(ePhaseCompute, _imp->generation));

    return true;
}

bool
VectorFollowTask::isFollowing() const
{
    return bool(_imp->run);
}

void
VectorFollowTask::abortFollow(bool wait)
{
    if (!isFollowing()) {
        return;
    }
    requestAbort();
    if (wait) {
        waitForAbortToComplete_enforce_blocking();
        onPhaseDone(ePhaseCompute, _imp->generation);
    }
}

void
VectorFollowTask::requestAbort()
{
    // The scheduler skips onAbortRequested while idle, which is where it sits between two phases.
    onAbortRequested(false);
    abortThreadedTask();
}

void
VectorFollowTask::onAbortRequested(bool /*keepOldestRender*/)
{
    _imp->aborted = true;
    if (_imp->sampler) {
        _imp->sampler->abort();
    }
}

void
VectorFollowTask::onQuitRequested(bool /*allowRestarts*/)
{
    onAbortRequested(false);
}

void
VectorFollowTask::onQueuedTasksDropped(const std::list<GenericThreadStartArgsPtr>& tasks)
{
    for (const GenericThreadStartArgsPtr& task : tasks) {
        std::shared_ptr<PhaseArgs> args = std::dynamic_pointer_cast<PhaseArgs>(task);
        if (args) {
            Q_EMIT phaseDone((int)args->phase, args->generation);
        }
    }
}

GenericSchedulerThread::ThreadStateEnum
VectorFollowTask::threadLoopOnce(const GenericThreadStartArgsPtr& inArgs)
{
    std::shared_ptr<PhaseArgs> args = std::dynamic_pointer_cast<PhaseArgs>(inArgs);
    assert(args);
    {
        // Must end before phaseDone: the main thread may destroy the job as soon as it is emitted.
        IsolatedJob::ThreadScope inJob(_imp->job.get());
        if (args->phase == ePhaseCompute) {
            _imp->run->compute(*_imp->sampler, [this](int shape, const VectorFollowFrame& frame) {
                PendingFrame pending;
                pending.shape = shape;
                pending.frame = frame;
                {
                    std::lock_guard<std::mutex> locker(_imp->framesMutex);
                    _imp->frames.push_back(pending);
                }
                Q_EMIT framesReady(); }, _imp->abortCheck());
        } else {
            _imp->run->reduce(_imp->abortCheck());
        }
    }
    Q_EMIT phaseDone((int)args->phase, args->generation);

    return eThreadStateActive;
}

void
VectorFollowTask::onFramesReady()
{
    std::deque<PendingFrame> frames;
    {
        std::lock_guard<std::mutex> locker(_imp->framesMutex);
        frames.swap(_imp->frames);
    }
    NodePtr host = _imp->host.lock();
    if (frames.empty() || !_imp->run || !host) {
        return;
    }

    RotoCurveContextPtr context = host->getCurveContext();
    const int current = (int)std::lround(context->getTimelineCurrentTime());
    bool showsCurrent = false;
    for (const PendingFrame& pending : frames) {
        _imp->run->applyFrame(pending.shape, pending.frame);
        showsCurrent = showsCurrent || (pending.frame.frame == current);
        ++_imp->framesDone;
    }
    context->emitRefreshViewerOverlays();
    if (showsCurrent) {
        context->evaluateChange();
    }
}

void
VectorFollowTask::onProgressPoll()
{
    NodePtr host = _imp->host.lock();
    if (!_imp->run || !host || _imp->aborted) {
        return;
    }
    if (!_imp->params.source->isActivated()) {
        onSourceDeactivated();
    } else if (!host->getApp()->progressUpdate(host, (double)_imp->framesDone / _imp->framesTotal)) {
        requestAbort();
    }
}

void
VectorFollowTask::onSourceDeactivated()
{
    if (!isFollowing()) {
        return;
    }
    _imp->sourceLost = true;
    requestAbort();
}

void
VectorFollowTask::onPhaseDone(int phase,
                              int generation)
{
    if (!_imp->run || (generation != _imp->generation)) {
        return;
    }
    onFramesReady();

    NodePtr host = _imp->host.lock();
    const bool canReduce = (phase == ePhaseCompute) && _imp->params.reduce && !_imp->run->wasAborted() && !_imp->aborted && !_imp->sourceLost && host;
    if (canReduce) {
        _imp->run->prepareReduction();
        startTask(std::make_shared<PhaseArgs>(ePhaseReduce, _imp->generation));

        return;
    }
    if ((phase == ePhaseReduce) && !_imp->aborted) {
        _imp->run->applyReduction();
    }

    _imp->progressPoll.stop();
    const NodePtr source = _imp->params.source;
    QObject::disconnect(source.get(), SIGNAL(deactivated(bool)), this, SLOT(onSourceDeactivated()));
    const bool sourceLost = _imp->sourceLost || !source->isActivated();
    // A follow the user stopped has no read error to tell: its sampler stopped because of that stop.
    const std::string error = _imp->aborted ? std::string() : _imp->sampler->lastError();
    const VectorFollowReport report = _imp->run->finish(sourceLost, error);
    _imp->run.reset();
    _imp->sampler.reset();
    _imp->job.reset();
    // Holding on to the source and the shapes would keep them alive after the user deleted them.
    _imp->params = VectorFollowParams();
    if (host) {
        host->getApp()->progressEnd(host);
        host->setPersistentMessage(eMessageTypeInfo, formatVectorFollowReport(report));
    }
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_VectorFollowTask.cpp"
