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

#include "GeoSolverEnd.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <optional>
#include <set>
#include <utility>

#include <QtCore/QCoreApplication>
#include <QtCore/QString>
#include <QtCore/QThread>

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Cache.h"
#include "Engine/ContentHasher.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Loop/GeoForEachEnd.h"
#include "Engine/Nodes/Loop/GeoSolverBegin.h"
#include "Engine/Nodes/Loop/LoopBegin.h"
#include "Engine/Nodes/Loop/LoopEnd.h"
#include "Engine/Nodes/Loop/LoopEndCooks.h"

NATRON_NAMESPACE_ENTER

namespace {

const LoopEndKind&
solverKind()
{
    static const LoopEndKind kind = { &GeoSolverBegin::isGeoSolverBegin, &GeoSolverEnd::isGeoSolverEnd };

    return kind;
}

std::string
spillParent()
{
    return (appPTR->getDiskCacheLocation() + QString::fromUtf8("/GeoSolver")).toStdString();
}

} // anonymous namespace

GeoSolverEnd::GeoSolverEnd(NodePtr node)
    : NativeEffectBase(node)
    , _pairing(solverKind())
    , _simCache(new GeoSimCache((std::size_t)kGeoSolverEndDefaultCacheMemoryMB << 20))
{
}

GeoSolverEnd::~GeoSolverEnd()
{
}

const NodeSpec*
GeoSolverEnd::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

bool
GeoSolverEnd::isGeoSolverEnd(const NodePtr& node)
{
    return node && (node->getPluginID() == PLUGINID_NATRON_GEOSOLVEREND);
}

std::shared_ptr<GeoSolverEnd>
GeoSolverEnd::pairedEndOf(const NodePtr& begin)
{
    const NodePtr end = loopPairedEnd(solverKind(), begin);

    return end ? std::dynamic_pointer_cast<GeoSolverEnd>(end->getEffectInstance()) : std::shared_ptr<GeoSolverEnd>();
}

std::vector<NodePtr>
GeoSolverEnd::getBegins() const
{
    return _pairing.getBegins(getNode());
}

std::string
GeoSolverEnd::getLoopName() const
{
    KnobStringPtr name = _loopName.lock();
    const std::string value = name ? name->getValue() : std::string();

    return value.empty() ? getScriptName_mt_safe() : value;
}

RenderContextName
GeoSolverEnd::hiddenVariable(const std::string& loopName)
{
    // One use: a substep's body output is never asked for again, so the geometry cache does not keep it.
    return RenderContextName::oneUse("geoSolver." + loopName, kGeoSolverHiddenPrevious);
}

const std::vector<std::string>&
GeoSolverEnd::loopVariables()
{
    static const std::vector<std::string> names = { "iteration", "numiterations", "count", "t", "value", "frame", "substep", "numsubsteps", "dt" };

    return names;
}

GeoSolverEnd::Settings
GeoSolverEnd::readSettings() const
{
    Settings s;
    KnobIntPtr startFrame = _startFrame.lock();
    KnobIntPtr substeps = _substeps.lock();
    KnobBoolPtr cacheSimulation = _cacheSimulation.lock();
    KnobIntPtr cacheMemory = _cacheMemory.lock();
    KnobBoolPtr spillToDisk = _spillToDisk.lock();

    if (startFrame) {
        s.startFrame = startFrame->getValue();
    }
    if (substeps) {
        s.substeps = std::max(1, substeps->getValue());
    }
    if (cacheSimulation) {
        s.cacheSimulation = cacheSimulation->getValue();
    }
    s.cacheBytes = (std::size_t)std::max(0, cacheMemory ? cacheMemory->getValue() : kGeoSolverEndDefaultCacheMemoryMB) << 20;
    if (spillToDisk) {
        s.spillToDisk = spillToDisk->getValue();
    }

    return s;
}

