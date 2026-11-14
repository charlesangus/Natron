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

#include "ViewerInstance.h"
#include "ViewerInstancePrivate.h"

#include <algorithm> // min, max
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring> // for std::memcpy
#include <iterator> // size
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

CLANG_DIAG_OFF(deprecated)
#include <QCoreApplication>
#include <QDebug>
#include <QFutureWatcher>
#include <QMutex>
#include <QRegularExpression>
#include <QThread>
#include <QThreadPool>
#include <QTimer>
#include <QWaitCondition>
#include <QtConcurrentMap> // QtCore on Qt4, QtConcurrent on Qt5
#include <QtGlobal>
CLANG_DIAG_ON(deprecated)

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Cache.h"
#include "Engine/FrameRenderContext.h"
#include "Engine/GenericSchedulerThread.h"
#include "Engine/GpuContext.h"
#include "Engine/GpuPlacement.h"
#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRuntime.h"
#endif
#include "Engine/Image.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Log.h"
#include "Engine/Lut.h"
#include "Engine/MemoryFile.h"
#include "Engine/MemoryInfo.h" // printAsRAM
#include "Engine/Node.h"
#include "Engine/Nodes/IO/OcioSpaceKnobs.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Raw/RawDisplay.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/OfxEffectInstance.h"
#include "Engine/OpenGLViewerI.h"
#include "Engine/OutputSchedulerThread.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/PoolParallelFor.h"
#include "Engine/Project.h"
#include "Engine/RenderScheduler.h"
#include "Engine/RenderStats.h"
#include "Engine/RotoContext.h"
#include "Engine/RotoLatencyProbe.h"
#include "Engine/RotoPaint.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/Settings.h"
#include "Engine/TimeLine.h"
#include "Engine/Timer.h"
#include "Engine/UpdateViewerParams.h"
#include "Engine/Utils.h"
#include "Engine/ViewIdx.h"
#include "Global/FloatingPointExceptions.h"
#include "Serialization/NodeSerialization.h"

#ifndef M_LN2
#define M_LN2       0.693147180559945309417232121458176568  /* loge(2)        */
#endif

#define NATRON_TIME_ELASPED_BEFORE_PROGRESS_REPORT 4. //!< do not display the progress report if estimated total time is less than this (in seconds)

NATRON_NAMESPACE_ENTER

using std::make_pair;
using std::shared_ptr;

NATRON_NAMESPACE_ANONYMOUS_ENTER

QMutex liveViewersMutex;
std::unordered_set<ViewerInstance*> liveViewers;

NATRON_NAMESPACE_ANONYMOUS_EXIT

static void scaleToTexture8bits(const RectI& roi,
                                const RenderViewerArgs & args,
                                ViewerInstance* viewer,
                                const UpdateViewerParams::CachedTile& tile,
                                U32* output);
static void scaleToTexture32bits(const RectI& roi,
                                 const RenderViewerArgs& args,
                                 const UpdateViewerParams::CachedTile& tile,
                                 float* output);
static void renderFunctor(const RectI& roi,
                          const RenderViewerArgs & args,
                          ViewerInstance* viewer,
                          UpdateViewerParams::CachedTile tile);

/**
 *@brief Actually converting to ARGB... but it is called BGRA by
   the texture format GL_UNSIGNED_INT_8_8_8_8_REV
 **/
static unsigned int toBGRA(unsigned char r, unsigned char g, unsigned char b, unsigned char a) WARN_UNUSED_RETURN;
unsigned int
toBGRA(unsigned char r,
       unsigned char g,
       unsigned char b,
       unsigned char a)
{
    return (a << 24) | (r << 16) | (g << 8) | b;
}

void
applyViewerDisplayTransform(const ProjectColorManagement::DisplayProcessor& processor,
                            float* rgba,
                            int width,
                            double gain,
                            double offset,
                            double gamma,
                            ViewerDisplayOutputEnum output)
{
    if (width <= 0) {
        return;
    }
    const float fGain = (float)gain;
    const float fOffset = (float)offset;
    if ((fGain != 1.f) || (fOffset != 0.f)) {
        for (int x = 0; x < width; ++x) {
            float* p = rgba + 4 * x;
            p[0] = p[0] * fGain + fOffset;
            p[1] = p[1] * fGain + fOffset;
            p[2] = p[2] * fGain + fOffset;
        }
    }
    if (processor.cpu) {
        // OCIO's CPU kernels call libm on NaN and Inf pixels, which raises FP exceptions that
        // debug builds trap.
        const boost_adaptbx::floating_point::FpTrapGuard fpGuard;
        try {
            // Any other gamma would amplify the approximation's error past a code value.
            if ((output == eViewerDisplayOutputEightBit) && (gamma == 1.)) {
                processor.applyForEightBitOutput(rgba, width);
            } else {
                OCIO_NAMESPACE::PackedImageDesc desc(rgba, width, 1, 4);
                processor.cpu->apply(desc);
            }
        } catch (const std::exception& e) {
            qDebug() << "applyViewerDisplayTransform:" << e.what();
        }
    }
    if (gamma <= 0.) {
        for (int x = 0; x < width; ++x) {
            float* p = rgba + 4 * x;
            p[0] = (p[0] >= 1.f) ? 1.f : 0.f;
            p[1] = (p[1] >= 1.f) ? 1.f : 0.f;
            p[2] = (p[2] >= 1.f) ? 1.f : 0.f;
        }
    } else if (gamma != 1.) {
        const float invGamma = (float)(1. / gamma);
        for (int x = 0; x < width; ++x) {
            float* p = rgba + 4 * x;
            p[0] = std::pow(std::max(p[0], 0.f), invGamma);
            p[1] = std::pow(std::max(p[1], 0.f), invGamma);
            p[2] = std::pow(std::max(p[2], 0.f), invGamma);
        }
    }
}

NATRON_NAMESPACE_ANONYMOUS_ENTER

const std::chrono::milliseconds kGpuPlacementDebounce(100);

// Nothing signals the end of what holds a plan, playback least of all: its engine may still report it active right when
// it reports having finished.
const std::chrono::milliseconds kGpuPlacementHoldPoll(100);

NATRON_NAMESPACE_ANONYMOUS_EXIT

/**
 * @brief Runs a viewer's dry runs. The thread starts with the first request, so a viewer pays nothing while the overlay
 * has never been on. A plan waits until requests have paused for kGpuPlacementDebounce: a knob drag edits the graph many
 * times a second, and the viewer's real renders keep the overlay current until the drag pauses.
 **/
class GpuPlacementPlanner
    : public GenericSchedulerThread {
public:
    explicit GpuPlacementPlanner(ViewerInstance* viewer)
        : GenericSchedulerThread()
        , _viewer(viewer)
    {
        setThreadName("GpuPlacementPlanner");
    }

    virtual ~GpuPlacementPlanner()
    {
    }

    void request()
    {
        bool start;
        {
            std::lock_guard<std::mutex> k(_mutex);
            _pending = true;
            _lastRequest = std::chrono::steady_clock::now();
            start = !_active;
            _active = true;
        }
        _wake.notify_all();
        if (start && !startTask(std::make_shared<GenericThreadStartArgs>())) {
            std::lock_guard<std::mutex> k(_mutex);
            _active = false;
        }
    }

    void abortRunningPlan()
    {
        AbortableRenderInfoPtr running;
        {
            std::lock_guard<std::mutex> k(_mutex);
            running = _running;
        }
        if (running) {
            running->setAborted();
        }
    }

private:
    virtual TaskQueueBehaviorEnum tasksQueueBehaviour() const OVERRIDE FINAL
    {
        return eTaskQueueBehaviorSkipToMostRecent;
    }

    virtual void onQuitRequested(bool /*allowRestarts*/) OVERRIDE FINAL
    {
        abortRunningPlan();
        {
            // A task queued but never run would otherwise leave every later request() waiting on it.
            std::lock_guard<std::mutex> k(_mutex);
            _active = false;
        }
        _wake.notify_all();
    }

    virtual ThreadStateEnum threadLoopOnce(const GenericThreadStartArgsPtr& /*inArgs*/) OVERRIDE FINAL
    {
        for (;;) {
            const AbortableRenderInfoPtr abortInfo = AbortableRenderInfo::create(true, 0);
            for (;;) {
                // Neither of these may run under _mutex: quitThread() can hold what they lock while it calls onQuitRequested().
                const ThreadStateEnum state = resolveState();
                if (state != eThreadStateActive) {
                    std::lock_guard<std::mutex> k(_mutex);
                    _active = false;

                    return state;
                }
                const bool held = _viewer->isGpuPlacementDryRunHeld();

                std::unique_lock<std::mutex> k(_mutex);
                if (!_pending) {
                    // request() starts a task only while this is false, so a request after this line is not lost.
                    _active = false;

                    return eThreadStateActive;
                }
                const std::chrono::steady_clock::time_point quietAt = _lastRequest + kGpuPlacementDebounce;
                if (std::chrono::steady_clock::now() < quietAt) {
                    _wake.wait_until(k, quietAt);
                } else if (held) {
                    _wake.wait_for(k, kGpuPlacementHoldPoll);
                } else {
                    _pending = false;
                    _running = abortInfo;
                    break;
                }
            }
            // aborted() prefers what is stored on an AbortableThread over the frame args, and this thread outlives a plan.
            setAbortInfo(true, abortInfo, _viewer->getNode()->getEffectInstance());
            try {
                _viewer->runGpuPlacementDryRun(abortInfo);
            } catch (const std::exception& e) {
                // GenericSchedulerThread::run() has no handler, so a throw from an effect's action would end the process.
                qDebug() << "GPU placement dry run failed:" << e.what();
            } catch (...) {
                qDebug() << "GPU placement dry run failed";
            }
            clearAbortInfo();
            std::lock_guard<std::mutex> k(_mutex);
            _running.reset();
        }
    }

    ViewerInstance* const _viewer;
    std::mutex _mutex;
    std::condition_variable _wake;
    bool _pending = false;

    // A task is queued or running, and will see _pending before it ends.
    bool _active = false;
    std::chrono::steady_clock::time_point _lastRequest;
    AbortableRenderInfoPtr _running;
};

EffectInstance*
ViewerInstance::BuildEffect(NodePtr n)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );

    return new ViewerInstance(n);
}

ViewerInstance::ViewerInstance(NodePtr node)
    : OutputEffectInstance(node)
    , _imp( new ViewerInstancePrivate(this) )
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );

    setSupportsRenderScaleMaybe(EffectInstance::eSupportsYes);

    _imp->gpuPlacementPlanner = std::make_shared<GpuPlacementPlanner>(this);

    QObject::connect( this, SIGNAL(disconnectTextureRequest(int,bool)), this, SLOT(executeDisconnectTextureRequestOnMainThread(int,bool)) );
    QObject::connect( _imp.get(), SIGNAL(mustRedrawViewer()), this, SLOT(redrawViewer()) );
    QObject::connect( this, SIGNAL(s_callRedrawOnMainThread()), this, SLOT(redrawViewer()) );
    if (node) {
        // dataKindChanged() also reaches the Viewer when a Group it shows changes what reaches
        // its output, which no input change on the Viewer itself reports.
        QObject::connect(node.get(), SIGNAL(dataKindChanged()), this, SLOT(refreshActiveInputDataKind()));
        if (dynamic_cast<InspectorNode*>(node.get())) {
            QObject::connect(node.get(), SIGNAL(activeInputsChanged()), this, SLOT(refreshActiveInputDataKind()));
            QObject::connect(node.get(), SIGNAL(activeInputsChanged()), this, SLOT(onInspectorActiveInputsChanged()));
        }
    }
    QMutexLocker l(&liveViewersMutex);
    liveViewers.insert(this);
}

ViewerInstance::~ViewerInstance()
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );

    {
        QMutexLocker l(&liveViewersMutex);
        liveViewers.erase(this);
    }
    EffectInstance::retireSceneContextId(_imp->sceneContextId);

    // A plan reads this viewer until its thread has stopped.
    _imp->gpuPlacementPlanner->quitThread(false);
    _imp->gpuPlacementPlanner->waitForThreadToQuit_enforce_blocking();

    // If _imp->updateViewerRunning is true, that means that the next updateViewer call was
    // not yet processed. Since we're in the main thread and it is processed in the main thread,
    // there is no way to wait for it (locking the mutex would cause a deadlock).
    // We don't care, after all.
    //{
    //    QMutexLocker locker(&_imp->updateViewerMutex);
    //    assert(!_imp->updateViewerRunning);
    //}
#ifdef NATRON_HAVE_GPU
    _imp->cacheFillGate->close();
#endif
    if (_imp->uiContext) {
        _imp->uiContext->removeGUI();
    }
}

bool
ViewerInstance::isAnyViewerConnected()
{
    // The nodes are checked, and released, outside the lock: dropping the last reference to one
    // destroys its viewer, which takes the lock.
    std::vector<NodePtr> nodes;
    {
        QMutexLocker l(&liveViewersMutex);
        nodes.reserve(liveViewers.size());
        for (ViewerInstance* viewer : liveViewers) {
            NodePtr node = viewer->getNode();
            if (node) {
                nodes.push_back(std::move(node));
            }
        }
    }
    for (const NodePtr& node : nodes) {
        if (node->hasInputOrQueuedInputConnected()) {
            return true;
        }
    }

    return false;
}

RenderEngine*
ViewerInstance::createRenderEngine()
{
    ViewerInstancePtr thisShared = std::dynamic_pointer_cast<ViewerInstance>( shared_from_this() );

    return new ViewerRenderEngine(thisShared);
}

void
ViewerInstance::getPluginGrouping(std::list<std::string>* grouping) const
{
    grouping->push_back(PLUGIN_GROUP_IMAGE);
}

OpenGLViewerI*
ViewerInstance::getUiContext() const
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );

    return _imp->uiContext;
}

std::string
ViewerInstance::getRawDisplayLabel(int textureIndex) const
{
    std::lock_guard<std::mutex> lock(_rawDisplayLabelMutex);

    return ((textureIndex == 0) || (textureIndex == 1)) ? _rawDisplayLabel[textureIndex] : std::string();
}

void
ViewerInstance::onKnobsAboutToBeLoaded(const Serialization::NodeSerializationPtr& serialization)
{
    std::lock_guard<std::mutex> lock(_sceneViewMutex);

    if (serialization && !serialization->sceneView.isEmpty()) {
        _loadedSceneView.reset(new Serialization::SceneViewSerialization(serialization->sceneView));
    } else {
        _loadedSceneView.reset();
    }
}

Serialization::SceneViewSerialization
ViewerInstance::getLoadedSceneView() const
{
    Serialization::SceneViewSerialization view;

    // A tab that went, or another one, saves the view as it last was: that is the view to restore.
    sceneViewToSerialization(&view);

    return view;
}

void
ViewerInstance::setSceneViewSaver(const SceneViewSaver& saver)
{
    std::lock_guard<std::mutex> lock(_sceneViewMutex);

    _sceneViewSaver = saver;
    _loadedSceneView.reset();
}

void
ViewerInstance::sceneViewToSerialization(Serialization::SceneViewSerialization* serialization) const
{
    // Held while the saver runs, so that the view it reads cannot be destroyed under it.
    std::lock_guard<std::mutex> lock(_sceneViewMutex);

    if (_sceneViewSaver) {
        _sceneViewSaver(serialization);
    } else if (_loadedSceneView) {
        *serialization = *_loadedSceneView;
    } else {
        *serialization = Serialization::SceneViewSerialization();
    }
}

void
ViewerInstance::forceFullComputationOnNextFrame()
{
    // this is called by the GUI when the user presses the "Refresh" button.
    // It set the flag forceRender to true, meaning no cache will be used.

    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );

    QMutexLocker forceRenderLocker(&_imp->forceRenderMutex);
    _imp->forceRender[0] = _imp->forceRender[1] = true;
}

void
ViewerInstance::clearLastRenderedImage()
{
    EffectInstance::clearLastRenderedImage();

    if (_imp->uiContext) {
        _imp->uiContext->clearLastRenderedImage();
    }
    {
        QMutexLocker k(&_imp->lastRenderParamsMutex);
        _imp->lastRenderParams[0].reset();
        _imp->lastRenderParams[1].reset();
    }
}

void
ViewerInstance::setUiContext(OpenGLViewerI* viewer)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );

    std::lock_guard<std::mutex> k(_imp->gpuPlacementUiContextMutex);
    _imp->uiContext = viewer;
}

void
ViewerInstance::invalidateUiContext()
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    {
        std::lock_guard<std::mutex> k(_imp->gpuPlacementUiContextMutex);
        _imp->uiContext = NULL;
    }

    // A running plan finds the UI context gone at its next read of it, and has no more use for the rest of its work.
    _imp->gpuPlacementPlanner->abortRunningPlan();
#ifdef NATRON_HAVE_GPU
    _imp->stagedFrames[0].reset();
    _imp->stagedFrames[1].reset();
#endif
}

int
ViewerInstance::getNInputs() const
{
    return 10;
}

namespace {
template <class K>
std::shared_ptr<K>
createViewerKnob(KnobHolder* holder,
                 const KnobPagePtr& page,
                 const QString& label,
                 const char* name,
                 const QString& hint,
                 int dimension = 1)
{
    std::shared_ptr<K> knob = AppManager::createKnob<K>(holder, label, dimension);

    knob->setName(name);
    knob->setHintToolTip(hint.toStdString());
    knob->setAnimationEnabled(false);
    knob->setEvaluateOnChange(false);
    // The viewer's own hash is the tree version of every FrameKey it caches, so a hashed viewer
    // knob would drop the cached viewer textures on each edit. Upstream hashes never include it.
    knob->setHashExempt(true);
    page->addKnob(knob);

    return knob;
}

// The order of the toolbar's channel menu, which the knob shares so a menu index is a knob index.
const DisplayChannelsEnum kDisplayChannelsByIndex[] = {
    eDisplayChannelsY,
    eDisplayChannelsRGB,
    eDisplayChannelsR,
    eDisplayChannelsG,
    eDisplayChannelsB,
    eDisplayChannelsA,
    eDisplayChannelsMatte,
};
const int kDisplayChannelsCount = sizeof(kDisplayChannelsByIndex) / sizeof(kDisplayChannelsByIndex[0]);

int
displayChannelsIndex(DisplayChannelsEnum channels)
{
    for (int i = 0; i < kDisplayChannelsCount; ++i) {
        if (kDisplayChannelsByIndex[i] == channels) {
            return i;
        }
    }

    return 1;
}

DisplayChannelsEnum
displayChannelsAt(int index)
{
    return ((index >= 0) && (index < kDisplayChannelsCount)) ? kDisplayChannelsByIndex[index] : eDisplayChannelsRGB;
}

// The selection a layer knob stores for a description handed to the viewer. The toolbar hands the
// colour storage plane, which only its views name.
std::string
layerSelectionID(const ImageLayerDesc& layer)
{
    if (layer.isColorLayer()) {
        return ImageLayerDesc::colorViewForNComps(layer.getNumComponents()).getLayerID();
    }

    return layer.getLayerID();
}

// Knobs whose edit re-runs the viewer render.
bool
knobRequestsRender(const std::string& name)
{
    static const char* const names[] = {
        kViewerParamDisplayChannels,
        kViewerParamDisplayChannelsB,
        kViewerParamLayer,
        kViewerParamAlphaChannel,
        kViewerParamAInput,
        kViewerParamBInput,
        kViewerParamEnableUserRoI,
        kViewerParamUserRoIBottomLeft,
        kViewerParamUserRoISize,
        kViewerParamClipToFormat,
        kViewerParamFullFrame,
        kViewerParamEnableDownscale,
        kViewerParamDownscaleLevel,
    };

    for (std::size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (name == names[i]) {
            return true;
        }
    }

    return false;
}

// Knobs applied when the frame is drawn: in 8-bit the display transform is baked into the
// texture, so they re-render there and only redraw otherwise.
bool
knobChangesDisplay(const std::string& name)
{
    return (name == kViewerParamEnableGain) || (name == kViewerParamGain) || (name == kViewerParamEnableGamma) || (name == kViewerParamGamma) || (name == kViewerParamDisplay) || (name == kViewerParamView) || (name == kViewerParamLook);
}

// The label is rich text: once edited in the panel its lines sit inside a font tag, possibly
// ahead of a custom-data tag, so the lock lines have to go inside those too.
QString
withLockLines(QString label,
              const QString& lockLines)
{
    const QString lockLine = QString::fromUtf8("Locked input \\d+ to [^\\n<]*");
    const QString fontStart = QString::fromUtf8("<font[^>]*>");

    label.remove(QRegularExpression(QString::fromUtf8("\\n") + lockLine));
    label.replace(QRegularExpression(QString::fromUtf8("(^|") + fontStart + QString::fromUtf8(")") + lockLine + QString::fromUtf8("\\n?")),
                  QString::fromUtf8("\\1"));
    if (lockLines.isEmpty()) {
        return label;
    }

    int insertAt = label.indexOf(QString::fromUtf8(NATRON_CUSTOM_HTML_TAG_START));
    if (insertAt == -1) {
        insertAt = label.lastIndexOf(QString::fromUtf8(kFontEndTag));
    }
    if (insertAt == -1) {
        insertAt = label.size();
    }
    const QString before = label.left(insertAt);
    const bool startsContent = before.isEmpty() || before.contains(QRegularExpression(fontStart + QString::fromUtf8("$")));
    label.insert(insertAt, (startsContent ? QString() : QString::fromUtf8("\n")) + lockLines);

    return label;
}

const char kViewerLookNoneID[] = "none";

// The name a display, view or look menu holds; empty for its first entry, Project default or None.
std::string
displayTransformName(const KnobChoicePtr& knob)
{
    if (!knob) {
        return std::string();
    }
    const std::string id = knob->getActiveEntry().id;

    return ((id == OcioSpaceKnobs::projectDefaultID()) || (id == kViewerLookNoneID)) ? std::string() : id;
}

class SetterKnobWrites {
public:
    explicit SetterKnobWrites(int* depth)
        : _depth(depth)
    {
        ++*_depth;
    }

    ~SetterKnobWrites()
    {
        --*_depth;
    }

private:
    int* _depth;
};

std::vector<EffectInstancePtr>
effectsRequestingOverlayImagesUpstream(const NodePtr& viewerNode,
                                       int inputNb)
{
    std::vector<EffectInstancePtr> effects;
    std::set<Node*> visited;
    // The real input keeps a group in the walk, the redirected one steps into it or out of a
    // GroupInput.
    std::list<NodePtr> queue = { viewerNode->getRealInput(inputNb), viewerNode->getInput(inputNb) };

    while (!queue.empty()) {
        const NodePtr node = queue.front();
        queue.pop_front();
        if (!node || !visited.insert(node.get()).second) {
            continue;
        }
        const EffectInstancePtr effect = node->getEffectInstance();
        if (effect && effect->isOverlayImageRequested() && !node->isNodeDisabled() && !effect->producesDeepData() && !effect->producesSceneData()) {
            effects.push_back(effect);
        }
        const int nInputs = node->getNInputs();
        for (int i = 0; i < nInputs; ++i) {
            queue.push_back(node->getRealInput(i));
            queue.push_back(node->getInput(i));
        }
    }

    return effects;
}
} // namespace

void
ViewerInstance::initializeKnobs()
{
    KnobPagePtr page = AppManager::createKnob<KnobPage>(this, tr("Viewer UI Controls"));
    page->setName("viewerUIControls");

    ViewerInstancePrivate::Knobs& k = _imp->knobs;

    KnobBoolPtr enableGain = createViewerKnob<KnobBool>(this, page, tr("Enable Gain"), kViewerParamEnableGain,
                                                        tr("Applies Gain. Off shows the image at a gain of 0 f-stops and keeps the Gain value."));
    k.enableGain = enableGain;

    KnobDoublePtr gain = createViewerKnob<KnobDouble>(this, page, tr("Gain"), kViewerParamGain,
                                                      tr("Gain in f-stops: the image is multiplied by pow(2, Gain) before display."));
    gain->setDisplayMinimum(-6.);
    gain->setDisplayMaximum(6.);
    k.gain = gain;

    KnobBoolPtr enableGamma = createViewerKnob<KnobBool>(this, page, tr("Enable Gamma"), kViewerParamEnableGamma,
                                                         tr("Applies Gamma. Off shows the image at a gamma of 1 and keeps the Gamma value."));
    k.enableGamma = enableGamma;

    KnobDoublePtr gamma = createViewerKnob<KnobDouble>(this, page, tr("Gamma"), kViewerParamGamma,
                                                       tr("Gamma correction, applied after gain and before the display transform."));
    gamma->setDefaultValue(1.);
    gamma->setMinimum(0.);
    gamma->setDisplayMinimum(0.);
    gamma->setDisplayMaximum(5.);
    k.gamma = gamma;

    KnobBoolPtr checkerboard = createViewerKnob<KnobBool>(this, page, tr("Enable Checker Board"), kViewerParamCheckerboard,
                                                          tr("Draws a checkerboard under input A instead of black."));
    k.checkerboard = checkerboard;

    KnobChoicePtr display = createViewerKnob<KnobChoice>(this, page, tr("Display"), kViewerParamDisplay,
                                                         tr("The display of the project's OpenColorIO config the image is shown on. "
                                                            "A saved display the config lacks is kept."));
    k.display = display;

    KnobChoicePtr view = createViewerKnob<KnobChoice>(this, page, tr("View"), kViewerParamView,
                                                      tr("The view transform of the display. A saved view the display lacks is kept."));
    k.view = view;

    KnobChoicePtr look = createViewerKnob<KnobChoice>(this, page, tr("Look"), kViewerParamLook,
                                                      tr("The OpenColorIO look applied before the view. A saved look the config lacks is kept "
                                                         "and applies none."));
    k.look = look;
    for (const KnobChoicePtr& menu : { display, view, look }) {
        menu->setMatchesExactID(true);
    }
    refreshDisplayTransformChoices();

    std::vector<ChoiceOption> channelEntries;
    channelEntries.push_back(ChoiceOption("Luminance", tr("Luminance").toStdString(), ""));
    channelEntries.push_back(ChoiceOption("RGB", tr("RGB").toStdString(), ""));
    channelEntries.push_back(ChoiceOption("R", tr("R").toStdString(), ""));
    channelEntries.push_back(ChoiceOption("G", tr("G").toStdString(), ""));
    channelEntries.push_back(ChoiceOption("B", tr("B").toStdString(), ""));
    channelEntries.push_back(ChoiceOption("A", tr("A").toStdString(), ""));
    channelEntries.push_back(ChoiceOption("Matte", tr("Matte").toStdString(), ""));
    assert((int)channelEntries.size() == kDisplayChannelsCount);
    for (int i = 0; i < 2; ++i) {
        KnobChoicePtr channels = createViewerKnob<KnobChoice>(this, page, i == 0 ? tr("Display Channels") : tr("Display Channels B"),
                                                              i == 0 ? kViewerParamDisplayChannels : kViewerParamDisplayChannelsB,
                                                              i == 0 ? tr("The channels of the layer shown for input A.")
                                                                     : tr("The channels of the layer shown for input B."));
        channels->populateChoices(channelEntries);
        channels->setDefaultValue(displayChannelsIndex(eDisplayChannelsRGB));
        k.displayChannels[i] = channels;
    }

    KnobLayerSelectPtr layer = AppManager::createKnob<KnobLayerSelect>(this, tr("Layer"));
    layer->setWithChannelButtons(false);
    layer->setAllowNone(true);
    layer->setName(kViewerParamLayer);
    layer->setHintToolTip(tr("The layer of input A the viewer shows. A saved layer the input lacks is kept.").toStdString());
    k.layer = layer;

    KnobChannelSelectPtr alphaChannel = AppManager::createKnob<KnobChannelSelect>(this, tr("Alpha Channel"));
    alphaChannel->setName(kViewerParamAlphaChannel);
    alphaChannel->setHintToolTip(tr("The channel shown as alpha when Display Channels is A or Matte. "
                                    "A saved channel the input lacks is kept.")
                                     .toStdString());
    k.alphaChannel = alphaChannel;

    const KnobIPtr layerKnobs[2] = { layer, alphaChannel };
    for (int i = 0; i < 2; ++i) {
        layerKnobs[i]->setAnimationEnabled(false);
        layerKnobs[i]->setEvaluateOnChange(false);
        layerKnobs[i]->setHashExempt(true);
        page->addKnob(layerKnobs[i]);
    }

    for (int i = 0; i < 2; ++i) {
        KnobIntPtr input = createViewerKnob<KnobInt>(this, page, i == 0 ? tr("A Input") : tr("B Input"),
                                                     i == 0 ? kViewerParamAInput : kViewerParamBInput,
                                                     i == 0 ? tr("The input shown as A, -1 for none.") : tr("The input shown as B, -1 for none."));
        input->setDefaultValue(-1);
        input->setMinimum(-1);
        input->setMaximum(getNInputs() - 1);
        k.inputs[i] = input;
    }

    const int nLockable = std::min<int>(getNInputs(), static_cast<int>(std::size(k.lockedInputs)));
    for (int i = 0; i < nLockable; ++i) {
        const std::string lockName = kViewerParamLockedInputPrefix + std::to_string(i + 1);
        KnobStringPtr lock = createViewerKnob<KnobString>(this, page, tr("Locked Input %1").arg(i + 1), lockName.c_str(),
                                                          tr("The script name of the node this input is locked to, empty for none."));
        lock->setSecretByDefault(true);
        k.lockedInputs[i] = lock;
    }

    KnobChoicePtr operation = createViewerKnob<KnobChoice>(this, page, tr("Operation"), kViewerParamOperation,
                                                           tr("Operation applied between inputs A and B. a and b are the alphas of "
                                                              "the inputs, d the wipe dissolve factor (Wipe Amount)."));
    {
        std::vector<ChoiceOption> entries;
        entries.push_back(ChoiceOption("-", "-", tr("No wipe or composite: A").toStdString()));
        entries.push_back(ChoiceOption("Wipe Under", tr("Wipe Under").toStdString(), "A(1 - d) + Bd"));
        entries.push_back(ChoiceOption("Wipe Over", tr("Wipe Over").toStdString(), "A + B(1 - a)d"));
        entries.push_back(ChoiceOption("Wipe Minus", tr("Wipe Minus").toStdString(), "A - B"));
        entries.push_back(ChoiceOption("Wipe Onion skin", tr("Wipe Onion skin").toStdString(), "A + B"));
        entries.push_back(ChoiceOption("Stack Under", tr("Stack Under").toStdString(), "B"));
        entries.push_back(ChoiceOption("Stack Over", tr("Stack Over").toStdString(), "A + B(1 - a)"));
        entries.push_back(ChoiceOption("Stack Minus", tr("Stack Minus").toStdString(), "A - B"));
        entries.push_back(ChoiceOption("Stack Onion skin", tr("Stack Onion skin").toStdString(), "A + B"));
        operation->populateChoices(entries);
    }
    operation->setDefaultValue((int)eViewerCompositingOperatorNone);
    k.operation = operation;

    KnobDoublePtr wipeCenter = createViewerKnob<KnobDouble>(this, page, tr("Wipe Center"), kViewerParamWipeCenter,
                                                            tr("The centre of the wipe handle, in canonical coordinates."), 2);
    if (AppInstancePtr app = getApp()) {
        if (ProjectPtr project = app->getProject()) {
            Format format;
            project->getProjectDefaultFormat(&format);
            const RectD canonical = format.toCanonicalFormat();
            wipeCenter->setDefaultValue((canonical.x1 + canonical.x2) / 2., 0);
            wipeCenter->setDefaultValue((canonical.y1 + canonical.y2) / 2., 1);
        }
    }
    k.wipeCenter = wipeCenter;

    KnobDoublePtr wipeAmount = createViewerKnob<KnobDouble>(this, page, tr("Wipe Amount"), kViewerParamWipeAmount,
                                                            tr("The wipe dissolve factor d."));
    wipeAmount->setDefaultValue(1.);
    wipeAmount->setMinimum(0.);
    wipeAmount->setMaximum(1.);
    k.wipeAmount = wipeAmount;

    KnobDoublePtr wipeAngle = createViewerKnob<KnobDouble>(this, page, tr("Wipe Angle"), kViewerParamWipeAngle,
                                                           tr("The angle of the wipe line, in radians."));
    k.wipeAngle = wipeAngle;

    KnobIntPtr activeView = createViewerKnob<KnobInt>(this, page, tr("Active View"), kViewerParamActiveView,
                                                      tr("The index of the project view the viewer shows."));
    activeView->setMinimum(0);
    k.activeView = activeView;

    double projectFps = 24.;
    double projectFirst = 1., projectLast = 1.;
    if (AppInstancePtr app = getApp()) {
        if (ProjectPtr project = app->getProject()) {
            projectFps = project->getProjectFrameRate();
            project->getFrameRange(&projectFirst, &projectLast);
            QObject::connect(project.get(), SIGNAL(loaded()), this, SLOT(onProjectLoaded()), Qt::UniqueConnection);
        }
    }

    KnobBoolPtr enableFps = createViewerKnob<KnobBool>(this, page, tr("Enable Fps"), kViewerParamEnableFps,
                                                       tr("Plays back at Desired Fps. Off follows the project frame rate."));
    k.enableFps = enableFps;

    KnobDoublePtr fps = createViewerKnob<KnobDouble>(this, page, tr("Desired Fps"), kViewerParamFps,
                                                     tr("The playback frame rate, in frames per second, while Enable Fps is on."));
    fps->setDefaultValue(std::max(projectFps, kViewerMinimumFps));
    fps->setMinimum(kViewerMinimumFps);
    fps->setDisplayMinimum(kViewerMinimumFps);
    fps->setDisplayMaximum(120.);
    k.fps = fps;

    KnobIntPtr inPoint = createViewerKnob<KnobInt>(this, page, tr("In Point"), kViewerParamInPoint, tr("The first frame of playback."));
    inPoint->setDefaultValue((int)projectFirst);
    k.inPoint = inPoint;

    KnobIntPtr outPoint = createViewerKnob<KnobInt>(this, page, tr("Out Point"), kViewerParamOutPoint, tr("The last frame of playback."));
    outPoint->setDefaultValue((int)projectLast);
    k.outPoint = outPoint;

    KnobBoolPtr enableUserRoI = createViewerKnob<KnobBool>(this, page, tr("Enable Region Of Interest"), kViewerParamEnableUserRoI,
                                                           tr("Renders only the region of interest set by User RoI Bottom Left and User RoI Size."));
    k.enableUserRoI = enableUserRoI;

    KnobDoublePtr userRoIBottomLeft = createViewerKnob<KnobDouble>(this, page, tr("User RoI Bottom Left"), kViewerParamUserRoIBottomLeft,
                                                                   tr("The bottom-left corner of the region of interest, in canonical coordinates."), 2);
    k.userRoIBottomLeft = userRoIBottomLeft;

    KnobDoublePtr userRoISize = createViewerKnob<KnobDouble>(this, page, tr("User RoI Size"), kViewerParamUserRoISize,
                                                             tr("The width and height of the region of interest, in canonical coordinates."), 2);
    userRoISize->setMinimum(0., 0);
    userRoISize->setMinimum(0., 1);
    k.userRoISize = userRoISize;

    KnobBoolPtr clipToFormat = createViewerKnob<KnobBool>(this, page, tr("Clip To Format"), kViewerParamClipToFormat,
                                                          tr("Shows only the part of the image inside the format of input A."));
    clipToFormat->setDefaultValue(true);
    k.clipToFormat = clipToFormat;

    KnobBoolPtr fullFrame = createViewerKnob<KnobBool>(this, page, tr("Full Frame"), kViewerParamFullFrame,
                                                       tr("Renders the whole image at full resolution, not just the visible part. "
                                                          "Downscale Level has no effect while it is on."));
    k.fullFrame = fullFrame;

    KnobBoolPtr enableDownscale = createViewerKnob<KnobBool>(this, page, tr("Enable Downscale"), kViewerParamEnableDownscale,
                                                             tr("Renders downscaled by Downscale Level to speed up rendering."));
    k.enableDownscale = enableDownscale;

    KnobChoicePtr downscaleLevel = createViewerKnob<KnobChoice>(this, page, tr("Downscale Level"), kViewerParamDownscaleLevel,
                                                                tr("The factor the image is downscaled by in both dimensions while Enable Downscale is on."));
    {
        std::vector<ChoiceOption> entries;
        for (int factor = 2; factor <= 32; factor *= 2) {
            const std::string id = QString::number(factor).toStdString();
            entries.push_back(ChoiceOption(id, id, ""));
        }
        downscaleLevel->populateChoices(entries);
    }
    k.downscaleLevel = downscaleLevel;

    KnobIntPtr maxSamples = createViewerKnob<KnobInt>(this, page, tr("Max Samples"), kViewerParamMaxSamples,
                                                      tr("The samples per pixel a progressive renderer refines the 3D view to; "
                                                         "0 uses the renderer's default. A change restarts the render."));
    maxSamples->setMinimum(0);
    maxSamples->setDisplayMinimum(0);
    maxSamples->setDisplayMaximum(4096);
    k.maxSamples = maxSamples;

    KnobBoolPtr refinementPaused = createViewerKnob<KnobBool>(this, page, tr("Refinement Paused"), kViewerParamRefinementPaused,
                                                              tr("Holds a progressive renderer's refinement of the 3D view at its "
                                                                 "current samples; off continues from them. Edits wait until it is off."));
    k.refinementPaused = refinementPaused;

    if (NodePtr node = getNode()) {
        node->declareLayerKnob(layer, 0, LayerKnobSpec::eRoleInputBound);
        node->declareLayerKnob(alphaChannel, 0, LayerKnobSpec::eRoleInputBound);
    }
} // ViewerInstance::initializeKnobs

