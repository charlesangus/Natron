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

#include "OutputSchedulerThread.h"

#include <algorithm> // min, max
#include <atomic>
#include <cassert>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <list>
#include <map>
#include <set>
#include <sstream> // stringstream
#include <stdexcept>
#include <utility>

#include <QMetaType>
#include <QMutex>
#include <QWaitCondition>
#include <QCoreApplication>
#include <QString>
#include <QThreadPool>
#include <QDebug>
#include <QTextStream>
#include <QRunnable>

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/FrameRenderContext.h"
#include "Engine/GenericSchedulerThreadWatcher.h"
#include "Engine/GpuPlacement.h"
#include "Engine/Image.h"
#include "Engine/IsolatedJob.h"
#include "Engine/KnobFile.h"
#include "Engine/Node.h"
#include "Engine/OpenGLViewerI.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Project.h"
#include "Engine/RenderEndWatch.h"
#include "Engine/RenderScheduler.h"
#include "Engine/RenderStats.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoLatencyProbe.h"
#include "Engine/SchedulerEligibility.h"
#include "Engine/Settings.h"
#include "Engine/StrokeRenderCoalescer.h"
#include "Engine/TLSHolder.h"
#include "Engine/TimeLine.h"
#include "Engine/Timer.h"
#include "Engine/UndoRecorder.h"
#include "Engine/UpdateViewerParams.h"
#include "Engine/ViewIdx.h"
#include "Engine/ViewerInstance.h"
#include "Global/FloatingPointExceptions.h"
#include "Global/MathUtils.h"

#ifdef DEBUG
//#define TRACE_SCHEDULER
//#define TRACE_CURRENT_FRAME_SCHEDULER
#endif

#define NATRON_FPS_REFRESH_RATE_SECONDS 1.5

#define NATRON_SCHEDULER_ABORT_AFTER_X_UNSUCCESSFUL_ITERATIONS 5000

NATRON_NAMESPACE_ENTER


///Sort the frames by time and then by view

struct BufferedFrameKey
{
    int time;
};

struct BufferedFrameCompare_less
{
    bool operator()(const BufferedFrameKey& lhs,
                    const BufferedFrameKey& rhs) const
    {
        return lhs.time < rhs.time;
    }
};

typedef std::multimap<BufferedFrameKey, BufferedFrame, BufferedFrameCompare_less> FrameBuffer;


NATRON_NAMESPACE_ANONYMOUS_ENTER

class MetaTypesRegistration
{
public:
    inline MetaTypesRegistration()
    {
        qRegisterMetaType<BufferedFrames>("BufferedFrames");
        qRegisterMetaType<BufferableObjectPtrList>("BufferableObjectPtrList");
    }
};