void
GeoSolverEnd::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls").toStdString());

    KnobIntPtr startFrame = createKnob<KnobInt>(tr("Start Frame").toStdString());
    startFrame->setName(kGeoSolverEndParamStartFrame);
    startFrame->setAnimationEnabled(false);
    startFrame->setHintToolTip(tr("The frame the simulation starts at. Up to it the output is the input of the Prev Frame "
                                  "GeoSolverBegin; the frame after is its first solved frame.")
                                   .toStdString());
    startFrame->setDefaultValue(1);
    page->addKnob(startFrame);
    _startFrame = startFrame;

    KnobIntPtr substeps = createKnob<KnobInt>(tr("Substeps").toStdString());
    substeps->setName(kGeoSolverEndParamSubsteps);
    substeps->setAnimationEnabled(false);
    substeps->setHintToolTip(tr("How many times the body cooks per frame. Each substep covers loop.dt = 1 / Substeps of a frame.").toStdString());
    substeps->setMinimum(1);
    substeps->setDisplayMinimum(1);
    substeps->setDisplayMaximum(10);
    substeps->setDefaultValue(1);
    page->addKnob(substeps);
    _substeps = substeps;

    KnobButtonPtr resetSimulation = createKnob<KnobButton>(tr("Reset Simulation").toStdString());
    resetSimulation->setName(kGeoSolverEndParamResetSimulation);
    resetSimulation->setHashExempt(true);
    resetSimulation->setHintToolTip(tr("Forgets every solved frame, so the simulation solves again from Start Frame.").toStdString());
    page->addKnob(resetSimulation);
    _resetSimulation = resetSimulation;

    KnobBoolPtr cacheSimulation = createKnob<KnobBool>(tr("Cache Simulation").toStdString());
    cacheSimulation->setName(kGeoSolverEndParamCacheSimulation);
    cacheSimulation->setAnimationEnabled(false);
    cacheSimulation->setHashExempt(true);
    cacheSimulation->setHintToolTip(tr("Keeps every solved frame, so playing back or scrubbing does not solve it again. Off, only "
                                       "the last solved frame is kept, and going back solves again from Start Frame.")
                                        .toStdString());
    cacheSimulation->setDefaultValue(true);
    page->addKnob(cacheSimulation);
    _cacheSimulation = cacheSimulation;

    KnobIntPtr cacheMemory = createKnob<KnobInt>(tr("Cache Memory").toStdString());
    cacheMemory->setName(kGeoSolverEndParamCacheMemory);
    cacheMemory->setAnimationEnabled(false);
    cacheMemory->setHashExempt(true);
    cacheMemory->setHintToolTip(tr("The RAM the solved frames of this node may hold, in MB. Past it the least recently used "
                                   "go to disk, or are dropped when Spill To Disk is off.")
                                    .toStdString());
    cacheMemory->setMinimum(0);
    cacheMemory->setDisplayMinimum(0);
    cacheMemory->setDisplayMaximum(16384);
    cacheMemory->setDefaultValue(kGeoSolverEndDefaultCacheMemoryMB);
    page->addKnob(cacheMemory);
    _cacheMemory = cacheMemory;

    KnobBoolPtr spillToDisk = createKnob<KnobBool>(tr("Spill To Disk").toStdString());
    spillToDisk->setName(kGeoSolverEndParamSpillToDisk);
    spillToDisk->setAnimationEnabled(false);
    spillToDisk->setHashExempt(true);
    spillToDisk->setHintToolTip(tr("Writes the solved frames that do not fit in Cache Memory to the disk cache directory, and "
                                   "reads them back when asked for. Off, they are dropped and solved again.")
                                    .toStdString());
    spillToDisk->setDefaultValue(true);
    page->addKnob(spillToDisk);
    _spillToDisk = spillToDisk;

    KnobStringPtr loopName = createKnob<KnobString>(tr("Loop Name").toStdString());
    loopName->setName(kGeoSolverEndParamLoopName);
    loopName->setAnimationEnabled(false);
    loopName->setHintToolTip(tr("The name of the loops.<name>.* variables this solver sets, so a loop inside it can still "
                                "read this one's. Empty uses the node's script name.")
                                 .toStdString());
    loopName->setDefaultValue(kLoopBlockDefaultName);
    page->addKnob(loopName);
    _loopName = loopName;

    KnobStringPtr drivenKnobs = createKnob<KnobString>(tr("Driven Knobs").toStdString());
    drivenKnobs->setName(kLoopBlockParamDrivenKnobs);
    drivenKnobs->setAnimationEnabled(false);
    drivenKnobs->setIsPersistent(false);
    drivenKnobs->setHashExempt(true);
    drivenKnobs->setHintToolTip(tr("The parameters of the solver's body that a loop variable drives: each row's "
                                   "variable, Start, End and Easing can be edited in place. Right-click a "
                                   "parameter in the body and choose Drive from loop to add one.")
                                    .toStdString());
    page->addKnob(drivenKnobs);
} // GeoSolverEnd::initializeKnobs