NATRON_NAMESPACE_ANONYMOUS_ENTER

const double kLockGrey = 0x11 / 255.;

bool
isLockGrey(const Node& node)
{
    double r, g, b;
    node.getUserColor(&r, &g, &b);

    return node.hasUserColor() && (std::abs(r - kLockGrey) < 1e-3) && (std::abs(g - kLockGrey) < 1e-3) && (std::abs(b - kLockGrey) < 1e-3);
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

std::string
ViewerInstance::getLockedInput(int inputNb) const
{
    if ((inputNb < 0) || (inputNb >= getNInputs()) || (inputNb >= static_cast<int>(std::size(_imp->knobs.lockedInputs)))) {
        return std::string();
    }
    KnobStringPtr knob = _imp->knobs.lockedInputs[inputNb].lock();

    return knob ? knob->getValue() : std::string();
}

void
ViewerInstance::setLockedInput(int inputNb,
                               const std::string& nodeName)
{
    if ((inputNb < 0) || (inputNb >= getNInputs()) || (inputNb >= static_cast<int>(std::size(_imp->knobs.lockedInputs)))) {
        return;
    }
    KnobStringPtr knob = _imp->knobs.lockedInputs[inputNb].lock();
    if (!knob || (knob->getValue() == nodeName)) {
        return;
    }
    {
        SetterKnobWrites guard(&_imp->knobWritesFromSetter);
        knob->setValue(nodeName);
    }
    refreshLockDecoration();
}

void
ViewerInstance::refreshLockDecoration()
{
    NodePtr node = getNode();
    if (!node) {
        return;
    }
    QString lockLines;
    bool anyLock = false;

    const int nLockable = std::min<int>(getNInputs(), static_cast<int>(std::size(_imp->knobs.lockedInputs)));
    for (int i = 0; i < nLockable; ++i) {
        const std::string name = getLockedInput(i);
        if (name.empty()) {
            continue;
        }
        if (anyLock) {
            lockLines += QLatin1Char('\n');
        }
        lockLines += QString::fromUtf8("Locked input %1 to %2").arg(i + 1).arg(QString::fromUtf8(name.c_str()));
        anyLock = true;
    }

    KnobStringPtr label = std::dynamic_pointer_cast<KnobString>(node->getKnobByName(kUserLabelKnobName));
    if (label) {
        const QString updated = withLockLines(QString::fromUtf8(label->getValue().c_str()), lockLines);
        SetterKnobWrites guard(&_imp->knobWritesFromSetter);
        label->setValue(updated.toStdString());
    }

    if (anyLock) {
        if (!_imp->lockColourApplied) {
            _imp->colourBeforeLock = node->hasUserColor();
            if (_imp->colourBeforeLock) {
                node->getUserColor(&_imp->savedColour[0], &_imp->savedColour[1], &_imp->savedColour[2]);
            }
            node->setColor(kLockGrey, kLockGrey, kLockGrey);
            _imp->lockColourApplied = true;
        }
    } else if (_imp->lockColourApplied) {
        _imp->lockColourApplied = false;
        // A colour other than the grey set on lock was chosen by the user meanwhile.
        if (isLockGrey(*node)) {
            if (_imp->colourBeforeLock) {
                node->setColor(_imp->savedColour[0], _imp->savedColour[1], _imp->savedColour[2]);
            } else {
                node->resetColor();
            }
        }
    }
}

bool
ViewerInstance::getColourToSave(bool* hasColour,
                                double* rgb) const
{
    NodePtr node = getNode();
    if (!_imp->lockColourApplied || !node || !isLockGrey(*node)) {
        return false;
    }
    *hasColour = _imp->colourBeforeLock;
    for (int i = 0; i < 3; ++i) {
        rgb[i] = _imp->savedColour[i];
    }

    return true;
}
bool
ViewerInstance::knobChanged(KnobI* k,
                            ValueChangedReasonEnum reason,
                            ViewSpec /*view*/,
                            double /*time*/,
                            bool /*originatedFromMainThread*/)
{
    if (!k || (_imp->knobWritesFromSetter > 0) || (reason == eValueChangedReasonTimeChanged)) {
        return false;
    }
    const std::string name = k->getName();

    if (name == kViewerParamDisplay) {
        refreshDisplayTransformChoices();
        // A view the new display lacks would read as missing; Project default follows the display.
        KnobChoicePtr view = _imp->knobs.view.lock();
        if (view && view->isActiveEntryUnmatchedByEntries()) {
            selectDisplayTransformEntry(view, std::string());
        }
    }
    if (knobChangesDisplay(name)) {
        redrawOrRenderForColorChange();

        return true;
    }
    if (name == kViewerParamEnableUserRoI) {
        beginKnobEdits();
        KnobBoolPtr enable = _imp->knobs.enableUserRoI.lock();
        if (enable && enable->getValue()) {
            fillUnsetUserRoI();
        }
        requestRenderFromKnob();
        endKnobEdits();

        return true;
    }
    if ((name == kViewerParamLayer) || (name == kViewerParamAlphaChannel)) {
        refreshResolvedLayers();
    } else if ((name == kViewerParamAInput) || (name == kViewerParamBInput)) {
        const int side = (name == kViewerParamAInput) ? 0 : 1;
        KnobIntPtr knob = _imp->knobs.inputs[side].lock();
        if (knob) {
            applyActiveInput(side, knob->getValue());
        }
    }
    if (knobRequestsRender(name)) {
        requestRenderFromKnob();

        return true;
    }

    return false;
}

void
ViewerInstance::onKnobsLoaded()
{
    refreshResolvedLayers();
    refreshDisplayTransformChoices();
}

unsigned int
ViewerInstance::getKnobRenderRequestCount() const
{
    return _imp->knobRenderRequests;
}

void
ViewerInstance::beginKnobEdits()
{
    ++_imp->knobEditDepth;
}

void
ViewerInstance::endKnobEdits()
{
    assert(_imp->knobEditDepth > 0);
    if (--_imp->knobEditDepth > 0) {
        return;
    }
    if (_imp->knobEditRenderPending) {
        _imp->knobEditRenderPending = false;
        requestRenderFromKnob();
    }
}

void
ViewerInstance::requestRenderFromKnob()
{
    if (_imp->knobEditDepth > 0) {
        _imp->knobEditRenderPending = true;

        return;
    }
    ++_imp->knobRenderRequests;
    if (_imp->uiContext && !getApp()->getProject()->isLoadingProject()) {
        renderCurrentFrame(true);
    }
}

void
ViewerInstance::redrawOrRenderForColorChange()
{
    if (!_imp->uiContext) {
        return;
    }
    // A scene has no 8-bit texture to bake into: Storm's view grades its pixels as it draws them,
    // and a render would only re-evaluate the scene and resync it. A progressive delegate's frames
    // come sRGB-encoded without the display transform, which a render would not change either.
    const bool bakesDisplay = (_imp->uiContext->getBitDepth() == eImageBitDepthByte) && (getActiveInputDataKind() != eDataKindScene);
    if (bakesDisplay && !getApp()->getProject()->isLoadingProject()) {
        renderCurrentFrame(true);
    } else {
        _imp->uiContext->redraw();
    }
}

bool
ViewerInstance::refreshResolvedLayers()
{
    NodePtr node = getNode();
    KnobLayerSelectPtr layerKnob = _imp->knobs.layer.lock();
    KnobChannelSelectPtr alphaKnob = _imp->knobs.alphaChannel.lock();

    if (!node || !layerKnob || !alphaKnob) {
        return false;
    }

    const std::string layerID = layerKnob->getLayer();
    std::optional<ImageLayerDesc> layer;
    if (layerID.empty()) {
        layer = ImageLayerDesc::getNoneComponents();
    } else {
        std::list<ImageLayerDesc> present;
        node->listLayersForKnob(layerKnob, &present);
        ResolvedLayer resolved;
        if (layerKnob->resolve(present, &resolved)) {
            layer = resolved.desc;
        }
    }

    const std::string alphaID = alphaKnob->get();
    std::optional<ImageLayerDesc> alphaLayer;
    std::string alphaChannelName;
    if (alphaID.empty()) {
        alphaLayer = ImageLayerDesc::getNoneComponents();
    } else {
        std::list<ImageLayerDesc> present;
        node->listLayersForKnob(alphaKnob, &present);
        ImageLayerDesc resolved;
        int channelIndex = -1;
        if (alphaKnob->resolve(present, &resolved, &channelIndex) && (channelIndex >= 0) && (channelIndex < (int)resolved.getChannels().size())) {
            alphaLayer = resolved;
            alphaChannelName = resolved.getChannels()[channelIndex];
        }
    }

    bool changed = false;
    QMutexLocker l(&_imp->viewerParamsMutex);
    if (layer && ((_imp->viewerParamsLayerID != layerID) || (_imp->viewerParamsLayer != *layer))) {
        _imp->viewerParamsLayerID = layerID;
        _imp->viewerParamsLayer = *layer;
        changed = true;
    }
    if (alphaLayer && ((_imp->viewerParamsAlphaID != alphaID) || (_imp->viewerParamsAlphaLayer != *alphaLayer) || (_imp->viewerParamsAlphaChannelName != alphaChannelName))) {
        _imp->viewerParamsAlphaID = alphaID;
        _imp->viewerParamsAlphaLayer = *alphaLayer;
        _imp->viewerParamsAlphaChannelName = alphaChannelName;
        changed = true;
    }

    return changed;
} // ViewerInstance::refreshResolvedLayers

void
ViewerInstance::applyActiveInput(int side,
                                 int inputNb)
{
    NodePtr n = getNode();
    InspectorNode* isInspector = dynamic_cast<InspectorNode*>(n.get());

    assert(isInspector);
    if (isInspector) {
        if (side == 0) {
            isInspector->setInputA(inputNb);
        } else {
            isInspector->setInputB(inputNb);
        }
        if (side == 0) {
            KnobLayerSelectPtr layer = _imp->knobs.layer.lock();
            KnobChannelSelectPtr alpha = _imp->knobs.alphaChannel.lock();
            const int listedInput = std::max(inputNb, 0);
            if (layer) {
                n->setLayerKnobInput(layer, listedInput);
            }
            if (alpha) {
                n->setLayerKnobInput(alpha, listedInput);
            }
        }
    }

    refreshActiveInputDataKind();
    Q_EMIT availableComponentsChanged();
}

void
ViewerInstance::onInspectorActiveInputsChanged()
{
    requestGpuPlacementDryRun();

    NodePtr node = getNode();
    InspectorNode* isInspector = dynamic_cast<InspectorNode*>(node.get());
    if (!isInspector) {
        return;
    }
    int active[2] = { -1, -1 };
    isInspector->getActiveInputs(active[0], active[1]);

    KnobIntPtr knobs[2] = { _imp->knobs.inputs[0].lock(), _imp->knobs.inputs[1].lock() };
    if (!knobs[0] || !knobs[1]) {
        return;
    }

    if (getApp()->getProject()->isLoadingProject()) {
        // A load reconnects the inputs one at a time, each moving the inspector's active inputs;
        // the saved knobs are applied over them once every input is back, in onProjectLoaded().
        return;
    }

    SetterKnobWrites guard(&_imp->knobWritesFromSetter);
    for (int side = 0; side < 2; ++side) {
        knobs[side]->setValue(active[side]);
    }
    KnobLayerSelectPtr layer = _imp->knobs.layer.lock();
    KnobChannelSelectPtr alpha = _imp->knobs.alphaChannel.lock();
    if (layer) {
        node->setLayerKnobInput(layer, std::max(active[0], 0));
    }
    if (alpha) {
        node->setLayerKnobInput(alpha, std::max(active[0], 0));
    }
}

void
ViewerInstance::onProjectLoaded()
{
    NodePtr node = getNode();
    InspectorNode* isInspector = dynamic_cast<InspectorNode*>(node.get());
    KnobIntPtr knobs[2] = { _imp->knobs.inputs[0].lock(), _imp->knobs.inputs[1].lock() };

    if (!isInspector || !knobs[0] || !knobs[1]) {
        return;
    }
    int active[2] = { -1, -1 };
    isInspector->getActiveInputs(active[0], active[1]);
    bool changed = false;
    for (int side = 0; side < 2; ++side) {
        const int saved = knobs[side]->getValue();
        if (saved != active[side]) {
            applyActiveInput(side, saved);
            changed = true;
        }
    }
    if (changed) {
        // The toolbar menus and the data kind follow the inspector through this signal.
        Q_EMIT isInspector->activeInputsChanged();
    }
}

void
ViewerInstance::getFrameRange(double *first,
                              double *last)
{
    double inpFirst = 1, inpLast = 1;
    int activeInputs[2];

    getActiveInputs(activeInputs[0], activeInputs[1]);
    EffectInstancePtr n1 = getInput(activeInputs[0]);
    if (n1) {
        n1->getFrameRange_public(n1->getRenderHash(), &inpFirst, &inpLast);
    }
    *first = inpFirst;
    *last = inpLast;

    inpFirst = 1;
    inpLast = 1;

    EffectInstancePtr n2 = getInput(activeInputs[1]);
    if (n2) {
        n2->getFrameRange_public(n2->getRenderHash(), &inpFirst, &inpLast);
        if (inpFirst < *first) {
            *first = inpFirst;
        }

        if (inpLast > *last) {
            *last = inpLast;
        }
    }
}

void
ViewerInstance::executeDisconnectTextureRequestOnMainThread(int index,bool clearRoD)
{
    assert( QThread::currentThread() == qApp->thread() );
#ifdef NATRON_HAVE_GPU
    _imp->discardStagedFrame(index);
#endif
    if (_imp->uiContext) {
        _imp->uiContext->disconnectInputTexture(index, clearRoD);
    }
}

static bool
isRotoPaintNodeInputRecursive(Node* node,
                              const NodePtr& rotoPaintNode)
{
    if ( node == rotoPaintNode.get() ) {
        return true;
    }
    int maxInputs = node->getNInputs();
    for (int i = 0; i < maxInputs; ++i) {
        NodePtr input = node->getInput(i);
        if (input) {
            if (input == rotoPaintNode) {
                return true;
            } else {
                bool ret = isRotoPaintNodeInputRecursive(input.get(), rotoPaintNode);
                if (ret) {
                    return true;
                }
            }
        }
    }

    return false;
}

static void
updateLastStrokeDataRecursively(Node* node,
                                const NodePtr& rotoPaintNode,
                                const RectD& lastStrokeBbox,
                                bool invalidate)
{
    if ( isRotoPaintNodeInputRecursive(node, rotoPaintNode) ) {
        if (invalidate) {
            node->invalidateLastPaintStrokeDataNoRotopaint();
        } else {
            node->setLastPaintStrokeDataNoRotopaint();
        }

        if ( node == rotoPaintNode.get() ) {
            return;
        }
        int maxInputs = node->getNInputs();
        for (int i = 0; i < maxInputs; ++i) {
            NodePtr input = node->getInput(i);
            if (input) {
                updateLastStrokeDataRecursively(input.get(), rotoPaintNode, lastStrokeBbox, invalidate);
            }
        }
    }
}

class ViewerParallelRenderArgsSetter
    : public ParallelRenderArgsSetter
{
    NodePtr rotoNode;
    NodePtr viewerNode;
    NodePtr viewerInputNode;

public:

    ViewerParallelRenderArgsSetter(double time,
                                   ViewIdx view,
                                   bool isRenderUserInteraction,
                                   bool isSequential,
                                   const AbortableRenderInfoPtr& abortInfo,
                                   const NodePtr& treeRoot,
                                   int textureIndex,
                                   const TimeLine* timeline,
                                   bool isAnalysis,
                                   const NodePtr& rotoPaintNode,
                                   const NodePtr& viewerInput,
                                   bool draftMode,
                                   const RenderStatsPtr& stats)
        : ParallelRenderArgsSetter(time, view, isRenderUserInteraction, isSequential, abortInfo, treeRoot, textureIndex, timeline, rotoPaintNode, isAnalysis, draftMode, stats)
        , rotoNode(rotoPaintNode)
        , viewerNode(treeRoot)
        , viewerInputNode()
    {
        ///There can be a case where the viewer input tree does not belong to the project, for example
        ///for the File Dialog preview.
        if ( viewerInput && !viewerInput->getGroup() ) {
            viewerInputNode = viewerInput;
            U64 nodeHash = viewerInput->getHashValue();

            viewerInput->getEffectInstance()->setParallelRenderArgsTLS(time, view, isRenderUserInteraction, isSequential, nodeHash, abortInfo, treeRoot, 1, NodeFrameRequestPtr(), _openGLContext.lock(), textureIndex, timeline, isAnalysis, false, NodesList(), viewerInput->getCurrentRenderThreadSafety(), viewerInput->getCurrentOpenGLRenderSupport(), draftMode, stats);
        }
    }

    virtual ~ViewerParallelRenderArgsSetter()
    {
        if (rotoNode) {
            updateLastStrokeDataRecursively(viewerNode.get(), rotoNode, RectD(), true);
        }
        if (viewerInputNode) {
            viewerInputNode->getEffectInstance()->invalidateParallelRenderArgsTLS();
        }
    }
};

void
ViewerInstance::scanFrameArgsForScheduler(const std::map<NodePtr, ParallelRenderArgsPtr>& args,
                                          SchedulerEligibility* eligibility)
{
    NATRON_NAMESPACE::scanFrameArgsForScheduler(args, eligibility);
}

bool
ViewerInstance::isFrameEligibleForScheduler(const SchedulerEligibility& eligibility,
                                            const char** reason)
{
    static const SchedulerIneligibilityReasons viewerReasons = {
        "render thread is a pool thread",
        "render thread is the main thread",
        "no frame args",
        "input outside the frame args",
        "partial updates",
        "deep input",
        "paint stroke",
        "refresh",
        "analysis",
    };

    return NATRON_NAMESPACE::isFrameEligibleForScheduler(eligibility, viewerReasons, reason);
}

NATRON_NAMESPACE_ANONYMOUS_ENTER

std::mutex gSchedulerResultHookMutex;
ViewerInstance::SchedulerResultHook gSchedulerResultHook;

std::atomic<bool> gGpuPlacementOverlayEnabled(false);
std::atomic<int> gGpuShadowPlacementCount(0);
std::atomic<int> gGpuPlacementDryRunCount(0);
std::atomic<bool> gGpuPlacementFrozenForTests(false);

// Lets a snapshot from before the overlay was last turned on be told from a current one without reaching every viewer.
std::atomic<U64> gGpuPlacementOverlayGeneration(0);

ViewerInstance::SchedulerResultHook
schedulerResultHook()
{
    std::lock_guard<std::mutex> k(gSchedulerResultHookMutex);

    return gSchedulerResultHook;
}

bool
viewerTakesCpuFramesOnly(OpenGLViewerI* uiContext)
{
#ifdef NATRON_HAVE_GPU
    return !uiContext || (uiContext->handoffRung() == ViewerHandoffRung::Cpu);
#else
    Q_UNUSED(uiContext);

    return true;
#endif
}

#ifdef NATRON_HAVE_GPU
// The GPU viewer process does not read a matte from a second plane, and a
// root rendered for only part of the texture would drop the tiles found in the cache.
bool
viewerFrameCanTakeDeviceRoot(const ViewerArgs& inArgs,
                             const std::list<ImageLayerDesc>& components,
                             bool cpuFramesOnly)
{
    if (cpuFramesOnly) {
        return false;
    }
    if ((inArgs.channels == eDisplayChannelsMatte) && (components.size() > 1)) {
        return false;
    }

    return !inArgs.isDoingPartialUpdates && (inArgs.params->nbCachedTile == 0);
}

/**
 * Pins the device roots of frames waiting to be shown. Left evictable, they are the oldest buffers in the pool's LRU
 * while playback renders ahead, so VRAM pressure evicts the next frame to show first and it takes the CPU viewer
 * process. Pins take at most half the pool budget, which leaves the other half to the renders running ahead; a root
 * that does not fit waits, oldest first, for a pin to be released, and stays evictable meanwhile.
 **/
class DeviceRootHolds {
public:
    static DeviceRootHolds& instance()
    {
        // Never destroyed, since a frame may still be released while the process exits.
        static DeviceRootHolds* holds = new DeviceRootHolds;

        return *holds;
    }

    // Released when the returned token is.
    std::shared_ptr<void> hold(const GpuDeviceImagePtr& root)
    {
        if (!root) {
            return std::shared_ptr<void>();
        }
        std::shared_ptr<Entry> entry = std::make_shared<Entry>();
        entry->root = root;
        entry->bytes = root->bytes();
        std::vector<std::shared_ptr<Entry>> visited;
        {
            std::lock_guard<std::mutex> k(_mutex);
            _waiting.push_back(entry);
            promoteLocked(&visited);
        }

        return entry;
    }

private:
    struct Entry {
        GpuDeviceImagePtr root;
        std::size_t bytes = 0;
        bool pinned = false;

        ~Entry() { DeviceRootHolds::instance().release(*this); }
    };

    static std::size_t limitOf(const GpuDeviceImage& root)
    {
        gpu::GpuRuntime* runtime = root.runtime();
        if (!runtime || runtime->isLost()) {
            return 0;
        }

        return static_cast<std::size_t>(runtime->pool().budget() / 2);
    }

    void release(Entry& entry)
    {
        std::vector<std::shared_ptr<Entry>> visited;
        std::lock_guard<std::mutex> k(_mutex);
        if (entry.pinned) {
            entry.root->unpin();
            entry.pinned = false;
            _heldBytes -= entry.bytes;
        }
        promoteLocked(&visited);
    }

    // The entries it locks are dropped by the caller after the mutex, since the last reference to one may be among
    // them and its destructor takes the mutex.
    void promoteLocked(std::vector<std::shared_ptr<Entry>>* visited)
    {
        for (std::list<std::weak_ptr<Entry>>::iterator it = _waiting.begin(); it != _waiting.end();) {
            std::shared_ptr<Entry> entry = it->lock();
            if (!entry) {
                it = _waiting.erase(it);
                continue;
            }
            visited->push_back(entry);
            const std::size_t limit = limitOf(*entry->root);
            if ((entry->bytes == 0) || (entry->bytes > limit)) {
                it = _waiting.erase(it);
                continue;
            }
            if (_heldBytes + entry->bytes > limit) {
                return;
            }
            // Null once the root was evicted or downloaded, and then there is nothing left to keep on the device.
            if (entry->root->pin()) {
                entry->pinned = true;
                _heldBytes += entry->bytes;
            }
            it = _waiting.erase(it);
        }
    }

    std::mutex _mutex;
    std::size_t _heldBytes = 0;
    std::list<std::weak_ptr<Entry>> _waiting;
};

// Counts a frame the hand-off refused, logging each reason the first time it is given in the session.
void
noteHandoffRefusal(const RenderStatsPtr& stats,
                   const std::string& reason)
{
    if (stats) {
        stats->incLegacyFallbacks("viewer handoff: " + reason);
    }
    static std::mutex mutex;
    static std::set<std::string> logged;
    {
        std::lock_guard<std::mutex> k(mutex);
        if (!logged.insert(reason).second) {
            return;
        }
    }
    qInfo().noquote() << QString::fromLatin1("Viewer hand-off refused a frame, drawn by the CPU viewer process instead: %1")
                             .arg(QString::fromStdString(reason));
}
#endif

// The graph of the viewer input for one frame, empty when the request pass does not reach the input. The root task
// renders exactly what renderRoI would: the viewer's RoI and displayed layers.
FrameGraph
buildViewerFrameGraph(const FrameRenderContextPtr& context,
                      const ViewerArgs& inArgs,
                      ViewIdx view,
                      const RectI& roi,
                      const std::list<ImageLayerDesc>& components,
                      ImageBitDepthEnum bitdepth,
                      bool cpuFramesOnly)
{
#ifdef NATRON_HAVE_GPU
    context->setWantsRootOnDevice(viewerFrameCanTakeDeviceRoot(inArgs, components, cpuFramesOnly));
#else
    Q_UNUSED(cpuFramesOnly);
#endif
    FrameGraph graph = RenderScheduler::buildGraph(context, inArgs.activeInputToRender->getNode(), inArgs.params->time, view, inArgs.params->mipmapLevel);
    if (!graph.tasks.empty()) {
        FrameGraph::Task& root = graph.tasks.front();
        root.roi = roi;
        root.components = components;
        root.bitdepth = bitdepth;
    }

    return graph;
}

// Returns whether a shadow plan was placed.
bool
addPlacedGraphToSnapshot(FrameGraph* graph,
                         const FrameRenderContextPtr& context,
                         GpuPlacementSnapshotBuilder* builder)
{
    ShadowPlacement shadow;
    if (placeRenderGraph(*graph, context, &shadow)) {
        builder->addGraph(*graph, shadow);

        return true;
    }
    builder->addUnplacedGraph(*graph);

    return false;
}

// Which layers the viewer asks of its input, none when it has no layer to show, and where the alpha it shows sits in
// them.
void
computeViewerRequestedLayers(const ViewerArgs& inArgs,
                             const ImageLayerDesc& inputLayers,
                             std::list<ImageLayerDesc>* requestedComponents,
                             int* alphaChannelIndex)
{
    *alphaChannelIndex = -1;
    if ((inArgs.channels != eDisplayChannelsA) && (inArgs.channels != eDisplayChannelsMatte)) {
        /// We fetch the Layer specified in the gui
        if (inArgs.params->layer.getNumComponents() > 0) {
            requestedComponents->push_back(inArgs.params->layer);
        }
    } else {
        /// We fetch the alpha layer
        if (!inArgs.params->alphaChannelName.empty()) {
            requestedComponents->push_back(inArgs.params->alphaLayer);
            const std::vector<std::string>& channels = inArgs.params->alphaLayer.getChannels();
            for (std::size_t i = 0; i < channels.size(); ++i) {
                if (channels[i] == inArgs.params->alphaChannelName) {
                    *alphaChannelIndex = i;
                    break;
                }
            }
            // alphaChannelIndex stays -1 when the colour views name an A the stream lacks (an
            // RGB-only plane): that alpha reads as zero, so the A display below renders black.
        }
        if (inArgs.channels == eDisplayChannelsMatte) {
            // For the matte overlay also display the alpha mask on top of the red channel of the image
            if ((inArgs.params->layer.getNumComponents() > 0) && (inArgs.params->layer != inArgs.params->alphaLayer)) {
                requestedComponents->push_back(inArgs.params->layer);
            }
        }
    }

    // Raw is shown from the input's own plane (its 1 or 3 channels), whatever layer the Viewer
    // is set to, and converted to RGBA with alpha 1 by RawDisplay below.
    if (inArgs.rawUpstream) {
        requestedComponents->clear();
        requestedComponents->push_back(inputLayers);
        *alphaChannelIndex = ((inArgs.channels == eDisplayChannelsA) || (inArgs.channels == eDisplayChannelsMatte)) ? 3 : -1;
    }
}

ViewerInstance::SchedulerEligibility
computeViewerFrameEligibility(const ParallelRenderArgsSetter* setter,
                              const std::shared_ptr<FrameRequestMap>& request,
                              const ViewerArgs& inArgs,
                              bool hasRotoPaintNode)
{
    const NodePtr inputNode = inArgs.activeInputToRender->getNode();
    ViewerInstance::SchedulerEligibility eligibility;

    eligibility.hasFrameArgs = setter && request;
    if (setter) {
        const std::map<NodePtr, ParallelRenderArgsPtr>& installedArgs = setter->getInstalledArgs();
        ViewerInstance::scanFrameArgsForScheduler(installedArgs, &eligibility);
        eligibility.inputHasFrameArgs = installedArgs.find(inputNode) != installedArgs.end();
    }
    eligibility.isDoingPartialUpdates = inArgs.isDoingPartialUpdates;
    eligibility.deepUpstream = inArgs.deepUpstream;
    eligibility.paintStroke = eligibility.paintStroke || hasRotoPaintNode;
    eligibility.forceRender = inArgs.forceRender;
    eligibility.onPoolThread = QThreadPool::globalInstance()->contains(QThread::currentThread());
    eligibility.onMainThread = QCoreApplication::instance() && (QThread::currentThread() == QCoreApplication::instance()->thread());

    return eligibility;
}

// Submits the viewer input's render of one frame to the RenderScheduler, or returns null and counts why it falls
// back to renderRoI.
FrameFuturePtr
submitViewerFrameToRenderScheduler(const ParallelRenderArgsSetter* setter,
                                   const std::shared_ptr<FrameRequestMap>& request,
                                   const ViewerArgs& inArgs,
                                   bool hasRotoPaintNode,
                                   ViewIdx view,
                                   const RectI& roi,
                                   const std::list<ImageLayerDesc>& components,
                                   ImageBitDepthEnum bitdepth,
                                   const RenderStatsPtr& stats,
                                   OpenGLViewerI* uiContext,
                                   GpuPlacementSnapshotBuilder* placementBuilder,
                                   FrameRenderContextPtr* contextOut)
{
    const ViewerInstance::SchedulerEligibility eligibility = computeViewerFrameEligibility(setter, request, inArgs, hasRotoPaintNode);
    const char* reason = 0;
    if (ViewerInstance::isFrameEligibleForScheduler(eligibility, &reason)) {
        FrameRenderContextPtr context = FrameRenderContext::createFromSetter(*setter, inArgs.params->abortInfo, stats, inArgs.params->time, view);
        context->setRequest(request);
        FrameGraph graph = buildViewerFrameGraph(context, inArgs, view, roi, components, bitdepth, viewerTakesCpuFramesOnly(uiContext));
        if (graph.tasks.empty()) {
            reason = "request does not reach the viewer input";
        } else {
            if (placementBuilder) {
                context->setRecordsRenderedNodes(true);
                if (addPlacedGraphToSnapshot(&graph, context, placementBuilder)) {
                    gGpuShadowPlacementCount.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                placeRenderGraph(graph, context);
            }
            if (contextOut) {
                *contextOut = context;
            }

            return appPTR->getRenderScheduler()->submit(context, std::move(graph), RenderScheduler::Priority::Interactive);
        }
    }
    if (reason && stats) {
        stats->incLegacyFallbacks(std::string("viewer: ") + reason);
    }

    return FrameFuturePtr();
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

void
ViewerInstance::setSchedulerResultHookForTests(const SchedulerResultHook& hook)
{
    std::lock_guard<std::mutex> k(gSchedulerResultHookMutex);

    gSchedulerResultHook = hook;
}

void
GpuPlacementSnapshotBuilder::addGraph(const FrameGraph& real,
                                      const ShadowPlacement& shadow)
{
    GraphPlan plan;
    const std::vector<FrameGraph::Task>& tasks = shadow.graph.tasks;

    plan.regionCount = (int)shadow.graph.regions.size();
    plan.tasks.reserve(tasks.size());
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        const FrameGraph::Task& task = tasks[i];
        TaskPlan taskPlan;
        taskPlan.node = task.key.node;
        taskPlan.region = task.regionId;
        taskPlan.realRegion = real.tasks[i].regionId;
        taskPlan.reason = task.placementReason;
        taskPlan.detail = task.placementDetail;
        plan.tasks.push_back(std::move(taskPlan));
        for (std::size_t e = 0; e < task.dependencies.size(); ++e) {
            // A negative input number is a read no pipe of the node graph stands for, so there is nothing to mark.
            if (task.dependencyInputNbs[e] >= 0) {
                EdgePlan edge;
                edge.source = task.dependencies[e];
                edge.dest = (int)i;
                edge.inputNb = task.dependencyInputNbs[e];
                plan.edges.push_back(edge);
            }
        }
    }
    _graphs.push_back(std::move(plan));
    _placed = true;
}

bool
GpuPlacementSnapshotBuilder::hasPlacedGraph() const
{
    return _placed;
}

void
GpuPlacementSnapshotBuilder::addUnplacedGraph()
{
    _graphs.emplace_back();
}

void
GpuPlacementSnapshotBuilder::addUnplacedGraph(const FrameGraph& real)
{
    GraphPlan plan;
    plan.tasks.reserve(real.tasks.size());
    for (const FrameGraph::Task& task : real.tasks) {
        TaskPlan taskPlan;
        taskPlan.node = task.key.node;
        plan.tasks.push_back(std::move(taskPlan));
    }
    _graphs.push_back(std::move(plan));
}

void
GpuPlacementSnapshotBuilder::finishGraph(EffectInstance::RenderRoIRetCode retCode,
                                         const std::vector<RegionFallback>& fallbacks,
                                         const std::unordered_set<const Node*>* renderedNodes)
{
    if (retCode == EffectInstance::eRenderRoIRetCodeAborted) {
        _aborted = true;
    }
    if (_graphs.empty()) {
        return;
    }
    GraphPlan& plan = _graphs.back();
    plan.fallbacks = fallbacks;
    if (renderedNodes) {
        plan.recordsRuns = true;
        for (TaskPlan& task : plan.tasks) {
            const NodePtr node = task.node.lock();
            task.ran = node && renderedNodes->count(node.get()) > 0;
        }
    }
}

NATRON_NAMESPACE_ANONYMOUS_ENTER

typedef std::owner_less<NodeWPtr> NodeOwnerLess;

struct PipeLess {
    bool operator()(const GpuPlacementSnapshot::Pipe& a,
                    const GpuPlacementSnapshot::Pipe& b) const
    {
        if (NodeOwnerLess()(a.destNode, b.destNode)) {
            return true;
        }
        if (NodeOwnerLess()(b.destNode, a.destNode)) {
            return false;
        }

        return a.inputNb < b.inputNb;
    }
};

struct NodeTally {
    int gpuTasks = 0;
    int cpuTasks = 0;
};

struct PipeTally {
    int exits = 0;
    int others = 0;
    FrameGraph::PlacementReason reason = FrameGraph::PlacementReason::None;
    std::string detail;
    bool reasonsDiffer = false;

    // The snapshot regions already listing this pipe.
    std::set<int> listedIn;
};

int
findRegionGroup(std::vector<int>& parents,
                int region)
{
    while (parents[region] != region) {
        parents[region] = parents[parents[region]];
        region = parents[region];
    }

    return region;
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

GpuPlacementSnapshotPtr
GpuPlacementSnapshotBuilder::build() const
{
    if (_aborted || _graphs.empty()) {
        return GpuPlacementSnapshotPtr();
    }

    std::vector<int> regionOffsets;
    int regionCount = 0;
    for (const GraphPlan& graph : _graphs) {
        regionOffsets.push_back(regionCount);
        regionCount += graph.regionCount;
    }

    std::vector<int> parents(regionCount);
    for (int r = 0; r < regionCount; ++r) {
        parents[r] = r;
    }
    std::map<NodeWPtr, int, NodeOwnerLess> regionOfNode;
    std::map<NodeWPtr, NodeTally, NodeOwnerLess> nodeTallies;
    for (std::size_t g = 0; g < _graphs.size(); ++g) {
        for (const TaskPlan& task : _graphs[g].tasks) {
            NodeTally& tally = nodeTallies[task.node];
            if (task.region < 0) {
                ++tally.cpuTasks;
                continue;
            }
            ++tally.gpuTasks;
            const int region = regionOffsets[g] + task.region;
            const std::pair<std::map<NodeWPtr, int, NodeOwnerLess>::iterator, bool> added = regionOfNode.insert(std::make_pair(task.node, region));
            if (!added.second) {
                parents[findRegionGroup(parents, region)] = findRegionGroup(parents, added.first->second);
            }
        }
    }

    std::shared_ptr<GpuPlacementSnapshot> snapshot = std::make_shared<GpuPlacementSnapshot>();

    // Snapshot regions and their members come in the order the graphs first name them, so that a merge is stable.
    std::vector<int> snapshotRegionOfGroup(regionCount, -1);
    const auto snapshotRegionOf = [&parents, &snapshotRegionOfGroup](int region) {
        return snapshotRegionOfGroup[findRegionGroup(parents, region)];
    };
    std::set<NodeWPtr, NodeOwnerLess> listedMembers;
    for (std::size_t g = 0; g < _graphs.size(); ++g) {
        for (const TaskPlan& task : _graphs[g].tasks) {
            if (task.region < 0) {
                continue;
            }
            const int group = findRegionGroup(parents, regionOffsets[g] + task.region);
            if (snapshotRegionOfGroup[group] < 0) {
                snapshotRegionOfGroup[group] = (int)snapshot->regions.size();
                snapshot->regions.emplace_back();
            }
            if (listedMembers.insert(task.node).second) {
                const NodeTally& tally = nodeTallies[task.node];
                GpuPlacementSnapshot::Member member;
                member.node = task.node;
                member.mixed = tally.cpuTasks > 0;
                snapshot->regions[snapshotRegionOfGroup[group]].members.push_back(std::move(member));
            }
        }
    }

    std::map<GpuPlacementSnapshot::Pipe, PipeTally, PipeLess> pipeTallies;
    const auto pipeOf = [](const TaskPlan& dest, const EdgePlan& edge) {
        GpuPlacementSnapshot::Pipe pipe;
        pipe.destNode = dest.node;
        pipe.inputNb = edge.inputNb;

        return pipe;
    };
    for (const GraphPlan& graph : _graphs) {
        for (const EdgePlan& edge : graph.edges) {
            const TaskPlan& source = graph.tasks[edge.source];
            const TaskPlan& dest = graph.tasks[edge.dest];
            PipeTally& tally = pipeTallies[pipeOf(dest, edge)];
            if ((source.region < 0) || (source.region == dest.region)) {
                ++tally.others;
                continue;
            }
            if (tally.exits == 0) {
                tally.reason = dest.reason;
                tally.detail = dest.detail;
            } else if ((tally.reason != dest.reason) || (tally.detail != dest.detail)) {
                tally.reasonsDiffer = true;
            }
            ++tally.exits;
        }
    }
    for (std::size_t g = 0; g < _graphs.size(); ++g) {
        const GraphPlan& graph = _graphs[g];
        for (const EdgePlan& edge : graph.edges) {
            const TaskPlan& source = graph.tasks[edge.source];
            const TaskPlan& dest = graph.tasks[edge.dest];
            const GpuPlacementSnapshot::Pipe pipe = pipeOf(dest, edge);
            PipeTally& tally = pipeTallies[pipe];
            if ((source.region >= 0) && (source.region != dest.region)) {
                const int region = snapshotRegionOf(regionOffsets[g] + source.region);
                if (tally.listedIn.insert(region).second) {
                    GpuPlacementSnapshot::Exit regionExit;
                    regionExit.pipe = pipe;
                    regionExit.reason = tally.reason;
                    regionExit.detail = tally.detail;
                    regionExit.mixed = tally.reasonsDiffer || (tally.others > 0);
                    snapshot->regions[region].exits.push_back(std::move(regionExit));
                }
            } else if ((source.region < 0) && (dest.region >= 0) && (tally.exits == 0)) {
                const int region = snapshotRegionOf(regionOffsets[g] + dest.region);

                // A source on the GPU in another graph is drawn inside this region, where a tick would mean nothing.
                const std::map<NodeWPtr, int, NodeOwnerLess>::const_iterator sourceRegion = regionOfNode.find(source.node);
                const bool insideRegion = (sourceRegion != regionOfNode.end()) && (snapshotRegionOf(sourceRegion->second) == region);
                if (!insideRegion && tally.listedIn.insert(region).second) {
                    snapshot->regions[region].entries.push_back(pipe);
                }
            }
        }
    }

    std::vector<bool> fellBack(regionCount, false);
    for (std::size_t g = 0; g < _graphs.size(); ++g) {
        const GraphPlan& graph = _graphs[g];
        for (const RegionFallback& fallback : graph.fallbacks) {
            std::set<int> planned;
            for (const TaskPlan& task : graph.tasks) {
                // A lost device sends the whole frame back to the CPU, not just the region that met it.
                if ((task.region >= 0) && (fallback.deviceLost || (task.realRegion == fallback.region))) {
                    planned.insert(regionOffsets[g] + task.region);
                }
            }
            for (int region : planned) {
                GpuPlacementSnapshot::Region& out = snapshot->regions[snapshotRegionOf(region)];
                if (!out.fellBack) {
                    out.fellBack = true;
                    out.fallbackReason = fallback.reason;
                } else if (out.fallbackReason != fallback.reason) {
                    out.fallbackMixed = true;
                }
                out.deviceLost = out.deviceLost || fallback.deviceLost;
                out.retries = std::max(out.retries, fallback.retries);
                fellBack[region] = true;
            }
        }
    }
    for (int region = 0; region < regionCount; ++region) {
        GpuPlacementSnapshot::Region& out = snapshot->regions[snapshotRegionOf(region)];
        if (out.fellBack && !fellBack[region]) {
            out.fallbackMixed = true;
        }
    }

    std::map<NodeWPtr, std::size_t, NodeOwnerLess> runOfNode;
    for (const GraphPlan& graph : _graphs) {
        if (!graph.recordsRuns) {
            continue;
        }
        for (const TaskPlan& task : graph.tasks) {
            const std::pair<std::map<NodeWPtr, std::size_t, NodeOwnerLess>::iterator, bool> added = runOfNode.insert(std::make_pair(task.node, snapshot->nodeRuns.size()));
            if (added.second) {
                GpuPlacementSnapshot::NodeRun run;
                run.node = task.node;
                snapshot->nodeRuns.push_back(run);
            }
            if (task.ran) {
                snapshot->nodeRuns[added.first->second].ran = true;
            }
        }
    }

    return snapshot;
} // GpuPlacementSnapshotBuilder::build

void
ViewerInstance::setGpuPlacementOverlayEnabled(bool enabled)
{
    if (gGpuPlacementOverlayEnabled.exchange(enabled) == enabled) {
        return;
    }
    if (enabled) {
        gGpuPlacementOverlayGeneration.fetch_add(1);
        requestGpuPlacementDryRunForEveryViewer();
    }
}

bool
ViewerInstance::isGpuPlacementOverlayEnabled()
{
    return gGpuPlacementOverlayEnabled.load();
}

int
ViewerInstance::getGpuShadowPlacementCountForTests()
{
    return gGpuShadowPlacementCount.load();
}

GpuPlacementSnapshotPtr
ViewerInstance::getGpuPlacementSnapshot() const
{
    if (!isGpuPlacementOverlayEnabled()) {
        return GpuPlacementSnapshotPtr();
    }
    std::lock_guard<std::mutex> k(_imp->gpuPlacementSnapshotMutex);
    if (_imp->gpuPlacementSnapshotStamp.overlayGeneration != gGpuPlacementOverlayGeneration.load()) {
        return GpuPlacementSnapshotPtr();
    }

    return _imp->gpuPlacementSnapshot;
}

NATRON_NAMESPACE_ANONYMOUS_ENTER

bool
sameNode(const NodeWPtr& a,
         const NodeWPtr& b)
{
    return !a.owner_before(b) && !b.owner_before(a);
}

bool
samePipe(const GpuPlacementSnapshot::Pipe& a,
         const GpuPlacementSnapshot::Pipe& b)
{
    return sameNode(a.destNode, b.destNode) && (a.inputNb == b.inputNb);
}

bool
sameMember(const GpuPlacementSnapshot::Member& a,
           const GpuPlacementSnapshot::Member& b)
{
    return sameNode(a.node, b.node) && (a.mixed == b.mixed);
}

bool
sameExit(const GpuPlacementSnapshot::Exit& a,
         const GpuPlacementSnapshot::Exit& b)
{
    return samePipe(a.pipe, b.pipe) && (a.reason == b.reason) && (a.detail == b.detail) && (a.mixed == b.mixed);
}

bool
sameRegion(const GpuPlacementSnapshot::Region& a,
           const GpuPlacementSnapshot::Region& b)
{
    return std::equal(a.members.begin(), a.members.end(), b.members.begin(), b.members.end(), sameMember) && std::equal(a.exits.begin(), a.exits.end(), b.exits.begin(), b.exits.end(), sameExit) && std::equal(a.entries.begin(), a.entries.end(), b.entries.begin(), b.entries.end(), samePipe) && (a.fellBack == b.fellBack) && (a.fallbackReason == b.fallbackReason) && (a.deviceLost == b.deviceLost) && (a.retries == b.retries) && (a.fallbackMixed == b.fallbackMixed);
}

bool
sameNodeRun(const GpuPlacementSnapshot::NodeRun& a,
            const GpuPlacementSnapshot::NodeRun& b)
{
    return sameNode(a.node, b.node) && (a.ran == b.ran);
}

bool
sameGpuPlacement(const GpuPlacementSnapshot& a,
                 const GpuPlacementSnapshot& b)
{
    return std::equal(a.regions.begin(), a.regions.end(), b.regions.begin(), b.regions.end(), sameRegion) && std::equal(a.nodeRuns.begin(), a.nodeRuns.end(), b.nodeRuns.begin(), b.nodeRuns.end(), sameNodeRun);
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

ViewerInstance::ViewerInstancePrivate::GpuPlacementStamp
ViewerInstance::ViewerInstancePrivate::currentGpuPlacementStamp() const
{
    GpuPlacementStamp stamp;

    stamp.overlayGeneration = gGpuPlacementOverlayGeneration.load();
    stamp.graphEdits = gpuPlacementGraphEdits.load();

    return stamp;
}

bool
ViewerInstance::ViewerInstancePrivate::hasDryRunGpuPlacement() const
{
    const GpuPlacementStamp stamp = currentGpuPlacementStamp();
    std::lock_guard<std::mutex> k(gpuPlacementSnapshotMutex);

    return gpuPlacementRegions && gpuPlacementRegionsFromDryRun && (gpuPlacementRegionsStamp == stamp);
}

NATRON_NAMESPACE_ANONYMOUS_ENTER

// run with the nodes earlier also ran and the regions that earlier fell back, for renders of one state of the graph:
// one that renders only the tiles another left out ran the rest of the nodes too, and a region that recovered from a
// lost device in one render fell back in the other.
GpuPlacementSnapshotPtr
withEarlierRuns(const GpuPlacementSnapshotPtr& run,
                const GpuPlacementSnapshotPtr& earlier)
{
    std::shared_ptr<GpuPlacementSnapshot> merged = std::make_shared<GpuPlacementSnapshot>(*run);
    std::set<NodeWPtr, NodeOwnerLess> fellBackMembers;
    for (const GpuPlacementSnapshot::Region& region : merged->regions) {
        if (region.fellBack) {
            for (const GpuPlacementSnapshot::Member& member : region.members) {
                fellBackMembers.insert(member.node);
            }
        }
    }
    for (const GpuPlacementSnapshot::Region& region : earlier->regions) {
        if (!region.fellBack || region.members.empty() || fellBackMembers.count(region.members.front().node)) {
            continue;
        }
        merged->regions.push_back(region);
        for (const GpuPlacementSnapshot::Member& member : region.members) {
            fellBackMembers.insert(member.node);
        }
    }
    std::map<NodeWPtr, std::size_t, NodeOwnerLess> runOfNode;
    for (std::size_t i = 0; i < merged->nodeRuns.size(); ++i) {
        runOfNode.insert(std::make_pair(merged->nodeRuns[i].node, i));
    }
    for (const GpuPlacementSnapshot::NodeRun& before : earlier->nodeRuns) {
        const std::pair<std::map<NodeWPtr, std::size_t, NodeOwnerLess>::iterator, bool> added = runOfNode.insert(std::make_pair(before.node, merged->nodeRuns.size()));
        if (added.second) {
            merged->nodeRuns.push_back(before);
        } else {
            GpuPlacementSnapshot::NodeRun& now = merged->nodeRuns[added.first->second];
            now.ran = now.ran || before.ran;
        }
    }

    return merged;
}

bool
anyNodeRan(const GpuPlacementSnapshot& snapshot)
{
    for (const GpuPlacementSnapshot::NodeRun& run : snapshot.nodeRuns) {
        if (run.ran) {
            return true;
        }
    }

    return false;
}

// The regions of placement, with which of them fell back in run. A region of run that shares a node with one of
// placement is that region.
GpuPlacementSnapshotPtr
composeGpuPlacement(const GpuPlacementSnapshot& placement,
                    const GpuPlacementSnapshot* run)
{
    std::shared_ptr<GpuPlacementSnapshot> out = std::make_shared<GpuPlacementSnapshot>();
    out->regions = placement.regions;
    std::map<NodeWPtr, std::vector<std::size_t>, NodeOwnerLess> runRegionsOfNode;
    if (run) {
        out->nodeRuns = run->nodeRuns;
        for (std::size_t r = 0; r < run->regions.size(); ++r) {
            for (const GpuPlacementSnapshot::Member& member : run->regions[r].members) {
                std::vector<std::size_t>& regions = runRegionsOfNode[member.node];
                if (regions.empty() || (regions.back() != r)) {
                    regions.push_back(r);
                }
            }
        }
    }
    // The region of out that last matched each region of run, so that a region of run is matched once per region.
    std::vector<std::size_t> matchedBy(run ? run->regions.size() : 0, out->regions.size());
    std::vector<std::size_t> matched;
    for (std::size_t r = 0; r < out->regions.size(); ++r) {
        GpuPlacementSnapshot::Region& region = out->regions[r];
        region.fellBack = false;
        region.fallbackReason.clear();
        region.deviceLost = false;
        region.retries = 0;
        region.fallbackMixed = false;
        matched.clear();
        for (const GpuPlacementSnapshot::Member& member : region.members) {
            const std::map<NodeWPtr, std::vector<std::size_t>, NodeOwnerLess>::const_iterator found = runRegionsOfNode.find(member.node);
            if (found == runRegionsOfNode.end()) {
                continue;
            }
            for (std::size_t ranRegion : found->second) {
                if (matchedBy[ranRegion] != r) {
                    matchedBy[ranRegion] = r;
                    matched.push_back(ranRegion);
                }
            }
        }
        bool anyKept = false;
        for (std::size_t index : matched) {
            const GpuPlacementSnapshot::Region& ranRegion = run->regions[index];
            if (!ranRegion.fellBack) {
                anyKept = true;
                continue;
            }
            if (!region.fellBack) {
                region.fellBack = true;
                region.fallbackReason = ranRegion.fallbackReason;
            } else if (region.fallbackReason != ranRegion.fallbackReason) {
                region.fallbackMixed = true;
            }
            region.deviceLost = region.deviceLost || ranRegion.deviceLost;
            region.retries = std::max(region.retries, ranRegion.retries);
            region.fallbackMixed = region.fallbackMixed || ranRegion.fallbackMixed;
        }
        if (region.fellBack && anyKept) {
            region.fallbackMixed = true;
        }
    }

    return out;
} // composeGpuPlacement

NATRON_NAMESPACE_ANONYMOUS_EXIT

bool
ViewerInstance::ViewerInstancePrivate::publishGpuPlacement(const GpuPlacementSnapshotBuilder& builder,
                                                           const GpuPlacementStamp& stamp,
                                                           bool isDryRun)
{
    if (!ViewerInstance::isGpuPlacementOverlayEnabled() || instance->isGpuPlacementFrozen()) {
        return false;
    }
    GpuPlacementSnapshotPtr snapshot = builder.build();
    if (!snapshot) {
        return false;
    }
    const bool placed = builder.hasPlacedGraph();

    // Publishers take turns, so the state below changes only while this is held, and is read here without
    // gpuPlacementSnapshotMutex, which the GUI takes and so is held only to store the result.
    std::unique_lock<std::mutex> publishing(gpuPlacementPublishMutex);

    // A plan of an older state is dropped: a re-plan of the new one is already asked for.
    if (stamp != currentGpuPlacementStamp()) {
        return false;
    }
    GpuPlacementSnapshotPtr regions = gpuPlacementRegions;
    GpuPlacementStamp regionsStamp = gpuPlacementRegionsStamp;
    bool regionsFromDryRun = gpuPlacementRegionsFromDryRun;
    bool regionsPlaced = gpuPlacementRegionsPlaced;
    GpuPlacementSnapshotPtr ran = gpuPlacementRun;
    GpuPlacementStamp ranStamp = gpuPlacementRunStamp;
    bool replan = false;
    const bool haveRegions = regions && (regionsStamp == stamp);
    if (isDryRun) {
        // A device lost since the render placed is no reason to stop showing where it ran.
        if (!haveRegions || placed || !regionsPlaced) {
            regions = snapshot;
            regionsStamp = stamp;
            regionsFromDryRun = true;
            regionsPlaced = placed;
        }
    } else {
        if (!haveRegions || (placed && !regionsPlaced)) {
            regions = snapshot;
            regionsStamp = stamp;
            regionsFromDryRun = false;
            regionsPlaced = placed;
            replan = placed;
        }
        if (!ran || (ranStamp != stamp)) {
            ran = snapshot;
            ranStamp = stamp;
        } else if (anyNodeRan(*snapshot)) {
            // A render that took everything from the cache, such as a redraw, says nothing new about this state.
            ran = withEarlierRuns(snapshot, ran);
        }
    }
    const bool haveRun = ran && (ranStamp == stamp);
    GpuPlacementSnapshotPtr composed = composeGpuPlacement(*regions, haveRun ? ran.get() : nullptr);
    const bool hadSnapshot = gpuPlacementSnapshot && (gpuPlacementSnapshotStamp.overlayGeneration == stamp.overlayGeneration);
    const bool changed = !hadSnapshot || !sameGpuPlacement(*gpuPlacementSnapshot, *composed);
    {
        std::lock_guard<std::mutex> k(gpuPlacementSnapshotMutex);
        gpuPlacementRegions = std::move(regions);
        gpuPlacementRegionsStamp = regionsStamp;
        gpuPlacementRegionsFromDryRun = regionsFromDryRun;
        gpuPlacementRegionsPlaced = regionsPlaced;
        gpuPlacementRun = std::move(ran);
        gpuPlacementRunStamp = ranStamp;
        if (changed) {
            gpuPlacementSnapshot = std::move(composed);
        }
        gpuPlacementSnapshotStamp = stamp;
    }
    publishing.unlock();
    if (replan) {
        gpuPlacementPlanner->request();
    }

    return changed;
} // ViewerInstance::ViewerInstancePrivate::publishGpuPlacement

void
ViewerInstance::requestGpuPlacementDryRun()
{
    if (!isGpuPlacementOverlayEnabled()) {
        return;
    }
    _imp->gpuPlacementGraphEdits.fetch_add(1);
    _imp->gpuPlacementPlanner->request();
}

void
ViewerInstance::requestGpuPlacementDryRunForEveryViewer()
{
    if (!isGpuPlacementOverlayEnabled() || !appPTR) {
        return;
    }
    const AppInstanceVec apps = appPTR->getAppInstances();
    for (AppInstanceVec::const_iterator app = apps.begin(); app != apps.end(); ++app) {
        std::list<ViewerInstance*> viewers;
        (*app)->getProject()->getViewers(&viewers);
        for (std::list<ViewerInstance*>::const_iterator viewer = viewers.begin(); viewer != viewers.end(); ++viewer) {
            (*viewer)->requestGpuPlacementDryRun();
        }
    }
}

int
ViewerInstance::getGpuPlacementDryRunCountForTests()
{
    return gGpuPlacementDryRunCount.load();
}

void
ViewerInstance::setGpuPlacementFrozenForTests(bool frozen)
{
    gGpuPlacementFrozenForTests.store(frozen);
}

void
ViewerInstance::getRenderAgesForTests(int textureIndex,
                                      U64* renderAge,
                                      U64* displayAge) const
{
    QMutexLocker k(&_imp->renderAgeMutex);

    *renderAge = _imp->renderAge[textureIndex];
    *displayAge = _imp->displayAge[textureIndex];
}

bool
ViewerInstance::isGpuPlacementFrozen() const
{
    return gGpuPlacementFrozenForTests.load() || isDoingSequentialRender();
}

bool
ViewerInstance::isGpuPlacementDryRunHeld() const
{
    if (isGpuPlacementFrozen() || isDoingPartialUpdates()) {
        return true;
    }
    const AppInstancePtr app = getApp();
    if (!app) {
        return true;
    }
    const ProjectPtr project = app->getProject();
    if (project->isLoadingProject() || project->isProjectClosing()) {
        return true;
    }
    NodePtr rotoPaintNode;
    RotoStrokeItemPtr stroke;
    bool isPainting = false;
    app->getActiveRotoDrawingStroke(&rotoPaintNode, &stroke, &isPainting);

    return isPainting;
}

void
ViewerInstance::onNodeHashChanged(U64 hash)
{
    OutputEffectInstance::onNodeHashChanged(hash);

    // A plan that finds the hash dirty reads the graph as that edit left it, so it would only drop itself for a re-plan.
    if (QThread::currentThread() != _imp->gpuPlacementPlanner.get()) {
        requestGpuPlacementDryRun();
    }
}

void
ViewerInstance::quitBackgroundWork(bool blocking)
{
    OutputEffectInstance::quitBackgroundWork(blocking);
    _imp->gpuPlacementPlanner->quitThread(true);
    if (blocking) {
        _imp->gpuPlacementPlanner->waitForThreadToQuit_enforce_blocking();
    }
}

static bool checkTreeCanRender(Node* node);

struct ViewerInstance::GpuPlacementUiState {
    ViewIdx view;
    int inputCount = 1;
    bool userRoIEnabled = false;
    double zoomFactor = 1.;
    bool cpuFramesOnly = true;
};

bool
ViewerInstance::readGpuPlacementUiState(GpuPlacementUiState* state) const
{
    // What the UI context would answer, read here because a knob's value can need the GIL, which the main thread may
    // hold while it waits on the mutex below.
    const KnobBoolPtr userRoI = getKnobByNameAndType<KnobBool>(kViewerParamEnableUserRoI);
    state->userRoIEnabled = userRoI && userRoI->getValue();

    std::lock_guard<std::mutex> k(_imp->gpuPlacementUiContextMutex);
    OpenGLViewerI* uiContext = _imp->uiContext;
    if (!uiContext) {
        return false;
    }
    state->view = uiContext->getCurrentView();
    state->inputCount = (uiContext->getCompositingOperator() == eViewerCompositingOperatorNone) ? 1 : 2;
    state->zoomFactor = uiContext->getZoomFactor();
    state->cpuFramesOnly = viewerTakesCpuFramesOnly(uiContext);

    return true;
}

bool
ViewerInstance::getGpuPlacementRoI(int textureIndex,
                                   const RectD& rod,
                                   double par,
                                   unsigned int mipmapLevel,
                                   bool exact,
                                   RectI* roi) const
{
    std::lock_guard<std::mutex> k(_imp->gpuPlacementUiContextMutex);
    OpenGLViewerI* uiContext = _imp->uiContext;
    if (!uiContext) {
        return false;
    }
    if (exact) {
        *roi = uiContext->getExactImageRectangleDisplayed(textureIndex, rod, par, mipmapLevel);
    } else {
        // Regions can depend on the RoI, so this asks for the tile-rounded one a real render would.
        std::vector<RectI> tiles, tilesRounded;
        int tileSize;
        RectI roiNotRounded;
        *roi = uiContext->getImageRectangleDisplayedRoundedToTileSize(textureIndex, rod, par, mipmapLevel, &tiles, &tilesRounded, &tileSize, &roiNotRounded);
    }

    return true;
}

void
ViewerInstance::planGpuPlacementOfInput(int textureIndex,
                                        SequenceTime time,
                                        const GpuPlacementUiState& ui,
                                        const AbortableRenderInfoPtr& abortInfo,
                                        GpuPlacementSnapshotBuilder* builder)
{
    const ViewIdx view = ui.view;
    int activeInputs[2];
    getActiveInputs(activeInputs[0], activeInputs[1]);

    ViewerArgs args;
    args.activeInputIndex = activeInputs[textureIndex];
    const EffectInstancePtr upstream = getInput(args.activeInputIndex);
    if (upstream) {
        args.activeInputToRender = upstream->getNearestNonDisabled(time);
    }
    if (!args.activeInputToRender || !checkTreeCanRender(args.activeInputToRender->getNode().get())) {
        builder->addUnplacedGraph();

        return;
    }
    args.deepUpstream = args.activeInputToRender->producesDeepData();
    if (args.deepUpstream || upstream->producesSceneData()) {
        builder->addUnplacedGraph();

        return;
    }
    args.rawUpstream = args.activeInputToRender->getNode()->getEffectiveOutputDataKind() == eDataKindRaw;
    args.forceRender = false;
    args.userRoIEnabled = ui.userRoIEnabled;
    args.mustComputeRoDAndLookupCache = false;
    args.channels = getChannels(textureIndex);
    args.params = std::make_shared<UpdateViewerParams>();
    args.params->textureIndex = textureIndex;
    args.params->time = time;
    args.params->view = view;
    args.params->abortInfo = abortInfo;
    args.params->nbCachedTile = 0;
    {
        QMutexLocker locker(&_imp->viewerParamsMutex);
        args.params->layer = _imp->viewerParamsLayer;
        args.params->alphaLayer = _imp->viewerParamsAlphaLayer;
        args.params->alphaChannelName = _imp->viewerParamsAlphaChannelName;
        args.isDoingPartialUpdates = _imp->isDoingPartialUpdates;
    }
    computeMipmapLevels(ui.zoomFactor, &args);
    const unsigned int mipmapLevel = args.mipmapLevelWithDraft;
    args.params->mipmapLevel = mipmapLevel;

    ViewerParallelRenderArgsSetter frameArgs(time,
                                             view,
                                             true,
                                             false,
                                             abortInfo,
                                             getNode(),
                                             textureIndex,
                                             getTimeline().get(),
                                             false,
                                             NodePtr(),
                                             args.activeInputToRender->getNode(),
                                             args.draftModeEnabled,
                                             RenderStatsPtr());

    const EffectInstance::SupportsEnum supportsRS = args.activeInputToRender->supportsRenderScaleMaybe();
    args.activeInputHash = args.activeInputToRender->getHash();
    RectD rod;
    if (args.activeInputToRender->getRegionOfDefinition_public(args.activeInputHash,
                                                               time,
                                                               (supportsRS == eSupportsNo) ? RenderScale::identity : RenderScale::fromMipmapLevel(mipmapLevel),
                                                               view,
                                                               &rod,
                                                               0)
        == eStatusFailed) {
        builder->addUnplacedGraph();

        return;
    }
    ifInfiniteclipRectToProjectDefault(&rod);
    args.params->rod = rod;

    const NodeMetadataConstPtr metadata = args.activeInputToRender->getRenderMetadata(time, view);
    args.params->pixelAspectRatio = metadata->getPixelAspectRatio(-1);
    RectI roi;
    const bool exact = args.userRoIEnabled || args.isDoingPartialUpdates;
    if (!getGpuPlacementRoI(textureIndex, rod, args.params->pixelAspectRatio, mipmapLevel, exact, &roi) || roi.isNull()) {
        builder->addUnplacedGraph();

        return;
    }

    std::shared_ptr<FrameRequestMap> request = std::make_shared<FrameRequestMap>();
    const RectD canonicalRoi = roi.toCanonical(mipmapLevel, args.params->pixelAspectRatio, rod);
    if (EffectInstance::computeRequestPass(time, view, mipmapLevel, canonicalRoi, getNode(), *request) == eStatusFailed) {
        builder->addUnplacedGraph();

        return;
    }
    frameArgs.updateNodesRequest(*request);

    ImageLayerDesc layers, pairedLayers;
    EffectInstance::getLayersFromMetadata(*metadata, -1, &layers, &pairedLayers);
    std::list<ImageLayerDesc> requestedLayers;
    int alphaChannelIndex;
    computeViewerRequestedLayers(args, layers, &requestedLayers, &alphaChannelIndex);
    if (requestedLayers.empty()) {
        builder->addUnplacedGraph();

        return;
    }

    if (!ViewerInstance::isFrameEligibleForScheduler(computeViewerFrameEligibility(&frameArgs, request, args, false), 0)) {
        builder->addUnplacedGraph();

        return;
    }

    FrameRenderContextPtr context = FrameRenderContext::createFromSetter(frameArgs, abortInfo, RenderStatsPtr(), time, view);
    context->setRequest(request);
    FrameGraph graph = buildViewerFrameGraph(context, args, view, roi, requestedLayers, metadata->getBitDepth(-1), ui.cpuFramesOnly);
    if (graph.tasks.empty()) {
        builder->addUnplacedGraph();

        return;
    }
    addPlacedGraphToSnapshot(&graph, context, builder);
} // ViewerInstance::planGpuPlacementOfInput

void
ViewerInstance::runGpuPlacementDryRun(const AbortableRenderInfoPtr& abortInfo)
{
    GpuPlacementUiState ui;
    if (!isGpuPlacementOverlayEnabled() || !getNode()->isActivated() || _imp->hasDryRunGpuPlacement() || !readGpuPlacementUiState(&ui)) {
        return;
    }
    if (GpuContext* gpu = appPTR->getGpuContext()) {
        // A loss is otherwise only recovered from by the next render, and until then nothing would be planned.
        gpu->recoverIfLost();
    }
    const ViewerInstancePrivate::GpuPlacementStamp stamp = _imp->currentGpuPlacementStamp();
    GpuPlacementSnapshotBuilder builder;
    if ((appPTR->getRenderSchedulerMode() != eRenderSchedulerModeTaskGraph) || !isGpuPlacementAvailable()) {
        builder.addUnplacedGraph();
    } else {
        const SequenceTime time = (SequenceTime)getTimeline()->currentFrame();
        for (int i = 0; i < ui.inputCount; ++i) {
            planGpuPlacementOfInput(i, time, ui, abortInfo, &builder);
        }
    }
    if (!abortInfo->isAborted() && _imp->publishGpuPlacement(builder, stamp, true)) {
        Q_EMIT gpuPlacementSnapshotChanged();
    }
    gGpuPlacementDryRunCount.fetch_add(1);
}

ViewerInstance::ViewerRenderRetCode
ViewerInstance::getViewerArgsAndRenderViewer(SequenceTime time,
                                             bool canAbort,
                                             ViewIdx view,
                                             U64 viewerHash,
                                             const NodePtr& rotoPaintNode,
                                             const RotoStrokeItemPtr& activeStroke,
                                             const RenderStatsPtr& stats,
                                             ViewerArgsPtr* argsA,
                                             ViewerArgsPtr* argsB)
{
    ///This is used only by the rotopaint while drawing. We must clear the action cache of the rotopaint node before calling
    ///getRoD or this will not work
    assert(rotoPaintNode);
    if ( !rotoPaintNode->getEffectInstance() ) {
        return eViewerRenderRetCodeFail;
    }
    rotoPaintNode->getEffectInstance()->clearActionsCache();

    ViewerRenderRetCode status[2] = {
        eViewerRenderRetCodeFail, eViewerRenderRetCodeFail
    };
    NodePtr thisNode = getNode();
    ViewerArgsPtr args[2];
    std::optional<GpuPlacementSnapshotBuilder> placementBuilder;
    ViewerInstancePrivate::GpuPlacementStamp placementStamp;
    if (isGpuPlacementOverlayEnabled()) {
        placementBuilder.emplace();
        placementStamp = _imp->currentGpuPlacementStamp();
    }
    for (int i = 0; i < 2; ++i) {
        args[i] = std::make_shared<ViewerArgs>();
        if ( (i == 1) && (_imp->uiContext->getCompositingOperator() == eViewerCompositingOperatorNone) ) {
            break;
        }

        // A paused viewer keeps its last frame and resolves no input to render from. The stroke's points stay
        // unconsumed, so the first render after un-pausing picks them all up.
        if (isViewerPaused(i)) {
            status[i] = eViewerRenderRetCodeRedraw;
            args[i].reset();
            continue;
        }

        AbortableRenderInfoPtr abortInfo = _imp->createNewRenderRequest(i, canAbort, false);

        /*FrameRequestMap request;
           RectI roi;
           {
            roi.x1 = args[i]->params->textureRect.x1;
            roi.y1 = args[i]->params->textureRect.y1;
            roi.x2 = args[i]->params->textureRect.x2;
            roi.y2 = args[i]->params->textureRect.y2;
           }
           status[i] = EffectInstance::computeRequestPass(time, view, args[i]->params->mipmapLevel, roi, thisNode, request);
           if (status[i] == eStatusFailed) {
            continue;
           }*/
        ViewerParallelRenderArgsSetter tls(time,
                                           view,
                                           true,
                                           false,
                                           abortInfo,
                                           thisNode,
                                           i,
                                           getTimeline().get(),
                                           false,
                                           rotoPaintNode,
                                           NodePtr(),
                                           false,
                                           stats);
        NodesList rotoPaintNodes;
        if (rotoPaintNode) {
            if (activeStroke) {
                EffectInstancePtr rotoLive = rotoPaintNode->getEffectInstance();
                assert(rotoLive);
                bool ok = rotoLive->getThreadLocalRotoPaintTreeNodes(&rotoPaintNodes);
                assert(ok);
                if (!ok) {
                    throw std::logic_error("ViewerParallelRenderArgsSetter(): getThreadLocalRotoPaintTreeNodes() failed");
                }

                /*
                   Take from the stroke all the points that were input by the user so far on the main thread and set them globally to the
                   application. These data are the ones that are going to be used by any Roto related tool. We ensure that they all access
                   the same data so we only access the real Roto datas now.
                 */

                //The last points input by the user
                std::list<std::pair<Point, double> > lastStrokePoints;

                //The stroke RoD so far
                RectD wholeStrokeRod;

                //The bbox of the lastStrokePoints
                RectD lastStrokeBbox;

                //The index in the stroke of the last point we have rendered and up to the new point we have rendered
                int lastAge, newAge;

                //get on the app object the last point index we have rendered on this stroke
                lastAge = getApp()->getStrokeLastIndex();

                //Get the active paint stroke so far and its multi-stroke index
                RotoStrokeItemPtr currentlyPaintedStroke;
                int currentlyPaintedStrokeMultiIndex;
                getApp()->getStrokeAndMultiStrokeIndex(&currentlyPaintedStroke, &currentlyPaintedStrokeMultiIndex);

                // A render queued for a stroke the user has since replaced has nothing left to draw.
                if (currentlyPaintedStroke != activeStroke) {
                    args[i].reset();

                    return eViewerRenderRetCodeRedraw;
                }

                //the multi-stroke index in case of a stroke containing multiple strokes from the user
                int strokeIndex;
                if ( activeStroke->getMostRecentStrokeChangesSinceAge(time, lastAge, currentlyPaintedStrokeMultiIndex, &lastStrokePoints, &lastStrokeBbox, &wholeStrokeRod, &newAge, &strokeIndex) ) {
                    getApp()->updateLastPaintStrokeData(newAge, lastStrokePoints, lastStrokeBbox, strokeIndex);
                    for (NodesList::iterator it = rotoPaintNodes.begin(); it != rotoPaintNodes.end(); ++it) {
                        (*it)->prepareForNextPaintStrokeRender();
                    }
                    updateLastStrokeDataRecursively(thisNode.get(), rotoPaintNode, lastStrokeBbox, false);
                } else {
                    ///The drawing is already up to date: all changes have been taken into account for this event
                    args[i].reset();

                    return eViewerRenderRetCodeRedraw;
                }
            }
        }


        if (args[i]) {
            status[i] = getRenderViewerArgsAndCheckCache( time, false, view, i, viewerHash, rotoPaintNode, abortInfo, stats,  args[i].get() );
        }


        if (status[i] != eViewerRenderRetCodeRender) {
            /*
               Either failure, black or nothing, the texture is junk, remove it from the cache
             */
            if (args[i] && args[i]->params) {
                for (std::list<UpdateViewerParams::CachedTile>::iterator it = args[i]->params->tiles.begin(); it != args[i]->params->tiles.end(); ++it) {
                    if (it->cachedData) {
                        //it->cachedData->setAborted(true);
                        //appPTR->removeFromViewerCache(it->cachedData);
                        it->cachedData.reset();
                    }
                }
                args[i]->params->tiles.clear();
            }
        }


        if ( (status[i] == eViewerRenderRetCodeFail) || (status[i] == eViewerRenderRetCodeBlack) ) {
            disconnectTextureRequest(i, status[i] == eViewerRenderRetCodeFail);
        } else {
            assert(args[i] && args[i]->params);
            assert(args[i]->params->textureIndex == i);


            if ( !_imp->addOngoingRender(args[i]->params->textureIndex, abortInfo) ) {
                /*
                   This may fail if another thread already pushed a more recent render in the render ages queue
                 */
                status[i] = eViewerRenderRetCodeRedraw;
                args[i].reset();
            }

            // Redraw arguments resolve no input: there is nothing to render.
            if (args[i] && (status[i] == eViewerRenderRetCodeRender)) {
                status[i] = renderViewer_internal(view,
                                                  QThread::currentThread() == qApp->thread(), // singleThreaded
                                                  false, // isSequentialRender
                                                  viewerHash,
                                                  canAbort,
                                                  rotoPaintNode,
                                                  false, // useTLS
                                                  ViewerCurrentFrameRequestSchedulerStartArgsPtr(),
                                                  stats,
                                                  placementBuilder ? &*placementBuilder : nullptr,
                                                  *args[i]);
                args[i]->isRenderingFlag.reset();
            }

            if (args[i] && args[i]->params) {
                if ( (status[i] == eViewerRenderRetCodeFail) || (status[i] == eViewerRenderRetCodeBlack) ) {
                    _imp->checkAndUpdateDisplayAge( args[i]->params->textureIndex, abortInfo->getRenderAge() );
                }
                _imp->removeOngoingRender( args[i]->params->textureIndex, abortInfo->getRenderAge() );
            }


            if (status[i] == eViewerRenderRetCodeRedraw) {
                args[i].reset();
            }
        }
    }

    if (placementBuilder && _imp->publishGpuPlacement(*placementBuilder, placementStamp, false)) {
        Q_EMIT gpuPlacementSnapshotChanged();
    }

    if ( (status[0] == eViewerRenderRetCodeFail) && (status[1] == eViewerRenderRetCodeFail) ) {
        return eViewerRenderRetCodeFail;
    }

    *argsA = args[0];
    *argsB = args[1];

    return eViewerRenderRetCodeRender;
} // ViewerInstance::getViewerArgsAndRenderViewer

ViewerInstance::ViewerRenderRetCode
ViewerInstance::renderViewer(ViewIdx view,
                             bool singleThreaded,
                             bool isSequentialRender,
                             U64 viewerHash,
                             bool canAbort,
                             const NodePtr& rotoPaintNode,
                             bool useTLS,
                             ViewerArgsPtr args[2],
                             const ViewerCurrentFrameRequestSchedulerStartArgsPtr& request,
                             const RenderStatsPtr& stats)
{
    if (!_imp->uiContext) {
        return eViewerRenderRetCodeFail;
    }

    /**
     * When entering this code, we already know that the textures are not cached for the A and B inputs
     * If the an input is not connected, it's args[i] will be NULL.
     * If either one of the renders fails, that means we should block playback and clear to black the viewer
     **/

    ViewerInstance::ViewerRenderRetCode ret[2] = {
        eViewerRenderRetCodeRedraw, eViewerRenderRetCodeRedraw
    };
    std::optional<GpuPlacementSnapshotBuilder> placementBuilder;
    ViewerInstancePrivate::GpuPlacementStamp placementStamp;
    if (isGpuPlacementOverlayEnabled() && !isSequentialRender) {
        placementBuilder.emplace();
        placementStamp = _imp->currentGpuPlacementStamp();
    }
    for (int i = 0; i < 2; ++i) {
        if (args[i] && args[i]->params) {
            if ( (i == 1) && (_imp->uiContext->getCompositingOperator() == eViewerCompositingOperatorNone) ) {
                args[i]->params->tiles.clear();
                break;
            }
            assert(args[i]->params->textureIndex == i);

            ///We enable render stats just for the A input (i == 0) otherwise we would get crappy results

            if (!isSequentialRender) {
                if ( !_imp->addOngoingRender(args[i]->params->textureIndex, args[i]->params->abortInfo) ) {
                    /*
                       This may fail if another thread already pushed a more recent render in the render ages queue
                     */
                    ret[i] = eViewerRenderRetCodeRedraw;
                    args[i].reset();
                }
            }
            if (args[i]) {
                for (const EffectInstancePtr& effect : args[i]->staleOverlayImageEffects) {
                    // The A and B inputs can share a node: the first of them may have refreshed it.
                    if (effect->overlayImagesStale(args[i]->overlayImageRequest)) {
                        effect->renderOverlayImages(args[i]->overlayImageRequest, args[i]->params->abortInfo);
                    }
                }
                ret[i] = renderViewer_internal(view, singleThreaded, isSequentialRender, viewerHash, canAbort, rotoPaintNode, useTLS, request,
                                               i == 0 ? stats : RenderStatsPtr(),
                                               placementBuilder ? &*placementBuilder : nullptr,
                                               *args[i]);

                // Reset the rednering flag
                args[i]->isRenderingFlag.reset();
            }

            if (ret[i] != eViewerRenderRetCodeRender) {
                /*
                   Either failure, black or nothing, the texture is junk, remove it from the cache
                 */
                if (args[i] && args[i]->params) {
                    for (std::list<UpdateViewerParams::CachedTile>::iterator it = args[i]->params->tiles.begin(); it != args[i]->params->tiles.end(); ++it) {
                        if (it->cachedData) {
                            //it->cachedData->setAborted(true);
                            //appPTR->removeFromViewerCache(it->cachedData);
                            it->cachedData.reset();
                        }
                    }
                    args[i]->params->tiles.clear();
                }
            }

            if (!isSequentialRender && args[i] && args[i]->params) {
                if ( (ret[i] == eViewerRenderRetCodeFail) || (ret[i] == eViewerRenderRetCodeBlack) ) {
                    _imp->checkAndUpdateDisplayAge( args[i]->params->textureIndex, args[i]->params->abortInfo->getRenderAge() );
                }
                _imp->removeOngoingRender( args[i]->params->textureIndex, args[i]->params->abortInfo->getRenderAge() );
            }

            if (ret[i] == eViewerRenderRetCodeBlack) {
                if (isSequentialRender) {
                    disconnectTexture(args[i]->params->textureIndex, false);
                } else {
                    disconnectTextureAfterRender(args[i]->params->textureIndex, false, args[i]->params->abortInfo->getRenderAge());
                }
            }

            if (ret[i] == eViewerRenderRetCodeFail) {
                args[i].reset();
            }
        }
    }

    if (placementBuilder && _imp->publishGpuPlacement(*placementBuilder, placementStamp, false)) {
        Q_EMIT gpuPlacementSnapshotChanged();
    }

    if ( (ret[0] == eViewerRenderRetCodeFail) || (ret[1] == eViewerRenderRetCodeFail) ) {
        return eViewerRenderRetCodeFail;
    }

    return eViewerRenderRetCodeRender;
} // ViewerInstance::renderViewer

static bool
checkTreeCanRender_internal(Node* node,
                            std::list<Node*>& marked)
{
    if ( std::find(marked.begin(), marked.end(), node) != marked.end() ) {
        return true;
    }

    marked.push_back(node);

    // check that the nodes upstream have all their nonoptional inputs connected
    int maxInput = node->getNInputs();
    for (int i = 0; i < maxInput; ++i) {
        NodePtr input = node->getInput(i);
        bool optional = node->getEffectInstance()->isInputOptional(i);
        if (optional) {
            continue;
        }
        if (!input) {
            return false;
        } else {
            bool ret = checkTreeCanRender_internal(input.get(), marked);
            if (!ret) {
                return false;
            }
        }
    }

    return true;
}

/**
 * @brief Returns false if the tree has unconnected mandatory inputs
 **/
static bool
checkTreeCanRender(Node* node)
{
    std::list<Node*> marked;
    bool ret = checkTreeCanRender_internal(node, marked);

    return ret;
}

static unsigned char*
getTexPixel(int x,
            int y,
            const TextureRect& bounds,
            std::size_t pixelDepth,
            unsigned char* bufStart)
{
    if ( ( x < bounds.x1 ) || ( x >= bounds.x2 ) || ( y < bounds.y1 ) || ( y >= bounds.y2 ) ) {
        return NULL;
    } else {
        int compDataSize = pixelDepth * 4;

        return (unsigned char*)(bufStart)
               + (qint64)( y - bounds.y1 ) * compDataSize * bounds.width()
               + (qint64)( x - bounds.x1 ) * compDataSize;
    }
}

static bool
copyAndSwap(const TextureRect& srcRect,
            const TextureRect& dstRect,
            std::size_t dstBytesCount,
            ImageBitDepthEnum bitdepth,
            unsigned char* srcBuf,
            unsigned char** dstBuf)
{
    // Ensure it has the correct size, resize it if needed
    if ( (srcRect.x1 == dstRect.x1) &&
         ( srcRect.y1 == dstRect.y1) &&
         ( srcRect.x2 == dstRect.x2) &&
         ( srcRect.y2 == dstRect.y2) ) {
        *dstBuf = srcBuf;

        return false;
    }

    // Use calloc so that newly allocated areas are already black and transparent
    unsigned char* tmpBuf = (unsigned char*)calloc(dstBytesCount, 1);

    if (!tmpBuf) {
        *dstBuf = 0;

        return true;
    }

    std::size_t pixelDepth = getSizeOfForBitDepth(bitdepth);
    unsigned char* dstPixels = getTexPixel(srcRect.x1, srcRect.y1, dstRect, pixelDepth, tmpBuf);
    assert(dstPixels);
    const unsigned char* srcPixels = getTexPixel(srcRect.x1, srcRect.y1, srcRect, pixelDepth, srcBuf);
    assert(srcPixels);

    std::size_t srcRowSize = srcRect.width() * 4 * pixelDepth;
    std::size_t dstRowSize = dstRect.width() * 4 * pixelDepth;

    for (int y = srcRect.y1; y < srcRect.y2;
         ++y, srcPixels += srcRowSize, dstPixels += dstRowSize) {
        std::memcpy(dstPixels, srcPixels, srcRowSize);
    }
    *dstBuf = tmpBuf;

    return true;
}

ViewerInstance::ViewerRenderRetCode
ViewerInstance::getRenderViewerArgsAndCheckCache_public(SequenceTime time,
                                                        bool isSequential,
                                                        ViewIdx view,
                                                        int textureIndex,
                                                        U64 viewerHash,
                                                        bool canAbort,
                                                        const NodePtr& rotoPaintNode,
                                                        const RenderStatsPtr& stats,
                                                        ViewerArgs* outArgs)
{
    // Held until playback stops, then planned on the frame it stopped at. Here rather than in renderViewer(), which a
    // playback of cached frames never reaches.
    if (isSequential) {
        requestGpuPlacementDryRun();
    }

    AbortableRenderInfoPtr abortInfo = _imp->createNewRenderRequest(textureIndex, canAbort, isSequential);
    ViewerRenderRetCode stat = getRenderViewerArgsAndCheckCache(time, isSequential, view, textureIndex, viewerHash, rotoPaintNode, abortInfo, stats, outArgs);

    if ( (stat == eViewerRenderRetCodeFail) || (stat == eViewerRenderRetCodeBlack) ) {
        _imp->checkAndUpdateDisplayAge( textureIndex, abortInfo->getRenderAge() );
    }

    return stat;
}

void
ViewerInstance::setupMinimalUpdateViewerParams(const SequenceTime time,
                                               const ViewIdx view,
                                               const int textureIndex,
                                               const AbortableRenderInfoPtr& abortInfo,
                                               const bool isSequential,
                                               ViewerArgs* outArgs)
{
    assert(_imp->uiContext);

    computeMipmapLevels(_imp->uiContext->getZoomFactor(), outArgs);

    // The hash of the node to render, we store it and make sure we never call getHash() again for the render of this frame.
    // It is the hash the metadata read here belong to, so the PAR placing the tiles looked up
    // under it describes that same state.
    U64 activeInputHash = 0;
    const NodeMetadataConstPtr activeInputMetadata = outArgs->activeInputToRender->getMetadata(time, view, &activeInputHash);
    outArgs->activeInputHash = activeInputHash ? activeInputHash : outArgs->activeInputToRender->getHash();

    // Initialize the flag
    outArgs->mustComputeRoDAndLookupCache = true;

    // Check if the render was issued from the "Refresh" button, in which case we compute images from nodes at least once
    {
        QMutexLocker forceRenderLocker(&_imp->forceRenderMutex);
        outArgs->forceRender = _imp->forceRender[textureIndex];
        _imp->forceRender[textureIndex] = false;
    }

    // Did the user enabled the user roi from the viewer UI?
    outArgs->userRoIEnabled = _imp->uiContext->isUserRegionOfInterestEnabled();

    outArgs->params = std::make_shared<UpdateViewerParams>();

    // The PAR of the input image
    outArgs->params->pixelAspectRatio = activeInputMetadata->getPixelAspectRatio(-1);

    // Is it playback ?
    outArgs->params->isSequential = isSequential;

    // Used to identify this render when calling EffectInstance::Implementation::aborted
    outArgs->params->abortInfo = abortInfo;

    // Used to differentiate the 2 different textures when wipe is enabled
    outArgs->params->setUniqueID(textureIndex);

    // The user requested bitdepth of the textures
    outArgs->params->depth = _imp->uiContext->getBitDepth();

    // The frame number
    outArgs->params->time = time;

    // The view to render
    outArgs->params->view = view;

    // A input = 0, B input = 1
    outArgs->params->textureIndex = textureIndex;

    // These are the user settings from the viewer UI
    outArgs->channels = getChannels(textureIndex);
    outArgs->params->gain = getGain();
    outArgs->params->gamma = getGamma();
    {
        QMutexLocker locker(&_imp->viewerParamsMutex);
        outArgs->params->layer = _imp->viewerParamsLayer;
        outArgs->params->alphaLayer = _imp->viewerParamsAlphaLayer;
        outArgs->params->alphaChannelName = _imp->viewerParamsAlphaChannelName;
        outArgs->isDoingPartialUpdates = _imp->isDoingPartialUpdates;
    }

    outArgs->params->displayProcessor = getDisplayProcessor();

    // Flag that we are going to render
    outArgs->isRenderingFlag = std::make_shared<RenderingFlagSetter>( getNode() );
} // ViewerInstance::setupMinimalUpdateViewerParams

void
ViewerInstance::computeMipmapLevels(double zoomFactor,
                                    ViewerArgs* outArgs) const
{
    outArgs->mipmapLevelWithoutDraft = getMipmapLevelWithoutDraft();
    outArgs->mipmapLevelWithDraft = outArgs->mipmapLevelWithoutDraft;

    outArgs->draftModeEnabled = getApp()->isDraftRenderEnabled();

    // If draft mode is enabled, compute the mipmap level according to the auto-proxy setting in the preferences
    if (outArgs->draftModeEnabled && appPTR->getCurrentSettings()->isAutoProxyEnabled()) {
        unsigned int autoProxyLevel = appPTR->getCurrentSettings()->getAutoProxyMipmapLevel();
        if (zoomFactor > 1) {
            // Decrease draft mode at each inverse mipmaplevel level taken
            unsigned int invLevel = Image::getLevelFromScale(1. / zoomFactor);
            if (invLevel < autoProxyLevel) {
                autoProxyLevel -= invLevel;
            } else {
                autoProxyLevel = 0;
            }
        }
        outArgs->mipmapLevelWithDraft = std::max(outArgs->mipmapLevelWithoutDraft, autoProxyLevel);
    }
}

void
ViewerInstance::getRegionsOfInterest(double /*time*/,
                                     const RenderScale & /*scale*/,
                                     const RectD & /*outputRoD*/,   //!< the RoD of the effect, in canonical coordinates
                                     const RectD & renderWindow,   //!< the region to be rendered in the output image, in Canonical Coordinates
                                     ViewIdx /*view*/,
                                     RoIMap* ret)
{
//#pragma message WARN("2.2: fix this and only add RoI for thread local input")
    for (int i = 0; i < getNInputs(); ++i) {
        EffectInstancePtr input = getInput(i);
        if (input) {
            ret->insert( std::make_pair(input, renderWindow) );
        }
    }
}

ViewerInstance::ViewerRenderRetCode
ViewerInstance::getViewerRoIAndTexture(const RectD& rod,
                                       const U64 viewerHash,
                                       const bool useCache,
                                       const bool isDraftMode,
                                       const unsigned int mipmapLevel,
                                       const RenderStatsPtr& stats,
                                       ViewerArgs* outArgs)
{
    // Roi is the coordinates of the 4 corners of the texture in the bounds with the current zoom
    // factor taken into account.

    // When user RoI is enabled, we compute exactly the image portion displayed in the rectangle and
    // do not round it to Viewer tiles.

    outArgs->params->tiles.clear();
    outArgs->params->nbCachedTile = 0;
    if (!useCache || outArgs->forceRender) {
        outArgs->params->roi = _imp->uiContext->getExactImageRectangleDisplayed(outArgs->params->textureIndex, rod, outArgs->params->pixelAspectRatio, mipmapLevel);
        outArgs->params->roiNotRoundedToTileSize = outArgs->params->roi;

        UpdateViewerParams::CachedTile tile;
        if (outArgs->isDoingPartialUpdates) {
            std::list<RectD> partialRects;
            QMutexLocker k(&_imp->viewerParamsMutex);
            partialRects = _imp->partialUpdateRects;
            for (std::list<RectD>::iterator it = partialRects.begin(); it != partialRects.end(); ++it) {
                RectI pixelRect = it->toPixelEnclosing(mipmapLevel, outArgs->params->pixelAspectRatio);
                ///Intersect to the RoI
                pixelRect.clip(outArgs->params->roi);
                if ( !pixelRect.isNull() ) {
                    tile.rect.set(pixelRect);
                    tile.rectRounded  = pixelRect;
                    tile.rect.closestPo2 = 1 << mipmapLevel;
                    tile.rect.par = outArgs->params->pixelAspectRatio;
                    tile.bytesCount = tile.rect.area() * 4;
                    assert(tile.bytesCount > 0);
                    if (outArgs->params->depth == eImageBitDepthFloat) {
                        tile.bytesCount *= sizeof(float);
                    }
                    outArgs->params->tiles.push_back(tile);
                }
            }
        } else {
            if (!outArgs->params->roi.isNull()) {
                tile.rect.set(outArgs->params->roi);
                tile.rectRounded = outArgs->params->roi;
                tile.rect.closestPo2 = 1 << mipmapLevel;
                tile.rect.par = outArgs->params->pixelAspectRatio;
                tile.bytesCount = tile.rect.area() * 4;
                assert(tile.bytesCount > 0);
                if (outArgs->params->depth == eImageBitDepthFloat) {
                    tile.bytesCount *= sizeof(float);
                }
                outArgs->params->tiles.push_back(tile);
            }
        }

    } else {
        std::vector<RectI> tiles, tilesRounded;
        outArgs->params->roi = _imp->uiContext->getImageRectangleDisplayedRoundedToTileSize(outArgs->params->textureIndex, rod, outArgs->params->pixelAspectRatio, mipmapLevel, &tiles, &tilesRounded, &outArgs->params->tileSize, &outArgs->params->roiNotRoundedToTileSize);

        assert(tiles.size() == tilesRounded.size());

        std::vector<RectI>::iterator itRounded = tilesRounded.begin();
        for (std::vector<RectI>::iterator it = tiles.begin(); it != tiles.end(); ++it, ++itRounded) {
            UpdateViewerParams::CachedTile tile;
            tile.rectRounded = *itRounded;
            tile.rect.set(*it);
            tile.rect.closestPo2 = 1 << mipmapLevel;
            tile.rect.par = outArgs->params->pixelAspectRatio;
            tile.bytesCount = outArgs->params->tileSize * outArgs->params->tileSize * 4; // RGBA
            assert( outArgs->params->roi.contains(tile.rect) );
            // If we are using floating point textures, multiply by size of float
            assert(tile.bytesCount > 0);
            if (outArgs->params->depth == eImageBitDepthFloat) {
                tile.bytesCount *= sizeof(float);
            }
            outArgs->params->tiles.push_back(tile);
        }
    }

    // If the RoI does not fall into the visible portion on the viewer, just clear the viewer to black
    if ( outArgs->params->roi.isNull() || (outArgs->params->tiles.size() == 0) ) {
        return eViewerRenderRetCodeBlack;
    }


    outArgs->params->rod = rod;
    outArgs->params->mipmapLevel = mipmapLevel;

    std::string inputToRenderName = outArgs->activeInputToRender->getNode()->getScriptName_mt_safe();


    // Texture rect contains the pixel coordinates in the image to be rendered

    if (useCache) {
        FrameEntryLocker entryLocker(_imp.get());
        const bool recordLookup = !isDraftMode && (outArgs->params->textureIndex == 0);
        ViewerFrameFilter lookup;
        for (std::list<UpdateViewerParams::CachedTile>::iterator it = outArgs->params->tiles.begin(); it != outArgs->params->tiles.end(); ++it) {
            FrameKey key(getNode().get(),
                         outArgs->params->time,
                         viewerHash,
                         outArgs->params->gain,
                         outArgs->params->gamma,
                         outArgs->params->displayProcessor ? outArgs->params->displayProcessor->cacheHash : 0,
                         (int)outArgs->params->depth,
                         outArgs->channels,
                         outArgs->params->view,
                         it->rect,
                         mipmapLevel,
                         inputToRenderName,
                         outArgs->params->layer,
                         outArgs->params->alphaLayer.getLayerID() + outArgs->params->alphaChannelName,
                         outArgs->params->depth == eImageBitDepthFloat,
                         isDraftMode);
            if (recordLookup) {
                lookup.settings = key;
                lookup.tiles.push_back(it->rect);
            }
            std::list<FrameEntryPtr> entries;
            bool hasTextureCached = appPTR->getTexture(key, &entries);
            if ( stats  && stats->isInDepthProfilingEnabled() ) {
                if (hasTextureCached) {
                    stats->addCacheInfosForNode(getNode(), true, false);
                } else {
                    stats->addCacheInfosForNode(getNode(), false, false);
                }
            }

            // If we want to force a refresh, we remove from  the cache texture
            if (outArgs->forceRender && hasTextureCached) {
                for (std::list<FrameEntryPtr>::iterator it = entries.begin(); it != entries.end(); ++it) {
                    appPTR->removeFromViewerCache(*it);
                }
            }
            // Find out if we have a corresponding tile in the cache
            FrameEntryPtr foundCachedEntry;
            for (std::list<FrameEntryPtr>::iterator it2 = entries.begin(); it2 != entries.end(); ++it2) {
                const TextureRect& entryRect = (*it2)->getKey().getTexRect();
                if (entryRect == it->rect) {
                    foundCachedEntry = *it2;
                    break;
                }
            }


            if (foundCachedEntry) {

                // If the tile is cached and we got it that means rendering is done
                entryLocker.lock(foundCachedEntry);

                /*if ( !entryLocker.tryLock(foundCachedEntry) ) {
                    // Another thread is rendering it, just return it is not useful to keep this thread waiting.
                    return eViewerRenderRetCodeRedraw;
                }*/

                /*if ( foundCachedEntry->getAborted() ) {
                    // The thread rendering the frame entry might have been aborted and the entry removed from the cache
                    // but another thread might successfully have found it in the cache. This flag is to notify it the frame

                    // is invalid.
                    return eViewerRenderRetCodeRedraw;
                }*/


                // The data will be valid as long as the cachedFrame shared pointer use_count is gt 1
                it->cachedData = foundCachedEntry;
                it->isCached = true;
                it->ramBuffer = foundCachedEntry->data();
                assert(it->ramBuffer);
                ++outArgs->params->nbCachedTile;
            }
        }
        if (recordLookup && !lookup.tiles.empty()) {
            QMutexLocker k(&_imp->lastLookupMutex);
            _imp->lastLookup = lookup;
            _imp->hasLastLookup = true;
        }
    }


    return eViewerRenderRetCodeRender;
} // ViewerInstance::getViewerRoIAndTexture

bool
ViewerInstance::getLastFullQualityLookup(ViewerFrameFilter* filter) const
{
    QMutexLocker k(&_imp->lastLookupMutex);
    if (!_imp->hasLastLookup) {
        return false;
    }
    *filter = _imp->lastLookup;

    return true;
}

ViewerInstance::ViewerRenderRetCode
ViewerInstance::getRoDAndLookupCache(const bool useOnlyRoDCache,
                                     const U64 viewerHash,
                                     const NodePtr& rotoPaintNode,
                                     const RenderStatsPtr& stats,
                                     ViewerArgs* outArgs)
{
    // We never use the texture cache when the user RoI is enabled or while painting, otherwise we would have
    // zillions of textures in the cache, each a few pixels different.
    // A texture found in the cache would end the job here, before its overlay images get a pass.
    const bool useTextureCache = !outArgs->userRoIEnabled && !rotoPaintNode.get() && !outArgs->isDoingPartialUpdates && outArgs->staleOverlayImageEffects.empty();

    // If it's eSupportsMaybe and mipmapLevel!=0, don't forget to update
    // this after the first call to getRegionOfDefinition().
    // This may be eSupportsMaybe
    EffectInstance::SupportsEnum supportsRS = outArgs->activeInputToRender->supportsRenderScaleMaybe();


    // When in draft mode first try to get a texture without draft and then try with draft
    const int nLookups = outArgs->draftModeEnabled ? 2 : 1;

    for (int lookup = 0; lookup < nLookups; ++lookup) {
        const unsigned mipmapLevel = lookup == 0 ? outArgs->mipmapLevelWithoutDraft : outArgs->mipmapLevelWithDraft;
        RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);


        RectD rod;
        // Get the RoD here to be able to figure out what is the RoI of the Viewer.
        // Note that we are in the main-thread (OpenGL) thread here to optimize the code path when all textures are cached
        // so we cannot afford computing the actual RoD. If it's not cached then we will actually compute the RoD in the render thread.
        StatusEnum stat;

        if (useOnlyRoDCache) {
            stat = outArgs->activeInputToRender->getRegionOfDefinitionFromCache(outArgs->activeInputHash,
                                                                                outArgs->params->time,
                                                                                scale,
                                                                                outArgs->params->view,
                                                                                &rod,
                                                                                0 /*isProjectFormat*/);
            if (stat == eStatusFailed) {
                // If was not cached, we cannot lookup the cache in the main-thread at this mipmapLevel
                continue;
            }
        } else {
            stat = outArgs->activeInputToRender->getRegionOfDefinition_public(outArgs->activeInputHash,
                                                                              outArgs->params->time,
                                                                              supportsRS ==  eSupportsNo ? RenderScale::identity : scale,
                                                                              outArgs->params->view,
                                                                              &rod,
                                                                              0 /*isProjectFormat*/);
            if (stat == eStatusFailed) {
                // It really failed, just exit
                return eViewerRenderRetCodeFail;
            }
        }

        // update scale after the first call to getRegionOfDefinition
        if ( (supportsRS == eSupportsMaybe) && (mipmapLevel != 0) ) {
            supportsRS = (outArgs->activeInputToRender)->supportsRenderScaleMaybe();
        }

        outArgs->mustComputeRoDAndLookupCache = false;


        bool isRodProjectFormat = ifInfiniteclipRectToProjectDefault(&rod);
        Q_UNUSED(isRodProjectFormat);

        // Ok we go the RoD, we can actually compute the RoI and look-up the cache
        ViewerRenderRetCode retCode = getViewerRoIAndTexture(rod, viewerHash, useTextureCache, lookup == 1, mipmapLevel, stats, outArgs);
        if (retCode != eViewerRenderRetCodeRender) {
            return retCode;
        }

        assert(outArgs->params->tiles.size() > 0);


        // We found something in the cache, stop now
        if (outArgs->params->nbCachedTile > 0) {
            break;
        }
    } // for (int lookup = 0; lookup < nLookups; ++lookup)

    return eViewerRenderRetCodeRender;
} // ViewerInstance::getRoDAndLookupCache

ViewerInstance::ViewerRenderRetCode
ViewerInstance::getRenderViewerArgsAndCheckCache(SequenceTime time,
                                                 bool isSequential,
                                                 ViewIdx view,
                                                 int textureIndex,
                                                 U64 viewerHash,
                                                 const NodePtr& rotoPaintNode,
                                                 const AbortableRenderInfoPtr& abortInfo,
                                                 const RenderStatsPtr& stats,
                                                 ViewerArgs* outArgs)
{
    // Just redraw if the viewer is paused
    if ( isViewerPaused(textureIndex) ) {
        outArgs->params = std::make_shared<UpdateViewerParams>();
        outArgs->params->isViewerPaused = true;
        outArgs->params->time = time;
        outArgs->params->setUniqueID(textureIndex);
        outArgs->params->textureIndex = textureIndex;
        outArgs->params->view = view;

        return eViewerRenderRetCodeRedraw;
    }
    // The viewer's textures are keyed by its plain hash: a render tree's root has no context.
    outArgs->cleanHash = std::make_shared<const CleanHashSnapshot>(getNode(), RenderContextPtr());

    int activeA, activeB;
    getActiveInputs(activeA, activeB);
    // Fetch the viewer indexes that we should render from the A or B input depending on the textureIndex parameter
    if (textureIndex == 0) {
        outArgs->activeInputIndex =  activeA;
    } else {
        if (_imp->uiContext->getCompositingOperator() == eViewerCompositingOperatorNone) {
            outArgs->activeInputIndex = -1;
        } else {
            outArgs->activeInputIndex =  activeB;
        }
    }


    // The active input providing the image is the first upstream non disabled node
    EffectInstancePtr upstreamInput = getInput(outArgs->activeInputIndex);
    outArgs->activeInputToRender.reset();
    if (upstreamInput) {
        outArgs->activeInputToRender = upstreamInput->getNearestNonDisabled(time);
    }
    // Detection only: this runs on the main thread, where the resolved kind is a memoized read
    // and no render may be started.
    outArgs->deepUpstream = outArgs->activeInputToRender && outArgs->activeInputToRender->producesDeepData();
    outArgs->rawUpstream = outArgs->activeInputToRender && (outArgs->activeInputToRender->getNode()->getEffectiveOutputDataKind() == eDataKindRaw);
    {
        std::string rawLabel;
        if (outArgs->rawUpstream) {
            NativeEffectBase* native = dynamic_cast<NativeEffectBase*>(outArgs->activeInputToRender.get());
            const ImageMetadata metadata = native ? native->getOutputMetadata(time, view) : ImageMetadata();
            outArgs->rawNormalisation = RawDisplay::normalisationOf(metadata);
            rawLabel = RawDisplay::label(RawDisplay::modeOf(metadata, EffectInstance::getNCompsFromMetadata(*outArgs->activeInputToRender->getMetadata(time, view), -1)));
        }
        std::lock_guard<std::mutex> lock(_rawDisplayLabelMutex);
        if ((textureIndex == 0) || (textureIndex == 1)) {
            _rawDisplayLabel[textureIndex] = rawLabel;
        }
    }
    // The scene action resolves disabled nodes itself, so a scene is evaluated from the input
    // as connected rather than from the nearest non-disabled node.
    outArgs->sceneUpstream = upstreamInput && upstreamInput->producesSceneData();
    if (outArgs->sceneUpstream) {
        outArgs->activeInputToRender = upstreamInput;
    }

    // Before rendering we check that all mandatory inputs in the graph are connected else we fail
    if ( !outArgs->activeInputToRender || !checkTreeCanRender( outArgs->activeInputToRender->getNode().get() ) ) {
        return eViewerRenderRetCodeFail;
    }

    // Fetch the render parameters from the Viewer UI
    setupMinimalUpdateViewerParams(time, view, textureIndex, abortInfo, isSequential, outArgs);

    // Playback gets no overlay images: they serve a node being edited, and a pass per played frame
    // would render every such node twice.
    outArgs->staleOverlayImageEffects.clear();
    outArgs->overlayImagePassesPending.clear();
    if (!isSequential && !rotoPaintNode) {
        const int maxCoord = std::numeric_limits<int>::max() / 2;
        OverlayImageRequest& request = outArgs->overlayImageRequest;
        request.time = time;
        request.view = view;
        request.draftMode = outArgs->draftModeEnabled;
        request.mipmapLevel = outArgs->draftModeEnabled ? outArgs->mipmapLevelWithDraft : outArgs->mipmapLevelWithoutDraft;
        request.visibleCanonical = _imp->uiContext->getImageRectangleDisplayed(RectI(-maxCoord, -maxCoord, maxCoord, maxCoord), 1., 0);
        for (const EffectInstancePtr& effect : effectsRequestingOverlayImagesUpstream(getNode(), outArgs->activeInputIndex)) {
            if (effect->overlayImagesStale(request)) {
                outArgs->staleOverlayImageEffects.push_back(effect);
                outArgs->overlayImagePassesPending.push_back(effect->getNode()->markOverlayImagePassPending());
            }
        }
    }

    // A scene has no RoD or texture to look up; it is evaluated on the render thread.
    if (outArgs->sceneUpstream) {
        return eViewerRenderRetCodeRender;
    }

    // Try to look-up the cache but do so only if we have a RoD valid in the cache because

    // we are on the main-thread here, it would be expensive to compute the RoD now.
    return getRoDAndLookupCache(true, viewerHash, rotoPaintNode, stats, outArgs);
}

ViewerInstance::ViewerRenderRetCode
ViewerInstance::evaluateSceneInput(ViewerArgs& inArgs)
{
    assert(inArgs.activeInputToRender && inArgs.params);
    const int textureIndex = inArgs.params->textureIndex;
    ScenePayload scene;
    std::string error;
    EffectInstance::RenderRoIRetCode code;
    const SceneEvaluationContext context(_imp->sceneContextId, getSceneLoadSet(), std::make_shared<SceneLoadReach>());
    try {
        code = inArgs.activeInputToRender->evaluateScene(EffectInstance::EvaluateSceneArgs(inArgs.params->time, inArgs.params->view, context), &scene, &error);
    } catch (const std::exception& e) {
        code = EffectInstance::eRenderRoIRetCodeFailed;
        error = e.what();
    } catch (...) {
        code = EffectInstance::eRenderRoIRetCodeFailed;
    }

    if (code == EffectInstance::eRenderRoIRetCodeAborted) {
        return eViewerRenderRetCodeRedraw;
    }

    const bool ok = (code == EffectInstance::eRenderRoIRetCodeOk);
    if (!ok && error.empty()) {
        error = inArgs.activeInputToRender->getScriptName_mt_safe() + ": scene evaluation failed";
    }
    if ((textureIndex == 0) || (textureIndex == 1)) {
        {
            std::lock_guard<std::mutex> k(_imp->lastSceneMutex);
            _imp->lastScene[textureIndex] = ok ? scene : ScenePayload();
            _imp->lastSceneError[textureIndex] = ok ? std::string() : error;
            _imp->lastSceneOk[textureIndex] = ok;
            _imp->lastSceneLoadSet[textureIndex] = context.loadSet;
            _imp->lastSceneReach[textureIndex] = context.reach;
        }
        Q_EMIT sceneEvaluated(textureIndex);
    }

    // Black rather than Fail even when the action failed: the failing node carries the error
    // itself, and Fail would stop playback and clear the whole viewer for what the 3D view shows.
    return eViewerRenderRetCodeBlack;
} // ViewerInstance::evaluateSceneInput

ViewerInstance::ViewerRenderRetCode
ViewerInstance::renderViewer_internal(ViewIdx view,
                                      bool singleThreaded,
                                      bool isSequentialRender,
                                      U64 viewerHash,
                                      bool /*canAbort*/,
                                      const NodePtr& rotoPaintNode,
                                      bool useTLS,
                                      const ViewerCurrentFrameRequestSchedulerStartArgsPtr& request,
                                      const RenderStatsPtr& stats,
                                      GpuPlacementSnapshotBuilder* placementBuilder,
                                      ViewerArgs& inArgs)
{
    assert(inArgs.activeInputToRender);

    // The request was queued behind a render and its node may have been destroyed since.
    const NodePtr activeInputNode = inArgs.activeInputToRender->getNode();
    if (!activeInputNode) {
        return eViewerRenderRetCodeRedraw;
    }

    if (inArgs.sceneUpstream) {
        return evaluateSceneInput(inArgs);
    }

    // We are in the render thread, we may not have computed the RoD and lookup the cache yet

    ViewerParallelRenderArgsSetterPtr frameArgs;

    if (useTLS) {
        frameArgs = std::make_shared<ViewerParallelRenderArgsSetter>(inArgs.params->time,
                                                                     inArgs.params->view,
                                                                     !isSequentialRender,
                                                                     isSequentialRender,
                                                                     inArgs.params->abortInfo,
                                                                     getNode(),
                                                                     inArgs.params->textureIndex,
                                                                     getTimeline().get(),
                                                                     false,
                                                                     rotoPaintNode,
                                                                     activeInputNode,
                                                                     inArgs.draftModeEnabled,
                                                                     stats);
    }


    if (inArgs.mustComputeRoDAndLookupCache) {
        // Since we will most likely need to compute the RoD now, we MUST setup the thread local
        // storage otherwise functions like EffectInstance::aborted() would not work.

        ViewerRenderRetCode retcode = getRoDAndLookupCache(false, viewerHash, rotoPaintNode, stats, &inArgs);
        if (retcode != eViewerRenderRetCodeRender) {
            return retcode;
        }


        if ( inArgs.params->nbCachedTile == (int)inArgs.params->tiles.size() ) {
            // Found a cached texture
            return eViewerRenderRetCodeRender;
        }
    }
    AbortableThread* isAbortable = dynamic_cast<AbortableThread*>( QThread::currentThread() );
    if (isAbortable) {
        isAbortable->setAbortInfo( !isSequentialRender, inArgs.params->abortInfo, getNode()->getEffectInstance() );
    }


    assert( !inArgs.params->nbCachedTile || inArgs.params->nbCachedTile < (int)inArgs.params->tiles.size() );

    /*
     * There are 3 types of renders:
     * 1) Playback: the canAbort flag is set to true and the isSequentialRender flag is set to true
     * 2) Single frame abortable render: the canAbort flag is set to true and the isSequentialRender flag is set to false. Basically
     * this kind of render is triggered by any parameter change, timeline scrubbing, curve positioning, etc... In this mode each image
     * rendered concurrently by the viewer is probably different than another one (different hash key or time) hence we want to abort
     * ongoing renders that are no longer corresponding to anything relevant to the actual state of the GUI. On the other hand, the user
     * still want a smooth feedback, e.g: If the user scrubs the timeline, we want to give him/her a continuous feedback, even with a
     * latency so it looks like it is actually seeking, otherwise it would just refresh the image upon the mouseRelease event because all
     * other renders would be aborted. To enable this behaviour, we ensure that at least 1 render is always running, even if it does not
     * correspond to the GUI state anymore.
     * 3) Single frame non-abortable render: the canAbort flag is set to false and the isSequentialRender flag is set to false.
     */

    const bool useTextureCache = !inArgs.forceRender && !inArgs.userRoIEnabled && rotoPaintNode.get() == 0 && !inArgs.isDoingPartialUpdates && inArgs.staleOverlayImageEffects.empty();
    RectI roi = inArgs.params->roi;

    // We might already have some tiles cached, get their bounding box to see if it is less than the actual RoI
    if (useTextureCache) {
        RectI tilesBbox;
        bool tilesBboxSet = false;
        for (std::list<UpdateViewerParams::CachedTile>::const_iterator it = inArgs.params->tiles.begin(); it != inArgs.params->tiles.end(); ++it) {
            if (it->ramBuffer) {
                continue;
            }
            if (!tilesBboxSet) {
                tilesBboxSet = true;
                tilesBbox.x1 = it->rectRounded.x1;
                tilesBbox.x2 = it->rectRounded.x2;
                tilesBbox.y1 = it->rectRounded.y1;
                tilesBbox.y2 = it->rectRounded.y2;
            } else {
                tilesBbox.merge(it->rectRounded.x1, it->rectRounded.y1, it->rectRounded.x2, it->rectRounded.y2);
            }
        }
        if ( roi.contains(tilesBbox) ) {
            roi = tilesBbox;
        }
    }

    ///Check that we were not aborted already
    if (!isSequentialRender && ((activeInputNode->getHashValue() != inArgs.activeInputHash) || (inArgs.params->time != getTimeline()->currentFrame()))) {
        return eViewerRenderRetCodeRedraw;
    }

    ///Notify the gui we're rendering.
    EffectInstance::NotifyRenderingStarted_RAII renderingNotifier( getNode().get() );

    std::shared_ptr<FrameRequestMap> requestPassData;
    if (useTLS) {
        const RectD canonicalRoi = roi.toCanonical(inArgs.params->mipmapLevel, inArgs.params->pixelAspectRatio, inArgs.params->rod);

        requestPassData = std::make_shared<FrameRequestMap>();
        StatusEnum stat = EffectInstance::computeRequestPass(inArgs.params->time, view, inArgs.params->mipmapLevel, canonicalRoi, getNode(), *requestPassData);
        if (stat == eStatusFailed) {
            return eViewerRenderRetCodeFail;
        }

        frameArgs->updateNodesRequest(*requestPassData);
    }
    const bool taskGraphMode = appPTR->getRenderSchedulerMode() == eRenderSchedulerModeTaskGraph;

    // Under frame args this is the snapshot the frame's request pass and hashes were taken with.
    const NodeMetadataConstPtr renderMetadata = inArgs.activeInputToRender->getRenderMetadata(inArgs.params->time, view);
    const double par = renderMetadata->getPixelAspectRatio(-1);

    // We are going to render a non cached frame and not in playback, clear persistent messages
    // (this code comes from https://github.com/NatronGitHub/Natron/commit/b0fcc946380571295b9230e9381218684474ca65)
    if (!inArgs.params->isSequential) {
        // Do not clear all previously set persistent message!
        // This also clears messages which do not come from the render action, such as messages set by InstanceChanged()!
        // Instead, the user can excplicitely clear the persistent messages using the refresh button, see ViewerTab::refresh(bool)
        //clearPersistentMessage(true);
    }

    ImageLayerDesc components, pairedComponents;
    EffectInstance::getLayersFromMetadata(*renderMetadata, -1, &components, &pairedComponents);
    ImageBitDepthEnum imageDepth = renderMetadata->getBitDepth(-1);
    std::list<ImageLayerDesc> requestedComponents;
    int alphaChannelIndex = -1;
    computeViewerRequestedLayers(inArgs, components, &requestedComponents, &alphaChannelIndex);

    if (requestedComponents.empty()) {
        return eViewerRenderRetCodeBlack;
    }

    EffectInstance::NotifyInputNRenderingStarted_RAII inputNIsRendering_RAII(getNode().get(), inArgs.activeInputIndex);
    std::vector<RectI> splitRoi;
    if (inArgs.isDoingPartialUpdates) {
        for (std::list<UpdateViewerParams::CachedTile>::iterator it = inArgs.params->tiles.begin(); it != inArgs.params->tiles.end(); ++it) {
            splitRoi.push_back(it->rect);
        }
    } else {
        /*
           Just render 1 tile
         */
        splitRoi.push_back(roi);
    }


    if ( stats && stats->isInDepthProfilingEnabled() ) {
        std::bitset<4> channelsRendered;
        switch (inArgs.channels) {
        case eDisplayChannelsMatte:
        case eDisplayChannelsRGB:
        case eDisplayChannelsY:
            channelsRendered[0] = channelsRendered[1] = channelsRendered[2] = true;
            channelsRendered[3] = false;
            break;
        case eDisplayChannelsR:
            channelsRendered[3] = channelsRendered[1] = channelsRendered[2] = false;
            channelsRendered[0] = true;
            break;
        case eDisplayChannelsG:
            channelsRendered[0] = channelsRendered[2] = channelsRendered[3] = false;
            channelsRendered[1] = true;
            break;
        case eDisplayChannelsB:
            channelsRendered[0] = channelsRendered[1] = channelsRendered[3] = false;
            channelsRendered[2] = true;
            break;
        case eDisplayChannelsA:
            channelsRendered[0] = channelsRendered[1] = channelsRendered[2] = false;
            channelsRendered[3] = true;
            break;
        }
        stats->setGlobalRenderInfosForNode(getNode(), inArgs.params->rod, channelsRendered, true, true, inArgs.params->mipmapLevel);
    }

//#pragma message WARN("Implement Viewer so it accepts OpenGL Textures in input")
    BufferableObjectPtrList partialUpdateObjects;
    for (std::size_t rectIndex = 0; rectIndex < splitRoi.size(); ++rectIndex) {
        //AlphaImage will only be set when displaying the Matte overlay
        ImagePtr alphaImage, colorImage;
#ifdef NATRON_HAVE_GPU
        GpuDeviceImagePtr deviceColor;
#endif
        bool hasDeviceColor = false;

        // If an exception occurs here it is probably fatal, since
        // it comes from Natron itself. All exceptions from plugins are already caught
        // by the HostSupport library.
        // We catch it  and rethrow it just to notify the rendering is done.
        try {
            std::map<ImageLayerDesc, ImagePtr> layers;
            EffectInstance::RenderRoIRetCode retCode;
            if (inArgs.deepUpstream) {
                if (taskGraphMode && stats) {
                    stats->incLegacyFallbacks("viewer: deep input");
                }
                // Only isDoingPartialUpdates (RotoPaint) ever splits the RoI, and deep has no
                // tiling, so there is exactly one rect to flatten.
                assert(splitRoi.size() == 1);
                EffectInstance::RenderDeepRoIArgs deepArgs(inArgs.params->time,
                                                           RenderScale::fromMipmapLevel(inArgs.params->mipmapLevel),
                                                           inArgs.params->mipmapLevel,
                                                           view,
                                                           inArgs.forceRender /*byPassCache*/,
                                                           splitRoi[rectIndex],
                                                           inArgs.params->rod,
                                                           this,
                                                           inArgs.params->time);
                DeepImagePtr deepImage;
                std::list<ImagePtr> flattened;
                retCode = inArgs.activeInputToRender->renderDeepRoIFlattened(deepArgs, requestedComponents, &flattened, &deepImage);
                if ((retCode == EffectInstance::eRenderRoIRetCodeOk) && !flattened.empty()) {
                    // Two layers only for the matte overlay, which requested the alpha layer first.
                    if (flattened.size() == 2) {
                        alphaImage = flattened.front();
                        colorImage = flattened.back();
                    } else {
                        colorImage = flattened.front();
                        if ((inArgs.channels == eDisplayChannelsMatte) && (inArgs.params->alphaLayer == inArgs.params->layer)) {
                            alphaImage = colorImage;
                        }
                    }
                }
                if (colorImage) {
                    inArgs.params->colorImage = colorImage;
                    inArgs.params->deepImage = deepImage;
                } else {
                    alphaImage.reset();
                }
            } else {
                FrameFuturePtr future;
                FrameRenderContextPtr frameContext;
                if (taskGraphMode) {
                    future = submitViewerFrameToRenderScheduler(frameArgs.get(), requestPassData, inArgs, bool(rotoPaintNode), view, splitRoi[rectIndex], requestedComponents, imageDepth, stats, _imp->uiContext, placementBuilder, &frameContext);
                }
                if (future) {
                    retCode = future->wait();
                    if (placementBuilder) {
                        const std::unordered_set<const Node*> renderedNodes = frameContext ? frameContext->renderedNodes() : std::unordered_set<const Node*>();
                        placementBuilder->finishGraph(retCode, future->getRegionFallbacks(), frameContext ? &renderedNodes : nullptr);
                    }
                    layers = future->getRootPlanes();
                    if (const SchedulerResultHook hook = schedulerResultHook()) {
                        hook(frameContext, &layers);
                    }
#ifdef NATRON_HAVE_GPU
                    if (frameContext) {
                        std::map<ImageLayerDesc, GpuDeviceImagePtr> devicePlanes = frameContext->takeRootDevicePlanes();
                        if (frameContext->wantsRootOnDevice() && (retCode == EffectInstance::eRenderRoIRetCodeOk) && !devicePlanes.empty()) {
                            std::map<ImageLayerDesc, GpuDeviceImagePtr>::const_iterator found = devicePlanes.find(requestedComponents.front());
                            if ((found == devicePlanes.end()) && (devicePlanes.size() == 1)) {
                                found = devicePlanes.begin();
                            }
                            if ((found != devicePlanes.end()) && found->second) {
                                const ViewerHandoffFormat format = (inArgs.params->depth == eImageBitDepthFloat) ? ViewerHandoffFormat::Rgba32F : ViewerHandoffFormat::Bgra8;
                                // Compiled here so that the main thread drawing the root never has to.
                                const ViewerHandoffResult prepared = _imp->uiContext->prepareHandoff(inArgs.params->displayProcessor, format);
                                if (prepared) {
                                    deviceColor = found->second;
                                    hasDeviceColor = true;
                                    layers.clear();
                                } else {
                                    noteHandoffRefusal(stats, prepared.reason);
                                    if (layers.empty()) {
                                        ImagePtr downloaded = found->second->downloadToRam();
                                        if (downloaded) {
                                            layers[found->first] = downloaded;
                                        }
                                    }
                                }
                            }
                        }
                    }
#endif
                } else {
                    std::unique_ptr<EffectInstance::RenderRoIArgs> renderArgs;
                    renderArgs.reset(new EffectInstance::RenderRoIArgs(inArgs.params->time,
                                                                       RenderScale::fromMipmapLevel(inArgs.params->mipmapLevel),
                                                                       inArgs.params->mipmapLevel,
                                                                       view,
                                                                       inArgs.forceRender,
                                                                       splitRoi[rectIndex],
                                                                       inArgs.params->rod,
                                                                       requestedComponents,
                                                                       imageDepth,
                                                                       false /*calledFromGetImage*/,
                                                                       this,
                                                                       eStorageModeRAM /*returnStorage*/,
                                                                       inArgs.params->time));
                    retCode = inArgs.activeInputToRender->renderRoI(*renderArgs, &layers);
                }
            }
            // Either rendering failed or we have 2 layers (alpha mask and color image) or we have a single layer (color image)
            assert(layers.size() == 0 || layers.size() <= 2);
            if (!layers.empty() && (retCode == EffectInstance::eRenderRoIRetCodeOk)) {
                if (layers.size() == 2) {
                    std::map<ImageLayerDesc, ImagePtr>::iterator foundColorLayer = layers.find(inArgs.params->layer);
                    if (foundColorLayer != layers.end()) {
                        colorImage = foundColorLayer->second;
                    }
                    std::map<ImageLayerDesc, ImagePtr>::iterator foundAlphaLayer = layers.find(inArgs.params->alphaLayer);
                    if (foundAlphaLayer != layers.end()) {
                        alphaImage = foundAlphaLayer->second;
                    }
                } else {
                    // only 1 layer, figure out if the alpha layer is the same as the color layer
                    if (inArgs.params->alphaLayer == inArgs.params->layer) {
                        if (inArgs.channels == eDisplayChannelsMatte) {
                            alphaImage = colorImage = layers.begin()->second;
                        } else {
                            colorImage = layers.begin()->second;
                        }
                    } else {
                        colorImage = layers.begin()->second;
                    }
                }
                assert(colorImage);
                inArgs.params->colorImage = colorImage;
            }
            if (!colorImage && !hasDeviceColor) {
                if (retCode == EffectInstance::eRenderRoIRetCodeFailed) {
                    return eViewerRenderRetCodeFail;
                } else if (retCode == EffectInstance::eRenderRoIRetCodeOk) {
                    return eViewerRenderRetCodeBlack;
                } else {
                    /*
                       The render was not aborted but did not return an image, this may be the case
                       for example when an effect returns a NULL RoD at some point. Don't fail but
                       display a black image
                     */
                    return eViewerRenderRetCodeRedraw;
                }
            }
        } catch (...) {
            ///If the plug-in was aborted, this is probably not a failure due to render but because of abortion.
            ///Don't forward the exception in that case.
            if ( inArgs.activeInputToRender->aborted() ) {
                return eViewerRenderRetCodeRedraw;
            }
            throw;
        }


        // Don't uncomment: if the image was rendered so far, render it to the display texture so that the user get some feedback
        /*if ( !_imp->checkAgeNoUpdate( inArgs.params->textureIndex, inArgs.params->abortInfo->getRenderAge() ) ) {
            return eViewerRenderRetCodeRedraw;
        }

        if ( inArgs.activeInputToRender->aborted() ) {
            return eViewerRenderRetCodeRedraw;
        }*/

        if (inArgs.rawUpstream && colorImage) {
            colorImage = RawDisplay::toDisplayImage(colorImage, inArgs.rawNormalisation ? &*inArgs.rawNormalisation : nullptr);
            inArgs.params->colorImage = colorImage;
            alphaImage = (inArgs.channels == eDisplayChannelsMatte) ? colorImage : ImagePtr();
        }
#ifdef NATRON_HAVE_GPU
        if (deviceColor) {
            if (((inArgs.channels == eDisplayChannelsA) || (inArgs.channels == eDisplayChannelsMatte)) && ((alphaChannelIndex < 0) || (alphaChannelIndex >= deviceColor->nComps()))) {
                return eViewerRenderRetCodeBlack;
            }
            // The CPU path finds no matte image when the only plane rendered is not the colour layer, and shows black.
            if ((inArgs.channels == eDisplayChannelsMatte) && (inArgs.params->alphaLayer != inArgs.params->layer)) {
                return eViewerRenderRetCodeBlack;
            }
            // The tiles keep describing the texture the root is drawn into, without any RAM buffer.
            const UpdateViewerParamsPtr& updateParams = inArgs.params;
            updateParams->deviceColor = deviceColor;
            updateParams->deviceRootHold = DeviceRootHolds::instance().hold(deviceColor);
            updateParams->deviceProcess.rect = updateParams->roi;
            updateParams->deviceProcess.channels = inArgs.channels;
            updateParams->deviceProcess.alphaChannelIndex = alphaChannelIndex;
            updateParams->deviceProcess.matteOverlay = (inArgs.channels == eDisplayChannelsMatte);
            updateParams->deviceProcess.gain = updateParams->gain;
            updateParams->deviceProcess.offset = updateParams->offset;
            updateParams->deviceProcess.gamma = updateParams->gamma;
            updateParams->deviceProcess.displayProcessor = updateParams->displayProcessor;
            updateParams->deviceStats = stats;
            updateParams->deviceCacheTiles.clear();
            // The tiles would be keyed by a viewer hash the upstream render may not have seen.
            if (useTextureCache && inArgs.cleanHash && inArgs.cleanHash->allowsStoreUnder(viewerHash)) {
                const std::string inputToRenderName = inArgs.activeInputToRender->getNode()->getScriptName_mt_safe();
                for (std::list<UpdateViewerParams::CachedTile>::const_iterator it = updateParams->tiles.begin(); it != updateParams->tiles.end(); ++it) {
                    UpdateViewerParams::DeviceCacheTile cacheTile;
                    cacheTile.key = FrameKey(getNode().get(),
                                             inArgs.params->time,
                                             viewerHash,
                                             inArgs.params->gain,
                                             inArgs.params->gamma,
                                             inArgs.params->displayProcessor ? inArgs.params->displayProcessor->cacheHash : 0,
                                             (int)inArgs.params->depth,
                                             inArgs.channels,
                                             inArgs.params->view,
                                             it->rect,
                                             inArgs.params->mipmapLevel,
                                             inputToRenderName,
                                             inArgs.params->layer,
                                             inArgs.params->alphaLayer.getLayerID() + inArgs.params->alphaChannelName,
                                             inArgs.params->depth == eImageBitDepthFloat,
                                             inArgs.draftModeEnabled);
                    cacheTile.rectRounded = it->rectRounded;
                    updateParams->deviceCacheTiles.push_back(cacheTile);
                }
            }
            updateParams->colorImage.reset();
            updateParams->mustFreeRamBuffer = false;
            for (std::list<UpdateViewerParams::CachedTile>::iterator it = updateParams->tiles.begin(); it != updateParams->tiles.end(); ++it) {
                assert(!it->ramBuffer && !it->cachedData);
                it->ramBuffer = 0;
                it->bytesCount = 0;
            }
            {
                QMutexLocker k(&_imp->lastRenderParamsMutex);
                _imp->lastRenderParams[updateParams->textureIndex].reset();
            }
            continue;
        }
#endif

        const bool viewerRenderRoiOnly = !useTextureCache;

        if ( ( (inArgs.channels == eDisplayChannelsA) && ( !colorImage || (alphaChannelIndex < 0) || ( alphaChannelIndex >= (int)colorImage->getComponentsCount() ) ) ) ||
            ( ( inArgs.channels == eDisplayChannelsMatte) && ( !alphaImage || ( alphaChannelIndex < 0) || ( alphaChannelIndex >= (int)alphaImage->getComponentsCount() ) ) ) ) {
            return eViewerRenderRetCodeBlack;
        }


        assert( ( inArgs.channels != eDisplayChannelsMatte && ( !colorImage || alphaChannelIndex < (int)colorImage->getComponentsCount() ) ) ||
               ( inArgs.channels == eDisplayChannelsMatte && ( !alphaImage || alphaChannelIndex < (int)alphaImage->getComponentsCount() ) ) );


        //Make sure the viewer does not render something outside the bounds
        RectI viewerRenderRoI;
        if (colorImage) {
            viewerRenderRoI = splitRoi[rectIndex].intersect(colorImage->getBounds());
        }

        UpdateViewerParamsPtr updateParams;
        if (!inArgs.isDoingPartialUpdates) {
            updateParams = inArgs.params;
        } else {
            updateParams = std::make_shared<UpdateViewerParams>(*inArgs.params);
            updateParams->mustFreeRamBuffer = true;
            updateParams->isPartialRect = true;
            UpdateViewerParams::CachedTile tile;
            tile.rect.set(viewerRenderRoI);
            tile.rectRounded = viewerRenderRoI;
            std::size_t pixelSize = 4;
            if (updateParams->depth == eImageBitDepthFloat) {
                pixelSize *= sizeof(float);
            }
            std::size_t dstRowSize = tile.rect.width() * pixelSize;
            tile.bytesCount = tile.rect.height() * dstRowSize;
            tile.ramBuffer =  (unsigned char*)malloc(tile.bytesCount);
            updateParams->tiles.clear();
            updateParams->tiles.push_back(tile);
        }

        // Allocate the texture on the CPU to apply the render viewer process
        FrameEntryLocker entryLocker( _imp.get() );
        RectI lastPaintBboxPixel;
        std::list<UpdateViewerParams::CachedTile> unCachedTiles;
        if (!useTextureCache) {
            assert(updateParams->tiles.size() == 1);

            UpdateViewerParams::CachedTile& tile = updateParams->tiles.front();


            // If we are tracking, only update the partial rectangles
            if (inArgs.isDoingPartialUpdates) {
                QMutexLocker k(&_imp->viewerParamsMutex);
                updateParams->recenterViewport = _imp->viewportCenterSet;
                updateParams->viewportCenter = _imp->viewportCenter;
                unCachedTiles.push_back(tile);
            }
            // If we are actively painting, re-use the last texture instead of re-drawing everything
            else if (rotoPaintNode) {
                /*
                   Check if we have a last valid texture and re-use the old texture only if the new texture is at least as big
                   as the old texture.
                 */
                QMutexLocker k(&_imp->lastRenderParamsMutex);
                assert(!_imp->lastRenderParams[updateParams->textureIndex] || _imp->lastRenderParams[updateParams->textureIndex]->tiles.size() == 1);


                bool canUseOldTex = _imp->lastRenderParams[updateParams->textureIndex] &&
                                    updateParams->mipmapLevel == _imp->lastRenderParams[updateParams->textureIndex]->mipmapLevel &&
                                    tile.rect.contains(_imp->lastRenderParams[updateParams->textureIndex]->tiles.front().rect);

                if (!canUseOldTex) {
                    //The old texture did not exist or was not usable, just make a new buffer
                    updateParams->mustFreeRamBuffer = true;
                    tile.ramBuffer =  (unsigned char*)malloc(tile.bytesCount);
                    unCachedTiles.push_back(tile);
                } else {
                    //Overwrite the RoI to only the last portion rendered
                    RectD lastPaintBbox = getApp()->getLastPaintStrokeBbox();

                    lastPaintBboxPixel = lastPaintBbox.toPixelEnclosing(updateParams->mipmapLevel, par);


                    //The last buffer must be valid
                    const UpdateViewerParams::CachedTile& lastTile = _imp->lastRenderParams[updateParams->textureIndex]->tiles.front();
                    assert(lastTile.ramBuffer);
                    tile.ramBuffer =  0;

                    //If the old buffer size is the same as the requested texture size, this function does nothing
                    //otherwise it allocates a new buffer and copies the content of the old one.
                    bool mustFreeSource = copyAndSwap(lastTile.rect, tile.rect, tile.bytesCount, updateParams->depth, lastTile.ramBuffer, &tile.ramBuffer);

                    if (mustFreeSource) {
                        //A new buffer was allocated and copied the previous content, free the old texture
                        _imp->lastRenderParams[updateParams->textureIndex]->mustFreeRamBuffer = true;
                    } else {
                        //The buffer did not change its size, make sure to keep it
                        _imp->lastRenderParams[updateParams->textureIndex]->mustFreeRamBuffer = false;
                    }

                    //This will delete the previous buffer if mustFreeRamBuffer was set to true
                    _imp->lastRenderParams[updateParams->textureIndex].reset();

                    //Allocation may have failed
                    if (!tile.ramBuffer) {
                        return eViewerRenderRetCodeFail;
                    }

                    unCachedTiles.push_back(tile);
                }
                _imp->lastRenderParams[updateParams->textureIndex] = updateParams;
            }
            // Make a new buffer that will be freed when the params get destroyed
            else {
                assert(updateParams->tiles.size() == 1);

                updateParams->mustFreeRamBuffer = true;
                tile.ramBuffer =  (unsigned char*)malloc(tile.bytesCount);
                unCachedTiles.push_back(tile);
            }
        } else { // useTextureCache
            //Look up the cache for a texture or create one
            {
                QMutexLocker k(&_imp->lastRenderParamsMutex);
                _imp->lastRenderParams[updateParams->textureIndex].reset();
            }

            // For the viewer, we need the enclosing rectangle to avoid black borders.
            // Do this here to avoid infinity values.

            const RectI bounds = updateParams->rod.toPixelEnclosing(updateParams->mipmapLevel, updateParams->pixelAspectRatio);

            const RectI tileBounds(0, 0, inArgs.params->tileSize, inArgs.params->tileSize);
            assert(!tileBounds.isNull());
            if (tileBounds.isNull()) {
                return eViewerRenderRetCodeRedraw;
            }
            std::string inputToRenderName = inArgs.activeInputToRender->getNode()->getScriptName_mt_safe();
            for (std::list<UpdateViewerParams::CachedTile>::iterator it = updateParams->tiles.begin(); it != updateParams->tiles.end(); ++it) {
                if (it->isCached) {
                    assert(it->ramBuffer);
                } else {
                    assert(!it->ramBuffer);

                    FrameKey key(getNode().get(),
                                 inArgs.params->time,
                                 viewerHash,
                                 inArgs.params->gain,
                                 inArgs.params->gamma,
                                 inArgs.params->displayProcessor ? inArgs.params->displayProcessor->cacheHash : 0,
                                 (int)inArgs.params->depth,
                                 inArgs.channels,
                                 inArgs.params->view,
                                 it->rect,
                                 inArgs.params->mipmapLevel,
                                 inputToRenderName,
                                 inArgs.params->layer,
                                 inArgs.params->alphaLayer.getLayerID() + inArgs.params->alphaChannelName,
                                 inArgs.params->depth == eImageBitDepthFloat,
                                 inArgs.draftModeEnabled);

                    FrameParamsPtr cachedFrameParams( new FrameParams(bounds , key.getBitDepth(), tileBounds, ImagePtr() ) );
                    bool cached = appPTR->getTextureOrCreate(key, cachedFrameParams, &entryLocker, &it->cachedData);
                    if (!it->cachedData) {
                        std::size_t size = cachedFrameParams->getStorageInfo().numComponents * cachedFrameParams->getStorageInfo().dataTypeSize * cachedFrameParams->getStorageInfo().bounds.area();
                        QString s = tr("Failed to allocate a texture of %1.").arg( printAsRAM(size) );
                        Dialogs::errorDialog( tr("Out of memory").toStdString(), s.toStdString() );

                        return eViewerRenderRetCodeFail;
                    }

                    ///The entry has already been locked by the cache
                    if (!cached) {
                        it->cachedData->allocateMemory();
                    } else {
                        // If the tile is cached and we got it that means rendering is done
                        entryLocker.lock(it->cachedData);
                        it->ramBuffer = it->cachedData->data();
                        it->isCached = true;
                        continue;
                    }


                    assert(it->cachedData);
                    // how do you make sure cachedFrame->data() is not freed after this line?
                    ///It is not freed as long as the cachedFrame shared_ptr has a used_count greater than 1.
                    ///Since it is used during the whole function scope it is guaranteed not to be freed before
                    ///The viewer is actually done with it.
                    /// @see Cache::clearInMemoryPortion and Cache::clearDiskPortion and LRUHashTable::evict
                    it->ramBuffer = it->cachedData->data();
                    assert(it->ramBuffer);
                    unCachedTiles.push_back(*it);
                } // !it->isCached

                if (it->cachedData) {
                    it->cachedData->setInternalImage(colorImage);
                    it->cachedData->setInternalDeepImage(inArgs.params->deepImage);
                }
            }
        } // !useTextureCache

        /*
           If we are not using the texture cache, there is a unique tile which has the required texture size on the viewer, so we render the RoI instead
           of just the tile portion in the render function
         */


        //If we are painting, only render the portion needed
        if ( !lastPaintBboxPixel.isNull() ) {
            viewerRenderRoI.clipIfOverlaps(lastPaintBboxPixel);
        }

        TimeLapsePtr viewerRenderTimeRecorder;
        if (stats) {
            viewerRenderTimeRecorder = std::make_shared<TimeLapse>();
        }

        std::size_t tileRowElements = inArgs.params->tileSize;
        // Internally the buffer is interpreted as U32 when 8bit, so we do not multiply it by 4 for RGBA
        if (updateParams->depth == eImageBitDepthFloat) {
            tileRowElements *= 4;
        }

        if (singleThreaded) {
            const RenderViewerArgs args(colorImage,
                                        alphaImage,
                                        inArgs.channels,
                                        updateParams->depth,
                                        updateParams->gain,
                                        updateParams->gamma,
                                        updateParams->offset,
                                        updateParams->displayProcessor,
                                        alphaChannelIndex,
                                        viewerRenderRoiOnly,
                                        tileRowElements);
            for (std::list<UpdateViewerParams::CachedTile>::iterator it = unCachedTiles.begin(); it != unCachedTiles.end(); ++it) {
                renderFunctor(viewerRenderRoI,
                              args,
                              this,
                              *it);
            }
        } else {
            bool runInCurrentThread = QThreadPool::globalInstance()->activeThreadCount() >= QThreadPool::globalInstance()->maxThreadCount();
            if ( !runInCurrentThread && (splitRoi.size() > 1) ) {
                runInCurrentThread = true;
            }

            const RenderViewerArgs args(colorImage,
                                        alphaImage,
                                        inArgs.channels,
                                        updateParams->depth,
                                        updateParams->gain,
                                        updateParams->gamma,
                                        updateParams->offset,
                                        updateParams->displayProcessor,
                                        alphaChannelIndex,
                                        viewerRenderRoiOnly,
                                        tileRowElements);

            if (runInCurrentThread) {
                for (std::list<UpdateViewerParams::CachedTile>::iterator it = unCachedTiles.begin(); it != unCachedTiles.end(); ++it) {
                    renderFunctor(viewerRenderRoI,
                                  args, this, *it);
                }
            } else {
                QtConcurrent::map( unCachedTiles,
                                   [&](const UpdateViewerParams::CachedTile &tile) {
                                    renderFunctor(viewerRenderRoI, args, this, tile);
                                   } ).waitForFinished();
            }

            if (inArgs.isDoingPartialUpdates) {
                partialUpdateObjects.push_back(updateParams);
            }
        } // if (singleThreaded)

        // The tiles created above were cached under viewerHash before the render: an edit or a
        // mark that came meanwhile means they may show another state, so they leave the cache,
        // still displayed by this update.
        if (useTextureCache && !(inArgs.cleanHash && inArgs.cleanHash->allowsStoreUnder(viewerHash))) {
            for (std::list<UpdateViewerParams::CachedTile>::const_iterator it = unCachedTiles.begin(); it != unCachedTiles.end(); ++it) {
                if (it->cachedData) {
                    appPTR->removeFromViewerCache(it->cachedData);
                }
            }
        }

        if ( colorImage && stats && stats->isInDepthProfilingEnabled() ) {
            stats->addRenderInfosForNode( getNode(), NodePtr(), colorImage->getComponents().getChannelsLabel(), viewerRenderRoI, viewerRenderTimeRecorder->getTimeSinceCreation() );
        }
    } // for (std::vector<RectI>::iterator rect = splitRoi.begin(); rect != splitRoi.end(), ++rect) {


    /*
       If we were rendering only partial rectangles, update them all at once
     */
    if ( !partialUpdateObjects.empty() ) {
        getRenderEngine()->notifyFrameProduced(partialUpdateObjects, stats, request);

        //If we reply eViewerRenderRetCodeRender, it will append another updateViewer for nothing
        return eViewerRenderRetCodeRedraw;
    }

    return eViewerRenderRetCodeRender;
} // renderViewer_internal

void
ViewerInstance::aboutToUpdateTextures()
{
    assert( qApp && qApp->thread() == QThread::currentThread() );
    _imp->uiContext->clearPartialUpdateTextures();
}

bool
ViewerInstance::isViewerUIVisible() const
{
    assert( qApp && qApp->thread() == QThread::currentThread() );

    return _imp->uiContext ? _imp->uiContext->isViewerUIVisible() : false;
}

void
ViewerInstance::updateViewer(UpdateViewerParamsPtr & frame)
{
    if (!frame) {
        return;
    }
    if (frame->isViewerPaused) {
        return;
    }
    if ( isViewerPaused(frame->textureIndex) ) {
        return;
    }
    _imp->updateViewer( std::dynamic_pointer_cast<UpdateViewerParams>(frame) );
}

void
renderFunctor(const RectI& roi,
              const RenderViewerArgs & args,
              ViewerInstance* viewer,
              UpdateViewerParams::CachedTile tile)
{
    if ( (args.bitDepth == eImageBitDepthFloat) ) {
        // image is stored as linear, the OpenGL shader with do gamma/sRGB/Rec709 decompression, as well as gain and offset
        scaleToTexture32bits(roi, args, tile, (float*)tile.ramBuffer);
    } else {
        // texture is stored as sRGB/Rec709 compressed 8-bit RGBA
        scaleToTexture8bits(roi, args, viewer, tile, (U32*)tile.ramBuffer);
    }
}

void
viewerProcessRectOnCpu(const ImageConstPtr& input,
                       const ImageConstPtr& matte,
                       DisplayChannelsEnum channels,
                       ImageBitDepthEnum depth,
                       double gain,
                       double gamma,
                       double offset,
                       const ProjectColorManagement::DisplayProcessorPtr& displayProcessor,
                       int alphaChannelIndex,
                       const RectI& rect,
                       void* output)
{
    if (rect.isNull() || !input || !output) {
        return;
    }
    const RenderViewerArgs args(input, matte, channels, depth, gain, gamma, offset, displayProcessor, alphaChannelIndex, true, 0);
    UpdateViewerParams::CachedTile tile;
    tile.rect = TextureRect(rect.x1, rect.y1, rect.x2, rect.y2, 1, 1.);
    tile.rectRounded = rect;
    tile.ramBuffer = (unsigned char*)output;
    renderFunctor(rect, args, NULL, tile);
}

template <typename PIX, int maxValue>
inline float
viewerPixelToFloat(PIX v)
{
    if (maxValue == 1) {
        return (float)v;
    }

    return Color::intToFloat<maxValue + 1>((int)v);
}

template <typename PIX, int maxValue, bool applyMatte, int rOffset, int gOffset, int bOffset>
void
scaleToTexture8bits_generic(const RectI& roi,
                            const RenderViewerArgs& args,
                            int nComps,
                            ViewerInstance* /*viewer*/,
                            const UpdateViewerParams::CachedTile& tile,
                            U32* tileBuffer)
{
    const bool luminance = (args.channels == eDisplayChannelsY);
    Image::ReadAccess acc = Image::ReadAccess(args.inputImage.get());

    if ( (args.renderOnlyRoI && !tile.rect.contains(roi)) || (!args.renderOnlyRoI && !roi.contains(tile.rect)) ) {
        return;
    }
    assert(tile.rect.x2 > tile.rect.x1);

    int dstRowElements;
    U32* dst_pixels;
    if (args.renderOnlyRoI) {
        dstRowElements = tile.rect.width();
        dst_pixels = tileBuffer + (roi.y1 - tile.rect.y1) * dstRowElements + (roi.x1 - tile.rect.x1);
    } else {
        dstRowElements = args.tileRowElements;
        dst_pixels = tileBuffer + (tile.rect.y1 - tile.rectRounded.y1) * args.tileRowElements + (tile.rect.x1 - tile.rectRounded.x1);
    }

    const int y1 = args.renderOnlyRoI ? roi.y1 : tile.rect.y1;
    const int y2 = args.renderOnlyRoI ? roi.y2 : tile.rect.y2;
    const int x1 = args.renderOnlyRoI ? roi.x1 : tile.rect.x1;
    const int x2 = args.renderOnlyRoI ? roi.x2 : tile.rect.x2;
    const int width = x2 - x1;
    if (width <= 0) {
        return;
    }
    const PIX* src_pixels = (const PIX*)acc.pixelAt(x1, y1);
    const int srcRowElements = (int)args.inputImage->getRowElements();
    Image::ReadAccessPtr matteAcc;
    if (applyMatte) {
        matteAcc = std::make_shared<Image::ReadAccess>( args.matteImage.get() );
    }

    const ProjectColorManagement::DisplayProcessor noTransform;
    const ProjectColorManagement::DisplayProcessor& processor = args.displayProcessor ? *args.displayProcessor : noTransform;

    std::vector<float> scanline(4 * width);
    std::vector<U8> alphas(width);
    std::vector<float> matteScanline(applyMatte ? 4 * width : 0);

    for (int y = y1; y < y2;
         ++y,
         dst_pixels += dstRowElements) {
        for (int x = 0; x < width; ++x) {
            float r = 0.f;
            float g = 0.f;
            float b = 0.f;
            float a = 0.f;
            if (src_pixels) {
                const PIX* pix = src_pixels + x * nComps;
                if (nComps >= 4) {
                    r = viewerPixelToFloat<PIX, maxValue>(pix[rOffset]);
                    g = viewerPixelToFloat<PIX, maxValue>(pix[gOffset]);
                    b = viewerPixelToFloat<PIX, maxValue>(pix[bOffset]);
                    a = viewerPixelToFloat<PIX, maxValue>(pix[3]);
                } else if (nComps == 3) {
                    // coverity[dead_error_line]
                    r = (rOffset < nComps) ? viewerPixelToFloat<PIX, maxValue>(pix[rOffset]) : 0.f;
                    // coverity[dead_error_line]
                    g = (gOffset < nComps) ? viewerPixelToFloat<PIX, maxValue>(pix[gOffset]) : 0.f;
                    // coverity[dead_error_line]
                    b = (bOffset < nComps) ? viewerPixelToFloat<PIX, maxValue>(pix[bOffset]) : 0.f;
                    a = 1.f;
                } else if (nComps == 2) {
                    // coverity[dead_error_line]
                    r = (rOffset < nComps) ? viewerPixelToFloat<PIX, maxValue>(pix[rOffset]) : 0.f;
                    // coverity[dead_error_line]
                    g = (gOffset < nComps) ? viewerPixelToFloat<PIX, maxValue>(pix[gOffset]) : 0.f;
                    a = 1.f;
                } else if (nComps == 1) {
                    // coverity[dead_error_line]
                    r = (rOffset < nComps) ? viewerPixelToFloat<PIX, maxValue>(pix[rOffset]) : 0.f;
                    g = b = r;
                    a = 1.f;
                } else {
                    assert(false);
                }
            }

            if (applyMatte) {
                float alphaMatteValue = 0.f;
                if (args.matteImage == args.inputImage) {
                    switch (args.alphaChannelIndex) {
                    case 0:
                        alphaMatteValue = r;
                        break;
                    case 1:
                        alphaMatteValue = g;
                        break;
                    case 2:
                        alphaMatteValue = b;
                        break;
                    case 3:
                        alphaMatteValue = a;
                        break;
                    default:
                        break;
                    }
                } else {
                    const PIX* mattePixels = (const PIX*)matteAcc->pixelAt(x1 + x, y);
                    if (mattePixels) {
                        alphaMatteValue = viewerPixelToFloat<PIX, maxValue>(mattePixels[args.alphaChannelIndex]);
                    }
                }
                float* m = &matteScanline[4 * x];
                m[0] = m[1] = m[2] = alphaMatteValue;
                m[3] = 1.f;
            }

            if (luminance) {
                r = 0.299f * r + 0.587f * g + 0.114f * b;
                g = r;
                b = r;
            }

            float* p = &scanline[4 * x];
            p[0] = r;
            p[1] = g;
            p[2] = b;
            p[3] = a;
            alphas[x] = (U8)Color::floatToInt<256>(a);
        }

        applyViewerDisplayTransform(processor, &scanline[0], width, args.gain, args.offset, args.gamma, eViewerDisplayOutputEightBit);
        if (applyMatte) {
            applyViewerDisplayTransform(processor, &matteScanline[0], width, 1., 0., 1., eViewerDisplayOutputEightBit);
        }

        // coverity[dont_call]
        int start = (int)(rand() % width);

        for (int backward = 0; backward < 2; ++backward) {
            int index = backward ? start - 1 : start;

            assert(backward == 1 || (index >= 0 && index < width));

            unsigned error_r = 0x80;
            unsigned error_g = 0x80;
            unsigned error_b = 0x80;

            while (index < width && index >= 0) {
                const float* p = &scanline[4 * index];
                error_r = (error_r & 0xff) + Color::floatToInt<0xff01>(p[0]);
                error_g = (error_g & 0xff) + Color::floatToInt<0xff01>(p[1]);
                error_b = (error_b & 0xff) + Color::floatToInt<0xff01>(p[2]);
                assert(error_r < 0x10000 && error_g < 0x10000 && error_b < 0x10000);
                U8 uR = (U8)(error_r >> 8);
                const U8 uG = (U8)(error_g >> 8);
                const U8 uB = (U8)(error_b >> 8);

                if (applyMatte) {
                    const U8 matteA = (U8)(Color::floatToInt<256>(matteScanline[4 * index]) / 2);
                    uR = Image::clampIfInt<U8>((float)uR + matteA);
                }

                dst_pixels[index] = toBGRA(uR, uG, uB, alphas[index]);

                if (backward) {
                    --index;
                } else {
                    ++index;
                }
            }
        }
        if (src_pixels) {
            src_pixels += srcRowElements;
        }
    }
} // scaleToTexture8bits_generic

template <typename PIX, int maxValue, int nComps, bool matteOverlay, int rOffset, int gOffset, int bOffset>
void
scaleToTexture8bits_internal(const RectI& roi,
                             const RenderViewerArgs& args,
                             ViewerInstance* viewer,
                             const UpdateViewerParams::CachedTile& tile,
                             U32* output)
{
    scaleToTexture8bits_generic<PIX, maxValue, matteOverlay, rOffset, gOffset, bOffset>(roi, args, nComps, viewer, tile, output);
}

template <typename PIX, int maxValue, int nComps, int rOffset, int gOffset, int bOffset>
void
scaleToTexture8bitsForMatte(const RectI& roi,
                            const RenderViewerArgs& args,
                            ViewerInstance* viewer,
                            const UpdateViewerParams::CachedTile& tile,
                            U32* output)
{
    bool applyMate = args.matteImage.get() && args.alphaChannelIndex >= 0;

    if (applyMate) {
        scaleToTexture8bits_internal<PIX, maxValue, nComps, true, rOffset, gOffset, bOffset>(roi, args, viewer, tile, output);
    } else {
        scaleToTexture8bits_internal<PIX, maxValue, nComps, false, rOffset, gOffset, bOffset>(roi, args, viewer, tile, output);
    }
}

template <typename PIX, int maxValue, int rOffset, int gOffset, int bOffset>
void
scaleToTexture8bitsForDepthForComponents(const RectI& roi,
                                         const RenderViewerArgs& args,
                                         ViewerInstance* viewer,
                                         const UpdateViewerParams::CachedTile& tile,
                                         U32* output)
{
    int nComps = args.inputImage->getComponents().getNumComponents();

    switch (nComps) {
    case 4:
        scaleToTexture8bitsForMatte<PIX, maxValue, 4, rOffset, gOffset, bOffset>(roi, args, viewer, tile, output);
        break;
    case 3:
        scaleToTexture8bitsForMatte<PIX, maxValue, 3, rOffset, gOffset, bOffset>(roi, args, viewer, tile, output);
        break;
    case 2:
        scaleToTexture8bitsForMatte<PIX, maxValue, 2, rOffset, gOffset, bOffset>(roi, args, viewer, tile, output);
        break;
    case 1:
        scaleToTexture8bitsForMatte<PIX, maxValue, 1, rOffset, gOffset, bOffset>(roi, args, viewer, tile, output);
        break;
    default:
        bool applyMate = args.matteImage.get() && args.alphaChannelIndex >= 0;
        if (applyMate) {
            scaleToTexture8bits_generic<PIX, maxValue, true, rOffset, gOffset, bOffset>(roi, args, nComps, viewer, tile, output);
        } else {
            scaleToTexture8bits_generic<PIX, maxValue, false, rOffset, gOffset, bOffset>(roi, args, nComps, viewer, tile, output);
        }
        break;
    }
}

template <typename PIX, int maxValue>
void
scaleToTexture8bitsForDepth(const RectI& roi,
                            const RenderViewerArgs& args,
                            ViewerInstance* viewer,
                            const UpdateViewerParams::CachedTile& tile,
                            U32* output)
{
    switch (args.channels) {
    case eDisplayChannelsRGB:
    case eDisplayChannelsY:
    case eDisplayChannelsMatte:

        scaleToTexture8bitsForDepthForComponents<PIX, maxValue, 0, 1, 2>(roi, args, viewer, tile, output);
        break;
    case eDisplayChannelsG:
        scaleToTexture8bitsForDepthForComponents<PIX, maxValue, 1, 1, 1>(roi, args, viewer, tile, output);
        break;
    case eDisplayChannelsB:
        scaleToTexture8bitsForDepthForComponents<PIX, maxValue, 2, 2, 2>(roi, args, viewer, tile, output);
        break;
    case eDisplayChannelsA:
        switch (args.alphaChannelIndex) {
        case -1:
            scaleToTexture8bitsForDepthForComponents<PIX, maxValue, 3, 3, 3>(roi, args, viewer, tile, output);
            break;
        case 0:
            scaleToTexture8bitsForDepthForComponents<PIX, maxValue, 0, 0, 0>(roi, args, viewer, tile, output);
            break;
        case 1:
            scaleToTexture8bitsForDepthForComponents<PIX, maxValue, 1, 1, 1>(roi, args, viewer, tile, output);
            break;
        case 2:
            scaleToTexture8bitsForDepthForComponents<PIX, maxValue, 2, 2, 2>(roi, args, viewer, tile, output);
            break;
        case 3:
            scaleToTexture8bitsForDepthForComponents<PIX, maxValue, 3, 3, 3>(roi, args, viewer, tile, output);
            break;
        default:
            scaleToTexture8bitsForDepthForComponents<PIX, maxValue, 3, 3, 3>(roi, args, viewer, tile, output);
        }

        break;
    case eDisplayChannelsR:
    default:
        scaleToTexture8bitsForDepthForComponents<PIX, maxValue, 0, 0, 0>(roi, args, viewer, tile, output);

        break;
    }
}

void
scaleToTexture8bits(const RectI& roi,
                    const RenderViewerArgs & args,
                    ViewerInstance* viewer,
                    const UpdateViewerParams::CachedTile& tile,
                    U32* output)
{
    assert(output);
    switch ( args.inputImage->getBitDepth() ) {
    case eImageBitDepthFloat:
        scaleToTexture8bitsForDepth<float, 1>(roi, args, viewer, tile, output);
        break;
    case eImageBitDepthByte:
        scaleToTexture8bitsForDepth<unsigned char, 255>(roi, args, viewer, tile, output);
        break;
    case eImageBitDepthShort:
        scaleToTexture8bitsForDepth<unsigned short, 65535>(roi, args, viewer, tile, output);
        break;
    case eImageBitDepthHalf:
        assert(false);
        break;
    case eImageBitDepthNone:
        break;
    }
} // scaleToTexture8bits

void
ViewerInstance::markAllOnGoingRendersAsAborted(bool keepOldestRender)
{
    //Do not abort the oldest render while scrubbing timeline or sliders so that the user gets some feedback
    bool keepOldest = getApp()->isDraftRenderEnabled() || isDoingPartialUpdates() || keepOldestRender;
    std::vector<AbortableRenderInfoPtr> abortedRenders;
    {
        QMutexLocker k(&_imp->renderAgeMutex);

        for (int i = 0; i < 2; ++i) {
            if (_imp->currentRenderAges[i].empty()) {
                continue;
            }

            // Do not abort the oldest render, let it finish
            OnGoingRenders::iterator it = _imp->currentRenderAges[i].begin();
            if (keepOldest) {
                ++it;
            }

            for (; it != _imp->currentRenderAges[i].end(); ++it) {
                (*it)->setAborted();
                abortedRenders.push_back(*it);
            }
        }
    }

    // Otherwise the queued tasks of a frame submitted to the RenderScheduler are only dropped as they are popped, and
    // its future finishes late. Outside renderAgeMutex so that no lock is held while the scheduler's is taken.
    if (!abortedRenders.empty() && appPTR->hasRenderScheduler()) {
        RenderScheduler* renderScheduler = appPTR->getRenderScheduler();
        for (std::vector<AbortableRenderInfoPtr>::const_iterator it = abortedRenders.begin(); it != abortedRenders.end(); ++it) {
            renderScheduler->abort(*it);
        }
    }
}

template <typename PIX, int maxValue, bool applyMatte, int rOffset, int gOffset, int bOffset>
void
scaleToTexture32bitsGeneric(const RectI& roi,
                            const RenderViewerArgs& args,
                            int nComps,
                            const UpdateViewerParams::CachedTile& tile,
                            float* tileBuffer)
{
    const bool luminance = (args.channels == eDisplayChannelsY);
    const int dstRowElements = args.renderOnlyRoI ? tile.rect.width() * 4 : args.tileRowElements;
    Image::ReadAccess acc = Image::ReadAccess( args.inputImage.get() );
    Image::ReadAccessPtr matteAcc;

    if (applyMatte) {
        matteAcc = std::make_shared<Image::ReadAccess>( args.matteImage.get() );
    }

    assert( (args.renderOnlyRoI && roi.x1 >= tile.rect.x1 && roi.x2 <= tile.rect.x2 && roi.y1 >= tile.rect.y1 && roi.y2 <= tile.rect.y2) || (!args.renderOnlyRoI && tile.rect.x1 >= roi.x1 && tile.rect.x2 <= roi.x2 && tile.rect.y1 >= roi.y1 && tile.rect.y2 <= roi.y2) );
    assert(tile.rect.x2 > tile.rect.x1);

    float* dst_pixels;
    if (args.renderOnlyRoI) {
        dst_pixels = tileBuffer + (roi.y1 - tile.rect.y1) * dstRowElements + (roi.x1 - tile.rect.x1) * 4;
    } else {
        dst_pixels = tileBuffer + (tile.rect.y1 - tile.rectRounded.y1) * dstRowElements + (tile.rect.x1 - tile.rectRounded.x1) * 4;
    }

    const int y1 = args.renderOnlyRoI ? roi.y1 : tile.rect.y1;
    const int y2 = args.renderOnlyRoI ? roi.y2 : tile.rect.y2;
    const int x1 = args.renderOnlyRoI ? roi.x1 : tile.rect.x1;
    const int x2 = args.renderOnlyRoI ? roi.x2 : tile.rect.x2;
    const PIX* src_pixels = (const PIX*)acc.pixelAt(x1, y1);
    const int srcRowElements = (const int)args.inputImage->getRowElements();

    for (int y = y1; y < y2;
         ++y,
         dst_pixels += dstRowElements) {
        for (int x = 0; x < (x2 - x1);
             ++x) {
            double r = 0.;
            double g = 0.;
            double b = 0.;
            double a = 0.;

            if (nComps >= 4) {
                r = (src_pixels && rOffset < nComps) ? src_pixels[x * nComps + rOffset] : 0.;
                g = (src_pixels && gOffset < nComps) ? src_pixels[x * nComps + gOffset] : 0.;
                b = (src_pixels && bOffset < nComps) ? src_pixels[x * nComps + bOffset] : 0.;
                a = src_pixels ? src_pixels[x * nComps + 3] : 0.;
            } else if (nComps == 3) {
                // coverity[dead_error_line]
                r = (src_pixels && rOffset < nComps) ? src_pixels[x * nComps + rOffset] : 0.;
                // coverity[dead_error_line]
                g = (src_pixels && gOffset < nComps) ? src_pixels[x * nComps + gOffset] : 0.;
                // coverity[dead_error_line]
                b = (src_pixels && bOffset < nComps) ? src_pixels[x * nComps + bOffset] : 0.;
                a = 1.;
            } else if (nComps == 2) {
                // coverity[dead_error_line]
                r = (src_pixels && rOffset < nComps) ? src_pixels[x * nComps + rOffset] : 0.;
                // coverity[dead_error_line]
                g = (src_pixels && gOffset < nComps) ? src_pixels[x * nComps + gOffset] : 0.;
                b = 0.;
                a = 1.;
            } else if (nComps == 1) {
                // coverity[dead_error_line]
                r = (src_pixels && rOffset < nComps) ? src_pixels[x * nComps + rOffset] : 0.;
                g = b = r;
                a = 1.;
            } else {
                assert(false);
            }

            if (maxValue != 1) {
                r = Color::intToFloat<maxValue + 1>((int)r);
                g = Color::intToFloat<maxValue + 1>((int)g);
                b = Color::intToFloat<maxValue + 1>((int)b);
                if (nComps >= 4) {
                    a = Color::intToFloat<maxValue + 1>((int)a);
                }
            }

            if (luminance) {
                r = 0.299 * r + 0.587 * g + 0.114 * b;
                g = r;
                b = r;
            }

            if (applyMatte) {
                double alphaMatteValue = 0;
                if (args.matteImage == args.inputImage) {
                    switch (args.alphaChannelIndex) {
                    case 0:
                        alphaMatteValue = r;
                        break;
                    case 1:
                        alphaMatteValue = g;
                        break;
                    case 2:
                        alphaMatteValue = b;
                        break;
                    case 3:
                        alphaMatteValue = a;
                        break;
                    default:
                        break;
                    }
                } else {
                    const PIX* mattePixels = (const PIX*)matteAcc->pixelAt(x1 + x, y);
                    if (mattePixels) {
                        alphaMatteValue = viewerPixelToFloat<PIX, maxValue>(mattePixels[args.alphaChannelIndex]);
                    }
                }
                r += alphaMatteValue * 0.5;
            }


            dst_pixels[x * 4] = r; // do not clamp! values may be more than 1 or less than 0
            dst_pixels[x * 4 + 1] = g;
            dst_pixels[x * 4 + 2] = b;
            dst_pixels[x * 4 + 3] = a;
        }
        if (src_pixels) {
            src_pixels += srcRowElements;
        }
    }
} // scaleToTexture32bitsGeneric

template <typename PIX, int maxValue, int nComps, bool applyMatte, int rOffset, int gOffset, int bOffset>
void
scaleToTexture32bitsInternal(const RectI& roi,
                             const RenderViewerArgs& args,
                             const UpdateViewerParams::CachedTile& tile,
                             float* output)
{
    scaleToTexture32bitsGeneric<PIX, maxValue, applyMatte, rOffset, gOffset, bOffset>(roi, args, nComps, tile, output);
}

template <typename PIX, int maxValue, int nComps, int rOffset, int gOffset, int bOffset>
void
scaleToTexture32bitsForMatte(const RectI& roi,
                             const RenderViewerArgs& args,
                             const UpdateViewerParams::CachedTile& tile,
                             float* output)
{
    bool applyMatte = args.matteImage.get() && args.alphaChannelIndex >= 0;

    if (applyMatte) {
        scaleToTexture32bitsInternal<PIX, maxValue, nComps, true, rOffset, gOffset, bOffset>(roi, args, tile, output);
    } else {
        scaleToTexture32bitsInternal<PIX, maxValue, nComps, false, rOffset, gOffset, bOffset>(roi, args, tile, output);
    }
}

template <typename PIX, int maxValue, int rOffset, int gOffset, int bOffset>
void
scaleToTexture32bitsForDepthForComponents(const RectI& roi,
                                          const RenderViewerArgs& args,
                                          const UpdateViewerParams::CachedTile& tile,
                                          float* output)
{
    int nComps = args.inputImage->getComponents().getNumComponents();

    switch (nComps) {
    case 4:
        scaleToTexture32bitsForMatte<PIX, maxValue, 4, rOffset, gOffset, bOffset>(roi, args, tile, output);
        break;
    case 3:
        scaleToTexture32bitsForMatte<PIX, maxValue, 3, rOffset, gOffset, bOffset>(roi, args, tile, output);
        break;
    case 2:
        scaleToTexture32bitsForMatte<PIX, maxValue, 2, rOffset, gOffset, bOffset>(roi, args, tile, output);
        break;
    case 1:
        scaleToTexture32bitsForMatte<PIX, maxValue, 1, rOffset, gOffset, bOffset>(roi, args, tile, output);
        break;
    default:
        bool applyMatte = args.matteImage.get() && args.alphaChannelIndex >= 0;
        if (applyMatte) {
            scaleToTexture32bitsGeneric<PIX, maxValue, true, rOffset, gOffset, bOffset>(roi, args, nComps, tile, output);
        } else {
            scaleToTexture32bitsGeneric<PIX, maxValue, false, rOffset, gOffset, bOffset>(roi, args, nComps, tile, output);
        }
        break;
    }
}

template <typename PIX, int maxValue>
void
scaleToTexture32bitsForDepth(const RectI& roi,
                             const RenderViewerArgs& args,
                             const UpdateViewerParams::CachedTile& tile,
                             float* output)
{
    switch (args.channels) {
    case eDisplayChannelsRGB:
    case eDisplayChannelsY:
    case eDisplayChannelsMatte:
        scaleToTexture32bitsForDepthForComponents<PIX, maxValue, 0, 1, 2>(roi, args, tile, output);
        break;
    case eDisplayChannelsG:
        scaleToTexture32bitsForDepthForComponents<PIX, maxValue, 1, 1, 1>(roi, args, tile, output);
        break;
    case eDisplayChannelsB:
        scaleToTexture32bitsForDepthForComponents<PIX, maxValue, 2, 2, 2>(roi, args, tile, output);
        break;
    case eDisplayChannelsA:
        switch (args.alphaChannelIndex) {
        case -1:
            scaleToTexture32bitsForDepthForComponents<PIX, maxValue, 3, 3, 3>(roi, args, tile, output);
            break;
        case 0:
            scaleToTexture32bitsForDepthForComponents<PIX, maxValue, 0, 0, 0>(roi, args, tile, output);
            break;
        case 1:
            scaleToTexture32bitsForDepthForComponents<PIX, maxValue, 1, 1, 1>(roi, args, tile, output);
            break;
        case 2:
            scaleToTexture32bitsForDepthForComponents<PIX, maxValue, 2, 2, 2>(roi, args, tile, output);
            break;
        case 3:
            scaleToTexture32bitsForDepthForComponents<PIX, maxValue, 3, 3, 3>(roi, args, tile, output);
            break;
        default:
            scaleToTexture32bitsForDepthForComponents<PIX, maxValue, 3, 3, 3>(roi, args, tile, output);
            break;
        }

        break;
    case eDisplayChannelsR:
    default:
        scaleToTexture32bitsForDepthForComponents<PIX, maxValue, 0, 0, 0>(roi, args, tile, output);
        break;
    }
}

void
scaleToTexture32bits(const RectI& roi,
                     const RenderViewerArgs & args,
                     const UpdateViewerParams::CachedTile& tile,
                     float *output)
{
    assert(output);

    switch ( args.inputImage->getBitDepth() ) {
    case eImageBitDepthFloat:
        scaleToTexture32bitsForDepth<float, 1>(roi, args, tile, output);
        break;
    case eImageBitDepthByte:
        scaleToTexture32bitsForDepth<unsigned char, 255>(roi, args, tile, output);
        break;
    case eImageBitDepthShort:
        scaleToTexture32bitsForDepth<unsigned short, 65535>(roi, args, tile, output);
        break;
    case eImageBitDepthHalf:
        assert(false);
        break;
    case eImageBitDepthNone:
        break;
    }
} // scaleToTexture32bits

#ifdef NATRON_HAVE_GPU
NATRON_NAMESPACE_ANONYMOUS_ENTER

// A device frame that is never read back is not worth waiting for, which also bounds how long a lost device keeps the
// poll alive.
const int kMaxCacheReadbackPolls = 200;
const int kCacheReadbackPollMs = 25;
const std::size_t kMaxPendingCacheFills = 4;

// Runs the CPU viewer process of the device root of params into the display texture, for a frame whose hand-off was
// refused, and returns the RAM copy of the root.
ImagePtr
drawDeviceRootOnCpu(OpenGLViewerI* uiContext,
                    const UpdateViewerParams& params,
                    const TextureRect& texRect,
                    TexturePtr* texture)
{
    ImagePtr downloaded = params.deviceColor->downloadToRam();
    if (!downloaded) {
        return ImagePtr();
    }
    const RectI& rect = params.roi;
    const std::size_t pixelBytes = (params.depth == eImageBitDepthFloat) ? 4 * sizeof(float) : sizeof(U32);
    const std::size_t rowBytes = rect.width() * pixelBytes;
    std::vector<unsigned char> buffer(rowBytes * rect.height(), 0);
    const ViewerHandoffParams& process = params.deviceProcess;
    const ImageConstPtr matte = process.matteOverlay ? ImageConstPtr(downloaded) : ImageConstPtr();
    // viewerProcessRectOnCpu() reads a rect whose bottom-left corner is off the image as all empty, so only the part
    // the root covers is processed.
    const RectI drawn = rect.intersect(downloaded->getBounds());
    if (!drawn.isNull()) {
        const bool whole = (drawn == rect);
        const std::size_t drawnRowBytes = drawn.width() * pixelBytes;
        std::vector<unsigned char> part(whole ? 0 : drawnRowBytes * drawn.height());
        unsigned char* const out = whole ? &buffer[0] : &part[0];
        // The 8-bit dither diffuses its error along each row from a random start of its own, so splitting the rect into
        // bands of whole rows leaves the output as one call makes it.
        const int maxThreads = std::max(1, QThreadPool::globalInstance()->maxThreadCount());
        const int bandCount = std::min(drawn.height(), 4 * maxThreads);
        const int rowsPerBand = (drawn.height() + bandCount - 1) / bandCount;
        parallelForOnGlobalPool(bandCount, maxThreads, [&](int i) {
            RectI band = drawn;
            band.y1 = drawn.y1 + i * rowsPerBand;
            band.y2 = std::min(drawn.y2, band.y1 + rowsPerBand);
            if (band.y1 >= band.y2) {
                return;
            }
            viewerProcessRectOnCpu(downloaded, matte, process.channels, params.depth, process.gain, process.gamma, process.offset,
                                   process.displayProcessor, process.alphaChannelIndex, band,
                                   out + std::size_t(band.y1 - drawn.y1) * drawnRowBytes);
        });
        if (!whole) {
            for (int y = drawn.y1; y < drawn.y2; ++y) {
                std::memcpy(&buffer[(y - rect.y1) * rowBytes + (drawn.x1 - rect.x1) * pixelBytes],
                            &part[(y - drawn.y1) * drawnRowBytes],
                            drawnRowBytes);
            }
        }
    }
    uiContext->transferBufferFromRAMtoGPU(&buffer[0], buffer.size(), params.roi, params.roiNotRoundedToTileSize, texRect,
                                          params.textureIndex, false, true, texture);

    return downloaded;
}

NATRON_NAMESPACE_ANONYMOUS_EXIT
#endif // NATRON_HAVE_GPU

#ifdef NATRON_HAVE_GPU
void
ViewerInstance::ViewerInstancePrivate::insertCacheTiles(const PendingCacheFill& fill,
                                                        const ViewerHandoffReadback& readback)
{
    const std::size_t pixelBytes = (std::size_t)fill.bytesPerPixel;
    const std::size_t frameRowBytes = (std::size_t)readback.frame.width * pixelBytes;
    if ((readback.frame.width != (unsigned)fill.roi.width()) || (readback.frame.height != (unsigned)fill.roi.height()) || !readback.data || (readback.size != frameRowBytes * readback.frame.height)) {
        return;
    }
    const RectI tileBounds(0, 0, fill.tileSize, fill.tileSize);
    const std::size_t tileRowBytes = (std::size_t)fill.tileSize * pixelBytes;
    struct TileCopy {
        FrameEntryPtr entry;
        unsigned char* dst;
        const unsigned char* src;
    };
    std::vector<TileCopy> copies;
    copies.reserve(fill.tiles.size());
    // Every new entry stays locked until its copy is done, so no reader sees a tile half filled.
    FrameEntryLocker entryLocker(this);
    for (std::vector<UpdateViewerParams::DeviceCacheTile>::const_iterator it = fill.tiles.begin(); it != fill.tiles.end(); ++it) {
        const RectI& r = it->rectRounded;
        if ((r.width() != fill.tileSize) || (r.height() != fill.tileSize) || !fill.roi.contains(r)) {
            continue;
        }
        FrameParamsPtr frameParams(new FrameParams(fill.bounds, it->key.getBitDepth(), tileBounds, ImagePtr()));
        FrameEntryPtr entry;
        // A tile that is in the cache already is complete, and another thread may be filling it.
        if (appPTR->getTextureOrCreate(it->key, frameParams, &entryLocker, &entry) || !entry) {
            continue;
        }
        entry->allocateMemory();
        unsigned char* dst = entry->data();
        if (!dst) {
            continue;
        }
        const std::size_t xBytes = (std::size_t)(r.x1 - fill.roi.x1) * pixelBytes;
        copies.push_back(TileCopy { entry, dst, readback.data + (std::size_t)(r.y1 - fill.roi.y1) * frameRowBytes + xBytes });
    }
    const int tileSize = fill.tileSize;
    parallelForOnGlobalPool((int)copies.size(), QThreadPool::globalInstance()->maxThreadCount(), [&](int i) {
        const TileCopy& c = copies[i];
        for (int row = 0; row < tileSize; ++row) {
            std::memcpy(c.dst + (std::size_t)row * tileRowBytes, c.src + (std::size_t)row * frameRowBytes, tileRowBytes);
        }
    });
}

void
ViewerInstance::ViewerInstancePrivate::pollCacheReadbacks()
{
    if (pendingCacheFills.empty()) {
        return;
    }
    if (!uiContext) {
        pendingCacheFills.clear();

        return;
    }
    const std::shared_ptr<CacheFillGate> gate = cacheFillGate;
    uiContext->consumeCompletedHandoffReadbacks([this, &gate](std::uint64_t token) -> ViewerHandoffReadbackFn {
        for (std::list<PendingCacheFill>::iterator it = pendingCacheFills.begin(); it != pendingCacheFills.end(); ++it) {
            if (it->token != token) {
                continue;
            }
            std::shared_ptr<const PendingCacheFill> fill = std::make_shared<const PendingCacheFill>(std::move(*it));
            pendingCacheFills.erase(it);

            return [this, gate, fill](const ViewerHandoffReadback& rb) {
                gate->runIfOpen([&]() { insertCacheTiles(*fill, rb); });
            };
        }

        return ViewerHandoffReadbackFn();
    });
    for (std::list<PendingCacheFill>::iterator it = pendingCacheFills.begin(); it != pendingCacheFills.end();) {
        if (++it->polls > kMaxCacheReadbackPolls) {
            it = pendingCacheFills.erase(it);
        } else {
            ++it;
        }
    }
}

void
ViewerInstance::ViewerInstancePrivate::scheduleCachePoll()
{
    if (cachePollScheduled || pendingCacheFills.empty()) {
        return;
    }
    cachePollScheduled = true;
    QTimer::singleShot(kCacheReadbackPollMs, this, [this]() {
        cachePollScheduled = false;
        if (!uiContext) {
            pendingCacheFills.clear();

            return;
        }
        uiContext->makeOpenGLcontextCurrent();
        pollCacheReadbacks();
        scheduleCachePoll();
    });
}
#endif // NATRON_HAVE_GPU

#ifdef NATRON_HAVE_GPU
NATRON_NAMESPACE_ANONYMOUS_ENTER

// Short, since a staged frame is shown only when the next one arrives or this poll finds it finished.
const int kStagedFramePollMs = 1;

TextureRect
deviceTextureRect(const UpdateViewerParams& params)
{
    const UpdateViewerParams::CachedTile& firstTile = params.tiles.front();
    TextureRect texRect;
    texRect.par = firstTile.rect.par;
    texRect.closestPo2 = firstTile.rect.closestPo2;
    texRect.set(params.roi);

    return texRect;
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

std::uint64_t
ViewerInstance::ViewerInstancePrivate::assignReadbackToken(UpdateViewerParams& params)
{
    const bool wantsCacheFill = !params.deviceCacheTiles.empty() && (params.tileSize > 0);
    params.deviceProcess.readbackToken = wantsCacheFill ? nextReadbackToken++ : 0;

    return params.deviceProcess.readbackToken;
}

void
ViewerInstance::ViewerInstancePrivate::addCacheFill(const UpdateViewerParams& params)
{
    PendingCacheFill fill;
    fill.token = params.deviceProcess.readbackToken;
    fill.tiles = params.deviceCacheTiles;
    fill.roi = params.roi;
    fill.bounds = params.rod.toPixelEnclosing(params.mipmapLevel, params.pixelAspectRatio);
    fill.tileSize = params.tileSize;
    fill.bytesPerPixel = (params.depth == eImageBitDepthFloat) ? 16 : 4;
    pendingCacheFills.push_back(fill);
    while (pendingCacheFills.size() > kMaxPendingCacheFills) {
        pendingCacheFills.pop_front();
    }
    scheduleCachePoll();
}

void
ViewerInstance::ViewerInstancePrivate::presentDeviceFrame(const UpdateViewerParamsPtr& params,
                                                          TexturePtr* texture,
                                                          ImagePtr* deviceDownload)
{
    const TextureRect texRect = deviceTextureRect(*params);
    const bool wantsCacheFill = assignReadbackToken(*params) != 0;
    const ViewerHandoffResult presented = uiContext->transferDeviceImageToGPU(params->deviceColor, texRect, params->roiNotRoundedToTileSize,
                                                                              params->textureIndex, params->deviceProcess, texture);
    // present() pins the root for as long as the device reads it, and a pinned root cannot be downloaded, neither by
    // the CPU fallback below nor by the picker reading the frame on screen.
    params->deviceRootHold.reset();
    if (presented && wantsCacheFill) {
        addCacheFill(*params);
    }
    if (!presented) {
        noteHandoffRefusal(params->deviceStats, presented.reason);
        *deviceDownload = drawDeviceRootOnCpu(uiContext, *params, texRect, texture);
    }
}

bool
ViewerInstance::ViewerInstancePrivate::stageDeviceFrame(const UpdateViewerParamsPtr& params)
{
    StagedDeviceFrame staged;
    staged.params = params;
    staged.texRect = deviceTextureRect(*params);
    const bool wantsCacheFill = assignReadbackToken(*params) != 0;
    if (!uiContext->stageDeviceImage(params->deviceColor, staged.texRect, params->textureIndex, params->deviceProcess, &staged.id)) {
        return false;
    }
    params->deviceRootHold.reset();
    if (wantsCacheFill) {
        addCacheFill(*params);
    }
    stagedFrames[params->textureIndex] = staged;

    return true;
}

void
ViewerInstance::ViewerInstancePrivate::showStagedFrame(const StagedDeviceFrame& staged)
{
    const UpdateViewerParamsPtr& params = staged.params;
    TexturePtr texture;
    ImagePtr deviceDownload;
    const ViewerHandoffResult presented = uiContext->transferStagedDeviceImageToGPU(staged.id, params->deviceColor, staged.texRect,
                                                                                    params->roiNotRoundedToTileSize, params->textureIndex, &texture);
    if (!presented) {
        noteHandoffRefusal(params->deviceStats, presented.reason);
        deviceDownload = drawDeviceRootOnCpu(uiContext, *params, staged.texRect, &texture);
    }
    showFrame(params, texture, deviceDownload, true);
}

void
ViewerInstance::ViewerInstancePrivate::discardStagedFrame(int textureIndex)
{
    std::optional<StagedDeviceFrame> staged;
    staged.swap(stagedFrames[textureIndex]);
    if (staged && uiContext) {
        uiContext->discardStagedDeviceImage(textureIndex, staged->id);
    }
}

void
ViewerInstance::ViewerInstancePrivate::scheduleStagedFlush()
{
    if (stagedFlushScheduled || (!stagedFrames[0] && !stagedFrames[1])) {
        return;
    }
    stagedFlushScheduled = true;
    QTimer::singleShot(kStagedFramePollMs, Qt::PreciseTimer, this, [this]() {
        stagedFlushScheduled = false;
        if (!uiContext) {
            stagedFrames[0].reset();
            stagedFrames[1].reset();

            return;
        }
        uiContext->makeOpenGLcontextCurrent();
        bool shown = false;
        for (int i = 0; i < 2; ++i) {
            if (!stagedFrames[i] || uiContext->isStagedDeviceImageComputing(i, stagedFrames[i]->id)) {
                continue;
            }
            std::optional<StagedDeviceFrame> staged;
            staged.swap(stagedFrames[i]);
            showStagedFrame(*staged);
            shown = true;
        }
        if (shown) {
            uiContext->redraw();
        }
        scheduleStagedFlush();
    });
}
#endif // NATRON_HAVE_GPU

void
ViewerInstance::ViewerInstancePrivate::updateViewer(UpdateViewerParamsPtr params)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );

    uiContext->makeOpenGLcontextCurrent();
#ifdef NATRON_HAVE_GPU
    pollCacheReadbacks();
#endif
    if (params->tiles.empty()) {
        return;
    }
    // A render still finishing after a newer request was already shown or failed renders a
    // frame or a state the user has left, so it must not replace what is on screen. Tiles of
    // the render last shown carry its own age and still go through.
    const U64 age = params->abortInfo->getRenderAge();
    if (!params->isPartialRect && !params->isSequential && !checkAgeNoUpdate(params->textureIndex, age)) {
        return;
    }
    checkAndUpdateDisplayAge(params->textureIndex, age);

    assert((params->isPartialRect && params->tiles.size() == 1) || !params->isPartialRect);

    TexturePtr texture;
    ImagePtr deviceDownload;
    bool deviceFrame = false;
#ifdef NATRON_HAVE_GPU
    std::optional<StagedDeviceFrame> previous;
    previous.swap(stagedFrames[params->textureIndex]);
    // In playback the readback rung shows the frame before this one while the device computes this one, rather than
    // blocking the main thread on it; a frame of a single render is shown at once, as nothing follows it to wait for.
    if (params->deviceColor && params->isSequential && (uiContext->handoffRung() == ViewerHandoffRung::Readback) && stageDeviceFrame(params)) {
        if (previous) {
            showStagedFrame(*previous);
        }
        scheduleStagedFlush();

        return;
    }
    if (previous) {
        // Playback must not skip a frame and a partial rect draws over the frame before it, while any other frame
        // replaces the staged one entirely.
        if (params->isSequential || params->isPartialRect) {
            showStagedFrame(*previous);
        } else {
            uiContext->discardStagedDeviceImage(params->textureIndex, previous->id);
        }
    }
    if (params->deviceColor) {
        assert(!params->isPartialRect);
        deviceFrame = true;
        presentDeviceFrame(params, &texture, &deviceDownload);
    }
#endif
    showFrame(params, texture, deviceDownload, deviceFrame);
} // ViewerInstance::ViewerInstancePrivate::updateViewer

void
ViewerInstance::ViewerInstancePrivate::showFrame(const UpdateViewerParamsPtr& params,
                                                 TexturePtr texture,
                                                 const ImagePtr& deviceDownload,
                                                 bool deviceFrame)
{
    bool isFirstTile = true;
    for (std::list<UpdateViewerParams::CachedTile>::iterator it = params->tiles.begin(); it != params->tiles.end(); ++it) {
        if (!it->ramBuffer) {
            continue;
        }

        // For cached tiles, some tiles might not have the standard tile, (i.e: the last tile column/row).
        // Since the internal buffer is rounded to the tile size anyway we want the glTexSubImage2D call to ensure
        // that we upload the full internal buffer
        TextureRect texRect;
        texRect.par = it->rect.par;
        texRect.closestPo2 = it->rect.closestPo2;
        texRect.set(it->rectRounded);

        assert(params->roi.contains(texRect));
        uiContext->transferBufferFromRAMtoGPU(it->ramBuffer, it->bytesCount, params->roi, params->roiNotRoundedToTileSize, texRect, params->textureIndex, params->isPartialRect, isFirstTile, &texture);
        isFirstTile = false;
    }
    if (RotoLatencyProbe* probe = RotoLatencyProbe::instance()) {
        probe->noteTextureUploaded();
    }

    NodePtr rotoPaintNode;
    RotoStrokeItemPtr curStroke;
    bool isDrawing = false;
    instance->getApp()->getActiveRotoDrawingStroke(&rotoPaintNode, &curStroke, &isDrawing);

    const UpdateViewerParams::CachedTile& firstTile = params->tiles.front();
    ImagePtr originalImage;
    originalImage = params->colorImage;
    DeepImagePtr deepImage = params->deepImage;
    if (firstTile.cachedData && !originalImage) {
        originalImage = firstTile.cachedData->getInternalImage();
        deepImage = firstTile.cachedData->getInternalDeepImage();
    }
    if (!originalImage) {
        originalImage = deviceDownload;
    }
    ImageBitDepthEnum depth;
    if (originalImage) {
        depth = originalImage->getBitDepth();
    } else if (deviceFrame) {
        depth = eImageBitDepthFloat;
    } else {
        assert(firstTile.cachedData);
        if (firstTile.cachedData) {
            depth = (ImageBitDepthEnum)firstTile.cachedData->getKey().getBitDepth();
        } else {
            depth = eImageBitDepthByte;
        }
    }

    uiContext->endTransferBufferFromRAMToGPU(params->textureIndex, texture, originalImage, params->time, params->rod, params->pixelAspectRatio, depth, params->mipmapLevel, params->gain, params->gamma, params->offset, params->recenterViewport, params->viewportCenter, params->isPartialRect);
    if (!params->isPartialRect && (originalImage || deviceFrame)) {
        uiContext->setLastRenderedDeepImage(params->textureIndex, params->mipmapLevel, deepImage);
    }

    if (!isDrawing) {
        uiContext->updateColorPicker(params->textureIndex);
    }
}

bool
ViewerInstance::isInputOptional(int n) const
{
    //activeInput() is MT-safe
    int activeInputs[2];

    getActiveInputs(activeInputs[0], activeInputs[1]);

    if ( (n == 0) && (activeInputs[0] == -1) && (activeInputs[1] == -1) ) {
        return false;
    }

    return n != activeInputs[0] && n != activeInputs[1];
}

void
ViewerInstance::onGammaChanged(double value)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    KnobBoolPtr enable = _imp->knobs.enableGamma.lock();
    KnobDoublePtr gamma = _imp->knobs.gamma.lock();
    if (!enable || !gamma || (getGamma() == value)) {
        return;
    }
    {
        // A gamma of 1 switches gamma off and keeps the last value for re-enabling.
        SetterKnobWrites guard(&_imp->knobWritesFromSetter);
        enable->setValue(value != 1.);
        if (value != 1.) {
            gamma->setValue(value);
        }
    }
    redrawOrRenderForColorChange();
}

double
ViewerInstance::getGamma() const
{
    KnobBoolPtr enable = _imp->knobs.enableGamma.lock();
    KnobDoublePtr gamma = _imp->knobs.gamma.lock();

    if (!enable || !gamma || !enable->getValue()) {
        return 1.;
    }

    return gamma->getValue();
}

void
ViewerInstance::onGainChanged(double exp)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    KnobBoolPtr enable = _imp->knobs.enableGain.lock();
    KnobDoublePtr gain = _imp->knobs.gain.lock();
    if (!enable || !gain || (getGain() == exp) || (exp <= 0.)) {
        return;
    }
    {
        // exp is the linear multiplier; the knob holds f-stops. A multiplier of 1 switches gain
        // off and keeps the last f-stop value for re-enabling.
        SetterKnobWrites guard(&_imp->knobWritesFromSetter);
        enable->setValue(exp != 1.);
        if (exp != 1.) {
            gain->setValue(std::log2(exp));
        }
    }
    redrawOrRenderForColorChange();
}

