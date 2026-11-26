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
#include <set>
#include <utility>

#include <QtCore/QCoreApplication>
#include <QtCore/QString>
#include <QtCore/QThread>

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
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
    static const std::vector<std::string> names = { "substep", "numsubsteps", "dt" };

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
    loopName->setDefaultValue(std::string());
    page->addKnob(loopName);
    _loopName = loopName;
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
GeoSolverEnd::resetSimulation()
{
    _simCache->clear();
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
                             int substep,
                             int substeps,
                             const GeoDetailConstPtr& previous) const
{
    RenderContext::Builder builder(base);
    const RenderContextName names[2][3] = {
        { RenderContextName::loop("substep"), RenderContextName::loop("numsubsteps"), RenderContextName::loop("dt") },
        { RenderContextName::loops(loopName, "substep"), RenderContextName::loops(loopName, "numsubsteps"), RenderContextName::loops(loopName, "dt") },
    };

    for (const RenderContextName(&set)[3] : names) {
        builder.set(set[0], (std::int64_t)substep).set(set[1], (std::int64_t)substeps).set(set[2], 1. / (double)substeps);
    }
    builder.set(hiddenVariable(loopName), (std::int64_t)GeoForEachEnd::publishFeedback(previous));

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
        const RenderContextPtr context = substepContext(base, loopName, k, s.substeps, current);
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
        if (running.editedSinceStart() || aborted()) {
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

        return eStatusOK;
    }
    const std::shared_ptr<std::timed_mutex> walk = walkMutex(series);
    std::unique_lock<std::timed_mutex> walking(*walk, std::defer_lock);
    while (!walking.try_lock_for(std::chrono::milliseconds(20))) {
        if (running.editedSinceStart() || aborted()) {
            return eStatusFailed;
        }
        *outputDetail = _simCache->find(series, frame);
        if (*outputDetail) {
            postMessage(eMessageTypeInfo, std::string());

            return eStatusOK;
        }
    }

    *outputDetail = _simCache->find(series, frame);
    if (*outputDetail) {
        postMessage(eMessageTypeInfo, std::string());

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
        previous = result;
    }
    postMessage(eMessageTypeInfo, std::string());
    *outputDetail = previous;

    return eStatusOK;
} // GeoSolverEnd::renderGeometry

NATRON_NAMESPACE_EXIT