bool
GeoSolverEnd::knobChanged(KnobI* k,
                          ValueChangedReasonEnum /*reason*/,
                          ViewSpec /*view*/,
                          double /*time*/,
                          bool /*originatedFromMainThread*/)
{
    KnobButtonPtr reset = _resetSimulation.lock();
    if (reset && (k == reset.get())) {
        resetSimulation();

        return true;
    }
    KnobStringPtr loopName = _loopName.lock();
    if (loopName && (k == loopName.get())) {
        // The variables the body and the GeoSolverBegins read move with it.
        invalidateRenderContextReads();

        return true;
    }
    const KnobIPtr cache[] = { _cacheSimulation.lock(), _cacheMemory.lock(), _spillToDisk.lock() };
    for (const KnobIPtr& knob : cache) {
        if (knob && (k == knob.get())) {
            const Settings s = readSettings();
            _simCache->setBudget(s.cacheBytes);
            _simCache->setSpillParent(s.spillToDisk ? spillParent() : std::string());

            return true;
        }
    }

    return false;
}

void
GeoSolverEnd::onScriptNameChanged(const std::string& /*fullyQualifiedName*/)
{
    // An empty Loop Name follows the script name.
    invalidateRenderContextReads();
}

void
GeoSolverEnd::onEffectCreated(bool /*mayCreateFileDialog*/,
                              const CreateNodeArgs& /*args*/)
{
    LoopBlocks::assignFreeName(this);
}

std::string
GeoSolverEnd::getBlockLoopName() const
{
    return getLoopName();
}

std::vector<NodePtr>
GeoSolverEnd::getBlockBegins() const
{
    return getBegins();
}

KnobStringPtr
GeoSolverEnd::getBlockNameKnob() const
{
    return _loopName.lock();
}

const std::vector<std::string>&
GeoSolverEnd::getBlockVariables() const
{
    return loopVariables();
}

RenderContextPtr
GeoSolverEnd::getBlockPreviewContext(const RenderContextPtr& base,
                                     double time) const
{
    const Settings s = readSettings();
    const int frame = std::max(s.startFrame + 1, (int)std::floor(time));

    return substepContext(base ? base : RenderContext::empty(), getLoopName(), s.startFrame, frame, s.substeps - 1, s.substeps, GeoDetailConstPtr());
}

std::map<int, bool>
GeoSolverEnd::getKeptFrames() const
{
    GeoSimSeries series;
    {
        QMutexLocker locker(&_outputMutex);
        if (!_hasOutput) {
            return std::map<int, bool>();
        }
        series = _outputSeries;
    }

    return _simCache->frames(series);
}

std::vector<std::shared_ptr<GeoSolverEnd>>
GeoSolverEnd::upstreamOf(const NodePtr& node)
{
    std::vector<std::shared_ptr<GeoSolverEnd>> solvers;
    std::set<Node*> visited;
    std::vector<NodePtr> stack;

    if (node) {
        stack.push_back(node);
    }
    while (!stack.empty()) {
        const NodePtr current = stack.back();
        stack.pop_back();
        if (!current || !visited.insert(current.get()).second) {
            continue;
        }
        const std::shared_ptr<GeoSolverEnd> solver = std::dynamic_pointer_cast<GeoSolverEnd>(current->getEffectInstance());
        if (solver) {
            solvers.push_back(solver);
        }
        for (int i = 0; i < current->getNInputs(); ++i) {
            stack.push_back(current->getInput(i));
        }
    }

    return solvers;
}

bool
GeoSolverEnd::outputKeptFrame(int frame) const
{
    GeoSimSeries series;
    {
        QMutexLocker locker(&_outputMutex);
        if (!_hasOutput) {
            return false;
        }
        if (_outputFrame == frame) {
            return _outputKept;
        }
        series = _outputSeries;
    }
    // Shown again from the geometry cache without asking this node: kept, as it was solved before.
    const std::map<int, bool> kept = _simCache->frames(series);

    return kept.count(frame) > 0;
}