void
ViewerInstance::onMipmapLevelChanged(unsigned int level)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    KnobBoolPtr enable = _imp->knobs.enableDownscale.lock();
    KnobChoicePtr downscale = _imp->knobs.downscaleLevel.lock();
    if (!enable || !downscale || (getMipmapLevel() == level)) {
        return;
    }
    // Level 0 switches downscaling off and keeps the factor for re-enabling.
    SetterKnobWrites guard(&_imp->knobWritesFromSetter);
    enable->setValue(level > 0);
    if (level > 0) {
        downscale->setValue((int)std::min(level, 5u) - 1);
    }
}

void
ViewerInstance::setDisplayTransform(const std::string& display,
                                    const std::string& view,
                                    const std::string& look)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    KnobChoicePtr knobs[3] = { _imp->knobs.display.lock(), _imp->knobs.view.lock(), _imp->knobs.look.lock() };
    const std::string values[3] = { display, view, look };
    bool changed = false;
    {
        SetterKnobWrites guard(&_imp->knobWritesFromSetter);
        for (int i = 0; i < 3; ++i) {
            if (knobs[i] && (displayTransformName(knobs[i]) != values[i])) {
                selectDisplayTransformEntry(knobs[i], values[i]);
                changed = true;
            }
            if (i == 0) {
                // The view menu lists the views of the display just set.
                refreshDisplayTransformChoices();
            }
        }
    }
    if (changed) {
        redrawOrRenderForColorChange();
    }
}