std::string
unsupportedOutputKindMessage(const NodePtr& writer)
{
    return std::string("No scheduler support for the output data kind of ") + writer->getScriptName_mt_safe() + " yet";
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

static MetaTypesRegistration registration;
// The point of a live stroke that a produced frame shows, so the overlay preview stops drawing what the frame already has,
// or that the frame is a neat stroke render, whose upload ends the stroke's preview.
struct StrokeDisplayMark {
    NodeWPtr rotoPaintNode;
    RotoStrokeItemWPtr stroke;
    int multiStrokeIndex;
    int age;
    bool neatRender;

    StrokeDisplayMark()
        : rotoPaintNode()
        , stroke()
        , multiStrokeIndex(-1)
        , age(-1)
        , neatRender(false)
    {
    }
};

struct ProducedFrame
{
    BufferableObjectPtrList frames;
    U64 age;
    RenderStatsPtr stats;
    StrokeDisplayMark strokeMark;
};

struct ProducedFrameCompareAgeLess
{
    bool operator() (const ProducedFrame& lhs,
                     const ProducedFrame& rhs) const
    {
        return lhs.age < rhs.age;
    }
};

typedef std::set<ProducedFrame, ProducedFrameCompareAgeLess> ProducedFrameSet;

class OutputSchedulerThreadExecMTArgs
    : public GenericThreadExecOnMainThreadArgs
{
public:

    BufferedFrames frames;

    OutputSchedulerThreadExecMTArgs()
        : GenericThreadExecOnMainThreadArgs()
    {}

    virtual ~OutputSchedulerThreadExecMTArgs() {}
};

typedef std::shared_ptr<OutputSchedulerThreadExecMTArgs> OutputSchedulerThreadExecMTArgsPtr;

namespace {

std::atomic<int> gFrameThreadsStarted { 0 };

} // namespace

/**
 * @brief The one thread of an OutputSchedulerThread that runs the frames the RenderScheduler cannot, one after the
 * other: those frames pull their inputs and wait for them, which a thread of the global pool must never do.
 **/
class FrameFallbackWorker
    : public QThread,
      public AbortableThread {
public:
    FrameFallbackWorker()
        : QThread()
        , AbortableThread(this)
        , _mutex()
        , _cond()
        , _jobs()
        , _mustQuit(false)
    {
        setThreadName("Frame fallback thread");
    }

    virtual ~FrameFallbackWorker()
    {
        {
            QMutexLocker k(&_mutex);
            _mustQuit = true;
            _cond.wakeAll();
        }
        wait();
    }

    void push(const std::function<void()>& job)
    {
        if (!isRunning()) {
            ++gFrameThreadsStarted;
            start();
        }
        QMutexLocker k(&_mutex);
        _jobs.push_back(job);
        _cond.wakeOne();
    }

private:
    virtual void run() OVERRIDE FINAL
    {
#ifdef DEBUG
        boost_adaptbx::floating_point::exception_trapping trap(boost_adaptbx::floating_point::exception_trapping::division_by_zero | boost_adaptbx::floating_point::exception_trapping::invalid | boost_adaptbx::floating_point::exception_trapping::overflow);
#endif
        for (;;) {
            std::function<void()> job;
            {
                QMutexLocker k(&_mutex);
                while (_jobs.empty() && !_mustQuit) {
                    _cond.wait(&_mutex);
                }
                if (_jobs.empty()) {
                    return;
                }
                job = _jobs.front();
                _jobs.pop_front();
            }
            boost_adaptbx::floating_point::follow_software_renderer();
            job();
            appPTR->getAppTLS()->cleanupTLSForThread();
        }
    }

    QMutex _mutex;
    QWaitCondition _cond;
    std::list<std::function<void()>> _jobs;
    bool _mustQuit;
};

static bool
isBufferFull(int nbBufferedElement,
             int hardwardIdealThreadCount)
{
    return nbBufferedElement >= hardwardIdealThreadCount * 3;
}

struct OutputSchedulerThreadPrivate
{
    FrameBuffer buf; //the frames rendered by the worker threads that needs to be rendered in order by the output device
    QWaitCondition bufEmptyCondition;
    mutable QMutex bufMutex;
    // Bumped under bufMutex with every bufEmptyCondition wake: the scheduler waits only if it is unchanged since it
    // last examined buf, since a frame it cannot process yet keeps buf non-empty and would make it spin.
    U64 bufWakeGeneration;

    //doesn't need any protection since it never changes and is set in the constructor
    OutputSchedulerThread::ProcessFrameModeEnum mode; //is the frame to be processed on the main-thread (i.e OpenGL rendering) or on the scheduler thread
    Timer timer; // Timer regulating the engine execution. It is controlled by the GUI and MT-safe.
    std::unique_ptr<TimeLapse> renderTimer; // Timer used to report stats when rendering

    ///When the render threads are not using the appendToBuffer API, the scheduler has no way to know the rendering is finished
    ///but to count the number of frames rendered via notifyFrameRended which is called by the render thread.
    mutable QMutex renderFinishedMutex;
    U64 nFramesRendered;
    bool renderFinished; //< set to true when nFramesRendered = runArgs->lastFrame - runArgs->firstFrame + 1

    // Pointer to the args used in threadLoopOnce(), only usable from the scheduler thread
    OutputSchedulerThreadStartArgsWPtr runArgs;
    mutable QMutex lastRunArgsMutex;
    std::vector<ViewIdx> lastPlaybackViewsToRender;
    RenderDirectionEnum lastPlaybackRenderDirection;

    QMutex framesToRenderMutex; // protects expectFrameToRender
    int expectFrameToRender;
    OutputEffectInstanceWPtr outputEffect; //< The effect used as output device
    RenderEngine* engine;

    QMutex bufferedOutputMutex;
    int lastBufferedOutputSize;

    // The steps of admitted frames that ended and wait for the scheduler thread to call their done, and how many
    // steps have not ended yet. Both under bufMutex.
    std::list<std::function<void()>> finishedFrameSteps;
    int nFrameStepsOutstanding;

    // The abort info of each step that has not ended, for onAbortRequested(), which may run on any thread.
    QMutex pendingStepsMutex;
    std::multimap<U64, AbortableRenderInfoPtr> pendingSteps;
    U64 nextStepId;

    // Admission state, written by the scheduler thread only.
    std::atomic<int> nAdmittedFrames;
    // How many frames may be in flight without asking the RenderScheduler's load. Only a passing load check adds one,
    // so a frame ending frees its slot for the next frame even while the frames left keep the pool busy.
    int admissionSlots;
    // The most frames in flight at once since startRender(), for tests.
    std::atomic<int> maxAdmittedFrames;
    int nextFrameToAdmit;
    bool admissionExhausted;
    // The mode admission and display both step with, so that they follow one sequence.
    PlaybackModeEnum playbackMode;
    bool stoppingAdmittedFrames;
    // The time of every frame in nAdmittedFrames.
    std::multiset<int> admittedTimes;
    // Times admission passes over once each, since a frame for them is already buffered or in flight.
    std::multiset<int> admissionSkips;
    // Lets the next admission past a full buffer, when nothing in it or in flight is the frame the display waits for.
    bool forceNextAdmission;

    std::unique_ptr<FrameFallbackWorker> fallbackWorker;

    // From startRender() to stopRender() of a render that is not a viewer's: what the render reads
    // refuses writes from outside it. The scheduler thread is in its context for as long.
    std::unique_ptr<IsolatedJob> isolatedJob;
    std::unique_ptr<IsolatedJob::ThreadScope> schedulerInJob;
    // isolatedJob, for the render's other threads.
    std::atomic<const IsolatedJob*> runningJob { nullptr };

    OutputSchedulerThreadPrivate(RenderEngine* engine,
                                 const OutputEffectInstancePtr& effect,
                                 OutputSchedulerThread::ProcessFrameModeEnum mode)
        : buf()
        , bufEmptyCondition()
        , bufMutex()
        , bufWakeGeneration(0)
        , mode(mode)
        , timer()
        , renderTimer()
        , renderFinishedMutex()
        , nFramesRendered(0)
        , renderFinished(false)
        , runArgs()
        , lastRunArgsMutex()
        , lastPlaybackViewsToRender()
        , lastPlaybackRenderDirection(eRenderDirectionForward)
        , framesToRenderMutex()
        , expectFrameToRender(0)
        , outputEffect(effect)
        , engine(engine)
        , bufferedOutputMutex()
        , lastBufferedOutputSize(0)
        , finishedFrameSteps()
        , nFrameStepsOutstanding(0)
        , pendingStepsMutex()
        , pendingSteps()
        , nextStepId(0)
        , nAdmittedFrames(0)
        , admissionSlots(1)
        , maxAdmittedFrames(0)
        , nextFrameToAdmit(0)
        , admissionExhausted(true)
        , playbackMode(ePlaybackModeOnce)
        , stoppingAdmittedFrames(false)
        , admittedTimes()
        , admissionSkips()
        , forceNextAdmission(false)
        , fallbackWorker()
    {
    }

    void wakeBufEmptyCondition()
    {
        assert(!bufMutex.tryLock());
        ++bufWakeGeneration;
        bufEmptyCondition.wakeOne();
    }

    void endIsolatedJob()
    {
        runningJob.store(nullptr);
        schedulerInJob.reset();
        isolatedJob.reset();
    }

    void appendBufferedFrame(double time,
                             ViewIdx view,
                             const RenderStatsPtr& stats,
                             const BufferableObjectPtr& image)
    {
        ///Private, shouldn't lock
        assert( !bufMutex.tryLock() );
#ifdef TRACE_SCHEDULER
        QString idStr;
        if (image) {
            idStr = QString::fromUtf8("ID: ") + QString::number( image->getUniqueID() );
        }
        qDebug() << "Parallel Render Thread: Rendered Frame:" << time << " View:" << (int)view << idStr;
#endif
        BufferedFrameKey key;
        BufferedFrame value;
        value.time = key.time = time;
        value.view = view;
        value.frame = image;
        value.stats = stats;
        buf.insert( std::make_pair(key, value) );
    }

    bool isFrameReachable(int time,
                          int fromTime,
                          RenderDirectionEnum direction,
                          int firstFrame,
                          int lastFrame) const
    {
        if ((time < firstFrame) || (time > lastFrame)) {
            return false;
        }
        if (playbackMode != ePlaybackModeOnce) {
            return true;
        }

        return (direction == eRenderDirectionForward) ? (time >= fromTime) : (time <= fromTime);
    }

    // A frame the display will not reach again would hold its image and a buffer slot until the playback ends.
    void eraseUnreachableBufferedFrames(int fromTime,
                                        RenderDirectionEnum direction,
                                        int firstFrame,
                                        int lastFrame)
    {
        assert(!bufMutex.tryLock());
        for (FrameBuffer::iterator it = buf.begin(); it != buf.end();) {
            if (isFrameReachable(roundTime(it->first.time), fromTime, direction, firstFrame, lastFrame)) {
                ++it;
            } else {
                buf.erase(it++);
            }
        }
    }

    static int roundTime(double time)
    {
        return (int)std::floor(time + 0.5);
    }

    struct ViewUniqueIDPair
    {
        int view;
        int uniqueId;
    };

    struct ViewUniqueIDPairCompareLess
    {
        bool operator() (const ViewUniqueIDPair& lhs,
                         const ViewUniqueIDPair& rhs) const
        {
            if (lhs.view < rhs.view) {
                return true;
            } else if (lhs.view > rhs.view) {
                return false;
            } else {
                if (lhs.uniqueId < rhs.uniqueId) {
                    return true;
                } else if (lhs.uniqueId > rhs.uniqueId) {
                    return false;
                } else {
                    return false;
                }
            }
        }
    };

    typedef std::set<ViewUniqueIDPair, ViewUniqueIDPairCompareLess> ViewUniqueIDSet;

    void getFromBufferAndErase(double time,
                               BufferedFrames& frames)
    {
        ///Private, shouldn't lock
        assert( !bufMutex.tryLock() );

        /*
           Note that the frame buffer does not hold any particular ordering and just contains all the frames as they
           were received by render threads.
           In the buffer, for any particular given time there can be:
           - Multiple views
           - Multiple "unique ID" (corresponds to viewer input A or B)

           Also since we are rendering ahead, we can have a buffered frame at time 23,
           and also another frame at time 23, each of which could have multiple unique IDs and so on

           To retrieve what we need to render, we extract at least one view and unique ID for this particular time
         */

        ViewUniqueIDSet uniqueIdsRetrieved;
        BufferedFrameKey key;
        key.time = time;
        std::pair<FrameBuffer::iterator, FrameBuffer::iterator> range = buf.equal_range(key);
        std::list<std::pair<BufferedFrameKey, BufferedFrame> > toKeep;
        FrameBuffer::iterator nothingToDisplay = buf.end();
        for (FrameBuffer::iterator it = range.first; it != range.second; ++it) {
            bool keepInBuf = true;
            if (it->second.displaysNothing && (nothingToDisplay == buf.end())) {
                nothingToDisplay = it;
                keepInBuf = false;
            } else if (it->second.frame) {
                ViewUniqueIDPair p;
                p.view = (int)it->second.view;
                p.uniqueId = it->second.frame->getUniqueID();
                std::pair<ViewUniqueIDSet::iterator, bool> alreadyRetrievedIndex = uniqueIdsRetrieved.insert(p);
                if (alreadyRetrievedIndex.second) {
                    frames.push_back(it->second);
                    keepInBuf = false;
                }
            }

            if (keepInBuf) {
                toKeep.push_back(*it);
            }
        }
        if (nothingToDisplay != buf.end()) {
            // Frames and a marker of one time come from different passes through it, each displayed in turn.
            if (frames.empty()) {
                frames.push_back(nothingToDisplay->second);
            } else {
                toKeep.push_back(*nothingToDisplay);
            }
        }
        if ( range.first != buf.end() ) {
            buf.erase(range.first, range.second);
            buf.insert( toKeep.begin(), toKeep.end() );
        }
    }

    int getNBufferedFrames() const
    {
        QMutexLocker l(&bufMutex);

        return buf.size();
    }

    static bool getNextFrameInSequence(PlaybackModeEnum pMode,
                                       RenderDirectionEnum direction,
                                       int frame,
                                       int firstFrame,
                                       int lastFrame,
                                       unsigned int frameStep,
                                       int* nextFrame,
                                       RenderDirectionEnum* newDirection);
    /**
     * @brief The frame played after frame, false once the playback ends. Admission and display both step with it, so
     * the display only ever waits for frames that were admitted.
     **/
    static bool getNextFrameToPlay(PlaybackModeEnum pMode,
                                   RenderDirectionEnum direction,
                                   int frame,
                                   int firstFrame,
                                   int lastFrame,
                                   int frameStep,
                                   int* nextFrame,
                                   RenderDirectionEnum* newDirection);
    static void getNearestInSequence(RenderDirectionEnum direction,
                                     int frame,
                                     int firstFrame,
                                     int lastFrame,
                                     int* nextFrame);
};

OutputSchedulerThread::OutputSchedulerThread(RenderEngine* engine,
                                             const OutputEffectInstancePtr& effect,
                                             ProcessFrameModeEnum mode)
    : GenericSchedulerThread()
    , _imp( new OutputSchedulerThreadPrivate(engine, effect, mode) )
{
    QObject::connect( &_imp->timer, SIGNAL(fpsChanged(double,double)), _imp->engine, SIGNAL(fpsChanged(double,double)) );

    setThreadName("Scheduler thread");
}

OutputSchedulerThread::~OutputSchedulerThread()
{
    // The scope belongs to the scheduler thread, not this one, whose context it must not change.
    if (_imp->schedulerInJob) {
        _imp->schedulerInJob->detach();
    }
    _imp->endIsolatedJob();
}

const IsolatedJob*
OutputSchedulerThread::getRunningIsolatedJob() const
{
    return _imp->runningJob.load();
}

bool
OutputSchedulerThreadPrivate::getNextFrameInSequence(PlaybackModeEnum pMode,
                                                     RenderDirectionEnum direction,
                                                     int frame,
                                                     int firstFrame,
                                                     int lastFrame,
                                                     unsigned int frameStep,
                                                     int* nextFrame,
                                                     RenderDirectionEnum* newDirection)
{
    assert(frameStep >= 1);
    *newDirection = direction;
    if (firstFrame == lastFrame) {
        *nextFrame = firstFrame;

        return true;
    }
    if (frame <= firstFrame) {
        switch (pMode) {
        case ePlaybackModeLoop:
            if (direction == eRenderDirectionForward) {
                *nextFrame = firstFrame + frameStep;
            } else {
                *nextFrame  = lastFrame - frameStep;
            }
            break;
        case ePlaybackModeBounce:
            if (direction == eRenderDirectionForward) {
                *newDirection = eRenderDirectionBackward;
                *nextFrame  = lastFrame - frameStep;
            } else {
                *newDirection = eRenderDirectionForward;
                *nextFrame  = firstFrame + frameStep;
            }
            break;
        case ePlaybackModeOnce:
        default:
            if (direction == eRenderDirectionForward) {
                *nextFrame = firstFrame + frameStep;
                break;
            } else {
                return false;
            }
        }
    } else if (frame >= lastFrame) {
        switch (pMode) {
        case ePlaybackModeLoop:
            if (direction == eRenderDirectionForward) {
                *nextFrame = firstFrame;
            } else {
                *nextFrame = lastFrame - frameStep;
            }
            break;
        case ePlaybackModeBounce:
            if (direction == eRenderDirectionForward) {
                *newDirection = eRenderDirectionBackward;
                *nextFrame = lastFrame - frameStep;
            } else {
                *newDirection = eRenderDirectionForward;
                *nextFrame = firstFrame + frameStep;
            }
            break;
        case ePlaybackModeOnce:
        default:
            if (direction == eRenderDirectionForward) {
                return false;
            } else {
                *nextFrame = lastFrame - frameStep;
                break;
            }
        }
    } else {
        if (direction == eRenderDirectionForward) {
            *nextFrame = frame + frameStep;
        } else {
            *nextFrame = frame - frameStep;
        }
    }

    return true;
} // OutputSchedulerThreadPrivate::getNextFrameInSequence

bool
OutputSchedulerThreadPrivate::getNextFrameToPlay(PlaybackModeEnum pMode,
                                                 RenderDirectionEnum direction,
                                                 int frame,
                                                 int firstFrame,
                                                 int lastFrame,
                                                 int frameStep,
                                                 int* nextFrame,
                                                 RenderDirectionEnum* newDirection)
{
    *newDirection = direction;
    if ((frameStep <= 0) || ((firstFrame == lastFrame) && (pMode == ePlaybackModeOnce))) {
        return false;
    }
    if (!getNextFrameInSequence(pMode, direction, frame, firstFrame, lastFrame, frameStep, nextFrame, newDirection)) {
        return false;
    }

    // Played once, a range whose length is not a multiple of the step ends at its last multiple, not past it.
    return (pMode != ePlaybackModeOnce) || ((*nextFrame >= firstFrame) && (*nextFrame <= lastFrame));
}

void
OutputSchedulerThreadPrivate::getNearestInSequence(RenderDirectionEnum direction,
                                                   int frame,
                                                   int firstFrame,
                                                   int lastFrame,
                                                   int* nextFrame)
{
    if ( (frame >= firstFrame) && (frame <= lastFrame) ) {
        *nextFrame = frame;
    } else if (frame < firstFrame) {
        if (direction == eRenderDirectionForward) {
            *nextFrame = firstFrame;
        } else {
            *nextFrame = lastFrame;
        }
    } else { // frame > lastFrame
        if (direction == eRenderDirectionForward) {
            *nextFrame = lastFrame;
        } else {
            *nextFrame = firstFrame;
        }
    }
}

void
OutputSchedulerThread::startRender()
{
    if ( isFPSRegulationNeeded() ) {
        _imp->timer.playState = ePlayStateRunning;
    }

    // Start measuring
    _imp->renderTimer.reset(new TimeLapse);

    ///We will push frame to renders starting at startingFrame.
    ///They will be in the range determined by firstFrame-lastFrame
    int startingFrame;
    int firstFrame, lastFrame;
    int frameStep;
    bool forward;
    OutputSchedulerThreadStartArgsPtr args = _imp->runArgs.lock();
    ///Copy the last requested run args

    firstFrame = args->firstFrame;
    lastFrame = args->lastFrame;
    frameStep = args->frameStep;
    startingFrame = timelineGetTime();
    forward = args->pushTimelineDirection == eRenderDirectionForward;

    _imp->endIsolatedJob();
    bool isViewer = false;
    {
        OutputEffectInstancePtr output = _imp->outputEffect.lock();
        isViewer = output && dynamic_cast<ViewerInstance*>(output.get());
        if (output && !isViewer) {
            _imp->isolatedJob.reset(new IsolatedJob(output->getNode(), "render"));
            _imp->schedulerInJob.reset(new IsolatedJob::ThreadScope(_imp->isolatedJob.get()));
            _imp->runningJob.store(_imp->isolatedJob.get());
        }
    }

    aboutToStartRender();

    ///Notify everyone that the render is started
    _imp->engine->s_renderStarted(forward);

    ///If the output effect is sequential (only WriteFFMPEG for now)
    EffectInstancePtr effect = _imp->outputEffect.lock();
    SequentialPreferenceEnum pref = effect->getSequentialPreference();
    if ( (pref == eSequentialPreferenceOnlySequential) || (pref == eSequentialPreferencePreferSequential) ) {
        if (effect->beginSequenceRender_public(firstFrame, lastFrame,
                                               frameStep,
                                               false,
                                               RenderScale::identity, true,
                                               true,
                                               false,
                                               ViewIdx(0),
                                               false /*useOpenGL*/,
                                               EffectInstance::OpenGLContextEffectDataPtr())
            == eStatusFailed) {
            _imp->engine->abortRenderingNoRestart();

            return;
        }
    }

    {
        QMutexLocker k(&_imp->framesToRenderMutex);
        _imp->expectFrameToRender = startingFrame;
    }
    assert(_imp->nAdmittedFrames == 0);
    _imp->admissionSlots = 1;
    _imp->maxAdmittedFrames = 0;
    _imp->admittedTimes.clear();
    _imp->admissionSkips.clear();
    _imp->forceNextAdmission = false;
    _imp->stoppingAdmittedFrames = false;
    _imp->admissionExhausted = (frameStep <= 0) || (firstFrame > lastFrame);
    _imp->playbackMode = _imp->engine->getPlaybackMode();
    if (isViewer) {
        // The display waits for the frames in its own sequence, which starts at the timeline's frame.
        _imp->nextFrameToAdmit = startingFrame;
    } else {
        _imp->nextFrameToAdmit = forward ? firstFrame : lastFrame;
    }
} // OutputSchedulerThread::startRender

void
OutputSchedulerThread::stopRender()
{
    _imp->timer.playState = ePlayStatePause;

    stopAdmittedFrames();

    ///If the output effect is sequential (only WriteFFMPEG for now)
    EffectInstancePtr effect = _imp->outputEffect.lock();
    SequentialPreferenceEnum pref = effect->getSequentialPreference();
    if ( (pref == eSequentialPreferenceOnlySequential) || (pref == eSequentialPreferencePreferSequential) ) {
        int firstFrame, lastFrame;
        OutputSchedulerThreadStartArgsPtr args = _imp->runArgs.lock();
        firstFrame = args->firstFrame;
        lastFrame = args->lastFrame;

        ignore_result( effect->endSequenceRender_public( firstFrame, lastFrame,
                                                         1,
                                                         !appPTR->isBackground(),
                                                         RenderScale::identity, true,
                                                         !appPTR->isBackground(),
                                                         false,
                                                         ViewIdx(0),
                                                         false /*use OpenGL render*/,
                                                         EffectInstance::OpenGLContextEffectDataPtr() ) );
    }


    bool wasAborted = isBeingAborted();

    // Before anything learns the render is over, so writes made once it has finished go through.
    _imp->endIsolatedJob();

    ///Notify everyone that the render is finished
    _imp->engine->s_renderFinished(wasAborted ? 1 : 0);

    onRenderStopped(wasAborted);

    // When playing once disable auto-restart
    if (!wasAborted && _imp->engine->getPlaybackMode() == ePlaybackModeOnce) {
        _imp->engine->setPlaybackAutoRestartEnabled(false);
    }


    {
        QMutexLocker k(&_imp->bufMutex);
        _imp->buf.clear();
    }

    _imp->renderTimer.reset();
} // OutputSchedulerThread::stopRender

GenericSchedulerThread::ThreadStateEnum
OutputSchedulerThread::threadLoopOnce(const GenericThreadStartArgsPtr &inArgs)
{
    OutputSchedulerThreadStartArgsPtr args = std::dynamic_pointer_cast<OutputSchedulerThreadStartArgs>(inArgs);

    assert(args);
    _imp->runArgs = args;

    ThreadStateEnum state = eThreadStateActive;
    int expectedTimeToRenderPreviousIteration = std::numeric_limits<int>::min();

    // This is the number of time this thread was woken up by a render thread because a frame was available for processing,
    // but this is not the frame this thread expects to render. If it reaches a certain amount, we detected a stall and abort.
    int nbIterationsWithoutProcessing = 0;

    startRender();

    // The frame the display last re-admitted for when nothing could bring it, so that it does so only once per frame.
    int stalledAdmissionReseededAt = std::numeric_limits<int>::min();

    for (;; ) {
        serviceAdmittedFrames();

        ///When set to true, we don't sleep in the bufEmptyCondition but in the startCondition instead, indicating
        ///we finished a render
        bool renderFinished = false;

        {
            ///_imp->renderFinished might be set when in FFA scheduling policy
            QMutexLocker l(&_imp->renderFinishedMutex);
            if (_imp->renderFinished) {
                renderFinished = true;
            }
        }
        bool bufferEmpty;
        U64 bufWakeGenerationSeen;
        {
            QMutexLocker l(&_imp->bufMutex);
            bufferEmpty = _imp->buf.empty();
            bufWakeGenerationSeen = _imp->bufWakeGeneration;
        }
        int expectedTimeToRender;


        while (!bufferEmpty) {
            state = resolveState();
            if ( (state == eThreadStateAborted) || (state == eThreadStateStopped) ) {
                ///Do not wait in the buf wait condition and go directly into the stopEngine()
                renderFinished = true;
                break;
            }


            {
                QMutexLocker k(&_imp->framesToRenderMutex);
                expectedTimeToRender = _imp->expectFrameToRender;
            }

#ifdef TRACE_SCHEDULER
            if ( (expectedTimeToRenderPreviousIteration == std::numeric_limits<int>::min()) || (expectedTimeToRenderPreviousIteration != expectedTimeToRender) ) {
                qDebug() << "Scheduler Thread: waiting for " << expectedTimeToRender << " to be rendered...";
            }
#endif
            if (expectedTimeToRenderPreviousIteration == expectedTimeToRender) {
                ++nbIterationsWithoutProcessing;
                if (nbIterationsWithoutProcessing >= NATRON_SCHEDULER_ABORT_AFTER_X_UNSUCCESSFUL_ITERATIONS) {
#ifdef TRACE_SCHEDULER
                    qDebug() << "Scheduler Thread: Detected stall after " << NATRON_SCHEDULER_ABORT_AFTER_X_UNSUCCESSFUL_ITERATIONS
                             << "unsuccessful iterations";
#endif
                    renderFinished = true;
                    break;
                }
            } else {
                nbIterationsWithoutProcessing = 0;
            }
            OutputSchedulerThreadExecMTArgsPtr framesToRender = std::make_shared<OutputSchedulerThreadExecMTArgs>();
            {
                QMutexLocker l(&_imp->bufMutex);
                _imp->getFromBufferAndErase(expectedTimeToRender, framesToRender->frames);
                bufWakeGenerationSeen = _imp->bufWakeGeneration;
            }

            ///The expected frame is not yet ready, go to sleep again
            if ( framesToRender->frames.empty() ) {
                expectedTimeToRenderPreviousIteration = expectedTimeToRender;
                break;
            }

#ifdef TRACE_SCHEDULER
            qDebug() << "Scheduler Thread: received frame to process" << expectedTimeToRender;
#endif

            int nextFrameToRender = -1;
            RenderDirectionEnum newDirection = eRenderDirectionForward;

            if (!renderFinished) {
                ///////////
                /////Refresh frame range if needed (for viewers)


                int firstFrame, lastFrame;
                getFrameRangeToRender(firstFrame, lastFrame);


                RenderDirectionEnum timelineDirection;
                int frameStep;

                const PlaybackModeEnum pMode = _imp->engine->getPlaybackMode();
                const bool sequenceChanged = (firstFrame != args->firstFrame) || (lastFrame != args->lastFrame) || (pMode != _imp->playbackMode);

                ///Refresh the firstframe/lastFrame as they might have changed on the timeline
                args->firstFrame = firstFrame;
                args->lastFrame = lastFrame;
                _imp->playbackMode = pMode;

                timelineDirection = args->processTimelineDirection;
                frameStep = args->frameStep;


                ///////////
                ///Determine if we finished rendering or if we should just increment/decrement the timeline
                ///or just loop/bounce
                renderFinished = !OutputSchedulerThreadPrivate::getNextFrameToPlay(pMode, timelineDirection,
                                                                                   expectedTimeToRender, firstFrame, lastFrame, frameStep,
                                                                                   &nextFrameToRender, &newDirection);

                if (newDirection != timelineDirection) {
                    args->processTimelineDirection = newDirection;
                }

                if (!renderFinished) {
                    {
                        QMutexLocker k(&_imp->framesToRenderMutex);
                        _imp->expectFrameToRender = nextFrameToRender;
                    }
                    if (sequenceChanged) {
                        reseedAdmission(nextFrameToRender, newDirection);
                    }
                }
            } // if (!renderFinished) {

            if (_imp->timer.playState == ePlayStateRunning) {
                waitUntilNextFrameIsDue();
            }


            state = resolveState();
            if ( (state == eThreadStateAborted) || (state == eThreadStateStopped) ) {
                // Do not wait in the buf wait condition and go directly into the stopEngine()
                renderFinished = true;
                break;
            }

            if (_imp->mode == eProcessFrameBySchedulerThread) {
                processFrame(framesToRender->frames);
            } else {
                requestExecutionOnMainThread(framesToRender);
            }

            expectedTimeToRenderPreviousIteration = expectedTimeToRender;

#ifdef TRACE_SCHEDULER
            QString pushDirectionStr = newDirection == eRenderDirectionForward ? QLatin1String("Forward") : QLatin1String("Backward");
            qDebug() << "Scheduler Thread: Frame " << expectedTimeToRender << " processed, setting expectedTimeToRender to " << nextFrameToRender
                     << ", new process direction is " << pushDirectionStr;
#endif
            if (!renderFinished) {
                ///Timeline might have changed if another thread moved the playhead
                int timelineCurrentTime = timelineGetTime();
                if (timelineCurrentTime != expectedTimeToRender) {
                    timelineGoTo(timelineCurrentTime);
                } else {
                    timelineGoTo(nextFrameToRender);
                }
            }

            ////////////
            /////At this point the frame has been processed by the output device

            assert( !framesToRender->frames.empty() );
            {
                const BufferedFrame& frame = framesToRender->frames.front();
                std::vector<ViewIdx> views(1);
                views[0] = frame.view;
                notifyFrameRendered(expectedTimeToRender, frame.view, views, frame.stats, eSchedulingPolicyOrdered);
            }
            stalledAdmissionReseededAt = std::numeric_limits<int>::min();

            // The display may go through the whole buffer without returning to the outer loop.
            serviceAdmittedFrames();

            ///////////
            /// End of the loop, refresh bufferEmpty
            {
                QMutexLocker l(&_imp->bufMutex);
                bufferEmpty = _imp->buf.empty();
                bufWakeGenerationSeen = _imp->bufWakeGeneration;
            }
        } // while(!bufferEmpty)

        if (state == eThreadStateActive) {
            state = resolveState();
        }

        if ( (state == eThreadStateAborted) || (state == eThreadStateStopped) ) {
            renderFinished = true;
        }

        if (!renderFinished) {
            assert(state == eThreadStateActive);
            int expected;
            {
                QMutexLocker k(&_imp->framesToRenderMutex);
                expected = _imp->expectFrameToRender;
            }
            bool admissionStalled;
            {
                QMutexLocker l(&_imp->bufMutex);
                BufferedFrameKey key;
                key.time = expected;
                admissionStalled = !_imp->buf.empty() && (_imp->buf.find(key) == _imp->buf.end()) && (_imp->nAdmittedFrames == 0) && (_imp->nFrameStepsOutstanding == 0) && _imp->finishedFrameSteps.empty();
            }
            if (admissionStalled) {
                if (expected != stalledAdmissionReseededAt) {
                    // Nothing buffered or in flight is the frame the display waits for, so no wake-up would ever come.
                    stalledAdmissionReseededAt = expected;
                    reseedAdmission(expected, args->processTimelineDirection);
                    _imp->forceNextAdmission = true;
                    continue;
                }
            }
            QMutexLocker bufLocker (&_imp->bufMutex);
            // An abort or quit raised since resolveState() wakes this condition under bufMutex only after flagging,
            // so re-checking the flags here means that wake-up is either seen now or arrives once waiting.
            if (!isBeingAborted() && !mustQuitThread() && (_imp->bufWakeGeneration == bufWakeGenerationSeen) && _imp->finishedFrameSteps.empty()) {
                // Wait here for more frames to be rendered, we will be woken up once appendToBuffer(...) is called
                _imp->bufEmptyCondition.wait(bufLocker.mutex());
            }
        } else {
            if ( !_imp->engine->isPlaybackAutoRestartEnabled() ) {
                //Move the timeline to the last rendered frame to keep it in sync with what is displayed
                timelineGoTo( getLastRenderedTime() );
            }
            break;
        }
    } // for (;;)

    stopRender();

    return state;
} // OutputSchedulerThread::threadLoopOnce

void
OutputSchedulerThread::onAbortRequested(bool /*keepOldestRender*/)
{
    // Otherwise the queued tasks of a frame submitted to the RenderScheduler are only dropped as they are popped, and
    // its future finishes late.
    abortPendingFrameSteps();

    ///If the scheduler is asleep waiting for the buffer to be filling up, we post a fake request
    ///that will not be processed anyway because the first thing it does is checking for abort
    {
        QMutexLocker l2(&_imp->bufMutex);
        _imp->wakeBufEmptyCondition();
    }
}

void
OutputSchedulerThread::abortPendingFrameSteps()
{
    std::vector<AbortableRenderInfoPtr> pending;
    {
        QMutexLocker l(&_imp->pendingStepsMutex);
        for (std::multimap<U64, AbortableRenderInfoPtr>::const_iterator it = _imp->pendingSteps.begin(); it != _imp->pendingSteps.end(); ++it) {
            if (it->second) {
                pending.push_back(it->second);
            }
        }
    }
    // No lock held: RenderScheduler::abort() may finish the frames, whose callbacks take pendingStepsMutex and bufMutex.
    const bool hasScheduler = appPTR->hasRenderScheduler();
    for (std::vector<AbortableRenderInfoPtr>::const_iterator it = pending.begin(); it != pending.end(); ++it) {
        (*it)->setAborted();
        if (hasScheduler) {
            appPTR->getRenderScheduler()->abort(*it);
        }
    }
}

void
OutputSchedulerThread::postFinishedFrameStep(FrameStepDone done)
{
    {
        QMutexLocker l(&_imp->bufMutex);

        _imp->finishedFrameSteps.push_back(std::move(done));
        _imp->wakeBufEmptyCondition();
    }
    // The display of a frame can keep this thread waiting for the main thread for longer than a frame takes.
    requestServiceWhileWaitingForMainThread();
    // Only now: once no step is outstanding, stopRender() returns and this scheduler may be destroyed.
    QMutexLocker l(&_imp->bufMutex);
    --_imp->nFrameStepsOutstanding;
    _imp->wakeBufEmptyCondition();
}

void
OutputSchedulerThread::awaitFrameFuture(const FrameFuturePtr& future,
                                        const AbortableRenderInfoPtr& abortInfo,
                                        const FrameStepDone& done)
{
    assert(QThread::currentThread() == this);
    U64 stepId;
    {
        QMutexLocker l(&_imp->pendingStepsMutex);
        stepId = _imp->nextStepId++;
        _imp->pendingSteps.insert(std::make_pair(stepId, abortInfo));
    }
    {
        QMutexLocker l(&_imp->bufMutex);
        ++_imp->nFrameStepsOutstanding;
    }
    // The callback may run on a pool thread, so it only hands done over to this thread. It hands its own copy over,
    // since the future destroys the callback after it returned, by when this scheduler may be gone.
    future->onFinished([this, stepId, done = FrameStepDone(done)](EffectInstance::RenderRoIRetCode /*retCode*/) mutable {
        {
            QMutexLocker l(&_imp->pendingStepsMutex);
            _imp->pendingSteps.erase(stepId);
        }
        FrameStepDone posted;
        posted.swap(done);
        postFinishedFrameStep(std::move(posted));
    });
}

namespace {

struct FrameStepState {
    U64 id = 0;
    std::vector<AbortableRenderInfoPtr> abortInfos;
    std::function<void(bool skipped)> work;
    std::function<void()> done;
    const IsolatedJob* job = nullptr;
};

} // namespace

std::function<void()>
OutputSchedulerThread::makeFrameStep(const std::vector<AbortableRenderInfoPtr>& abortInfos,
                                     const std::function<void(bool skipped)>& work,
                                     const FrameStepDone& done)
{
    assert(QThread::currentThread() == this);
    std::shared_ptr<FrameStepState> state = std::make_shared<FrameStepState>();
    {
        QMutexLocker l(&_imp->pendingStepsMutex);
        state->id = _imp->nextStepId++;
        for (std::vector<AbortableRenderInfoPtr>::const_iterator it = abortInfos.begin(); it != abortInfos.end(); ++it) {
            _imp->pendingSteps.insert(std::make_pair(state->id, *it));
        }
    }
    {
        QMutexLocker l(&_imp->bufMutex);
        ++_imp->nFrameStepsOutstanding;
    }
    state->abortInfos = abortInfos;
    state->work = work;
    state->done = done;
    state->job = getRunningIsolatedJob();

    // The thread running the step destroys it once it returned, by when this scheduler may be gone: what work holds
    // is released before the step ends, and done is handed over rather than copied.
    return [this, state]() {
        try {
            IsolatedJob::ThreadScope inJob(state->job);
            bool aborted = false;
            for (std::vector<AbortableRenderInfoPtr>::const_iterator it = state->abortInfos.begin(); it != state->abortInfos.end(); ++it) {
                aborted |= *it && (*it)->isAborted();
            }
            std::function<void(bool skipped)> work;
            work.swap(state->work);
            work(aborted);
        } catch (...) {
            // done must run on every path, or stopRender() waits for this step forever.
        }
        state->abortInfos.clear();
        {
            QMutexLocker l(&_imp->pendingStepsMutex);
            _imp->pendingSteps.erase(state->id);
        }
        FrameStepDone posted;
        posted.swap(state->done);
        postFinishedFrameStep(std::move(posted));
    };
}

void
OutputSchedulerThread::runOnFallbackWorker(const std::vector<AbortableRenderInfoPtr>& abortInfos,
                                           const std::function<void(bool skipped)>& work,
                                           const FrameStepDone& done)
{
    const std::function<void()> step = makeFrameStep(abortInfos, work, done);
    if (!_imp->fallbackWorker) {
        _imp->fallbackWorker.reset(new FrameFallbackWorker);
    }
    _imp->fallbackWorker->push(step);
}

void
OutputSchedulerThread::runOnGlobalPool(const std::vector<AbortableRenderInfoPtr>& abortInfos,
                                       int runnablePriority,
                                       const std::function<void(bool skipped)>& work,
                                       const FrameStepDone& done)
{
    const std::function<void()> step = makeFrameStep(abortInfos, work, done);
    QThreadPool::globalInstance()->start([step, runnablePriority]() {
        // The outermost scope of the task, so the pool thread leaves it with its TLS cleaned up.
        AppTLS::FrameContextScope scope(nullptr, 0, runnablePriority);
        step();
    },
                                         runnablePriority);
}

void
OutputSchedulerThread::appendNothingToDisplay(double time,
                                              ViewIdx view,
                                              const RenderStatsPtr& stats)
{
    QMutexLocker l(&_imp->bufMutex);
    BufferedFrameKey key;
    key.time = time;
    BufferedFrame value;
    value.time = time;
    value.view = view;
    value.stats = stats;
    value.displaysNothing = true;
    _imp->buf.insert(std::make_pair(key, value));
    _imp->wakeBufEmptyCondition();
}

int
OutputSchedulerThread::getNFramesInFlight() const
{
    return _imp->nAdmittedFrames.load();
}

int
OutputSchedulerThread::getMaxFramesInFlight() const
{
    return _imp->maxAdmittedFrames.load();
}

void
OutputSchedulerThread::runFinishedFrameSteps()
{
    for (;;) {
        FrameStepDone done;
        {
            QMutexLocker l(&_imp->bufMutex);
            if (_imp->finishedFrameSteps.empty()) {
                return;
            }
            done = _imp->finishedFrameSteps.front();
            _imp->finishedFrameSteps.pop_front();
        }
        done();
    }
}

void
OutputSchedulerThread::serviceAdmittedFrames()
{
    runFinishedFrameSteps();
    OutputSchedulerThreadStartArgsPtr args = _imp->runArgs.lock();
    if (args) {
        int expected;
        {
            QMutexLocker k(&_imp->framesToRenderMutex);
            expected = _imp->expectFrameToRender;
        }
        // Frames in flight when the range changed arrive afterwards.
        QMutexLocker l(&_imp->bufMutex);
        _imp->eraseUnreachableBufferedFrames(expected, args->processTimelineDirection, args->firstFrame, args->lastFrame);
    }
    admitFrames();
}

void
OutputSchedulerThread::serviceWhileWaitingForMainThread()
{
    serviceAdmittedFrames();
}

void
OutputSchedulerThread::waitUntilNextFrameIsDue()
{
    for (;;) {
        serviceAdmittedFrames();
        const double secondsLeft = _imp->timer.getSecondsUntilNextFrameIsDue();
        if (secondsLeft <= 0.) {
            break;
        }
        QMutexLocker l(&_imp->bufMutex);
        if (isBeingAborted() || mustQuitThread()) {
            return;
        }
        if (_imp->finishedFrameSteps.empty()) {
            _imp->bufEmptyCondition.wait(&_imp->bufMutex, (unsigned long)std::ceil(secondsLeft * 1000.));
        }
    }
    // Keeps the timer's frame rate accounting; it returns at once when the frame is due.
    _imp->timer.waitUntilNextFrameIsDue();
}

void
OutputSchedulerThread::reseedAdmission(int fromTime,
                                       RenderDirectionEnum direction)
{
    OutputSchedulerThreadStartArgsPtr args = _imp->runArgs.lock();
    if (!args) {
        return;
    }
    _imp->admissionSkips.clear();
    {
        QMutexLocker l(&_imp->bufMutex);
        _imp->eraseUnreachableBufferedFrames(fromTime, direction, args->firstFrame, args->lastFrame);

        // The display takes one entry per view and input of a time at once, and a nothing-to-display entry on its own.
        std::map<int, int> nothingEntries;
        std::map<std::pair<int, std::pair<int, int>>, int> imageEntries;
        std::map<int, int> maxImageEntries;
        for (FrameBuffer::const_iterator it = _imp->buf.begin(); it != _imp->buf.end(); ++it) {
            const int time = OutputSchedulerThreadPrivate::roundTime(it->first.time);
            if (it->second.displaysNothing) {
                ++nothingEntries[time];
            } else if (it->second.frame) {
                const int n = ++imageEntries[std::make_pair(time, std::make_pair((int)it->second.view, it->second.frame->getUniqueID()))];
                int& maxN = maxImageEntries[time];
                maxN = std::max(maxN, n);
            }
        }
        for (std::map<int, int>::const_iterator it = nothingEntries.begin(); it != nothingEntries.end(); ++it) {
            for (int i = 0; i < it->second; ++i) {
                _imp->admissionSkips.insert(it->first);
            }
        }
        for (std::map<int, int>::const_iterator it = maxImageEntries.begin(); it != maxImageEntries.end(); ++it) {
            for (int i = 0; i < it->second; ++i) {
                _imp->admissionSkips.insert(it->first);
            }
        }
    }
    for (std::multiset<int>::const_iterator it = _imp->admittedTimes.begin(); it != _imp->admittedTimes.end(); ++it) {
        if (_imp->isFrameReachable(*it, fromTime, direction, args->firstFrame, args->lastFrame)) {
            _imp->admissionSkips.insert(*it);
        }
    }
    // The frames admitted ahead followed the old sequence: admission resumes from the frame the display now waits for.
    _imp->nextFrameToAdmit = fromTime;
    args->pushTimelineDirection = direction;
    _imp->admissionExhausted = false;
}

void
OutputSchedulerThread::endAdmittedFrame(int time)
{
    assert(QThread::currentThread() == this);
    assert(_imp->nAdmittedFrames > 0);
    std::multiset<int>::iterator it = _imp->admittedTimes.find(time);
    if (it != _imp->admittedTimes.end()) {
        _imp->admittedTimes.erase(it);
    }
    --_imp->nAdmittedFrames;
}

bool
OutputSchedulerThread::isStoppingAdmittedFrames() const
{
    return _imp->stoppingAdmittedFrames;
}

void
OutputSchedulerThread::admitFrames()
{
    OutputSchedulerThreadStartArgsPtr args = _imp->runArgs.lock();
    if (!args) {
        return;
    }
    while (!_imp->admissionExhausted && !_imp->stoppingAdmittedFrames && !isBeingAborted() && !mustQuitThread()) {
        {
            // Frames the output has not taken yet hold their images, so a slow output stops admission before RAM runs out.
            QMutexLocker l(&_imp->bufMutex);
            if (!_imp->forceNextAdmission && isBufferFull((int)_imp->buf.size() + _imp->nAdmittedFrames, appPTR->getHardwareIdealThreadCount())) {
                return;
            }
        }
        QThreadPool* pool = QThreadPool::globalInstance();
        const int poolMax = std::max(1, pool->maxThreadCount());
        const int userSettingParallelRenders = appPTR->getCurrentSettings()->getNumberOfParallelRenders();
        const int cap = (userSettingParallelRenders > 0) ? userSettingParallelRenders : poolMax;
        _imp->admissionSlots = std::min(_imp->admissionSlots, cap);
        if (_imp->nAdmittedFrames >= cap) {
            return;
        }
        bool addsSlot = false;
        if (_imp->admissionSlots < cap) {
            // The frames only wait for the RenderScheduler, which runs their work on the global pool, so another slot
            // opens only while the work already queued cannot keep every pool thread busy. This is asked before the
            // frame that replaces an ended one is admitted: a frame that queues more work than the pool runs at once
            // would fail it straight after, and the scheduler thread wakes only when a frame step ends.
            int schedulerRunning = 0;
            int schedulerReady = 0;
            appPTR->getRenderScheduler()->getLoad(&schedulerRunning, &schedulerReady);
            addsSlot = (schedulerRunning + schedulerReady < poolMax) && (pool->activeThreadCount() < poolMax);
        }
        if (!addsSlot && (_imp->nAdmittedFrames >= _imp->admissionSlots)) {
            return;
        }

        const int time = _imp->nextFrameToAdmit;
        RenderDirectionEnum newDirection;
        _imp->admissionExhausted = !OutputSchedulerThreadPrivate::getNextFrameToPlay(_imp->playbackMode, args->pushTimelineDirection,
                                                                                     time, args->firstFrame, args->lastFrame, args->frameStep,
                                                                                     &_imp->nextFrameToAdmit, &newDirection);
        args->pushTimelineDirection = newDirection;
        std::multiset<int>::iterator skip = _imp->admissionSkips.find(time);
        if (skip != _imp->admissionSkips.end()) {
            _imp->admissionSkips.erase(skip);
            continue;
        }
        _imp->forceNextAdmission = false;
        if (addsSlot) {
            ++_imp->admissionSlots;
        }
        const int nAdmitted = ++_imp->nAdmittedFrames;
        if (nAdmitted > _imp->maxAdmittedFrames) {
            _imp->maxAdmittedFrames = nAdmitted;
        }
        _imp->admittedTimes.insert(time);
        {
            // Without it, aborted() on this thread keeps the abort info of the first frame it prepared for good.
            ScopedThreadAbortInfo abortScope(false, AbortableRenderInfoPtr(), _imp->outputEffect.lock());
            admitFrame(time, args->viewsToRender, args->enableRenderStats);
        }
    }
}

void
OutputSchedulerThread::stopAdmittedFrames()
{
    _imp->stoppingAdmittedFrames = true;
    _imp->admissionExhausted = true;
    abortPendingFrameSteps();
    for (;;) {
        runFinishedFrameSteps();
        QMutexLocker l(&_imp->bufMutex);
        if (_imp->finishedFrameSteps.empty()) {
            if (_imp->nFrameStepsOutstanding == 0) {
                break;
            }
            _imp->bufEmptyCondition.wait(&_imp->bufMutex);
        }
    }
}

int
OutputSchedulerThread::getFrameThreadsStartedForTests()
{
    return gFrameThreadsStarted.load();
}

void
OutputSchedulerThread::executeOnMainThread(const GenericThreadExecOnMainThreadArgsPtr& inArgs)
{
    OutputSchedulerThreadExecMTArgs* args = dynamic_cast<OutputSchedulerThreadExecMTArgs*>( inArgs.get() );

    assert(args);
    processFrame(args->frames);
}

void
OutputSchedulerThread::notifyFrameRendered(int frame,
                                           ViewIdx viewIndex,
                                           const std::vector<ViewIdx>& viewsToRender,
                                           const RenderStatsPtr& stats,
                                           SchedulingPolicyEnum policy)
{
    assert(viewsToRender.size() > 0);

    bool isLastView = viewIndex == viewsToRender[viewsToRender.size() - 1] || viewIndex == -1;

    // Report render stats if desired
    OutputEffectInstancePtr effect = _imp->outputEffect.lock();
    if (stats) {
        double timeSpentForFrame;
        std::map<NodePtr, NodeRenderStats > statResults = stats->getStats(&timeSpentForFrame);
        if ( !statResults.empty() ) {
            effect->reportStats(frame, viewIndex, timeSpentForFrame, statResults, stats);
        }
    }


    bool isBackground = appPTR->isBackground();
    OutputSchedulerThreadStartArgsPtr runArgs = _imp->runArgs.lock();
    assert(runArgs);

    // If FFA all parallel renders call render on the Writer in their own thread,
    // otherwise the OutputSchedulerThread thread calls the render of the Writer.
    U64 nbTotalFrames;
    U64 nbFramesRendered;
    //bool renderingIsFinished = false;
    if (policy == eSchedulingPolicyFFA) {
        QMutexLocker l(&_imp->renderFinishedMutex);
        if (isLastView) {
            ++_imp->nFramesRendered;
        }
        nbTotalFrames = std::ceil( (double)(runArgs->lastFrame - runArgs->firstFrame + 1) / runArgs->frameStep );
        nbFramesRendered = _imp->nFramesRendered;


        if (_imp->nFramesRendered == nbTotalFrames) {
            _imp->renderFinished = true;
            l.unlock();

            // Notify the scheduler rendering is finished by append a fake frame to the buffer
            {
                QMutexLocker bufLocker (&_imp->bufMutex);
                _imp->appendBufferedFrame( 0, viewIndex, RenderStatsPtr(), BufferableObjectPtr() );
                _imp->wakeBufEmptyCondition();
            }
        }
        //renderingIsFinished = _imp->renderFinished;
    } else {
        nbTotalFrames = std::floor( (double)(runArgs->lastFrame - runArgs->firstFrame + 1) / runArgs->frameStep );
        if (runArgs->processTimelineDirection == eRenderDirectionForward) {
            nbFramesRendered = (frame - runArgs->firstFrame) / runArgs->frameStep;
        } else {
            nbFramesRendered = (runArgs->lastFrame - frame) / runArgs->frameStep;
        }
    } // if (policy == eSchedulingPolicyFFA) {

    double fractionDone = 0.;
    assert(nbTotalFrames > 0);
    if (nbTotalFrames != 0) {
        QMutexLocker k(&_imp->renderFinishedMutex);
        fractionDone = (double)_imp->nFramesRendered / nbTotalFrames;
    }
    assert(_imp->renderTimer);
    double timeSpentSinceStartSec = _imp->renderTimer->getTimeSinceCreation();
    double estimatedFps = (double)nbFramesRendered / timeSpentSinceStartSec;
    // total estimated time is: timeSpentSinceStartSec / fractionDone
    // remaining time is thus:
    double timeRemaining = (nbTotalFrames <= 0 || _imp->nFramesRendered <= 0) ? -1. : timeSpentSinceStartSec / fractionDone - timeSpentSinceStartSec;

    // If running in background, notify to the pipe that we rendered a frame
    if (isBackground) {
        QString longMessage;
        QTextStream ts(&longMessage);
        QString frameStr = QString::number(frame);
        QString fpsStr = QString::number(estimatedFps, 'f', 1);
        QString percentageStr = QString::number(fractionDone * 100, 'f', 1);
        QString timeRemainingStr = timeRemaining < 0 ? tr("unknown") : Timer::printAsTime(timeRemaining, true);

        longMessage = (tr("%1 ==> Frame: %2, Progress: %3%, %4 Fps, Time Remaining: %5")
                       .arg(QString::fromUtf8(effect->getScriptName_mt_safe().c_str()))
                       .arg(frameStr)
                       .arg(percentageStr)
                       .arg(fpsStr)
                       .arg(timeRemainingStr));

        QString shortMessage = QString::fromUtf8(kFrameRenderedStringShort) + frameStr + QString::fromUtf8(kProgressChangedStringShort) + QString::number(fractionDone);
        {
            QMutexLocker l(&_imp->bufferedOutputMutex);
            std::string toPrint = longMessage.toStdString();
            if ( (_imp->lastBufferedOutputSize != 0) && ( _imp->lastBufferedOutputSize > longMessage.size() ) ) {
                int nSpacesToAppend = _imp->lastBufferedOutputSize - longMessage.size();
                toPrint.append(nSpacesToAppend, ' ');
            }
            //std::cout << '\r';
            std::cout << toPrint;
            std::cout << std::endl;

            /*std::cout.flush();
               if (renderingIsFinished) {
                std::cout << std::endl;
               }*/
            _imp->lastBufferedOutputSize = longMessage.size();
        }

        appPTR->writeToOutputPipe(longMessage, shortMessage, false);
    }

    // Notify we rendered a frame
    if (isLastView) {
        _imp->engine->s_frameRendered(frame, fractionDone);
    }

    // Call Python after frame ranedered callback
    if ( isLastView && effect->isWriter() ) {
        std::string cb = effect->getNode()->getAfterFrameRenderCallback();
        if ( !cb.empty() ) {
            std::vector<std::string> args;
            std::string error;
            try {
                NATRON_PYTHON_NAMESPACE::getFunctionArguments(cb, &error, &args);
            } catch (const std::exception& e) {
                effect->getApp()->appendToScriptEditor( std::string("Failed to get signature of onFrameRendered callback: ")
                                                        + e.what() );

                return;
            }

            if ( !error.empty() ) {
                effect->getApp()->appendToScriptEditor("Failed to get signature of onFrameRendered callback: " + error);

                return;
            }

            std::string signatureError;
            signatureError.append("The after frame render callback supports the following signature(s):\n");
            signatureError.append("- callback(frame, thisNode, app)");
            if ( (args.size() != 3) || (args[0] != "frame") || (args[1] != "thisNode") || (args[2] != "app") ) {
                effect->getApp()->appendToScriptEditor("Wrong signature of onFrameRendered callback: " + signatureError);

                return;
            }

            std::stringstream ss;
            std::string appStr = effect->getApp()->getAppIDString();
            std::string outputNodeName = appStr + "." + effect->getNode()->getFullyQualifiedName();
            ss << cb << "(" << frame << ", ";
            ss << outputNodeName << ", " << appStr << ")";
            std::string script = ss.str();
            try {
                runCallbackWithVariables( QString::fromUtf8( script.c_str() ) );
            } catch (const std::exception& e) {
                notifyRenderFailure( e.what() );

                return;
            }
        }
    }
} // OutputSchedulerThread::notifyFrameRendered

void
OutputSchedulerThread::appendToBuffer_internal(double time,
                                               ViewIdx view,
                                               const RenderStatsPtr& stats,
                                               const BufferableObjectPtr& frame,
                                               bool wakeThread)
{
    if ( QThread::currentThread() == qApp->thread() ) {
        ///Single-threaded , call directly the function
        if (frame) {
            BufferedFrame b;
            b.time = time;
            b.view = view;
            b.frame = frame;
            BufferedFrames frames;
            frames.push_back(b);
            processFrame(frames);
        }
    } else {
        ///Called by the scheduler thread when an image is rendered

        QMutexLocker l(&_imp->bufMutex);
        _imp->appendBufferedFrame(time, view, stats, frame);
        if (wakeThread) {
            ///Wake up the scheduler thread that an image is available if it is asleep so it can process it.
            _imp->wakeBufEmptyCondition();
        }
    }
}

void
OutputSchedulerThread::appendToBuffer(double time,
                                      ViewIdx view,
                                      const RenderStatsPtr& stats,
                                      const BufferableObjectPtr& image)
{
    appendToBuffer_internal(time, view, stats, image, true);
}

void
OutputSchedulerThread::appendToBuffer(double time,
                                      ViewIdx view,
                                      const RenderStatsPtr& stats,
                                      const BufferableObjectPtrList& frames)
{
    if ( frames.empty() ) {
        return;
    }
    BufferableObjectPtrList::const_iterator next = frames.begin();
    if ( next != frames.end() ) {
        ++next;
    }
    for (BufferableObjectPtrList::const_iterator it = frames.begin(); it != frames.end(); ++it) {
        if ( next != frames.end() ) {
            appendToBuffer_internal(time, view, stats, *it, false);
            ++next;
        } else {
            appendToBuffer_internal(time, view, stats, *it, true);
        }
    }
}

void
OutputSchedulerThread::setDesiredFPS(double d)
{
    _imp->timer.setDesiredFrameRate(d);
}

double
OutputSchedulerThread::getDesiredFPS() const
{
    return _imp->timer.getDesiredFrameRate();
}

void
OutputSchedulerThread::getLastRunArgs(RenderDirectionEnum* direction,
                                      std::vector<ViewIdx>* viewsToRender) const
{
    QMutexLocker k(&_imp->lastRunArgsMutex);

    *direction = _imp->lastPlaybackRenderDirection;
    *viewsToRender = _imp->lastPlaybackViewsToRender;
}

void
OutputSchedulerThread::renderFrameRange(bool isBlocking,
                                        bool enableRenderStats,
                                        int firstFrame,
                                        int lastFrame,
                                        int frameStep,
                                        const std::vector<ViewIdx>& viewsToRender,
                                        RenderDirectionEnum direction)
{
    {
        QMutexLocker k(&_imp->lastRunArgsMutex);
        _imp->lastPlaybackRenderDirection = direction;
        _imp->lastPlaybackViewsToRender = viewsToRender;
    }
    if (direction == eRenderDirectionForward) {
        timelineGoTo(firstFrame);
    } else {
        timelineGoTo(lastFrame);
    }

    OutputSchedulerThreadStartArgsPtr args = std::make_shared<OutputSchedulerThreadStartArgs>(isBlocking, enableRenderStats, firstFrame, lastFrame, frameStep, viewsToRender, direction);

    {
        QMutexLocker k(&_imp->renderFinishedMutex);
        _imp->nFramesRendered = 0;
        _imp->renderFinished = false;
    }

    startTask(args);
}

void
OutputSchedulerThread::renderFromCurrentFrame(bool enableRenderStats,
                                              const std::vector<ViewIdx>& viewsToRender,
                                              RenderDirectionEnum timelineDirection)
{
    {
        QMutexLocker k(&_imp->lastRunArgsMutex);
        _imp->lastPlaybackRenderDirection = timelineDirection;
        _imp->lastPlaybackViewsToRender = viewsToRender;
    }
    int firstFrame, lastFrame;

    getFrameRangeToRender(firstFrame, lastFrame);

    ///Make sure current frame is in the frame range
    int currentTime = timelineGetTime();
    OutputSchedulerThreadPrivate::getNearestInSequence(timelineDirection, currentTime, firstFrame, lastFrame, &currentTime);
    OutputSchedulerThreadStartArgsPtr args = std::make_shared<OutputSchedulerThreadStartArgs>(false, enableRenderStats, firstFrame, lastFrame, 1, viewsToRender, timelineDirection);
    startTask(args);
}

void
OutputSchedulerThread::notifyRenderFailure(const std::string& errorMessage)
{
    assert(_imp->runArgs.lock());

    ///Handle failure: for viewers we make it black and don't display the error message which is irrelevant
    handleRenderFailure(errorMessage);

    _imp->engine->abortRenderingNoRestart();
}

OutputSchedulerThreadStartArgsPtr
OutputSchedulerThread::getCurrentRunArgs() const
{
    return _imp->runArgs.lock();
}

RenderEngine*
OutputSchedulerThread::getEngine() const
{
    return _imp->engine;
}

void
OutputSchedulerThread::runCallbackWithVariables(const QString& callback)
{
    if ( !callback.isEmpty() ) {
        // The render's own callbacks may change what it reads.
        IsolatedJob::ThreadScope inJob(_imp->runningJob.load());
        OutputEffectInstancePtr effect = _imp->outputEffect.lock();
        QString script = callback;
        std::string appID = effect->getApp()->getAppIDString();
        // Callbacks run by a render are not edits the user made.
        UndoSuppressGuard noUndo(effect->getApp());
        std::string nodeName = effect->getNode()->getFullyQualifiedName();
        std::string nodeFullName = appID + "." + nodeName;
        script.append( QString::fromUtf8( nodeFullName.c_str() ) );
        script.append( QLatin1Char(',') );
        script.append( QString::fromUtf8( appID.c_str() ) );
        script.append( QString::fromUtf8(")\n") );

        std::string err, output;
        if ( !NATRON_PYTHON_NAMESPACE::interpretPythonScript(callback.toStdString(), &err, &output) ) {
            effect->getApp()->appendToScriptEditor("Failed to run callback: " + err);
            throw std::runtime_error(err);
        } else if ( !output.empty() ) {
            effect->getApp()->appendToScriptEditor(output);
        }
    }
}

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
DefaultScheduler::DefaultScheduler(RenderEngine* engine,
                                   const OutputEffectInstancePtr& effect)
    : OutputSchedulerThread(engine, effect, eProcessFrameBySchedulerThread)
    , _effect(effect)
    , _currentTimeMutex()
    , _currentTime(0)
{
    engine->setPlaybackMode(ePlaybackModeOnce);
}

DefaultScheduler::~DefaultScheduler()
{
}

namespace {

struct FrameStatsObserverSlot {
    QMutex mutex;
    DefaultScheduler::FrameStatsObserver observer;
};

FrameStatsObserverSlot&
frameStatsObserverSlot()
{
    static FrameStatsObserverSlot slot;

    return slot;
}

void
notifyFrameStatsObserver(int time,
                         ViewIdx view,
                         const RenderStatsPtr& stats)
{
    DefaultScheduler::FrameStatsObserver observer;
    {
        FrameStatsObserverSlot& slot = frameStatsObserverSlot();
        QMutexLocker k(&slot.mutex);
        observer = slot.observer;
    }
    if (observer) {
        observer(time, view, stats);
    }
}

/**
 * @brief Why the frame whose args setter installed on this thread cannot be rendered by the RenderScheduler, or null if
 * it can.
 **/
const char*
taskGraphIneligibility(const ParallelRenderArgsSetter& setter)
{
    // Only the flags a writer frame can set have a reason.
    static const SchedulerIneligibilityReasons writerReasons = {
        .onPoolThread = "frame thread is a pool thread",
        .onMainThread = "frame thread is the main thread",
        .noFrameArgs = "no frame args",
        .paintStroke = "paint stroke",
        .analysis = "analysis",
    };
    const std::map<NodePtr, ParallelRenderArgsPtr>& args = setter.getInstalledArgs();
    SchedulerEligibility eligibility;

    eligibility.onPoolThread = QThreadPool::globalInstance()->contains(QThread::currentThread());
    eligibility.onMainThread = QCoreApplication::instance() && (QThread::currentThread() == QCoreApplication::instance()->thread());
    eligibility.hasFrameArgs = !args.empty();
    eligibility.inputHasFrameArgs = true;
    scanFrameArgsForScheduler(args, &eligibility);

    const char* reason = 0;
    isFrameEligibleForScheduler(eligibility, writerReasons, &reason);

    return reason;
}

} // namespace

void
DefaultScheduler::setFrameStatsObserverForTests(const FrameStatsObserver& observer)
{
    FrameStatsObserverSlot& slot = frameStatsObserverSlot();
    QMutexLocker k(&slot.mutex);

    slot.observer = observer;
}

struct DefaultScheduler::WriterFrame {
    int time;
    std::vector<ViewIdx> views;
    std::size_t view;
    RenderStatsPtr stats;
    AbortableRenderInfoPtr abortInfo;
    FrameFuturePtr future;
    std::unique_ptr<RenderingFlagSetter> renderingFlag;
    // Set when the view cannot go to the RenderScheduler and must render on the fallback worker.
    const char* fallbackReason;
    bool failed;
    std::string failureMessage;
    EffectInstance::RenderRoIRetCode retCode;

    WriterFrame(int time,
                const std::vector<ViewIdx>& views,
                bool enableRenderStats)
        : time(time)
        , views(views)
        , view(0)
        // Even without render stats, writers profile the time spent rendering the frame.
        , stats(std::make_shared<RenderStats>(enableRenderStats))
        , abortInfo()
        , future()
        , renderingFlag()
        , fallbackReason(0)
        , failed(false)
        , failureMessage()
        , retCode(EffectInstance::eRenderRoIRetCodeOk)
    {
    }

    void fail(const std::string& message)
    {
        failed = true;
        failureMessage = message;
    }
};

void
DefaultScheduler::admitFrame(int time,
                             const std::vector<ViewIdx>& viewsToRender,
                             bool enableRenderStats)
{
    OutputEffectInstancePtr output = _effect.lock();

    if (!output || viewsToRender.empty()) {
        notifyRenderFailure("");
        endAdmittedFrame(time);

        return;
    }

    NodePtr outputNode = output->getNode();
    std::string cb = outputNode->getBeforeFrameRenderCallback();
    if (!cb.empty()) {
        std::vector<std::string> args;
        std::string error;
        try {
            NATRON_PYTHON_NAMESPACE::getFunctionArguments(cb, &error, &args);
        } catch (const std::exception& e) {
            error = e.what();
        }

        if (!error.empty()) {
            const std::string message = "Failed to get signature of beforeFrameRendered callback: " + error;
            output->getApp()->appendToScriptEditor(message);
            notifyRenderFailure(message);
            endAdmittedFrame(time);

            return;
        }

        std::string signatureError;
        signatureError.append("The before frame render callback supports the following signature(s):\n");
        signatureError.append("- callback(frame, thisNode, app)");
        if ((args.size() != 3) || (args[0] != "frame") || (args[1] != "thisNode") || (args[2] != "app")) {
            const std::string message = "Wrong signature of beforeFrameRendered callback: " + signatureError;
            output->getApp()->appendToScriptEditor(message);
            notifyRenderFailure(message);
            endAdmittedFrame(time);

            return;
        }

        std::stringstream ss;
        std::string appStr = outputNode->getApp()->getAppIDString();
        std::string outputNodeName = appStr + "." + outputNode->getFullyQualifiedName();
        ss << cb << "(" << time << ", " << outputNodeName << ", " << appStr << ")";
        std::string script = ss.str();
        try {
            runCallbackWithVariables(QString::fromUtf8(script.c_str()));
        } catch (const std::exception& e) {
            notifyRenderFailure(e.what());
            endAdmittedFrame(time);

            return;
        }
    }

    startWriterView(std::make_shared<WriterFrame>(time, viewsToRender, enableRenderStats));
}

void
DefaultScheduler::startWriterView(const std::shared_ptr<WriterFrame>& frame)
{
    frame->abortInfo = AbortableRenderInfo::create(true, 0);
    frame->future.reset();
    frame->fallbackReason = 0;

    OutputEffectInstancePtr output = _effect.lock();
    bool onWorker = false;
    if (output) {
        // These pull their inputs and wait for them, which only the worker may do. Deciding before the frame's args
        // are installed keeps the scheduler thread from building a request the worker would build again.
        if (output->writesFrameRangeAtOnce()) {
            onWorker = true;
        } else if (output->getOutputDataKind() == eDataKindDeep) {
            frame->fallbackReason = "deep output";
            onWorker = true;
        }
    }

    if (!onWorker && runWriterView(*frame, false)) {
        awaitFrameFuture(frame->future, frame->abortInfo, [this, frame]() {
            frame->retCode = frame->future->result();
            frame->renderingFlag.reset();
            frame->future.reset();
            finishWriterView(frame);
        });

        return;
    }
    if (onWorker || frame->fallbackReason) {
        runOnFallbackWorker(std::vector<AbortableRenderInfoPtr>(1, frame->abortInfo), [this, frame](bool skipped) {
            if (skipped) {
                frame->retCode = EffectInstance::eRenderRoIRetCodeAborted;
            } else {
                ignore_result( runWriterView(*frame, true) );
            } }, [this, frame]() { finishWriterView(frame); });

        return;
    }
    finishWriterView(frame);
}

bool
DefaultScheduler::runWriterView(WriterFrame& frame,
                                bool onFallbackWorker)
{
    OutputEffectInstancePtr output = _effect.lock();

    if (!output) {
        frame.fail("");

        return false;
    }

    const int time = frame.time;
    const ViewIdx view = frame.views[frame.view];
    const RenderStatsPtr& stats = frame.stats;
    const AbortableRenderInfoPtr& abortInfo = frame.abortInfo;

    try {
        ////Writers always render at scale 1.
        const int mipmapLevel = 0;
        const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
        RectD rod;
        bool isProjectFormat;

        // Do not catch exceptions: if an exception occurs here it is probably fatal, since
        // it comes from Natron itself. All exceptions from plugins are already caught
        // by the HostSupport library.
        EffectInstancePtr activeInputToRender = output;
        NodePtr activeInputNode = activeInputToRender->getNode();
        const bool isRenderDueToRenderInteraction = false;
        const bool isSequentialRender = true;
        const DataKindEnum outputKind = activeInputToRender->getOutputDataKind();

        // Both threads that run this prepare frame after frame, so each frame's abort info must go when it ends.
        ScopedThreadAbortInfo abortScope(isRenderDueToRenderInteraction, abortInfo, activeInputToRender);

        ParallelRenderArgsSetter frameRenderArgs(time,
                                                 view,
                                                 isRenderDueToRenderInteraction, // is this render due to user interaction ?
                                                 isSequentialRender,
                                                 abortInfo, // abortInfo
                                                 activeInputNode, // viewer requester
                                                 0, // texture index
                                                 output->getApp()->getTimeLine().get(),
                                                 NodePtr(),
                                                 false,
                                                 false,
                                                 stats);

        // After the frame's snapshot is taken, so the check reads it rather than deriving
        // the graph's metadata on its own.
        if (output->checkFrameToWrite(time, view) == eStatusFailed) {
            QString message;
            int type = 0;
            activeInputNode->getPersistentMessage(&message, &type, false);
            frame.fail(message.isEmpty() ? std::string("Error caught while rendering") : message.toStdString());

            return false;
        }
        if (output->writesFrameRangeAtOnce()) {
            // The render was narrowed to this one frame; the node writes the whole range
            // it was asked for here, rather than rendering an output of its own.
            int firstFrame = time;
            int lastFrame = time;
            output->getRunningRenderRequestedRange(&firstFrame, &lastFrame);
            if (output->writeFrameRange(firstFrame, lastFrame, view) == eStatusFailed) {
                QString message;
                int type = 0;
                activeInputNode->getPersistentMessage(&message, &type, false);
                frame.fail(message.isEmpty() ? std::string("Error caught while writing") : message.toStdString());
            }

            return false;
        }

        // The frame's region, layers and layout all come from the snapshot its args took,
        // so an edit made while the sequence renders cannot pair one state's region with
        // another's layout.
        const U64 activeInputToRenderHash = activeInputToRender->getRenderHash();
        const NodeMetadataConstPtr renderMetadata = activeInputToRender->getRenderMetadata(time, view);
        StatusEnum stat = activeInputToRender->getRegionOfDefinition_public(activeInputToRenderHash, time, scale, view, &rod, &isProjectFormat);
        if (stat == eStatusFailed) {
            frame.fail("Error caught while rendering");

            return false;
        }
        std::list<ImageLayerDesc> components;
        ImageBitDepthEnum imageDepth = eImageBitDepthNone;

        if ((outputKind == eDataKindImage) || (outputKind == eDataKindRaw)) {
            // Use needed components to figure out what we need to render
            EffectInstance::ComponentsNeededMap neededComps;
            std::list<ImageLayerDesc> passThroughLayers;
            double ptTime;
            int ptView;
            std::bitset<4> processChannels;
            EffectInstance::ProcessChannelsPerPlaneMap processChannelsPerPlane;
            int ptInput;
            activeInputToRender->getComponentsNeededAndProduced_public(activeInputToRenderHash, time, view, &neededComps, &passThroughLayers, &ptTime, &ptView, &processChannels, &processChannelsPerPlane, &ptInput);

            // Retrieve bitdepth only
            imageDepth = renderMetadata->getBitDepth(-1);
            components.clear();

            EffectInstance::ComponentsNeededMap::iterator foundOutput = neededComps.find(-1);
            if (foundOutput != neededComps.end()) {
                for (std::list<ImageLayerDesc>::const_iterator it2 = foundOutput->second.begin(); it2 != foundOutput->second.end(); ++it2) {
                    components.push_back(*it2);
                }
            }
        }
        const RectI renderWindow = rod.toPixelEnclosing(scale, renderMetadata->getPixelAspectRatio(-1));

        std::shared_ptr<FrameRequestMap> request = std::make_shared<FrameRequestMap>();
        stat = EffectInstance::computeRequestPass(time, view, mipmapLevel, rod, activeInputNode, *request);
        if (stat == eStatusFailed) {
            frame.fail("Error caught while rendering");

            return false;
        }
        frameRenderArgs.updateNodesRequest(*request);

        std::unique_ptr<RenderingFlagSetter> flagIsRendering(new RenderingFlagSetter(activeInputToRender->getNode()));
        switch (outputKind) {
        // A raw payload is a float Image in the image cache, so it renders like an image.
        case eDataKindRaw:
        case eDataKindImage: {
            if (!onFallbackWorker) {
                const char* fallbackReason = taskGraphIneligibility(frameRenderArgs);
                if (!fallbackReason) {
                    FrameRenderContextPtr context = FrameRenderContext::createFromSetter(frameRenderArgs, abortInfo, stats, time, view);
                    context->setRequest(request);
                    FrameGraph graph = RenderScheduler::buildGraph(context, activeInputNode, time, view, mipmapLevel);
                    if (graph.tasks.empty()) {
                        fallbackReason = "request does not reach the writer";
                    } else {
                        RenderScheduler::pruneBehindCacheHits(graph, context);
                        placeRenderGraph(graph, context);
                        frame.future = appPTR->getRenderScheduler()->submit(context, std::move(graph), RenderScheduler::Priority::Background);
                        frame.renderingFlag = std::move(flagIsRendering);

                        return true;
                    }
                }
                frame.fallbackReason = fallbackReason;

                return false;
            }
            if (frame.fallbackReason && stats) {
                stats->incLegacyFallbacks(frame.fallbackReason);
            }
            std::map<ImageLayerDesc, ImagePtr> layers;
            EffectInstance::RenderRoIArgs renderArgs(time, //< the time at which to render
                                                     scale, //< the scale at which to render
                                                     mipmapLevel, //< the mipmap level (redundant with the scale)
                                                     view, //< the view to render
                                                     false,
                                                     renderWindow, //< the region of interest (in pixel coordinates)
                                                     rod, // < any precomputed rod ? in canonical coordinates
                                                     components,
                                                     imageDepth,
                                                     false,
                                                     activeInputToRender.get(),
                                                     eStorageModeRAM,
                                                     time);
            frame.retCode = activeInputToRender->renderRoI(renderArgs, &layers);
            break;
        }
        case eDataKindDeep: {
            if (stats) {
                stats->incLegacyFallbacks("deep output");
            }
            DeepImagePtr deepImage;
            EffectInstance::RenderDeepRoIArgs renderArgs(time,
                                                         scale,
                                                         mipmapLevel,
                                                         view,
                                                         false, //< byPassCache
                                                         renderWindow,
                                                         rod,
                                                         activeInputToRender.get(),
                                                         time);
            frame.retCode = activeInputToRender->renderDeepRoI(renderArgs, &deepImage);
            break;
        }
        default:
            frame.fail(unsupportedOutputKindMessage(activeInputNode));
            break;
        }
    } catch (const std::exception& e) {
        frame.fail(std::string("Error while rendering: ") + e.what());
    }

    return false;
} // DefaultScheduler::runWriterView

void
DefaultScheduler::finishWriterView(const std::shared_ptr<WriterFrame>& frame)
{
    bool ok = false;
    if (frame->failed) {
        notifyRenderFailure(frame->failureMessage);
    } else if (frame->retCode == EffectInstance::eRenderRoIRetCodeAborted) {
        notifyRenderFailure("Render aborted");
    } else if (frame->retCode != EffectInstance::eRenderRoIRetCodeOk) {
        notifyRenderFailure("Error caught while rendering");
    } else {
        ok = true;
        const ViewIdx view = frame->views[frame->view];
        notifyFrameStatsObserver(frame->time, view, frame->stats);
        notifyFrameRendered(frame->time, view, frame->views, frame->stats, eSchedulingPolicyFFA);
    }

    ++frame->view;
    if (ok && (frame->view < frame->views.size()) && !isStoppingAdmittedFrames() && !isBeingAborted()) {
        startWriterView(frame);

        return;
    }
    endAdmittedFrame(frame->time);
}

void
DefaultScheduler::timelineStepOne(RenderDirectionEnum direction)
{
    QMutexLocker k(&_currentTimeMutex);

    if (direction == eRenderDirectionForward) {
        ++_currentTime;
    } else {
        --_currentTime;
    }
}

void
DefaultScheduler::timelineGoTo(int time)
{
    QMutexLocker k(&_currentTimeMutex);

    _currentTime =  time;
}

int
DefaultScheduler::timelineGetTime() const
{
    QMutexLocker k(&_currentTimeMutex);

    return _currentTime;
}

void
DefaultScheduler::getFrameRangeToRender(int& first,
                                        int& last) const
{
    OutputSchedulerThreadStartArgsPtr args = getCurrentRunArgs();

    first = args->firstFrame;
    last = args->lastFrame;
}

void
DefaultScheduler::handleRenderFailure(const std::string& errorMessage)
{
    if ( appPTR->isBackground() ) {
        std::cerr << errorMessage << std::endl;
    }
}

SchedulingPolicyEnum
DefaultScheduler::getSchedulingPolicy() const
{
    /*SequentialPreferenceEnum sequentiallity = _effect.lock()->getSequentialPreference();
       if (sequentiallity == eSequentialPreferenceNotSequential) {*/
    return eSchedulingPolicyFFA;
    /*} else {
        return eSchedulingPolicyOrdered;
       }*/
}

void
DefaultScheduler::aboutToStartRender()
{
    OutputSchedulerThreadStartArgsPtr args = getCurrentRunArgs();
    OutputEffectInstancePtr effect = _effect.lock();

    {
        QMutexLocker k(&_currentTimeMutex);
        if (args->pushTimelineDirection == eRenderDirectionForward) {
            _currentTime  = args->firstFrame;
        } else {
            _currentTime  = args->lastFrame;
        }
    }
    bool isBackGround = appPTR->isBackground();

    if (!isBackGround) {
        effect->setKnobsFrozen(true);
    } else {
        QString longText = QString::fromUtf8( effect->getScriptName_mt_safe().c_str() ) + tr(" ==> Rendering started");
        appPTR->writeToOutputPipe(longText, QString::fromUtf8(kRenderingStartedShort), true);
    }

    std::string cb = effect->getNode()->getBeforeRenderCallback();
    if ( !cb.empty() ) {
        std::vector<std::string> args;
        std::string error;
        try {
            NATRON_PYTHON_NAMESPACE::getFunctionArguments(cb, &error, &args);
        } catch (const std::exception& e) {
            effect->getApp()->appendToScriptEditor( std::string("Failed to get signature of beforeRender callback: ")
                                                    + e.what() );

            return;
        }

        if ( !error.empty() ) {
            effect->getApp()->appendToScriptEditor("Failed to get signature of beforeRender callback: " + error);

            return;
        }

        std::string signatureError;
        signatureError.append("The beforeRender callback supports the following signature(s):\n");
        signatureError.append("- callback(thisNode, app)");
        if ( (args.size() != 2) || (args[0] != "thisNode") || (args[1] != "app") ) {
            effect->getApp()->appendToScriptEditor("Failed to run beforeRender callback: " + signatureError);

            return;
        }


        std::stringstream ss;
        std::string appStr = effect->getApp()->getAppIDString();
        std::string outputNodeName = appStr + "." + effect->getNode()->getFullyQualifiedName();
        ss << cb << "(" << outputNodeName << ", " << appStr << ")";
        std::string script = ss.str();
        try {
            runCallbackWithVariables( QString::fromUtf8( script.c_str() ) );
        } catch (const std::exception &e) {
            notifyRenderFailure( e.what() );
        }
    }
} // DefaultScheduler::aboutToStartRender

void
DefaultScheduler::runAfterRenderCallback(const OutputEffectInstancePtr& effect,
                                         bool aborted)
{
    std::string cb = effect->getNode()->getAfterRenderCallback();
    if (cb.empty()) {
        return;
    }

    std::vector<std::string> args;
    std::string error;
    try {
        NATRON_PYTHON_NAMESPACE::getFunctionArguments(cb, &error, &args);
    } catch (const std::exception& e) {
        effect->getApp()->appendToScriptEditor(std::string("Failed to get signature of afterRender callback: ")
                                               + e.what());

        return;
    }

    if (!error.empty()) {
        effect->getApp()->appendToScriptEditor("Failed to get signature of afterRender callback: " + error);

        return;
    }

    std::string signatureError;
    signatureError.append("The after render callback supports the following signature(s):\n");
    signatureError.append("- callback(aborted, thisNode, app)");
    if ((args.size() != 3) || (args[0] != "aborted") || (args[1] != "thisNode") || (args[2] != "app")) {
        effect->getApp()->appendToScriptEditor("Wrong signature of afterRender callback: " + signatureError);

        return;
    }

    std::stringstream ss;
    std::string appStr = effect->getApp()->getAppIDString();
    std::string outputNodeName = appStr + "." + effect->getNode()->getFullyQualifiedName();
    ss << cb << "(";
    if (aborted) {
        ss << "True, ";
    } else {
        ss << "False, ";
    }
    ss << outputNodeName << ", " << appStr << ")";
    std::string script = ss.str();
    try {
        runCallbackWithVariables(QString::fromUtf8(script.c_str()));
    } catch (...) {
        // Ignore expcetions in callback since the render is finished anyway
    }
} // DefaultScheduler::runAfterRenderCallback

void
DefaultScheduler::onRenderStopped(bool aborted)
{
    OutputEffectInstancePtr effect = _effect.lock();
    bool isBackGround = appPTR->isBackground();

    if (!isBackGround) {
        effect->setKnobsFrozen(false);
    }

    {
        QString longText = QString::fromUtf8( effect->getScriptName_mt_safe().c_str() ) + tr(" ==> Rendering finished");
        appPTR->writeToOutputPipe(longText, QString::fromUtf8(kRenderingFinishedStringShort), true);
    }

    runAfterRenderCallback(effect, aborted);

    // Last, so that a blocking render, which this releases, returns only once its callback has run.
    effect->notifyRenderFinished();
} // DefaultScheduler::onRenderStopped

////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////
//////////////////////// ViewerDisplayScheduler ////////////


ViewerDisplayScheduler::ViewerDisplayScheduler(RenderEngine* engine,
                                               const ViewerInstancePtr& viewer)
    : OutputSchedulerThread(engine, viewer, eProcessFrameByMainThread) //< OpenGL rendering is done on the main-thread
    , _viewer(viewer)
{
}

ViewerDisplayScheduler::~ViewerDisplayScheduler()
{
}

/**
 * @brief Called whenever there are images available to process in the buffer.
 * Once processed, the frame will be removed from the buffer.
 *
 * According to the ProcessFrameModeEnum given to the scheduler this function will be called either by the scheduler thread (this)
 * or by the application's main-thread (typically to do OpenGL rendering).
 **/
void
ViewerDisplayScheduler::processFrame(const BufferedFrames& frames)
{
    ViewerInstancePtr viewer = _viewer.lock();

    std::list<UpdateViewerParamsPtr> toDisplay;
    for (BufferedFrames::const_iterator it = frames.begin(); it != frames.end(); ++it) {
        if (it->frame) {
            UpdateViewerParamsPtr params = std::dynamic_pointer_cast<UpdateViewerParams>(it->frame);
            assert(params);
            toDisplay.push_back(params);
        }
    }
    if (!toDisplay.empty()) {
        viewer->aboutToUpdateTextures();
        for (std::list<UpdateViewerParamsPtr>::iterator it = toDisplay.begin(); it != toDisplay.end(); ++it) {
            viewer->updateViewer(*it);
        }
        viewer->redrawViewerNow();
    } else {
        viewer->redrawViewer();
    }
}

void
ViewerDisplayScheduler::timelineStepOne(RenderDirectionEnum direction)
{
    ViewerInstancePtr viewer = _viewer.lock();

    if (direction == eRenderDirectionForward) {
        viewer->getTimeline()->incrementCurrentFrame();
    } else {
        viewer->getTimeline()->decrementCurrentFrame();
    }
}

void
ViewerDisplayScheduler::timelineGoTo(int time)
{
    ViewerInstancePtr viewer = _viewer.lock();

    viewer->getTimeline()->seekFrame(time, false, 0, eTimelineChangeReasonPlaybackSeek);
}

int
ViewerDisplayScheduler::timelineGetTime() const
{
    return _viewer.lock()->getTimeline()->currentFrame();
}

void
ViewerDisplayScheduler::getFrameRangeToRender(int &first,
                                              int &last) const
{
    ViewerInstancePtr viewer = _viewer.lock();
    ViewerInstance* leadViewer = viewer->getApp()->getLastViewerUsingTimeline();
    ViewerInstance* v = leadViewer ? leadViewer : viewer.get();

    assert(v);
    v->getTimelineBounds(&first, &last);
}

struct ViewerDisplayScheduler::ViewerFrame {
    ViewerInstancePtr viewer;
    ViewerInstance::PlaybackFrame playback;
    // Aborted before its finish ran: whatever it left undone is no failure.
    bool skipped = false;
};

void
ViewerDisplayScheduler::admitFrame(int time,
                                   const std::vector<ViewIdx>& viewsToRender,
                                   bool enableRenderStats)
{
    std::shared_ptr<ViewerFrame> frame = std::make_shared<ViewerFrame>();
    frame->viewer = _viewer.lock();
    if (!frame->viewer || viewsToRender.empty()) {
        notifyRenderFailure(std::string());
        endAdmittedFrame(time);

        return;
    }
    ViewerInstance::PlaybackFrame& playback = frame->playback;
    playback.time = time;
    // A viewer displays one view.
    playback.view = viewsToRender.front();
    if (enableRenderStats) {
        playback.stats = std::make_shared<RenderStats>(enableRenderStats);
    }

    ViewerInstance::PlaybackFrameStageEnum stage = ViewerInstance::ePlaybackFrameStageDone;
    try {
        stage = frame->viewer->preparePlaybackFrame(&playback);
    } catch (...) {
        playback.failed = true;
        stage = ViewerInstance::ePlaybackFrameStageDone;
        // Nothing waits for what was submitted before the throw any more.
        for (std::vector<AbortableRenderInfoPtr>::const_iterator it = playback.abortInfos.begin(); it != playback.abortInfos.end(); ++it) {
            (*it)->setAborted();
            if (appPTR->hasRenderScheduler()) {
                appPTR->getRenderScheduler()->abort(*it);
            }
        }
    }

    switch (stage) {
    case ViewerInstance::ePlaybackFrameStageAwaitFutures: {
        playback.futuresOutstanding = (int)playback.futures.size();
        for (std::size_t i = 0; i < playback.futures.size(); ++i) {
            awaitFrameFuture(playback.futures[i], playback.futureAbortInfos[i], [this, frame]() {
                if (--frame->playback.futuresOutstanding > 0) {
                    return;
                }
                if (isStoppingAdmittedFrames()) {
                    frame->skipped = true;
                    finishViewerFrame(frame);

                    return;
                }
                // Off this thread, which would convert the frames one at a time, and on the pool, since the futures
                // it completes have finished.
                runOnGlobalPool(frame->playback.abortInfos, RenderScheduler::runnablePriority(RenderScheduler::Priority::Interactive), [frame](bool skipped) {
                    if (skipped) {
                        frame->skipped = true;
                    } else {
                        frame->viewer->finishPlaybackFrame(frame->playback);
                    } }, [this, frame]() { finishViewerFrame(frame); });
            });
        }
        break;
    }
    case ViewerInstance::ePlaybackFrameStageFallback:
        // An input that pulls its own inputs waits on them, which a pool thread must not do.
        runOnFallbackWorker(playback.abortInfos, [frame](bool skipped) {
            if (skipped) {
                frame->skipped = true;
            } else {
                frame->viewer->finishPlaybackFrame(frame->playback);
            } }, [this, frame]() { finishViewerFrame(frame); });
        break;
    case ViewerInstance::ePlaybackFrameStageDone:
        finishViewerFrame(frame);
        break;
    }
} // ViewerDisplayScheduler::admitFrame

void
ViewerDisplayScheduler::finishViewerFrame(const std::shared_ptr<ViewerFrame>& frame)
{
    ViewerInstance::PlaybackFrame& playback = frame->playback;

    if (playback.failed && !frame->skipped) {
        // The plug-in that failed posts its own, more helpful, error message.
        notifyRenderFailure(std::string());
    }
    if (playback.toAppend.empty()) {
        appendNothingToDisplay(playback.time, playback.view, playback.stats);
    } else {
        appendToBuffer(playback.time, playback.view, playback.stats, playback.toAppend);
    }
    // The callbacks of the futures hold the frame.
    playback.futures.clear();
    for (int i = 0; i < 2; ++i) {
        playback.pending[i].reset();
    }
    endAdmittedFrame(playback.time);
}

void
ViewerDisplayScheduler::handleRenderFailure(const std::string& /*errorMessage*/)
{
    _viewer.lock()->disconnectViewer();
}

void
ViewerDisplayScheduler::onRenderStopped(bool /*/aborted*/)
{
    ///Refresh all previews in the tree
    ViewerInstancePtr viewer = _viewer.lock();

    viewer->getApp()->refreshAllPreviews();

    if ( !viewer->getApp() || viewer->getApp()->isGuiFrozen() ) {
        getEngine()->s_refreshAllKnobs();
    }
}

int
ViewerDisplayScheduler::getLastRenderedTime() const
{
    return _viewer.lock()->getLastRenderedTime();
}

////////////////////////// RenderEngine

struct RenderEnginePrivate
{
    QMutex schedulerCreationLock;
    OutputSchedulerThread* scheduler;

    //If true then a current frame render can start playback, protected by abortedRequestedMutex
    bool canAutoRestartPlayback;
    QMutex canAutoRestartPlaybackMutex; // protects abortRequested
    OutputEffectInstanceWPtr output;
    mutable QMutex pbModeMutex;
    PlaybackModeEnum pbMode;
    ViewerCurrentFrameRequestScheduler* currentFrameScheduler;

    // Only used on the main-thread
    std::unique_ptr<RenderEngineWatcher> engineWatcher;
    struct RefreshRequest
    {
        bool enableStats;
        bool enableAbort;
    };

    /*
       This queue tracks all calls made to renderCurrentFrame() and attempts to concatenate the calls
       once the event loop fires the signal currentFrameRenderRequestPosted()
       This is only accessed on the main thread
     */
    std::list<RefreshRequest> refreshQueue;

    RenderEnginePrivate(const OutputEffectInstancePtr& output)
        : schedulerCreationLock()
        , scheduler(0)
        , canAutoRestartPlayback(false)
        , canAutoRestartPlaybackMutex()
        , output(output)
        , pbModeMutex()
        , pbMode(ePlaybackModeLoop)
        , currentFrameScheduler(0)
        , refreshQueue()
    {
    }
};

RenderEngine::RenderEngine(const OutputEffectInstancePtr& output)
    : _imp( new RenderEnginePrivate(output) )
{
    QObject::connect(this, SIGNAL(currentFrameRenderRequestPosted()), this, SLOT(onCurrentFrameRenderRequestPosted()), Qt::QueuedConnection);
}

RenderEngine::~RenderEngine()
{
    delete _imp->currentFrameScheduler;
    _imp->currentFrameScheduler = 0;
    delete _imp->scheduler;
    _imp->scheduler = 0;
}

OutputSchedulerThread*
RenderEngine::createScheduler(const OutputEffectInstancePtr& effect)
{
    return new DefaultScheduler(this, effect);
}

OutputEffectInstancePtr
RenderEngine::getOutput() const
{
    return _imp->output.lock();
}

void
RenderEngine::renderFrameRange(bool isBlocking,
                               bool enableRenderStats,
                               int firstFrame,
                               int lastFrame,
                               int frameStep,
                               const std::vector<ViewIdx>& viewsToRender,
                               RenderDirectionEnum forward)
{
    setPlaybackAutoRestartEnabled(true);

    {
        QMutexLocker k(&_imp->schedulerCreationLock);
        if (!_imp->scheduler) {
            _imp->scheduler = createScheduler( _imp->output.lock() );
        }
    }

    _imp->scheduler->renderFrameRange(isBlocking, enableRenderStats, firstFrame, lastFrame, frameStep, viewsToRender, forward);
}

void
RenderEngine::renderFromCurrentFrame(bool enableRenderStats,
                                     const std::vector<ViewIdx>& viewsToRender,
                                     RenderDirectionEnum forward)
{
    setPlaybackAutoRestartEnabled(true);

    {
        QMutexLocker k(&_imp->schedulerCreationLock);
        if (!_imp->scheduler) {
            _imp->scheduler = createScheduler( _imp->output.lock() );
        }
    }

    _imp->scheduler->renderFromCurrentFrame(enableRenderStats, viewsToRender, forward);
}

void
RenderEngine::onCurrentFrameRenderRequestPosted()
{
    assert( QThread::currentThread() == qApp->thread() );

    //Okay we are at the end of the event loop, concatenate all similar events
    RenderEnginePrivate::RefreshRequest r;
    bool rSet = false;
    while ( !_imp->refreshQueue.empty() ) {
        const RenderEnginePrivate::RefreshRequest& queueBegin = _imp->refreshQueue.front();
        if (!rSet) {
            rSet = true;
        } else {
            if ( (queueBegin.enableAbort == r.enableAbort) && (queueBegin.enableStats == r.enableStats) ) {
                _imp->refreshQueue.pop_front();
                continue;
            }
        }
        r = queueBegin;
        renderCurrentFrameInternal(r.enableStats, r.enableAbort);
        _imp->refreshQueue.pop_front();
    }
}

void
RenderEngine::renderCurrentFrameInternal(bool enableRenderStats,
                                         bool canAbort)
{
    assert( QThread::currentThread() == qApp->thread() );

    ViewerInstance* isViewer = dynamic_cast<ViewerInstance*>( _imp->output.lock().get() );
    if (!isViewer) {
        qDebug() << "RenderEngine::renderCurrentFrame for a writer is unsupported";

        return;
    }

    ///If the scheduler is already doing playback, continue it
    if (_imp->scheduler) {
        bool working = _imp->scheduler->isWorking();
        if (working) {
            _imp->scheduler->abortThreadedTask();
        }
        if ( working || isPlaybackAutoRestartEnabled() ) {
            RenderDirectionEnum lastDirection;
            std::vector<ViewIdx> lastViews;
            _imp->scheduler->getLastRunArgs(&lastDirection, &lastViews);
            _imp->scheduler->renderFromCurrentFrame( enableRenderStats, lastViews,  lastDirection);

            return;
        }
    }


    {
        QMutexLocker k(&_imp->schedulerCreationLock);
        if (!_imp->scheduler) {
            _imp->scheduler = createScheduler( _imp->output.lock() );
        }
    }

    if (!_imp->currentFrameScheduler) {
        _imp->currentFrameScheduler = new ViewerCurrentFrameRequestScheduler(isViewer);
    }

    _imp->currentFrameScheduler->renderCurrentFrame(enableRenderStats, canAbort);
}

void
RenderEngine::renderCurrentFrame(bool enableRenderStats,
                                 bool canAbort)
{
    assert( QThread::currentThread() == qApp->thread() );
    RenderEnginePrivate::RefreshRequest r;
    r.enableStats = enableRenderStats;
    r.enableAbort = canAbort;
    _imp->refreshQueue.push_back(r);
    Q_EMIT currentFrameRenderRequestPosted();
}

void
RenderEngine::setPlaybackAutoRestartEnabled(bool enabled)
{
    QMutexLocker k(&_imp->canAutoRestartPlaybackMutex);

    _imp->canAutoRestartPlayback = enabled;
}

bool
RenderEngine::isPlaybackAutoRestartEnabled() const
{
    QMutexLocker k(&_imp->canAutoRestartPlaybackMutex);

    return _imp->canAutoRestartPlayback;
}

void
RenderEngine::quitEngine(bool allowRestarts)
{
    if (_imp->scheduler) {
        _imp->scheduler->quitThread(allowRestarts);
    }

    if (_imp->currentFrameScheduler) {
        _imp->currentFrameScheduler->quitThread(allowRestarts);
    }
}

void
RenderEngine::waitForEngineToQuit_not_main_thread()
{
    if (_imp->scheduler) {
        _imp->scheduler->waitForThreadToQuit_not_main_thread();
    }

    if (_imp->currentFrameScheduler) {
        _imp->currentFrameScheduler->waitForThreadToQuit_not_main_thread();
    }
}

void
RenderEngine::waitForEngineToQuit_main_thread(bool allowRestart)
{
    assert( QThread::currentThread() == qApp->thread() );
    assert(!_imp->engineWatcher);
    _imp->engineWatcher.reset( new RenderEngineWatcher(this) );
    QObject::connect( _imp->engineWatcher.get(), SIGNAL(taskFinished(int,GenericWatcherCallerArgsPtr)), this, SLOT(onWatcherEngineQuitEmitted()) );
    _imp->engineWatcher->scheduleBlockingTask(allowRestart ? RenderEngineWatcher::eBlockingTaskWaitForQuitAllowRestart : RenderEngineWatcher::eBlockingTaskWaitForQuitDisallowRestart);
}

void
RenderEngine::waitForEngineToQuit_enforce_blocking()
{
    if (_imp->scheduler) {
        _imp->scheduler->waitForThreadToQuit_enforce_blocking();
    }

    if (_imp->currentFrameScheduler) {
        _imp->currentFrameScheduler->waitForThreadToQuit_enforce_blocking();
    }
}

bool
RenderEngine::abortRenderingInternal(bool keepOldestRender)
{
    bool ret = false;

    if (_imp->currentFrameScheduler) {
        ret |= _imp->currentFrameScheduler->abortThreadedTask(keepOldestRender);
    }

    if ( _imp->scheduler && _imp->scheduler->isWorking() ) {
        //If any playback active, abort it
        ret |= _imp->scheduler->abortThreadedTask(keepOldestRender);
    }

    return ret;
}

bool
RenderEngine::abortRenderingNoRestart(bool keepOldestRender)
{
    if ( abortRenderingInternal(keepOldestRender) ) {
        setPlaybackAutoRestartEnabled(false);

        return true;
    }

    return false;
}

bool
RenderEngine::abortRenderingAutoRestart()
{
    if ( abortRenderingInternal(true) ) {
        return true;
    }

    return false;
}

void
RenderEngine::waitForAbortToComplete_not_main_thread()
{
    if (_imp->currentFrameScheduler) {
        _imp->currentFrameScheduler->waitForAbortToComplete_not_main_thread();
    }
    if (_imp->scheduler) {
        _imp->scheduler->waitForAbortToComplete_not_main_thread();
    }
}

void
RenderEngine::onWatcherEngineQuitEmitted()
{
    assert(_imp->engineWatcher);
    if (!_imp->engineWatcher) {
        return;
    }
    _imp->engineWatcher.reset();
    Q_EMIT engineQuit();
}

bool
RenderEngine::isSequentialRenderBeingAborted() const
{
    if (!_imp->scheduler) {
        return false;
    }

    return _imp->scheduler->isBeingAborted();
}

int
RenderEngine::getNFramesInFlight() const
{
    return _imp->scheduler ? _imp->scheduler->getNFramesInFlight() : 0;
}

int
RenderEngine::getMaxFramesInFlight() const
{
    return _imp->scheduler ? _imp->scheduler->getMaxFramesInFlight() : 0;
}

bool
RenderEngine::hasThreadsAlive() const
{
    bool schedulerRunning = false;

    if (_imp->scheduler) {
        schedulerRunning = _imp->scheduler->isRunning();
    }
    bool currentFrameSchedulerRunning = false;
    if (_imp->currentFrameScheduler) {
        // The render itself may run on the backup thread after the scheduler thread has stopped.
        currentFrameSchedulerRunning = _imp->currentFrameScheduler->isRunning() || _imp->currentFrameScheduler->isBackupThreadRunning();
    }

    return schedulerRunning || currentFrameSchedulerRunning;
}

bool
RenderEngine::hasThreadsWorking() const
{
    bool working = false;

    if (_imp->scheduler) {
        working |= _imp->scheduler->isWorking();
    }

    if (!working && _imp->currentFrameScheduler) {
        working |= _imp->currentFrameScheduler->isWorking();
    }

    return working;
}

bool
RenderEngine::isDoingSequentialRender() const
{
    return _imp->scheduler ? _imp->scheduler->isWorking() : false;
}

void
RenderEngine::waitUntilSequentialRenderStops(const std::function<bool()>& stopWaiting) const
{
    if (_imp->scheduler) {
        _imp->scheduler->waitUntilNotActive(stopWaiting);
    }
}

void
RenderEngine::setPlaybackMode(int mode)
{
    QMutexLocker l(&_imp->pbModeMutex);

    _imp->pbMode = (PlaybackModeEnum)mode;
}

PlaybackModeEnum
RenderEngine::getPlaybackMode() const
{
    QMutexLocker l(&_imp->pbModeMutex);

    return _imp->pbMode;
}

void
RenderEngine::setDesiredFPS(double d)
{
    {
        QMutexLocker k(&_imp->schedulerCreationLock);
        if (!_imp->scheduler) {
            _imp->scheduler = createScheduler( _imp->output.lock() );
        }
    }
    _imp->scheduler->setDesiredFPS(d);
}

double
RenderEngine::getDesiredFPS() const
{
    return _imp->scheduler ? _imp->scheduler->getDesiredFPS() : 24;
}

void
RenderEngine::notifyFrameProduced(const BufferableObjectPtrList& frames,
                                  const RenderStatsPtr& stats,
                                  const ViewerCurrentFrameRequestSchedulerStartArgsPtr& request)
{
    _imp->currentFrameScheduler->notifyFrameProduced(frames, stats, request);
}

OutputSchedulerThread*
ViewerRenderEngine::createScheduler(const OutputEffectInstancePtr& effect)
{
    return new ViewerDisplayScheduler( this, std::dynamic_pointer_cast<ViewerInstance>(effect) );
}

////////////////////////ViewerCurrentFrameRequestScheduler////////////////////////

// One live stroke render in flight per viewer.
struct StrokeRenderGate {
    StrokeRenderCoalescer coalescer;
    ViewerInstance* viewer;

    explicit StrokeRenderGate(ViewerInstance* viewer)
        : coalescer()
        , viewer(viewer)
    {
    }
};

typedef std::shared_ptr<StrokeRenderGate> StrokeRenderGatePtr;
typedef std::weak_ptr<StrokeRenderGate> StrokeRenderGateWPtr;

static void
releaseStrokeRenderGate(const StrokeRenderGateWPtr& weakGate)
{
    assert(QThread::currentThread() == qApp->thread());
    StrokeRenderGatePtr gate = weakGate.lock();
    if (!gate) {
        return;
    }
    if (!gate->coalescer.finish()) {
        return;
    }
    NodePtr rotoPaintNode;
    RotoStrokeItemPtr stroke;
    bool isDrawing = false;
    gate->viewer->getApp()->getActiveRotoDrawingStroke(&rotoPaintNode, &stroke, &isDrawing);
    // After pen-up the neat render covers the whole stroke.
    if (isDrawing) {
        gate->viewer->renderCurrentFrame(true);
    }
}

class CurrentFrameFunctorArgs
    : public GenericThreadStartArgs
{
public:

    ViewIdx view;
    int time;
    RenderStatsPtr stats;
    ViewerInstance* viewer;
    U64 viewerHash;
    ViewerCurrentFrameRequestSchedulerStartArgsPtr request;
    ViewerCurrentFrameRequestSchedulerPrivate* scheduler;
    bool canAbort;
    NodePtr isRotoPaintRequest;
    RotoStrokeItemWPtr strokeItem;
    ViewerArgsPtr args[2];
    bool isRotoNeatRender;
    // Two viewers may both see the neat render request; only the one that took it ends it.
    bool tookNeatRender;
    std::uint64_t latencyMark;
    StrokeRenderGateWPtr strokeGate;
    RenderEndWatch::TicketPtr renderEndTicket;

    CurrentFrameFunctorArgs()
        : GenericThreadStartArgs()
        , view(0)
        , time(0)
        , stats()
        , viewer(0)
        , viewerHash(0)
        , request()
        , scheduler(0)
        , canAbort(true)
        , isRotoPaintRequest()
        , strokeItem()
        , args()
        , isRotoNeatRender(false)
        , tookNeatRender(false)
        , latencyMark(0)
        , strokeGate()
        , renderEndTicket()
    {
    }

    CurrentFrameFunctorArgs(ViewIdx view,
                            int time,
                            const RenderStatsPtr& stats,
                            ViewerInstance* viewer,
                            U64 viewerHash,
                            ViewerCurrentFrameRequestSchedulerPrivate* scheduler,
                            bool canAbort,
                            const NodePtr& isRotoPaintRequest,
                            const RotoStrokeItemPtr& strokeItem,
                            bool isRotoNeatRender)
        : GenericThreadStartArgs()
        , view(view)
        , time(time)
        , stats(stats)
        , viewer(viewer)
        , viewerHash(viewerHash)
        , request()
        , scheduler(scheduler)
        , canAbort(canAbort)
        , isRotoPaintRequest(isRotoPaintRequest)
        , strokeItem(strokeItem)
        , args()
        , isRotoNeatRender(isRotoNeatRender)
        , tookNeatRender(false)
        , latencyMark(0)
        , strokeGate()
        , renderEndTicket()
    {
        if (isRotoPaintRequest && isRotoNeatRender) {
            tookNeatRender = isRotoPaintRequest->getRotoContext()->setIsDoingNeatRender(true);
        }
    }

    ~CurrentFrameFunctorArgs()
    {
        if (isRotoPaintRequest && tookNeatRender) {
            isRotoPaintRequest->getRotoContext()->setIsDoingNeatRender(false);
        }
        if (!strokeGate.expired()) {
            StrokeRenderGateWPtr gate = strokeGate;
            QMetaObject::invokeMethod(qApp, [gate]() { releaseStrokeRenderGate(gate); }, Qt::QueuedConnection);
        }
    }
};

typedef std::shared_ptr<CurrentFrameFunctorArgs> CurrentFrameFunctorArgsPtr;

class RenderCurrentFrameFunctorRunnable;
struct ViewerCurrentFrameRequestSchedulerPrivate
{
    ViewerInstance* viewer;
    QThreadPool* threadPool;
    QMutex producedFramesMutex;
    ProducedFrameSet producedFrames;
    QWaitCondition producedFramesNotEmpty;


    /**
     * Single thread used by the ViewerCurrentFrameRequestScheduler when the global thread pool has reached its maximum
     * activity to keep the renders responsive even if the thread pool is choking.
     **/
    ViewerCurrentFrameRequestRendererBackup backupThread;
    mutable QMutex currentFrameRenderTasksMutex;
    QWaitCondition currentFrameRenderTasksCond;
    std::list<RenderCurrentFrameFunctorRunnable*> currentFrameRenderTasks;

    // Used to attribute an age to each renderCurrentFrameRequest
    U64 ageCounter;

    StrokeRenderGatePtr strokeGate;

    ViewerCurrentFrameRequestSchedulerPrivate(ViewerInstance* viewer)
        : viewer(viewer)
        , threadPool(QThreadPool::globalInstance())
        , producedFramesMutex()
        , producedFrames()
        , producedFramesNotEmpty()
        , backupThread()
        , currentFrameRenderTasksCond()
        , currentFrameRenderTasks()
        , ageCounter(0)
        , strokeGate(std::make_shared<StrokeRenderGate>(viewer))
    {
    }

    void appendRunnableTask(RenderCurrentFrameFunctorRunnable* task)
    {
        {
            QMutexLocker k(&currentFrameRenderTasksMutex);
            currentFrameRenderTasks.push_back(task);
        }
    }

    void removeRunnableTask(RenderCurrentFrameFunctorRunnable* task)
    {
        {
            QMutexLocker k(&currentFrameRenderTasksMutex);
            for (std::list<RenderCurrentFrameFunctorRunnable*>::iterator it = currentFrameRenderTasks.begin();
                 it != currentFrameRenderTasks.end(); ++it) {
                if (*it == task) {
                    currentFrameRenderTasks.erase(it);

                    currentFrameRenderTasksCond.wakeAll();
                    break;
                }
            }
        }
    }

    void waitForRunnableTasks()
    {
        QMutexLocker k(&currentFrameRenderTasksMutex);

        while ( !currentFrameRenderTasks.empty() ) {
            currentFrameRenderTasksCond.wait(k.mutex());
        }
    }

    void notifyFrameProduced(const BufferableObjectPtrList& frames,
                             const RenderStatsPtr& stats,
                             U64 age,
                             const StrokeDisplayMark& strokeMark = StrokeDisplayMark())
    {
        QMutexLocker k(&producedFramesMutex);
        ProducedFrame p;

        p.frames = frames;
        p.age = age;
        p.stats = stats;
        p.strokeMark = strokeMark;
        producedFrames.insert(p);
        producedFramesNotEmpty.wakeOne();
    }

    void processProducedFrame(const RenderStatsPtr& stats,
                              const BufferableObjectPtrList& frames,
                              const StrokeDisplayMark& strokeMark = StrokeDisplayMark());
};

class RenderCurrentFrameFunctorRunnable
    : public QRunnable
{
    CurrentFrameFunctorArgsPtr _args;

public:

    RenderCurrentFrameFunctorRunnable(const CurrentFrameFunctorArgsPtr& args)
        : _args(args)
    {
    }

    virtual ~RenderCurrentFrameFunctorRunnable()
    {
    }

    virtual void run() OVERRIDE FINAL
    {
#ifdef DEBUG
        boost_adaptbx::floating_point::exception_trapping trap(boost_adaptbx::floating_point::exception_trapping::division_by_zero |
                                                               boost_adaptbx::floating_point::exception_trapping::invalid |
                                                               boost_adaptbx::floating_point::exception_trapping::overflow);
#endif
        ///The viewer always uses the scheduler thread to regulate the output rate, @see ViewerInstance::renderViewer_internal
        ///it calls appendToBuffer by itself
        ViewerInstance::ViewerRenderRetCode stat = ViewerInstance::eViewerRenderRetCodeFail;
        BufferableObjectPtrList ret;

        U64 renderAges[2] = { 0, 0 };
        bool hadRender[2] = { false, false };
        for (int i = 0; i < 2; ++i) {
            if (_args->args[i] && _args->args[i]->params && _args->args[i]->params->abortInfo) {
                renderAges[i] = _args->args[i]->params->abortInfo->getRenderAge();
                hadRender[i] = true;
            }
        }

        RotoLatencyProbe* latencyProbe = RotoLatencyProbe::instance();
        if (latencyProbe) {
            latencyProbe->noteRenderStarted(_args->latencyMark);
        }

        try {
            if (!_args->isRotoPaintRequest || _args->isRotoNeatRender) {
                stat = _args->viewer->renderViewer(_args->view, QThread::currentThread() == qApp->thread(), false, _args->viewerHash, _args->canAbort,
                                                   NodePtr(), true, _args->args, _args->request, _args->stats);
            } else {
                stat = _args->viewer->getViewerArgsAndRenderViewer(_args->time, _args->canAbort, _args->view, _args->viewerHash, _args->isRotoPaintRequest, _args->strokeItem.lock(), _args->stats, &_args->args[0], &_args->args[1]);
            }
        } catch (...) {
            stat = ViewerInstance::eViewerRenderRetCodeFail;
        }

        if (stat == ViewerInstance::eViewerRenderRetCodeFail) {
            // An exception or a lost UI context leaves the requests unsettled; settling twice is a no-op.
            for (int i = 0; i < 2; ++i) {
                if (hadRender[i]) {
                    _args->viewer->renderFailed(i, renderAges[i]);
                }
            }
            /// Don't report any error message otherwise we will flood the viewer with irrelevant messages such as
            ///"Render failed", instead we let the plug-in that failed post an error message which will be more helpful.
            // renderViewer() drops the arguments of each input that failed.
            bool failed[2] = { false, false };
            bool anyKnownFailure = false;
            for (int i = 0; i < 2; ++i) {
                failed[i] = hadRender[i] && !_args->args[i];
                anyKnownFailure |= failed[i];
            }
            if (anyKnownFailure && (!_args->isRotoPaintRequest || _args->isRotoNeatRender)) {
                _args->viewer->disconnectViewerAfterFailedRender(renderAges, failed);
            } else {
                _args->viewer->disconnectViewer();
            }
            ret.clear();
        } else {
            for (int i = 0; i < 2; ++i) {
                if (_args->args[i] && _args->args[i]->params) {
                    if (_args->args[i]->params->tiles.size() > 0) {
                        ret.push_back(_args->args[i]->params);
                    }
                }
            }
            if (latencyProbe) {
                latencyProbe->noteRenderFinished(_args->latencyMark);
            }
        }

        StrokeDisplayMark strokeMark;
        if (_args->isRotoPaintRequest && !_args->isRotoNeatRender && !ret.empty()) {
            // Renders of a stroke run one at a time, so the app's stroke data are still the ones this render used.
            RotoStrokeItemPtr stroke = _args->strokeItem.lock();
            RotoStrokeItemPtr appStroke;
            int multiStrokeIndex = -1;
            _args->viewer->getApp()->getStrokeAndMultiStrokeIndex(&appStroke, &multiStrokeIndex);
            if (stroke && (appStroke == stroke)) {
                strokeMark.rotoPaintNode = _args->isRotoPaintRequest;
                strokeMark.stroke = stroke;
                strokeMark.multiStrokeIndex = multiStrokeIndex;
                strokeMark.age = _args->viewer->getApp()->getStrokeLastIndex();
            }
        }
        if (_args->tookNeatRender && !ret.empty()) {
            // Before the frame is handed on, so its display cannot overtake the hand-over.
            strokeMark.rotoPaintNode = _args->isRotoPaintRequest;
            strokeMark.neatRender = true;
            _args->isRotoPaintRequest->getRotoContext()->notifyNeatRenderFrameProduced();
            _args->tookNeatRender = false;
        }

        if (_args->request) {
#ifdef DEBUG
            for (BufferableObjectPtrList::iterator it = ret.begin(); it != ret.end(); ++it) {
                UpdateViewerParams* isParams = dynamic_cast<UpdateViewerParams*>( it->get() );
                assert(isParams);
                assert( !isParams->tiles.empty() );
#ifdef NATRON_HAVE_GPU
                const bool deviceFrame = bool(isParams->deviceColor);
#else
                const bool deviceFrame = false;
#endif
                for (std::list<UpdateViewerParams::CachedTile>::iterator it2 = isParams->tiles.begin(); it2 != isParams->tiles.end(); ++it2) {
                    assert(deviceFrame || it2->ramBuffer);
                }
                Q_UNUSED(deviceFrame);
            }
#endif
            _args->scheduler->notifyFrameProduced(ret, _args->stats, _args->request->age, strokeMark);
        } else {
            assert( QThread::currentThread() == qApp->thread() );
            _args->scheduler->processProducedFrame(_args->stats, ret, strokeMark);
        }

        ///This thread is done, clean-up its TLS
        appPTR->getAppTLS()->cleanupTLSForThread();


        _args->scheduler->removeRunnableTask(this);
    } // run
};


class ViewerCurrentFrameRequestSchedulerExecOnMT
    : public GenericThreadExecOnMainThreadArgs
{
public:

    RenderStatsPtr stats;
    BufferableObjectPtrList frames;
    StrokeDisplayMark strokeMark;

    ViewerCurrentFrameRequestSchedulerExecOnMT()
        : GenericThreadExecOnMainThreadArgs()
    {
    }

    virtual ~ViewerCurrentFrameRequestSchedulerExecOnMT()
    {
    }
};

typedef std::shared_ptr<ViewerCurrentFrameRequestSchedulerExecOnMT> ViewerCurrentFrameRequestSchedulerExecOnMTPtr;

ViewerCurrentFrameRequestScheduler::ViewerCurrentFrameRequestScheduler(ViewerInstance* viewer)
    : GenericSchedulerThread()
    , _imp( new ViewerCurrentFrameRequestSchedulerPrivate(viewer) )
{
    setThreadName("ViewerCurrentFrameRequestScheduler");
}

ViewerCurrentFrameRequestScheduler::~ViewerCurrentFrameRequestScheduler()
{
    // The viewer may already be destroyed, so only the backup thread is quit here: quitting this thread would call
    // back into the viewer. Joining it before _imp goes keeps a render from outliving the scheduler.
    _imp->backupThread.quitThread(false);
    _imp->backupThread.wait();
}

bool
ViewerCurrentFrameRequestScheduler::isBackupThreadRunning() const
{
    return _imp->backupThread.isRunning();
}

GenericSchedulerThread::TaskQueueBehaviorEnum
ViewerCurrentFrameRequestScheduler::tasksQueueBehaviour() const
{
    return eTaskQueueBehaviorProcessInOrder;
}

GenericSchedulerThread::ThreadStateEnum
ViewerCurrentFrameRequestScheduler::threadLoopOnce(const GenericThreadStartArgsPtr &inArgs)
{
    ThreadStateEnum state = eThreadStateActive;
    ViewerCurrentFrameRequestSchedulerStartArgsPtr args = std::dynamic_pointer_cast<ViewerCurrentFrameRequestSchedulerStartArgs>(inArgs);

    assert(args);

#ifdef TRACE_CURRENT_FRAME_SCHEDULER
    qDebug() << getThreadName().c_str() << "Thread loop once, waiting for" << args->age << "to be produced";
#endif

    ///Wait for the work to be done
    ViewerCurrentFrameRequestSchedulerExecOnMTPtr mtArgs = std::make_shared<ViewerCurrentFrameRequestSchedulerExecOnMT>();
    {
        QMutexLocker k(&_imp->producedFramesMutex);
        ProducedFrameSet::iterator found = _imp->producedFrames.end();
        for (ProducedFrameSet::iterator it = _imp->producedFrames.begin(); it != _imp->producedFrames.end(); ++it) {
            if (it->age == args->age) {
                found = it;
                break;
            }
        }

        while ( found == _imp->producedFrames.end() ) {
            state = resolveState();
            if ( (state == eThreadStateStopped) || (state == eThreadStateAborted) ) {
                //_imp->producedFrames.clear();
                break;
            }
            _imp->producedFramesNotEmpty.wait(k.mutex());

            for (ProducedFrameSet::iterator it = _imp->producedFrames.begin(); it != _imp->producedFrames.end(); ++it) {
                if (it->age == args->age) {
                    found = it;
                    break;
                }
            }
        }

        if ( found != _imp->producedFrames.end() ) {
#ifdef TRACE_CURRENT_FRAME_SCHEDULER
            qDebug() << getThreadName().c_str() << "Found" << args->age << "produced";
#endif

            mtArgs->frames = found->frames;
            mtArgs->stats = found->stats;
            mtArgs->strokeMark = found->strokeMark;

            // Erase from the produced frames all renders that are older that the age we want to render
            // since they are no longer going to be used.
            // Only do this if the render had frames it is not just a redraw
            //if (!found->frames.empty()) {
            //    ++found;
            //    _imp->producedFrames.erase(_imp->producedFrames.begin(), found);
            //} else {
            ++found;
            _imp->producedFrames.erase(_imp->producedFrames.begin(), found);
            //}
        } else {
#ifdef TRACE_CURRENT_FRAME_SCHEDULER
            qDebug() << getThreadName().c_str() << "Got aborted, skip waiting for" << args->age;
#endif
        }
    } // QMutexLocker k(&_imp->producedQueueMutex);


    if (state == eThreadStateActive) {
        state = resolveState();
    }
    // Do not uncomment the second part: if we don't update on the viewer aborted tasks then user will never even see valid images that were fully rendered
    if (state == eThreadStateStopped /*|| (state == eThreadStateAborted)*/ ) {
        return state;
    }

    requestExecutionOnMainThread(mtArgs);

#ifdef TRACE_CURRENT_FRAME_SCHEDULER
    qDebug() << getThreadName().c_str() << "Frame processed" << args->age;
#endif

    return state;
} // ViewerCurrentFrameRequestScheduler::threadLoopOnce

void
ViewerCurrentFrameRequestScheduler::executeOnMainThread(const GenericThreadExecOnMainThreadArgsPtr& inArgs)
{
    ViewerCurrentFrameRequestSchedulerExecOnMT* args = dynamic_cast<ViewerCurrentFrameRequestSchedulerExecOnMT*>( inArgs.get() );

    assert(args);
    if (args) {
        _imp->processProducedFrame(args->stats, args->frames, args->strokeMark);
    }
}

void
ViewerCurrentFrameRequestSchedulerPrivate::processProducedFrame(const RenderStatsPtr& stats,
                                                                const BufferableObjectPtrList& frames,
                                                                const StrokeDisplayMark& strokeMark)
{
    assert( QThread::currentThread() == qApp->thread() );

    if ( !frames.empty() ) {
        viewer->aboutToUpdateTextures();
    }

    bool updatedTextures = false;
    for (BufferableObjectPtrList::const_iterator it2 = frames.begin(); it2 != frames.end(); ++it2) {
        assert(*it2);
        UpdateViewerParamsPtr params = std::dynamic_pointer_cast<UpdateViewerParams>(*it2);
        assert(params);
        if ( params && (params->tiles.size() >= 1) ) {
            if (stats) {
                double timeSpent;
                std::map<NodePtr, NodeRenderStats > ret = stats->getStats(&timeSpent);
                viewer->reportStats(0, ViewIdx(0), timeSpent, ret, stats);
            }

            viewer->updateViewer(params);
            updatedTextures = true;
        }
    }

    if (updatedTextures && (strokeMark.age >= 0) && !viewer->isViewerPaused(0)) {
        NodePtr rotoPaintNode = strokeMark.rotoPaintNode.lock();
        RotoStrokeItemPtr stroke = strokeMark.stroke.lock();
        RotoContextPtr roto = rotoPaintNode ? rotoPaintNode->getRotoContext() : RotoContextPtr();
        if (roto && stroke) {
            roto->noteStrokeDisplayed(stroke, strokeMark.multiStrokeIndex, strokeMark.age);
        }
    }
    if (updatedTextures && strokeMark.neatRender) {
        // The texture is uploaded, so the paint that drops the preview already shows the neat frame.
        NodePtr rotoPaintNode = strokeMark.rotoPaintNode.lock();
        RotoContextPtr roto = rotoPaintNode ? rotoPaintNode->getRotoContext() : RotoContextPtr();
        if (roto) {
            roto->noteNeatRenderDisplayed();
        }
    }

    ///At least redraw the viewer, we might be here when the user removed a node upstream of the viewer.
    viewer->redrawViewer();
}

void
ViewerCurrentFrameRequestScheduler::onAbortRequested(bool keepOldestRender)
{
#ifdef TRACE_CURRENT_FRAME_SCHEDULER
    qDebug() << getThreadName().c_str() << "Received abort request";
#endif
    //This will make all processing nodes that call the abort() function return true
    //This function marks all active renders of the viewer as aborted (except the oldest one)
    //and each node actually check if the render has been aborted in EffectInstance::Implementation::aborted()
    _imp->viewer->markAllOnGoingRendersAsAborted(keepOldestRender);
    _imp->backupThread.abortThreadedTask();
}

void
ViewerCurrentFrameRequestScheduler::onQuitRequested(bool allowRestarts)
{
    // quitThread aborts only while this thread is active, but the render itself may be running on the backup thread
    // after this one went idle; it must be told to stop or joining the backup waits for the whole render.
    _imp->viewer->markAllOnGoingRendersAsAborted(false);
    _imp->backupThread.quitThread(allowRestarts);
}

void
ViewerCurrentFrameRequestScheduler::onWaitForThreadToQuit()
{
    _imp->waitForRunnableTasks();
    _imp->backupThread.waitForThreadToQuit_enforce_blocking();
}

void
ViewerCurrentFrameRequestScheduler::onWaitForAbortCompleted()
{
    _imp->waitForRunnableTasks();
    _imp->backupThread.waitForAbortToComplete_enforce_blocking();
}

void
ViewerCurrentFrameRequestScheduler::notifyFrameProduced(const BufferableObjectPtrList& frames,
                                                        const RenderStatsPtr& stats,
                                                        const ViewerCurrentFrameRequestSchedulerStartArgsPtr& request)
{
    _imp->notifyFrameProduced(frames, stats,  request->age);
}

void
ViewerCurrentFrameRequestScheduler::renderCurrentFrame(bool enableRenderStats,
                                                       bool canAbort)
{
    int frame = _imp->viewer->getTimeline()->currentFrame();
    int viewsCount = _imp->viewer->getRenderViewsCount();
    ViewIdx view = viewsCount > 0 ? _imp->viewer->getViewerCurrentView() : ViewIdx(0);
    U64 viewerHash = _imp->viewer->getHash();
    ViewerInstance::ViewerRenderRetCode status[2] = {
        ViewerInstance::eViewerRenderRetCodeFail, ViewerInstance::eViewerRenderRetCodeFail
    };

    // Do not render viewer that are not made visible by the user
    if ( !_imp->viewer->isViewerUIVisible() ) {
        return;
    }

    RenderStatsPtr stats;
    if (enableRenderStats) {
        stats.reset( new RenderStats(enableRenderStats) );
    }

    NodePtr rotoPaintNode;
    RotoStrokeItemPtr curStroke;
    bool isDrawing = false;
    _imp->viewer->getApp()->getActiveRotoDrawingStroke(&rotoPaintNode, &curStroke, &isDrawing);

    bool isRotoNeatRender = false;
    if (!isDrawing) {
        isRotoNeatRender = rotoPaintNode ? rotoPaintNode->getRotoContext()->mustDoNeatRender() : false;
        if (!(rotoPaintNode && isRotoNeatRender)) {
            rotoPaintNode.reset();
            curStroke.reset();
        }
    } else {
        assert(rotoPaintNode);
        if (!_imp->strokeGate->coalescer.request()) {
            return;
        }
    }

    ViewerArgsPtr args[2];
    if (!rotoPaintNode || isRotoNeatRender) {
        bool clearTexture[2] = {false, false};

        for (int i = 0; i < 2; ++i) {
            args[i] = std::make_shared<ViewerArgs>();
            status[i] = _imp->viewer->getRenderViewerArgsAndCheckCache_public( frame, false, view, i, viewerHash, canAbort, rotoPaintNode, stats, args[i].get() );

            clearTexture[i] = status[i] == ViewerInstance::eViewerRenderRetCodeFail || status[i] == ViewerInstance::eViewerRenderRetCodeBlack;
            if (clearTexture[i] || args[i]->params->isViewerPaused) {
                //Just clear the viewer, nothing to do
                args[i]->params.reset();
            }

            if ( (status[i] == ViewerInstance::eViewerRenderRetCodeRedraw) && args[i]->params ) {
                //We must redraw (re-render) don't hold a pointer to the cached frame
                args[i]->params->tiles.clear();
            }
        }

        if (status[0] == ViewerInstance::eViewerRenderRetCodeFail && status[1] == ViewerInstance::eViewerRenderRetCodeFail) {
            _imp->viewer->disconnectViewer();
            return;
        }
        if (clearTexture[0]) {
            _imp->viewer->disconnectTexture(0, status[0] == ViewerInstance::eViewerRenderRetCodeFail);
        }
        if (clearTexture[0]) {
            _imp->viewer->disconnectTexture(1, status[1] == ViewerInstance::eViewerRenderRetCodeFail);
        }

        if ( (status[0] == ViewerInstance::eViewerRenderRetCodeRedraw) && (status[1] == ViewerInstance::eViewerRenderRetCodeRedraw) ) {
            _imp->viewer->redrawViewer();

            return;
        }

        bool hasTextureCached = false;
        for (int i = 0; i < 2; ++i) {
            if ( args[i]->params && (args[i]->params->nbCachedTile > 0) && ( args[i]->params->nbCachedTile == (int)args[i]->params->tiles.size() ) ) {
                hasTextureCached = true;
                break;
            }
        }
        if (hasTextureCached) {
            _imp->viewer->aboutToUpdateTextures();

            // A cached frame renders nothing, so no render retires the missing-channel error a
            // frame the user left may have posted. The requests just created superseded every
            // render still running, so none of those can post after this check.
            NodesList owners;
            Node::getNodesOwningChannelSelectorMessage(&owners);
            if (!owners.empty()) {
                NodesList nodesWithMessage;
                _imp->viewer->getUpstreamNodesWithPersistentMessage(&nodesWithMessage);
                for (NodesList::iterator it = nodesWithMessage.begin(); it != nodesWithMessage.end(); ++it) {
                    (*it)->refreshChannelSelectorMessageAtTime(frame, view);
                }
            }
        }

        for (int i = 0; i < 2; ++i) {
            if ( args[i]->params && (args[i]->params->nbCachedTile > 0) && ( args[i]->params->nbCachedTile == (int)args[i]->params->tiles.size() ) ) {
                /*
                   The texture was cached
                 */
                if ( stats && (i == 0) ) {
                    double timeSpent;
                    std::map<NodePtr, NodeRenderStats > statResults = stats->getStats(&timeSpent);
                    _imp->viewer->reportStats(frame, view, timeSpent, statResults, stats);
                }
                _imp->viewer->updateViewer(args[i]->params);
                args[i].reset();
            }
        }
        if ( (!args[0] && !args[1]) ||
             ( !args[0] && ( status[0] == ViewerInstance::eViewerRenderRetCodeRender) && args[1] && ( status[1] == ViewerInstance::eViewerRenderRetCodeFail) ) ||
             ( !args[1] && ( status[1] == ViewerInstance::eViewerRenderRetCodeRender) && args[0] && ( status[0] == ViewerInstance::eViewerRenderRetCodeFail) ) ) {
            _imp->viewer->redrawViewer();

            return;
        }
    }
#ifdef BOOST_NO_CXX11_VARIADIC_TEMPLATES
    CurrentFrameFunctorArgsPtr functorArgs( new CurrentFrameFunctorArgs(view,
                                                                                        frame,
                                                                                        stats,
                                                                                        _imp->viewer,
                                                                                        viewerHash,
                                                                                        _imp.get(),
                                                                                        canAbort,
                                                                                        rotoPaintNode,
                                                                                        curStroke,
                                                                                        isRotoNeatRender) );
#else
    CurrentFrameFunctorArgsPtr functorArgs = std::make_shared<CurrentFrameFunctorArgs>(view,
                                                                                                         frame,
                                                                                                         stats,
                                                                                                         _imp->viewer,
                                                                                                         viewerHash,
                                                                                                         _imp.get(),
                                                                                                         canAbort,
                                                                                                         rotoPaintNode,
                                                                                                         curStroke,
                                                                                                         isRotoNeatRender);
#endif
    functorArgs->args[0] = args[0];
    functorArgs->args[1] = args[1];
    if (isDrawing) {
        functorArgs->strokeGate = _imp->strokeGate;
    }
    if (RotoLatencyProbe* probe = RotoLatencyProbe::instance()) {
        functorArgs->latencyMark = probe->noteRenderRequested();
    }
    functorArgs->renderEndTicket = RenderEndWatch::takeTicket();

    if (appPTR->getCurrentSettings()->getNumberOfThreads() == -1) {
        RenderCurrentFrameFunctorRunnable task(functorArgs);
        task.run();
    } else {
        // Identify this render request with an age
        ViewerCurrentFrameRequestSchedulerStartArgsPtr request = std::make_shared<ViewerCurrentFrameRequestSchedulerStartArgs>();
        request->age = _imp->ageCounter;

        // If we reached the max amount of age, reset to 0... should never happen anyway
        if ( _imp->ageCounter >= std::numeric_limits<U64>::max() ) {
            _imp->ageCounter = 0;
        } else {
            ++_imp->ageCounter;
        }

        startTask(request);
        functorArgs->request = request;

        // The render thread waits for the RenderScheduler, which a pool thread must never do.
        _imp->backupThread.startTask(functorArgs);
    }
} // ViewerCurrentFrameRequestScheduler::renderCurrentFrame

ViewerCurrentFrameRequestRendererBackup::ViewerCurrentFrameRequestRendererBackup()
    : GenericSchedulerThread()
{
    setThreadName("ViewerCurrentFrameRequestRendererBackup");
}

ViewerCurrentFrameRequestRendererBackup::~ViewerCurrentFrameRequestRendererBackup()
{
    // Joined before ~GenericSchedulerThread, which would otherwise let run() call the pure tasksQueueBehaviour().
    quitThread(false);
    wait();
}

GenericSchedulerThread::ThreadStateEnum
ViewerCurrentFrameRequestRendererBackup::threadLoopOnce(const GenericThreadStartArgsPtr& inArgs)
{
    CurrentFrameFunctorArgsPtr args = std::dynamic_pointer_cast<CurrentFrameFunctorArgs>(inArgs);

    assert(args);
    RenderCurrentFrameFunctorRunnable task(args);
    task.run();

    return eThreadStateActive;
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_OutputSchedulerThread.cpp"