void
GeoSolverEnd::recordOutput(const GeoSimSeries& series,
                           int frame,
                           bool kept)
{
    QMutexLocker locker(&_outputMutex);

    _outputSeries = series;
    _outputFrame = frame;
    _outputKept = kept;
    _hasOutput = true;
}

void
GeoSolverEnd::notifyKeptFramesChanged(int frame) const
{
    if (!appPTR || appPTR->isBackground()) {
        return;
    }
    const CacheSignalEmitterPtr emitter = appPTR->getOrActivateViewerCacheSignalEmitter();
    if (emitter) {
        // The timelines rebuild their whole cache line on any entry signal.
        emitter->emitAddedEntry(frame);
    }
}

void
GeoSolverEnd::resetSimulation()
{
    _simCache->clear();
    notifyKeptFramesChanged(0);
    // The output at every frame changes, though no knob that is hashed did.
    ++_resetGeneration;
    const NodePtr node = getNode();
    if (!node || !node->isActivated()) {
        return;
    }
    node->markHashDirty();
    const AppInstancePtr app = getApp();
    if (app && qApp && (QThread::currentThread() == qApp->thread())) {
        app->renderAllViewers(true);
    }
}

std::uint64_t
GeoSolverEnd::getSolvedFrameCount() const
{
    return _solvedFrames.load();
}

const GeoSimCache&
GeoSolverEnd::getSimCache() const
{
    return *_simCache;
}

bool
GeoSolverEnd::needsOrderedFrames() const
{
    return true;
}

StatusEnum
GeoSolverEnd::getPreferredMetadataAt(double /*time*/,
                                     ViewIdx /*view*/,
                                     NodeMetadata& metadata)
{
    // The result of the frame before changes with the frame, even under a time-invariant input.
    metadata.setIsFrameVarying(true);

    return eStatusOK;
}

RenderContextReadSet
GeoSolverEnd::getOverriddenContextVariables(int inputNb) const
{
    RenderContextReadSet overridden;

    if (inputNb != 0) {
        return overridden;
    }
    const std::string loopName = getLoopName();
    for (const std::string& name : loopVariables()) {
        overridden.add(RenderContextName::loop(name));
        overridden.add(RenderContextName::loops(loopName, name));
    }
    overridden.add(hiddenVariable(loopName));

    return overridden;
}

bool
GeoSolverEnd::pullsInputInRender(int inputNb,
                                 double /*time*/,
                                 ViewIdx /*view*/,
                                 const RenderContextPtr& /*context*/) const
{
    return inputNb == 0;
}

void
GeoSolverEnd::appendNodeContentHash(ContentHasher& h) const
{
    h.appendI64((std::int64_t)_resetGeneration.load());
}

void
GeoSolverEnd::appendExtraHashDependents(std::vector<NodePtr>* dependents) const
{
    const std::vector<NodePtr> begins = getBegins();

    dependents->insert(dependents->end(), begins.begin(), begins.end());
}

void
GeoSolverEnd::postMessage(MessageTypeEnum type,
                          const std::string& message)
{
    QMutexLocker k(&_messageMutex);

    refreshLoopMessage(getNode(), type, message, &_posted);
}

StatusEnum
GeoSolverEnd::fail(const std::string& message,
                   std::string* errorMessage)
{
    const std::string full = getScriptName_mt_safe() + ": " + message;

    postMessage(eMessageTypeError, full);
    if (errorMessage) {
        *errorMessage = full;
    }

    return eStatusFailed;
}