void
ViewerInstance::setDisplayChannels(DisplayChannelsEnum channels,
                                   bool bothInputs)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );

    const int index = displayChannelsIndex(channels);
    bool changed = false;
    {
        SetterKnobWrites guard(&_imp->knobWritesFromSetter);
        for (int i = 0; i < (bothInputs ? 2 : 1); ++i) {
            KnobChoicePtr knob = _imp->knobs.displayChannels[i].lock();
            if (knob && (knob->getValue() != index)) {
                knob->setValue(index);
                changed = true;
            }
        }
    }
    if ( changed && !getApp()->getProject()->isLoadingProject() ) {
        renderCurrentFrame(true);
    }
}

void
ViewerInstance::setActiveLayer(const ImageLayerDesc& layer,
                               bool doRender)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    const std::string layerID = (layer.getNumComponents() == 0) ? std::string() : layerSelectionID(layer);
    bool changed = false;
    {
        QMutexLocker l(&_imp->viewerParamsMutex);
        if (_imp->viewerParamsLayer != layer) {
            _imp->viewerParamsLayer = layer;
            changed = true;
        }
        _imp->viewerParamsLayerID = layerID;
    }
    KnobLayerSelectPtr knob = _imp->knobs.layer.lock();
    if (knob && (knob->getLayer() != layerID)) {
        SetterKnobWrites guard(&_imp->knobWritesFromSetter);
        knob->setLayer(layerID);
    }
    if ( doRender && changed && !getApp()->getProject()->isLoadingProject() ) {
        renderCurrentFrame(true);
    }
}

