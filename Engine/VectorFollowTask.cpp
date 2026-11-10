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
#include <functional>
#include <mutex>

#include <QtCore/QPointer>
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

std::mutex phaseStartHookMutex;
std::function<void()> phaseStartHook;

} // anonymous namespace

struct VectorFollowTask::Imp {
    NodeWPtr host;
    VectorFollowParams params;
    std::unique_ptr<VectorFollowRun> run;
    std::unique_ptr<VectorSampler> sampler;
    std::unique_ptr<IsolatedJob> job;
    // Holds the shapes still for the follow: a shape edited while it runs would be overwritten by
    // the frames still to come, and its undo would take back the edit with the follow.
    std::unique_ptr<IsolatedJob> hostJob;
    std::atomic<bool> aborted { false };
    // Held by the follow's thread while it runs a phase, and by the main thread while it sets or
    // drops what a phase reads, so a waited abort can wait the phase out before dropping it.
    std::mutex workMutex;
    // A waited abort ends the follow with this phase's signal still queued; the next follow must not take it for its own.
    int generation = 0;
    bool sourceLost = false;
    // Cancelling is only seen by asking, and one frame of an upstream render or the reduction can take long.
    QTimer progressPoll;
    std::mutex framesMutex;
    std::deque<PendingFrame> frames;
    int framesTotal = 1;
    int framesDone = 0;
    std::unique_ptr<VectorFollowReport> lastReport;
    // The interface's modal progress dialog, held open until the follow has written its last key:
    // the progress task ends first, and the reduction or the bake of the last frames may follow.
    std::shared_ptr<void> modal;

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

    _imp->lastReport.reset();
    {
        std::lock_guard<std::mutex> locker(_imp->workMutex);
        _imp->params = params;
        _imp->run = std::move(run);
        _imp->sampler.reset(new VectorSampler(params.source));
        _imp->job.reset(new IsolatedJob(params.source, "vector follow"));
        _imp->hostJob.reset(new IsolatedJob(host, "vector follow"));
        _imp->aborted = false;
        ++_imp->generation;
    }
    _imp->sourceLost = false;
    _imp->framesDone = 0;
    _imp->framesTotal = std::max(1, (int)(params.items.size() + params.points.size()) * (params.last - params.first + 1));
    QObject::connect(params.source.get(), SIGNAL(deactivated(bool)), this, SLOT(onSourceDeactivated()), Qt::UniqueConnection);
    host->clearPersistentMessage(false);
    host->getApp()->progressStart(host, tr("Following vectors...").toStdString(), "");
    QPointer<VectorFollowTask> self(this);
    _imp->modal = host->getApp()->showProgressModal(host, tr("Following Vectors").toStdString(), [self]() {
        if (self) {
            self->requestAbort();
        }
    });
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
        // Not waitForAbortToComplete_enforce_blocking(): it returns at once whatever the thread
        // does, and a phase queued but not yet begun is not aborted and would start after it.
        {
            std::lock_guard<std::mutex> waited(_imp->workMutex);
        }
        onPhaseDone(ePhaseCompute, _imp->generation);
    }
}

void
VectorFollowTask::setPhaseStartHookForTesting(const std::function<void()>& hook)
{
    std::lock_guard<std::mutex> locker(phaseStartHookMutex);
    phaseStartHook = hook;
}

bool
VectorFollowTask::getLastReport(VectorFollowReport* report) const
{
    if (!_imp->lastReport) {
        return false;
    }
    *report = *_imp->lastReport;

    return true;
}

void
VectorFollowTask::forgetLastReport()
{
    _imp->lastReport.reset();
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
        // Must end before phaseDone: the main thread may drop the job and the run as soon as it is
        // emitted.
        std::lock_guard<std::mutex> working(_imp->workMutex);
        // A phase queued before a waited abort can begin after that abort already ended its follow.
        if ((args->generation == _imp->generation) && _imp->run) {
            std::function<void()> hook;
            {
                std::lock_guard<std::mutex> locker(phaseStartHookMutex);
                hook = phaseStartHook;
            }
            if (hook) {
                hook();
            }
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
    {
        // Kept off the render the change below starts, which would otherwise run in the job.
        IsolatedJob::ThreadScope inJob(_imp->hostJob.get());
        for (const PendingFrame& pending : frames) {
            _imp->run->applyFrame(pending.shape, pending.frame);
            showsCurrent = showsCurrent || (pending.frame.frame == current);
            ++_imp->framesDone;
        }
    }
    context->emitRefreshViewerOverlays();
    for (const VectorFollowPointsPtr& points : _imp->params.points) {
        if (points) {
            points->framesWritten();
        }
    }
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
        IsolatedJob::ThreadScope inJob(_imp->hostJob.get());
        _imp->run->prepareReduction();
        startTask(std::make_shared<PhaseArgs>(ePhaseReduce, _imp->generation));

        return;
    }

    _imp->progressPoll.stop();
    const NodePtr source = _imp->params.source;
    QObject::disconnect(source.get(), SIGNAL(deactivated(bool)), this, SLOT(onSourceDeactivated()));
    const bool sourceLost = _imp->sourceLost || !source->isActivated();
    // A follow the user stopped has no read error to tell: its sampler stopped because of that stop.
    // One that went on past an unreadable frame has none either: its report tells of that frame.
    const std::string error = (_imp->aborted || !_imp->run->wasAborted()) ? std::string() : _imp->sampler->lastError();
    VectorFollowReport report;
    {
        // Must end before the dialog below, whose event loop would let the user's edits through as
        // the follow's own.
        IsolatedJob::ThreadScope inJob(_imp->hostJob.get());
        if ((phase == ePhaseReduce) && !_imp->aborted) {
            _imp->run->applyReduction();
        }
        report = _imp->run->finish(sourceLost, error);
    }
    // The stop can reach the sampler before the run asks whether to go on, and then the run only
    // sees a frame it could not read.
    report.userStopped = report.userStopped || _imp->aborted;
    // Kept before the report is stored and any dialog opens: that dialog's event loop may already
    // ask for the report.
    _imp->lastReport.reset(new VectorFollowReport(report));
    {
        std::lock_guard<std::mutex> locker(_imp->workMutex);
        _imp->run.reset();
        _imp->sampler.reset();
        _imp->job.reset();
        _imp->hostJob.reset();
        // Holding on to the source and the shapes would keep them alive after the user deleted them.
        _imp->params = VectorFollowParams();
    }
    if (host) {
        host->getApp()->progressEnd(host);
    }
    _imp->modal.reset();
    if (host) {
        const std::string text = formatVectorFollowReport(report);
        host->setQuietInfoMessage(text);
        if (!isCleanVectorFollowReport(report) && !isUserStopVectorFollowReport(report)) {
            host->message(eMessageTypeInfo, text);
        }
    }
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_VectorFollowTask.cpp"