RenderContextPtr
GeoSolverEnd::substepContext(const RenderContextPtr& base,
                             const std::string& loopName,
                             int startFrame,
                             int frame,
                             int substep,
                             int substeps,
                             const GeoDetailConstPtr& previous) const
{
    // Counted from Start Frame, not from where a walk resumes, so a frame solves the same
    // whichever frame was asked for.
    const std::int64_t iteration = (std::int64_t)(frame - startFrame - 1) * substeps + substep;
    const double t = (double)(substep + 1) / (double)substeps;
    const double value = (double)(frame - 1) + t;
    RenderContext::Builder builder(base);
    const std::vector<std::string>& variables = loopVariables();

    for (const std::string& name : variables) {
        const RenderContextName names[2] = { RenderContextName::loop(name), RenderContextName::loops(loopName, name) };
        for (const RenderContextName& variable : names) {
            if (name == "iteration") {
                builder.set(variable, iteration);
            } else if ((name == "numiterations") || (name == "count")) {
                builder.set(variable, (std::int64_t)(frame - startFrame) * substeps);
            } else if (name == "t") {
                builder.set(variable, t);
            } else if (name == "value") {
                builder.set(variable, value);
            } else if (name == "frame") {
                builder.set(variable, (std::int64_t)frame);
            } else if (name == "substep") {
                builder.set(variable, (std::int64_t)substep);
            } else if (name == "numsubsteps") {
                builder.set(variable, (std::int64_t)substeps);
            } else if (name == "dt") {
                builder.set(variable, 1. / (double)substeps);
            }
        }
    }
    // A preview has no state to start from: without the variable, GeoSolverBegin passes its input through.
    if (previous) {
        builder.set(hiddenVariable(loopName), (std::int64_t)GeoForEachEnd::publishFeedback(previous));
    } else {
        builder.erase(hiddenVariable(loopName));
    }

    return builder.build();
}

void
GeoSolverEnd::onNodeHashChanged(U64 hash)
{
    NativeEffectBase::onNodeHashChanged(hash);

    _cooks.onHashChanged(hash);
    // The frames of the series this node has left are of no further use.
    _simCache->retainHash(hash);
}

void
GeoSolverEnd::abortCooksStaleAfterEdit()
{
    _cooks.abortStaleAfterEdit();
}

StatusEnum
GeoSolverEnd::solveFrame(const LoopEndCooks::Scope& running,
                         const EffectInstancePtr& body,
                         const GeoRenderActionArgs& args,
                         const RenderContextPtr& base,
                         const std::string& loopName,
                         const Settings& s,
                         int frame,
                         const GeoDetailConstPtr& previous,
                         GeoDetailConstPtr* result,
                         std::string* errorMessage)
{
    // Held across each cook: the published result before is only findable while it lives.
    GeoDetailConstPtr current = previous;

    for (int k = 0; k < s.substeps; ++k) {
        const RenderContextPtr context = substepContext(base, loopName, s.startFrame, frame, k, s.substeps, current);
        const double time = (double)(frame - 1) + (double)(k + 1) / (double)s.substeps;
        GeoDetailConstPtr next;
        std::string stepError;
        const RenderRoIRetCode code = body->evaluateGeometry(EvaluateGeometryArgs(time, args.view, args.byPassCache, context), &next, &stepError);
        if (code == eRenderRoIRetCodeAborted) {
            return eStatusFailed;
        }
        if ((code != eRenderRoIRetCodeOk) || !next) {
            const QString what = (s.substeps > 1) ? tr("frame %1, substep %2 failed: %3").arg(frame).arg(k + 1).arg(QString::fromUtf8(stepError.c_str()))
                                                  : tr("frame %1 failed: %2").arg(frame).arg(QString::fromUtf8(stepError.c_str()));

            return fail(what.toStdString(), errorMessage);
        }
        current = next;
        // A superseded render is aborted here as well as by an edit: the render thread it holds
        // is where the newer request, a kept frame say, is queued.
        if (running.editedSinceStart() || aborted() || abortIfSuperseded()) {
            return eStatusFailed;
        }
    }
    *result = current;

    return eStatusOK;
}

std::shared_ptr<std::timed_mutex>
GeoSolverEnd::walkMutex(const GeoSimSeries& series)
{
    std::lock_guard<std::mutex> lock(_walksMutex);

    for (std::map<GeoSimSeries, std::shared_ptr<std::timed_mutex>>::iterator it = _walks.begin(); it != _walks.end();) {
        // Nobody walks a series whose mutex only this map holds.
        it = ((it->second.use_count() == 1) && !(it->first == series)) ? _walks.erase(it) : std::next(it);
    }
    std::shared_ptr<std::timed_mutex>& walk = _walks[series];
    if (!walk) {
        walk = std::make_shared<std::timed_mutex>();
    }

    return walk;
}

/**
 * @brief Shows a walk's progress in the progress panel, cancellable, while no other walk of the
 * node shows its own, and tells the timelines of the frames it keeps. Off the main thread both are
 * reported at most once per kReportInterval, so a walk of many quick frames does not flood the GUI
 * thread with progress-bar repaints and cache-line rebuilds. On it, each report is also where the
 * GUI gets to process its events, so every frame reports.
 **/