ImageLayerDesc
ViewerInstance::getActiveLayer() const
{
    QMutexLocker l(&_imp->viewerParamsMutex);

    return _imp->viewerParamsLayer;
}

void
ViewerInstance::setAlphaChannel(const ImageLayerDesc& layer,
                                const std::string& channelName,
                                bool doRender)
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    const bool none = (layer.getNumComponents() == 0) || channelName.empty();
    const std::string alphaID = none ? std::string() : layerSelectionID(layer) + "." + channelName;
    bool changed = false;
    {
        QMutexLocker l(&_imp->viewerParamsMutex);
        if (_imp->viewerParamsAlphaLayer != layer) {
            _imp->viewerParamsAlphaLayer = layer;
            changed = true;
        }
        if (_imp->viewerParamsAlphaChannelName != channelName) {
            _imp->viewerParamsAlphaChannelName = channelName;
            changed = true;
        }
        _imp->viewerParamsAlphaID = alphaID;
    }
    KnobChannelSelectPtr knob = _imp->knobs.alphaChannel.lock();
    if (knob && (knob->get() != alphaID)) {
        SetterKnobWrites guard(&_imp->knobWritesFromSetter);
        knob->set(alphaID);
    }
    if ( changed && doRender && !getApp()->getProject()->isLoadingProject() ) {
        renderCurrentFrame(true);
    }
}

void
ViewerInstance::disconnectViewer()
{
    // always running in the render thread
    if (_imp->uiContext) {
        Q_EMIT viewerDisconnected();
    }
}

void
ViewerInstance::disconnectViewerAfterFailedRender(const U64 renderAges[2],
                                                  const bool failed[2])
{
    if (!_imp->uiContext) {
        return;
    }
    const U64 ages[2] = { renderAges[0], renderAges[1] };
    const bool failedInput[2] = { failed[0], failed[1] };
    auto disconnectUnlessNewerShown = [this, ages, failedInput]() {
        for (int i = 0; i < 2; ++i) {
            if (failedInput[i] && !_imp->checkAgeNoUpdate(i, ages[i])) {
                return;
            }
        }
        disconnectViewer();
    };
    QMetaObject::invokeMethod(this, disconnectUnlessNewerShown, Qt::AutoConnection);
}

void
ViewerInstance::disconnectTextureAfterRender(int index,
                                             bool clearRod,
                                             U64 renderAge)
{
    if (!_imp->uiContext) {
        return;
    }
    auto disconnectUnlessNewerShown = [this, index, clearRod, renderAge]() {
        if (_imp->checkAgeNoUpdate(index, renderAge)) {
            executeDisconnectTextureRequestOnMainThread(index, clearRod);
        }
    };
    QMetaObject::invokeMethod(this, disconnectUnlessNewerShown, Qt::AutoConnection);
}