class GeoSolverEnd::SolveProgress {
public:
    explicit SolveProgress(GeoSolverEnd* effect)
        : _effect(effect)
        , _app(effect->getApp())
        , _node(effect->getNode())
        , _mainThread(!qApp || (QThread::currentThread() == qApp->thread()))
    {
    }

    ~SolveProgress()
    {
        if (_unreportedFrame) {
            _effect->notifyKeptFramesChanged(*_unreportedFrame);
        }
        if (!_held) {
            return;
        }
        const AppInstancePtr app = _app.lock();
        const NodePtr node = _node.lock();
        if (app && node) {
            app->progressEnd(node);
        }
        _effect->_progressShown = false;
    }

    SolveProgress(const SolveProgress&) = delete;
    SolveProgress& operator=(const SolveProgress&) = delete;

    void start(const std::string& message)
    {
        const AppInstancePtr app = _app.lock();
        const NodePtr node = _node.lock();
        bool expected = false;

        if (!app || !node || !_effect->_progressShown.compare_exchange_strong(expected, true)) {
            return;
        }
        _held = true;
        // From another thread the task is registered later, when the main thread gets to it:
        // until then an update finds none, which is no cancel.
        _registered = _mainThread;
        app->progressStart(node, message, std::string(), true);
    }

    // After each solved frame. False once the user cancelled, which is seen at the next report.
    bool solved(int frame,
                double progress)
    {
        const Clock::time_point now = Clock::now();

        _unreportedFrame = frame;
        if (!_mainThread && ((now - _lastReport) < kReportInterval)) {
            return true;
        }
        _lastReport = now;
        _effect->notifyKeptFramesChanged(frame);
        _unreportedFrame.reset();

        return update(progress);
    }

private:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::milliseconds kReportInterval { 100 };

    bool update(double progress)
    {
        const AppInstancePtr app = _app.lock();
        const NodePtr node = _node.lock();

        if (!_held || !app || !node) {
            return true;
        }
        if (app->progressUpdate(node, progress)) {
            _registered = true;

            return true;
        }

        return !_registered;
    }

    GeoSolverEnd* _effect;
    AppInstanceWPtr _app;
    NodeWPtr _node;
    const bool _mainThread;
    bool _held = false;
    bool _registered = false;
    Clock::time_point _lastReport = Clock::now();
    std::optional<int> _unreportedFrame;
};