void
ViewerInstance::disconnectTexture(int index,bool clearRod)
{
    if (_imp->uiContext) {
        Q_EMIT disconnectTextureRequest(index, clearRod);
    }
}

void
ViewerInstance::redrawViewer()
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    if (!_imp->uiContext) {
        return;
    }
    _imp->uiContext->redraw();
}

void
ViewerInstance::redrawViewerNow()
{
    // always running in the main thread
    assert( qApp && qApp->thread() == QThread::currentThread() );
    assert(_imp->uiContext);
    _imp->uiContext->redrawNow();
}

static std::string
firstListedName(const std::vector<std::string>& names,
                const std::string& a,
                const std::string& b,
                const std::string& c)
{
    const std::string* candidates[] = { &a, &b, &c };
    for (const std::string* name : candidates) {
        if (!name->empty() && (std::find(names.begin(), names.end(), *name) != names.end())) {
            return *name;
        }
    }

    return names.empty() ? std::string() : names.front();
}

void
ViewerInstance::getResolvedDisplayTransform(std::string* display,
                                            std::string* view,
                                            std::string* look) const
{
    const std::string savedDisplay = displayTransformName(_imp->knobs.display.lock());
    const std::string savedView = displayTransformName(_imp->knobs.view.lock());
    const std::string savedLook = displayTransformName(_imp->knobs.look.lock());
    AppInstancePtr app = getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    ProjectColorManagementPtr colorManagement = project ? project->getColorManagement() : ProjectColorManagementPtr();
    std::string resolvedDisplay, resolvedView, resolvedLook;

    if (colorManagement) {
        std::string projectDisplay, projectView;
        project->getDefaultDisplayView(&projectDisplay, &projectView);
        resolvedDisplay = firstListedName(colorManagement->getDisplays(), savedDisplay, projectDisplay, colorManagement->getDefaultDisplay());
        resolvedView = firstListedName(colorManagement->getViews(resolvedDisplay), savedView, projectView, colorManagement->getDefaultView(resolvedDisplay));
        const std::vector<std::string> looks = colorManagement->getLooks();
        if (std::find(looks.begin(), looks.end(), savedLook) != looks.end()) {
            resolvedLook = savedLook;
        }
    }
    if (display) {
        *display = resolvedDisplay;
    }
    if (view) {
        *view = resolvedView;
    }
    if (look) {
        *look = resolvedLook;
    }
}

void
ViewerInstance::refreshDisplayTransformChoices()
{
    KnobChoicePtr displayKnob = _imp->knobs.display.lock();
    KnobChoicePtr viewKnob = _imp->knobs.view.lock();
    KnobChoicePtr lookKnob = _imp->knobs.look.lock();
    if (!displayKnob || !viewKnob || !lookKnob) {
        return;
    }
    AppInstancePtr app = getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    ProjectColorManagementPtr colorManagement = project ? project->getColorManagement() : ProjectColorManagementPtr();
    std::vector<std::string> displays, views, looks;
    std::string defaultDisplay, defaultView, resolvedDisplay;
    if (colorManagement) {
        std::string projectDisplay, projectView;
        project->getDefaultDisplayView(&projectDisplay, &projectView);
        displays = colorManagement->getDisplays();
        defaultDisplay = firstListedName(displays, std::string(), projectDisplay, colorManagement->getDefaultDisplay());
        getResolvedDisplayTransform(&resolvedDisplay, NULL, NULL);
        views = colorManagement->getViews(resolvedDisplay);
        defaultView = firstListedName(views, std::string(), projectView, colorManagement->getDefaultView(resolvedDisplay));
        looks = colorManagement->getLooks();
    }

    std::vector<ChoiceOption> displayEntries(1, OcioSpaceKnobs::projectDefaultOption(defaultDisplay, tr("The project's default display.")));
    for (const std::string& name : displays) {
        displayEntries.push_back(ChoiceOption(name, name, std::string()));
    }
    std::vector<ChoiceOption> viewEntries(1, OcioSpaceKnobs::projectDefaultOption(defaultView, tr("The project's default view of the display.")));
    for (const std::string& name : views) {
        viewEntries.push_back(ChoiceOption(name, name, std::string()));
    }
    std::vector<ChoiceOption> lookEntries(1, ChoiceOption(kViewerLookNoneID, tr("None").toStdString(), tr("No look.").toStdString()));
    for (const std::string& name : looks) {
        lookEntries.push_back(ChoiceOption(name, name, std::string()));
    }
    displayKnob->populateChoices(displayEntries);
    viewKnob->populateChoices(viewEntries);
    lookKnob->populateChoices(lookEntries);
}

void
ViewerInstance::onProjectDisplayTransformChanged()
{
    refreshDisplayTransformChoices();
    redrawOrRenderForColorChange();
}

void
ViewerInstance::getDisplayTransform(std::string* display,
                                    std::string* view,
                                    std::string* look) const
{
    if (display) {
        *display = displayTransformName(_imp->knobs.display.lock());
    }
    if (view) {
        *view = displayTransformName(_imp->knobs.view.lock());
    }
    if (look) {
        *look = displayTransformName(_imp->knobs.look.lock());
    }
}

void
ViewerInstance::selectDisplayTransformEntry(const KnobChoicePtr& knob,
                                            const std::string& name)
{
    if (!knob || (knob->getNumEntries() == 0)) {
        return;
    }
    ChoiceOption entry(name, name, std::string());
    int index = 0;
    if (name.empty()) {
        entry = knob->getEntries_mt_safe().front();
    } else {
        index = knob->findEntry(name, &entry);
    }
    const int previous = knob->getValue();
    if ((knob->getActiveEntry().id == entry.id) && ((index < 0) || (index == previous))) {
        return;
    }
    if ((index >= 0) && (index != previous)) {
        // While the viewer renders, setValue() is queued: the index reads the new value at once but
        // the active entry would follow only at dequeue, and a menu refresh in between would restore
        // the old entry over the queued one.
        knob->setActiveEntry(entry);
        knob->setValue(index);
    } else {
        knob->setActiveEntryAndNotify(entry, eValueChangedReasonPluginEdited);
    }
}

void
ViewerInstance::fillUnsetUserRoI()
{
    KnobDoublePtr bottomLeft = _imp->knobs.userRoIBottomLeft.lock();
    KnobDoublePtr size = _imp->knobs.userRoISize.lock();
    if (!bottomLeft || !size) {
        return;
    }
    // A zero size is an RoI that shows nothing: only knobs still at their all-zero defaults hold
    // none. The values, not hasModifications(), which a bracket of edits updates only at its end.
    for (int i = 0; i < 2; ++i) {
        if ((bottomLeft->getValue(i) != 0.) || (size->getValue(i) != 0.)) {
            return;
        }
    }
    Format format;
    AppInstancePtr app = getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    if (project) {
        project->getProjectDefaultFormat(&format);
    }
    const RectD canonical = format.toCanonicalFormat();
    const double w = canonical.width();
    const double h = canonical.height();
    const RectD roi(canonical.x1 + w / 4., canonical.y1 + h / 4., canonical.x2 - w / 4., canonical.y2 - h / 4.);
    if (roi.isNull()) {
        return;
    }
    bottomLeft->setValue(roi.x1, ViewSpec::all(), 0);
    bottomLeft->setValue(roi.y1, ViewSpec::all(), 1);
    size->setValue(roi.width(), ViewSpec::all(), 0);
    size->setValue(roi.height(), ViewSpec::all(), 1);
}

ProjectColorManagement::DisplayProcessorPtr
ViewerInstance::getDisplayProcessor() const
{
    AppInstancePtr app = getApp();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    ProjectColorManagementPtr colorManagement = project ? project->getColorManagement() : ProjectColorManagementPtr();
    if (!colorManagement) {
        return ProjectColorManagement::DisplayProcessorPtr();
    }
    std::string display, view, look;
    getResolvedDisplayTransform(&display, &view, &look);

    return colorManagement->getDisplayProcessor(project->getWorkingColorSpace(), display, view, look);
}

double
ViewerInstance::getGain() const
{
    // MT-SAFE: called from main thread and Serialization (pooled) thread
    KnobBoolPtr enable = _imp->knobs.enableGain.lock();
    KnobDoublePtr gain = _imp->knobs.gain.lock();

    if (!enable || !gain || !enable->getValue()) {
        return 1.;
    }

    return std::pow(2., gain->getValue());
}

unsigned int
ViewerInstance::getMipmapLevel() const
{
    // MT-SAFE: called from main thread and Serialization (pooled) thread
    KnobBoolPtr enable = _imp->knobs.enableDownscale.lock();
    KnobChoicePtr downscale = _imp->knobs.downscaleLevel.lock();

    if (!enable || !downscale || !enable->getValue()) {
        return 0;
    }

    return (unsigned int)std::max(downscale->getValue(), 0) + 1;
}

DisplayChannelsEnum
ViewerInstance::getChannels(int texIndex) const
{
    // MT-SAFE: called from main thread and Serialization (pooled) thread
    KnobChoicePtr knob = _imp->knobs.displayChannels[texIndex].lock();

    return knob ? displayChannelsAt(knob->getValue()) : eDisplayChannelsRGB;
}

void
ViewerInstance::setFullFrameProcessingEnabled(bool fullFrame)
{
    KnobBoolPtr knob = _imp->knobs.fullFrame.lock();
    if (!knob || (knob->getValue() == fullFrame)) {
        return;
    }
    {
        SetterKnobWrites guard(&_imp->knobWritesFromSetter);
        knob->setValue(fullFrame);
    }
    if ( !getApp()->getProject()->isLoadingProject() ) {
        renderCurrentFrame(true);
    }
}

bool
ViewerInstance::isFullFrameProcessingEnabled() const
{
    KnobBoolPtr knob = _imp->knobs.fullFrame.lock();

    return knob ? knob->getValue() : false;
}

void
ViewerInstance::addAcceptedComponents(int /*inputNb*/,
                                      std::list<ImageLayerDesc>* comps)
{
    ///Viewer only supports RGBA for now.
    comps->push_back(ImageLayerDesc::getRGBAComponents());
    comps->push_back(ImageLayerDesc::getRGBComponents());
    comps->push_back(ImageLayerDesc::getAlphaComponents());
}

ViewIdx
ViewerInstance::getViewerCurrentView() const
{
    return _imp->uiContext ? _imp->uiContext->getCurrentView() : ViewIdx(0);
}

void
ViewerInstance::setViewerPaused(bool paused,
                                bool allInputs)
{
    QMutexLocker k(&_imp->isViewerPausedMutex);

    _imp->isViewerPaused[0] = paused;
    if (allInputs) {
        _imp->isViewerPaused[1] = paused;
    }
}

bool
ViewerInstance::isViewerPaused(int texIndex) const
{
    QMutexLocker k(&_imp->isViewerPausedMutex);

    return _imp->isViewerPaused[texIndex];
}

void
ViewerInstance::onInputChanged(int /*inputNb*/)
{
    refreshResolvedLayers();
    refreshActiveInputDataKind();
    Q_EMIT clipPreferencesChanged();
}

void
ViewerInstance::onCurrentFrameMetadataChanged(const NodeMetadata& /*metadata*/)
{
    // Also the first call once a loaded project is complete, when every input is in place.
    refreshActiveInputDataKind();
    Q_EMIT clipPreferencesChanged();
}

static DataKindEnum
viewerInputDataKind(const ViewerInstance* viewer,
                    int inputNb)
{
    if (inputNb < 0) {
        return eDataKindImage;
    }
    EffectInstancePtr input = viewer->getInput(inputNb);
    NodePtr inputNode = input ? input->getNode() : NodePtr();
    if (!inputNode) {
        return eDataKindImage;
    }
    bool isAmbiguous = false;
    const DataKindEnum kind = inputNode->getEffectiveOutputDataKind(&isAmbiguous);

    return isAmbiguous ? eDataKindPolymorphic : kind;
}

DataKindEnum
ViewerInstance::getInputDataKindForTexture(int textureIndex) const
{
    int a = -1, b = -1;

    getActiveInputs(a, b);

    return viewerInputDataKind(this, textureIndex == 0 ? a : b);
}

DataKindEnum
ViewerInstance::getActiveInputDataKind() const
{
    return getInputDataKindForTexture(0);
}

bool
ViewerInstance::isWipeDisabledByDataKind() const
{
    return (getInputDataKindForTexture(0) == eDataKindScene) || (getInputDataKindForTexture(1) == eDataKindScene);
}

bool
ViewerInstance::getLastEvaluatedScene(int textureIndex,
                                      ScenePayload* scene,
                                      std::string* error,
                                      SceneLoadSetPtr* loadSet,
                                      SceneLoadReachPtr* reach) const
{
    if ((textureIndex != 0) && (textureIndex != 1)) {
        return false;
    }
    std::lock_guard<std::mutex> k(_imp->lastSceneMutex);
    if (scene) {
        *scene = _imp->lastScene[textureIndex];
    }
    if (error) {
        *error = _imp->lastSceneError[textureIndex];
    }
    if (loadSet) {
        *loadSet = _imp->lastSceneLoadSet[textureIndex];
    }
    if (reach) {
        *reach = _imp->lastSceneReach[textureIndex];
    }

    return _imp->lastSceneOk[textureIndex];
}

void
ViewerInstance::setSceneLoadSet(const SceneLoadSetPtr& loadSet)
{
    bool changed;
    {
        std::lock_guard<std::mutex> k(_imp->sceneLoadSetMutex);
        changed = _imp->sceneLoadSet ? (!loadSet || (*_imp->sceneLoadSet != *loadSet)) : bool(loadSet);
        _imp->sceneLoadSet = loadSet;
    }
    if (changed) {
        SceneMenuRefresh::scheduleFollowLoadChanges();
    }
}

SceneLoadSetPtr
ViewerInstance::getSceneLoadSet() const
{
    std::lock_guard<std::mutex> k(_imp->sceneLoadSetMutex);

    return _imp->sceneLoadSet;
}

SceneEvaluationContext
ViewerInstance::getSceneEvaluationContext() const
{
    return SceneEvaluationContext(_imp->sceneContextId, getSceneLoadSet());
}

bool
ViewerInstance::evaluateSceneForLoadSet(const SceneLoadSetPtr& loadSet,
                                        ScenePayload* scene,
                                        SceneLoadReachPtr* reach,
                                        std::string* error)
{
    assert(scene);
    *scene = ScenePayload();
    int activeA, activeB;
    getActiveInputs(activeA, activeB);
    const EffectInstancePtr input = getInput(activeA);
    if (!input || !input->producesSceneData()) {
        return false;
    }
    const SceneEvaluationContext context(_imp->sceneContextId, loadSet, std::make_shared<SceneLoadReach>());
    std::string evaluationError;
    EffectInstance::RenderRoIRetCode code;
    try {
        // The view only decides identities, and the viewer's widget is not read off the main thread.
        code = input->evaluateScene(EffectInstance::EvaluateSceneArgs(getCurrentTime(), ViewIdx(0), context), scene, &evaluationError);
    } catch (const std::exception& e) {
        code = EffectInstance::eRenderRoIRetCodeFailed;
        evaluationError = e.what();
    } catch (...) {
        code = EffectInstance::eRenderRoIRetCodeFailed;
    }
    if (code != EffectInstance::eRenderRoIRetCodeOk) {
        *scene = ScenePayload();
        if (error) {
            *error = evaluationError;
        }

        return false;
    }
    if (reach) {
        *reach = context.reach;
    }

    return true;
}

void
ViewerInstance::refreshActiveInputDataKind()
{
    if (!qApp || (QThread::currentThread() != qApp->thread())) {
        QMetaObject::invokeMethod(this, "refreshActiveInputDataKind", Qt::QueuedConnection);

        return;
    }
    if (!getNode()) {
        return;
    }

    const DataKindEnum kinds[2] = { getInputDataKindForTexture(0), getInputDataKindForTexture(1) };
    const bool wipeDisabled = (kinds[0] == eDataKindScene) || (kinds[1] == eDataKindScene);
    {
        std::lock_guard<std::mutex> k(_imp->lastSceneMutex);
        for (int i = 0; i < 2; ++i) {
            if (kinds[i] != eDataKindScene) {
                _imp->lastScene[i] = ScenePayload();
                _imp->lastSceneError[i].clear();
                _imp->lastSceneOk[i] = false;
                _imp->lastSceneLoadSet[i].reset();
                _imp->lastSceneReach[i].reset();
            }
        }
    }

    if ((kinds[0] == _imp->notifiedInputDataKind) && (wipeDisabled == _imp->notifiedWipeDisabled)) {
        return;
    }
    _imp->notifiedInputDataKind = kinds[0];
    _imp->notifiedWipeDisabled = wipeDisabled;
    Q_EMIT activeInputDataKindChanged();
}

void
ViewerInstance::onChannelsSelectorRefreshed()
{
    Q_EMIT availableComponentsChanged();
}

void
ViewerInstance::addSupportedBitDepth(std::list<ImageBitDepthEnum>* depths) const
{
    depths->push_back(eImageBitDepthFloat);
    depths->push_back(eImageBitDepthShort);
    depths->push_back(eImageBitDepthByte);
}

void
ViewerInstance::getActiveInputs(int & a,
                                int &b) const
{
    NodePtr n = getNode();
    InspectorNode* isInspector = dynamic_cast<InspectorNode*>( n.get() );

    assert(isInspector);
    if (isInspector) {
        isInspector->getActiveInputs(a, b);
    }
}

void
ViewerInstance::getUpstreamNodesWithPersistentMessage(NodesList* nodes) const
{
    NodePtr viewerNode = getNode();
    if (!viewerNode) {
        return;
    }
    int activeInputs[2] = { -1, -1 };
    getActiveInputs(activeInputs[0], activeInputs[1]);

    NodesList roots;
    for (int i = 0; i < 2; ++i) {
        if (activeInputs[i] < 0) {
            continue;
        }
        roots.push_back(viewerNode->getRealInput(activeInputs[i]));
        roots.push_back(viewerNode->getInput(activeInputs[i]));
    }
    Node::getNodesWithPersistentMessageUpstream(roots, nodes);
}

void
ViewerInstance::setInputA(int inputNb)
{
    if (KnobIntPtr knob = _imp->knobs.inputs[0].lock()) {
        SetterKnobWrites guard(&_imp->knobWritesFromSetter);
        knob->setValue(inputNb);
    }
    applyActiveInput(0, inputNb);
}

void
ViewerInstance::setInputB(int inputNb)
{
    if (KnobIntPtr knob = _imp->knobs.inputs[1].lock()) {
        SetterKnobWrites guard(&_imp->knobWritesFromSetter);
        knob->setValue(inputNb);
    }
    applyActiveInput(1, inputNb);
}

int
ViewerInstance::getLastRenderedTime() const
{
    return _imp->uiContext ? _imp->uiContext->getCurrentlyDisplayedTime() : getApp()->getTimeLine()->currentFrame();
}

TimeLinePtr
ViewerInstance::getTimeline() const
{
    return _imp->uiContext ? _imp->uiContext->getTimeline() : getApp()->getTimeLine();
}

void
ViewerInstance::getTimelineBounds(int* first,
                                  int* last) const
{
    if (_imp->uiContext) {
        _imp->uiContext->getViewerFrameRange(first, last);
    } else {
        *first = 0;
        *last = 0;
    }
}

unsigned int
ViewerInstance::getMipmapLevelWithoutDraft() const
{
    if (isFullFrameProcessingEnabled()) {
        return 0;
    }
    const unsigned int proxyLevel = getMipmapLevel();

    // The nearest mipmap level higher in quality than the zoom factor: 48% renders at 50%
    // (level 1), 51% at 100% (level 0).
    double zoomFactor = _imp->uiContext->getZoomFactor();
    double closestPowerOf2 = zoomFactor >= 1 ? 1 : ipow(2, (int)-std::ceil(std::log(zoomFactor) / M_LN2));
    unsigned int zoomMipmapLevel = std::log(closestPowerOf2) / M_LN2;

    return std::max(proxyLevel, zoomMipmapLevel);
}

unsigned int
ViewerInstance::getMipmapLevelFromZoomFactor() const
{
    double zoomFactor = _imp->uiContext->getZoomFactor();
    double closestPowerOf2 = zoomFactor >= 1 ? 1 : std::pow( 2, -std::ceil(std::log(zoomFactor) / M_LN2) );

    return std::log(closestPowerOf2) / M_LN2;
}

double
ViewerInstance::getCurrentTime() const
{
    return getFrameRenderArgsCurrentTime();
}

ViewIdx
ViewerInstance::getCurrentView() const
{
    return getFrameRenderArgsCurrentView();
}

bool
ViewerInstance::isLatestRender(int textureIndex,
                               U64 renderAge) const
{
    return _imp->isLatestRender(textureIndex, renderAge);
}

void
ViewerInstance::setPartialUpdateParams(const std::list<RectD>& rois,
                                       bool recenterViewer)
{
    double viewerCenterX = 0;
    double viewerCenterY = 0;

    if (recenterViewer) {
        RectD bbox;
        bool bboxSet = false;
        for (std::list<RectD>::const_iterator it = rois.begin(); it != rois.end(); ++it) {
            if (!bboxSet) {
                bboxSet = true;
                bbox = *it;
            } else {
                bbox.merge(*it);
            }
        }
        viewerCenterX = (bbox.x1 + bbox.x2) / 2.;
        viewerCenterY = (bbox.y1 + bbox.y2) / 2.;
    }
    QMutexLocker k(&_imp->viewerParamsMutex);
    _imp->partialUpdateRects = rois;
    _imp->viewportCenterSet = recenterViewer;
    _imp->viewportCenter.x = viewerCenterX;
    _imp->viewportCenter.y = viewerCenterY;
}

void
ViewerInstance::clearPartialUpdateParams()
{
    QMutexLocker k(&_imp->viewerParamsMutex);

    _imp->partialUpdateRects.clear();
    _imp->viewportCenterSet = false;
}

void
ViewerInstance::setDoingPartialUpdates(bool doing)
{
    QMutexLocker k(&_imp->viewerParamsMutex);

    _imp->isDoingPartialUpdates = doing;
}

bool
ViewerInstance::isDoingPartialUpdates() const
{
    QMutexLocker k(&_imp->viewerParamsMutex);

    return _imp->isDoingPartialUpdates;
}

void
ViewerInstance::reportStats(int time,
                            ViewIdx view,
                            double wallTime,
                            const RenderStatsMap& stats,
                            const RenderStatsPtr& /*renderStats*/)
{
    Q_EMIT renderStatsAvailable(time, view, wallTime, stats);
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_ViewerInstance.cpp"
#include "moc_ViewerInstancePrivate.cpp"