StatusEnum
GeoSolverEnd::renderGeometry(const GeoRenderActionArgs& args,
                             GeoDetailConstPtr* outputDetail,
                             std::string* errorMessage)
{
    const LoopEndCooks::Scope running(&_cooks);
    const Settings s = readSettings();
    const std::string loopName = getLoopName();
    const RenderContextPtr base = getCurrentRenderContext();
    const EffectInstancePtr body = getInput(0);

    if (!body || !body->producesGeometryData()) {
        return fail(tr("connect the solver body to Source.").toStdString(), errorMessage);
    }

    GeoSolverBegin* startBegin = NULL;
    for (const NodePtr& node : getBegins()) {
        GeoSolverBegin* begin = dynamic_cast<GeoSolverBegin*>(node->getEffectInstance().get());
        if (begin && (begin->getMethod() == eGeoSolverBeginPrevFrame)) {
            startBegin = begin;
            break;
        }
    }
    if (!startBegin) {
        return fail(tr("no GeoSolverBegin of this solver is set to Prev Frame: there is nothing to start the simulation from.").toStdString(), errorMessage);
    }
    const EffectInstancePtr source = startBegin->getInput(0);
    // Failed, or eStatusOK with *detail set; an abort posts no error.
    const auto evaluateSource = [&](double time,
                                    GeoDetailConstPtr* detail) {
        if (!source || !source->producesGeometryData()) {
            *detail = std::make_shared<const GeoDetail>();

            return eStatusOK;
        }
        std::string sourceError;
        const RenderRoIRetCode code = source->evaluateGeometry(EvaluateGeometryArgs(time, args.view, args.byPassCache, base), detail, &sourceError);
        if (code == eRenderRoIRetCodeOk) {
            return eStatusOK;
        }
        if ((code == eRenderRoIRetCodeAborted) || aborted()) {
            return eStatusFailed;
        }

        return fail(tr("the input of %1 failed at frame %2: %3")
                        .arg(QString::fromUtf8(startBegin->getScriptName_mt_safe().c_str()))
                        .arg(time)
                        .arg(QString::fromUtf8(sourceError.c_str()))
                        .toStdString(),
                    errorMessage);
    };

    // A time between frames shows the frame before it: the simulation only has whole frames.
    const int frame = (int)std::floor(args.time);
    if (frame <= s.startFrame) {
        if (evaluateSource((frame < s.startFrame) ? args.time : (double)s.startFrame, outputDetail) != eStatusOK) {
            return eStatusFailed;
        }
        postMessage(eMessageTypeInfo, std::string());
        {
            QMutexLocker locker(&_outputMutex);
            _outputKept = false;
            _outputFrame = frame;
        }

        return eStatusOK;
    }

    _simCache->setBudget(s.cacheBytes);
    _simCache->setSpillParent(s.spillToDisk ? spillParent() : std::string());

    GeoSimSeries series;
    series.hash = getContextFreeRenderHash();
    series.contextKey = getCurrentRenderContextKey();
    series.view = (int)args.view;
    // An edit anywhere above makes a new series; what the old one solved is of no further use. A
    // render started before the edit would forget the new one's instead.
    const NodePtr node = getNode();
    U64 current = 0;
    if (node && node->getHashValueIfUpToDate(&current) && (current == series.hash)) {
        _simCache->retainHash(series.hash);
    }

    // A frame kept is shown without waiting for a walk of the series to another one.
    *outputDetail = _simCache->find(series, frame);
    if (*outputDetail) {
        postMessage(eMessageTypeInfo, std::string());
        recordOutput(series, frame, true);

        return eStatusOK;
    }
    const std::shared_ptr<std::timed_mutex> walk = walkMutex(series);
    std::unique_lock<std::timed_mutex> walking(*walk, std::defer_lock);
    while (!walking.try_lock_for(std::chrono::milliseconds(20))) {
        if (running.editedSinceStart() || aborted() || abortIfSuperseded()) {
            return eStatusFailed;
        }
        *outputDetail = _simCache->find(series, frame);
        if (*outputDetail) {
            postMessage(eMessageTypeInfo, std::string());
            recordOutput(series, frame, true);

            return eStatusOK;
        }
    }

    *outputDetail = _simCache->find(series, frame);
    if (*outputDetail) {
        postMessage(eMessageTypeInfo, std::string());
        recordOutput(series, frame, true);

        return eStatusOK;
    }

    GeoDetailConstPtr previous;
    int from = s.startFrame;
    int kept = 0;
    while (!previous && _simCache->latest(series, s.startFrame, frame - 1, &kept)) {
        // Null when its spilled file cannot be read back; it is then forgotten and the next is tried.
        previous = _simCache->find(series, kept);
        from = kept;
    }
    if (!previous) {
        from = s.startFrame;
        if (evaluateSource((double)s.startFrame, &previous) != eStatusOK) {
            return eStatusFailed;
        }
        _simCache->insert(series, from, previous);
    }

    SolveProgress progress(this);
    const int total = frame - from;
    if (total > 2) {
        progress.start(tr("Solving frames %1 to %2 of %3").arg(from + 1).arg(frame).arg(QString::fromUtf8(getScriptName_mt_safe().c_str())).toStdString());
    }
    for (int h = from + 1; h <= frame; ++h) {
        GeoDetailConstPtr result;
        const StatusEnum status = solveFrame(running, body, args, base, loopName, s, h, previous, &result, errorMessage);
        if (status != eStatusOK) {
            return status;
        }
        ++_solvedFrames;
        _simCache->insert(series, h, result);
        if (!s.cacheSimulation) {
            _simCache->retainOnly(series, h);
        }
        recordOutput(series, h, false);
        previous = result;
        if (!progress.solved(h, (double)(h - from) / (double)total)) {
            // Cancelled: the frames solved so far are kept, the one asked for is not output.
            return eStatusFailed;
        }
    }
    postMessage(eMessageTypeInfo, std::string());
    *outputDetail = previous;

    return eStatusOK;
} // GeoSolverEnd::renderGeometry

NATRON_NAMESPACE_EXIT
