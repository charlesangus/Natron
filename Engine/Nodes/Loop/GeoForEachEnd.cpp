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

#include "GeoForEachEnd.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

#include <QtCore/QString>
#include <QtCore/QThread>

#include "Engine/AppManager.h"
#include "Engine/ContentHasher.h"
#include "Engine/IsolatedJob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Geometry/GeoMerge.h"
#include "Engine/Nodes/Loop/GeoForEachBegin.h"
#include "Engine/Nodes/Loop/LoopBegin.h"
#include "Engine/Nodes/Loop/LoopEnd.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/PyRFV.h"
#include "Engine/TLSHolder.h"
#include "Global/PythonUtils.h"

NATRON_NAMESPACE_ENTER

namespace {

typedef std::pair<Node*, int> WalkStep;

// Counts the loops it enters: a GeoForEachEnd met going up opens a nested loop, whose own
// GeoForEachBegins close it, so only a GeoForEachBegin met at depth 0 pairs. A nested
// GeoForEachEnd's Piece input is outside its loop and is walked at the depth outside it.
void
collectPairedBegins(const NodePtr& from,
                    std::set<Node*>* found)
{
    std::set<WalkStep> visited;
    std::vector<std::pair<NodePtr, int>> pending;

    pending.push_back(std::make_pair(from, 0));
    while (!pending.empty()) {
        const NodePtr node = pending.back().first;
        int depth = pending.back().second;
        pending.pop_back();
        if (!node || !visited.insert(WalkStep(node.get(), depth)).second) {
            continue;
        }
        if (GeoForEachBegin::isGeoForEachBegin(node)) {
            if (depth == 0) {
                found->insert(node.get());
                continue;
            }
            --depth;
        } else if (GeoForEachEnd::isGeoForEachEnd(node)) {
            NodePtr piece = node->getInput(1);
            if (piece) {
                pending.push_back(std::make_pair(piece, depth));
            }
            NodePtr body = node->getInput(0);
            if (body) {
                pending.push_back(std::make_pair(body, depth + 1));
            }
            continue;
        }
        for (int i = 0, n = node->getNInputs(); i < n; ++i) {
            NodePtr input = node->getInput(i);
            if (input) {
                pending.push_back(std::make_pair(input, depth));
            }
        }
    }
}

void
collectPairedEnds(const NodePtr& begin,
                  std::set<Node*>* found)
{
    std::set<WalkStep> visited;
    std::vector<std::pair<NodePtr, int>> pending;
    std::vector<NodePtr> outputs;

    begin->appendOutputsWithGroupRedirection(&outputs);
    for (NodePtr& output : outputs) {
        pending.push_back(std::make_pair(std::move(output), 0));
    }
    while (!pending.empty()) {
        const NodePtr node = pending.back().first;
        int depth = pending.back().second;
        pending.pop_back();
        if (!node || !visited.insert(WalkStep(node.get(), depth)).second) {
            continue;
        }
        if (GeoForEachEnd::isGeoForEachEnd(node)) {
            if (depth == 0) {
                found->insert(node.get());
                continue;
            }
            --depth;
        } else if (GeoForEachBegin::isGeoForEachBegin(node)) {
            ++depth;
        }
        outputs.clear();
        node->appendOutputsWithGroupRedirection(&outputs);
        for (NodePtr& output : outputs) {
            pending.push_back(std::make_pair(std::move(output), depth));
        }
    }
}

struct FeedbackRegistry {
    std::mutex mutex;
    std::unordered_map<U64, std::weak_ptr<const GeoDetail>> details;
};

FeedbackRegistry&
feedbackRegistry()
{
    static FeedbackRegistry registry;

    return registry;
}

const std::size_t kFeedbackSweepSize = 1024;

} // anonymous namespace

GeoForEachEnd::GeoForEachEnd(NodePtr node)
    : NativeEffectBase(node)
{
}

GeoForEachEnd::~GeoForEachEnd()
{
}

const NodeSpec*
GeoForEachEnd::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

bool
GeoForEachEnd::isGeoForEachEnd(const NodePtr& node)
{
    return node && (node->getPluginID() == PLUGINID_NATRON_GEOFOREACHEND);
}

std::shared_ptr<GeoForEachEnd>
GeoForEachEnd::pairedEndOf(const NodePtr& begin)
{
    if (!begin) {
        return std::shared_ptr<GeoForEachEnd>();
    }
    std::set<Node*> found;
    collectPairedEnds(begin, &found);
    if (found.size() != 1) {
        return std::shared_ptr<GeoForEachEnd>();
    }
    Node* end = *found.begin();

    return std::dynamic_pointer_cast<GeoForEachEnd>(end->getEffectInstance());
}

std::vector<NodePtr>
GeoForEachEnd::getBegins() const
{
    const U64 generation = LoopEnd::getGraphGeneration();
    {
        QMutexLocker k(&_pairingMutex);
        if (_pairingValid && (_pairingGeneration == generation)) {
            std::vector<NodePtr> begins;
            bool expired = false;
            for (const NodeWPtr& weak : _begins) {
                NodePtr begin = weak.lock();
                if (!begin) {
                    expired = true;
                    break;
                }
                begins.push_back(std::move(begin));
            }
            if (!expired) {
                return begins;
            }
        }
    }

    std::vector<NodePtr> begins;
    const NodePtr self = getNode();
    const NodePtr source = self ? self->getInput(0) : NodePtr();
    if (source) {
        std::set<Node*> found;
        collectPairedBegins(source, &found);
        for (Node* node : found) {
            // Only those that pair with this node alone open its loop.
            NodePtr begin = node->shared_from_this();
            std::set<Node*> ends;
            collectPairedEnds(begin, &ends);
            if ((ends.size() == 1) && (*ends.begin() == self.get())) {
                begins.push_back(std::move(begin));
            }
        }
    }
    std::sort(begins.begin(), begins.end(), [](const NodePtr& a, const NodePtr& b) { return a->getScriptName_mt_safe() < b->getScriptName_mt_safe(); });

    QMutexLocker k(&_pairingMutex);
    _begins.assign(begins.begin(), begins.end());
    _pairingGeneration = generation;
    _pairingValid = true;

    return begins;
}

std::string
GeoForEachEnd::getLoopName() const
{
    KnobStringPtr name = _loopName.lock();
    const std::string value = name ? name->getValue() : std::string();

    return value.empty() ? getScriptName_mt_safe() : value;
}

RenderContextName
GeoForEachEnd::hiddenVariable(const std::string& loopName,
                              const std::string& name)
{
    return RenderContextName("geoForEach." + loopName, name);
}

const std::vector<std::string>&
GeoForEachEnd::loopVariables()
{
    static const std::vector<std::string> names = { "iteration", "numiterations", "count", "t", "value", "ivalue", "svalue" };

    return names;
}

GeoForEachEnd::Settings
GeoForEachEnd::readSettings() const
{
    Settings s;
    KnobChoicePtr iterationMethod = _iterationMethod.lock();
    KnobChoicePtr gatherMethod = _gatherMethod.lock();
    KnobIntPtr iterations = _iterations.lock();
    KnobDoublePtr startValue = _startValue.lock();
    KnobDoublePtr increment = _increment.lock();
    KnobChoicePtr pieceElements = _pieceElements.lock();
    KnobChoicePtr pieceMethod = _pieceMethod.lock();
    KnobStringPtr pieceAttribute = _pieceAttribute.lock();
    KnobIntPtr maxIterations = _maxIterations.lock();
    KnobBoolPtr singlePass = _singlePass.lock();
    KnobIntPtr singlePassIteration = _singlePassIteration.lock();
    KnobStringPtr stopCondition = _stopCondition.lock();
    KnobStringPtr stopAttribute = _stopAttribute.lock();

    if (iterationMethod) {
        s.iteration = (GeoForEachIterationEnum)iterationMethod->getValue();
    }
    if (gatherMethod) {
        s.gather = (GeoForEachGatherEnum)gatherMethod->getValue();
    }
    if (iterations) {
        s.iterations = std::max(0, iterations->getValue());
    }
    if (startValue) {
        s.startValue = startValue->getValue();
    }
    if (increment) {
        s.increment = increment->getValue();
    }
    if (pieceElements) {
        s.pieces.elements = (GeoPieceElementsEnum)pieceElements->getValue();
    }
    if (pieceMethod) {
        s.pieces.method = (GeoPieceMethodEnum)pieceMethod->getValue();
    }
    if (pieceAttribute) {
        s.pieces.attribute = pieceAttribute->getValue();
    }
    if (maxIterations) {
        s.maxIterations = std::max(1, maxIterations->getValue());
    }
    if (singlePass) {
        s.singlePass = singlePass->getValue();
    }
    if (singlePassIteration) {
        s.singlePassIteration = std::max(0, singlePassIteration->getValue());
    }
    if (stopCondition) {
        s.stopCondition = stopCondition->getValue();
        if (s.stopCondition.find_first_not_of(" \t\r\n") == std::string::npos) {
            s.stopCondition.clear();
        }
    }
    if (stopAttribute) {
        s.stopAttribute = stopAttribute->getValue();
    }

    return s;
}

void
GeoForEachEnd::appendBeginKnobsHash(ContentHasher& h) const
{
    const Settings s = readSettings();

    h.appendI64((std::int64_t)s.iteration);
    h.appendI64((std::int64_t)s.pieces.elements);
    h.appendI64((std::int64_t)s.pieces.method);
    h.appendString(s.pieces.attribute);
    h.appendI64(s.iterations);
    h.appendDouble(s.startValue);
    h.appendDouble(s.increment);
    h.appendI64(s.singlePass ? s.singlePassIteration : 0);
}

U64
GeoForEachEnd::publishFeedback(const GeoDetailConstPtr& detail)
{
    const U64 hash = detail->contentHash();
    FeedbackRegistry& registry = feedbackRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);

    if (registry.details.size() >= kFeedbackSweepSize) {
        for (auto it = registry.details.begin(); it != registry.details.end();) {
            it = it->second.expired() ? registry.details.erase(it) : std::next(it);
        }
    }
    registry.details[hash] = detail;

    return hash;
}

GeoDetailConstPtr
GeoForEachEnd::findFeedback(U64 contentHash)
{
    FeedbackRegistry& registry = feedbackRegistry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    const auto it = registry.details.find(contentHash);

    return (it == registry.details.end()) ? GeoDetailConstPtr() : it->second.lock();
}

void
GeoForEachEnd::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls").toStdString());

    KnobChoicePtr iterationMethod = createKnob<KnobChoice>(tr("Iteration Method").toStdString());
    iterationMethod->setName(kGeoForEachEndParamIterationMethod);
    iterationMethod->setAnimationEnabled(false);
    iterationMethod->setHintToolTip(tr("What the iterations step through."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("pieces", tr("Pieces or Points").toStdString(),
                                       tr("One iteration per piece of the piece geometry, as Piece Elements and Piece Method split it.").toStdString()));
        options.push_back(ChoiceOption("count", tr("Count").toStdString(),
                                       tr("Iterations iterations, whose value goes from Start Value by Increment.").toStdString()));
        iterationMethod->populateChoices(options);
    }
    iterationMethod->setDefaultValue((int)eGeoForEachIteratePieces);
    page->addKnob(iterationMethod);
    _iterationMethod = iterationMethod;

    KnobChoicePtr gatherMethod = createKnob<KnobChoice>(tr("Gather Method").toStdString());
    gatherMethod->setName(kGeoForEachEndParamGatherMethod);
    gatherMethod->setAnimationEnabled(false);
    gatherMethod->setHintToolTip(tr("How the iterations make the output."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("merge", tr("Merge").toStdString(),
                                       tr("Every iteration's result, merged in iteration order as GeoMerge merges. The iterations cook in parallel.").toStdString()));
        options.push_back(ChoiceOption("feedback", tr("Feedback").toStdString(),
                                       tr("Each iteration starts from the result of the one before, which a GeoForEachBegin set to Fetch Feedback outputs; "
                                          "the last result is the output. The iterations cook one after the other.")
                                           .toStdString()));
        gatherMethod->populateChoices(options);
    }
    gatherMethod->setDefaultValue((int)eGeoForEachGatherMerge);
    page->addKnob(gatherMethod);
    _gatherMethod = gatherMethod;

    KnobChoicePtr pieceElements = createKnob<KnobChoice>(tr("Piece Elements").toStdString());
    pieceElements->setName(kGeoForEachEndParamPieceElements);
    pieceElements->setAnimationEnabled(false);
    pieceElements->setHintToolTip(tr("Whether pieces are made of primitives, with the points they use, or of points."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("primitives", tr("Primitives").toStdString(),
                                       tr("A piece is primitives and the points they use.").toStdString()));
        options.push_back(ChoiceOption("points", tr("Points").toStdString(),
                                       tr("A piece is points and the primitives all of whose points it holds.").toStdString()));
        pieceElements->populateChoices(options);
    }
    pieceElements->setDefaultValue((int)eGeoPieceElementsPrimitives);
    page->addKnob(pieceElements);
    _pieceElements = pieceElements;

    KnobChoicePtr pieceMethod = createKnob<KnobChoice>(tr("Piece Method").toStdString());
    pieceMethod->setName(kGeoForEachEndParamPieceMethod);
    pieceMethod->setAnimationEnabled(false);
    pieceMethod->setHintToolTip(tr("How the piece geometry splits into pieces."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("attribute", tr("Attribute").toStdString(),
                                       tr("Elements with the same value of Piece Attribute, an integer or string attribute of the Piece Elements.").toStdString()));
        options.push_back(ChoiceOption("connectivity", tr("Connectivity").toStdString(),
                                       tr("Elements connected through shared points. Pieces are numbered in the order of their first element.").toStdString()));
        options.push_back(ChoiceOption("each", tr("Each Element").toStdString(),
                                       tr("Every primitive or point on its own.").toStdString()));
        pieceMethod->populateChoices(options);
    }
    pieceMethod->setDefaultValue((int)eGeoPieceMethodConnectivity);
    page->addKnob(pieceMethod);
    _pieceMethod = pieceMethod;

    KnobStringPtr pieceAttribute = createKnob<KnobString>(tr("Piece Attribute").toStdString());
    pieceAttribute->setName(kGeoForEachEndParamPieceAttribute);
    pieceAttribute->setAnimationEnabled(false);
    pieceAttribute->setHintToolTip(tr("The integer or string attribute whose values split the pieces under Piece Method Attribute, "
                                      "such as name. Missing on the piece geometry is an error."));
    pieceAttribute->setDefaultValue("name");
    page->addKnob(pieceAttribute);
    _pieceAttribute = pieceAttribute;

    KnobIntPtr iterations = createKnob<KnobInt>(tr("Iterations").toStdString());
    iterations->setName(kGeoForEachEndParamIterations);
    iterations->setAnimationEnabled(false);
    iterations->setHintToolTip(tr("How many times the body cooks under Iteration Method Count. 0 outputs empty geometry."));
    iterations->setMinimum(0);
    iterations->setDisplayMinimum(0);
    iterations->setDisplayMaximum(100);
    iterations->setDefaultValue(10);
    page->addKnob(iterations);
    _iterations = iterations;

    KnobDoublePtr startValue = createKnob<KnobDouble>(tr("Start Value").toStdString());
    startValue->setName(kGeoForEachEndParamStartValue);
    startValue->setAnimationEnabled(false);
    startValue->setHintToolTip(tr("loop.value of the first iteration under Iteration Method Count."));
    startValue->setDefaultValue(0.);
    page->addKnob(startValue);
    _startValue = startValue;

    KnobDoublePtr increment = createKnob<KnobDouble>(tr("Increment").toStdString());
    increment->setName(kGeoForEachEndParamIncrement);
    increment->setAnimationEnabled(false);
    increment->setHintToolTip(tr("What loop.value grows by from one iteration to the next under Iteration Method Count."));
    increment->setDefaultValue(1.);
    page->addKnob(increment);
    _increment = increment;

    KnobIntPtr maxIterations = createKnob<KnobInt>(tr("Max Iterations").toStdString());
    maxIterations->setName(kGeoForEachEndParamMaxIterations);
    maxIterations->setAnimationEnabled(false);
    maxIterations->setHintToolTip(tr("A safety cap: more iterations than this, from Iterations or from the pieces, is an error."));
    maxIterations->setMinimum(1);
    maxIterations->setDisplayMinimum(1);
    maxIterations->setDisplayMaximum(kGeoForEachEndDefaultMaxIterations);
    maxIterations->setDefaultValue(kGeoForEachEndDefaultMaxIterations);
    page->addKnob(maxIterations);
    _maxIterations = maxIterations;

    KnobBoolPtr singlePass = createKnob<KnobBool>(tr("Single Pass").toStdString());
    singlePass->setName(kGeoForEachEndParamSinglePass);
    singlePass->setAnimationEnabled(false);
    singlePass->setHintToolTip(tr("Cooks only the Single Pass Iteration and outputs its result, to look at one piece or iteration. "
                                  "Under Feedback it starts from the GeoForEachBegin's input."));
    singlePass->setDefaultValue(false);
    page->addKnob(singlePass);
    _singlePass = singlePass;

    KnobIntPtr singlePassIteration = createKnob<KnobInt>(tr("Single Pass Iteration").toStdString());
    singlePassIteration->setName(kGeoForEachEndParamSinglePassIteration);
    singlePassIteration->setAnimationEnabled(false);
    singlePassIteration->setHintToolTip(tr("The iteration Single Pass cooks, counted from 0. Past the last iteration is an error. "
                                           "With Single Pass on, a GeoForEachBegin viewed outside a cook of the loop shows this iteration's piece."));
    singlePassIteration->setMinimum(0);
    singlePassIteration->setDisplayMinimum(0);
    singlePassIteration->setDisplayMaximum(64);
    singlePassIteration->setDefaultValue(0);
    page->addKnob(singlePassIteration);
    _singlePassIteration = singlePassIteration;

    KnobStringPtr stopCondition = createKnob<KnobString>(tr("Stop Condition").toStdString());
    stopCondition->setName(kGeoForEachEndParamStopCondition);
    stopCondition->setAnimationEnabled(false);
    stopCondition->setHintToolTip(tr("A Python expression, written as a parameter expression is, evaluated before each iteration "
                                     "with loop.iteration, loop.numiterations, loop.count, loop.t, loop.value, loop.ivalue and "
                                     "loop.svalue of that iteration, also as loops.<name>.*, and the loops.<name>.* of the loops "
                                     "around this one. The loop ends before the first iteration it is true for. It may read "
                                     "parameters, never geometry: Stop Attribute reads geometry. An exception is an error. Empty "
                                     "runs every iteration."));
    stopCondition->setDefaultValue(std::string());
    page->addKnob(stopCondition);
    _stopCondition = stopCondition;

    KnobStringPtr stopAttribute = createKnob<KnobString>(tr("Stop Attribute").toStdString());
    stopAttribute->setName(kGeoForEachEndParamStopAttribute);
    stopAttribute->setAnimationEnabled(false);
    stopAttribute->setHintToolTip(tr("Under Feedback, an integer detail attribute of each iteration's result: when it is not 0 the "
                                     "loop ends there and that result is the output. Empty or missing never stops the loop."));
    stopAttribute->setDefaultValue(std::string());
    page->addKnob(stopAttribute);
    _stopAttribute = stopAttribute;

    KnobStringPtr loopName = createKnob<KnobString>(tr("Loop Name").toStdString());
    loopName->setName(kGeoForEachEndParamLoopName);
    loopName->setAnimationEnabled(false);
    loopName->setHintToolTip(tr("The name of the loops.<name>.* variables this loop sets, so a loop inside it can still "
                                "read this one's. Empty uses the node's script name."));
    loopName->setDefaultValue(std::string());
    page->addKnob(loopName);
    _loopName = loopName;

    refreshKnobVisibility();
} // GeoForEachEnd::initializeKnobs

void
GeoForEachEnd::refreshKnobVisibility()
{
    const Settings s = readSettings();
    const bool pieces = (s.iteration == eGeoForEachIteratePieces);
    const std::pair<KnobIPtr, bool> knobs[] = {
        { _pieceElements.lock(), pieces },
        { _pieceMethod.lock(), pieces },
        { _pieceAttribute.lock(), pieces && (s.pieces.method == eGeoPieceMethodAttribute) },
        { _iterations.lock(), !pieces },
        { _startValue.lock(), !pieces },
        { _increment.lock(), !pieces },
        { _singlePassIteration.lock(), s.singlePass },
        { _stopAttribute.lock(), s.gather == eGeoForEachGatherFeedback },
    };
    for (const std::pair<KnobIPtr, bool>& knob : knobs) {
        if (knob.first) {
            knob.first->setSecret(!knob.second);
        }
    }
}

void
GeoForEachEnd::refreshStopDependencies()
{
    KnobStringPtr stopCondition = _stopCondition.lock();

    if (!stopCondition) {
        return;
    }
    const std::string expression = stopCondition->getValue();
    stopCondition->setReadDependencyExpression(0, (expression.find_first_not_of(" \t\r\n") == std::string::npos) ? std::string() : expression);
    invalidateRenderContextReads();
}

bool
GeoForEachEnd::knobChanged(KnobI* k,
                           ValueChangedReasonEnum /*reason*/,
                           ViewSpec /*view*/,
                           double /*time*/,
                           bool /*originatedFromMainThread*/)
{
    KnobStringPtr loopName = _loopName.lock();
    KnobChoicePtr iterationMethod = _iterationMethod.lock();
    if ((loopName && (k == loopName.get())) || (iterationMethod && (k == iterationMethod.get()))) {
        // The variables the body and the GeoForEachBegins read move with them.
        invalidateRenderContextReads();
        refreshKnobVisibility();

        return true;
    }
    KnobStringPtr stopCondition = _stopCondition.lock();
    if (stopCondition && (k == stopCondition.get())) {
        refreshStopDependencies();

        return true;
    }
    const KnobIPtr visibility[] = { _gatherMethod.lock(), _pieceMethod.lock(), _singlePass.lock() };
    for (const KnobIPtr& knob : visibility) {
        if (knob && (k == knob.get())) {
            refreshKnobVisibility();

            return true;
        }
    }

    return false;
}

void
GeoForEachEnd::onScriptNameChanged(const std::string& /*fullyQualifiedName*/)
{
    // An empty Loop Name follows the script name.
    invalidateRenderContextReads();
}

void
GeoForEachEnd::onKnobsLoaded()
{
    refreshKnobVisibility();
    refreshStopDependencies();
}

StatusEnum
GeoForEachEnd::getPreferredMetadataAt(double /*time*/,
                                      ViewIdx /*view*/,
                                      NodeMetadata& metadata)
{
    // The stop condition may read the frame, or knobs at it.
    if (!readSettings().stopCondition.empty()) {
        metadata.setIsFrameVarying(true);
    }

    return eStatusOK;
}

RenderContextReadSet
GeoForEachEnd::getOverriddenContextVariables(int inputNb) const
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
    overridden.add(hiddenVariable(loopName, kGeoForEachHiddenPiece));
    overridden.add(hiddenVariable(loopName, kGeoForEachHiddenFeedback));

    return overridden;
}

bool
GeoForEachEnd::pullsInputInRender(int inputNb,
                                  double /*time*/,
                                  ViewIdx /*view*/,
                                  const RenderContextPtr& /*context*/) const
{
    return inputNb == 0;
}

void
GeoForEachEnd::appendExtraHashDependents(std::vector<NodePtr>* dependents) const
{
    const std::vector<NodePtr> begins = getBegins();

    dependents->insert(dependents->end(), begins.begin(), begins.end());
}

void
GeoForEachEnd::postMessage(MessageTypeEnum type,
                           const std::string& message)
{
    QMutexLocker k(&_messageMutex);

    refreshLoopMessage(getNode(), type, message, &_posted);
}

StatusEnum
GeoForEachEnd::fail(const std::string& message,
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
GeoForEachEnd::iterationContext(const RenderContextPtr& base,
                                const std::string& loopName,
                                const Iteration& iteration,
                                std::int64_t count,
                                bool pieces,
                                const GeoDetailConstPtr& feedback) const
{
    const double t = (count > 1) ? (double)iteration.index / (double)(count - 1) : 0.;
    RenderContext::Builder builder(base);
    const RenderContextName names[2][7] = {
        { RenderContextName::loop("iteration"), RenderContextName::loop("numiterations"), RenderContextName::loop("count"), RenderContextName::loop("t"),
          RenderContextName::loop("value"), RenderContextName::loop("ivalue"), RenderContextName::loop("svalue") },
        { RenderContextName::loops(loopName, "iteration"), RenderContextName::loops(loopName, "numiterations"), RenderContextName::loops(loopName, "count"),
          RenderContextName::loops(loopName, "t"), RenderContextName::loops(loopName, "value"), RenderContextName::loops(loopName, "ivalue"),
          RenderContextName::loops(loopName, "svalue") },
    };

    for (const RenderContextName(&set)[7] : names) {
        builder.set(set[0], iteration.index)
            .set(set[1], count)
            .set(set[2], count)
            .set(set[3], t)
            .set(set[4], iteration.value)
            .set(set[5], iteration.ivalue)
            .set(set[6], iteration.svalue);
    }
    if (pieces) {
        builder.set(hiddenVariable(loopName, kGeoForEachHiddenPiece), iteration.piece);
    } else {
        builder.erase(hiddenVariable(loopName, kGeoForEachHiddenPiece));
    }
    if (feedback) {
        builder.set(hiddenVariable(loopName, kGeoForEachHiddenFeedback), (std::int64_t)publishFeedback(feedback));
    } else {
        builder.erase(hiddenVariable(loopName, kGeoForEachHiddenFeedback));
    }

    return builder.build();
}

int
GeoForEachEnd::evaluateStop(const std::vector<Iteration>& iterations,
                            const std::string& loopName,
                            double time,
                            std::string* error) const
{
    static const std::string kLoopsPrefix("loops.");
    KnobStringPtr stopCondition = _stopCondition.lock();
    const std::string expression = stopCondition ? stopCondition->getValue() : std::string();
    const RenderContextPtr context = getCurrentRenderContext();
    const int count = (int)iterations.size();
    int stopAt = count;

    error->clear();
    PythonGILLocker pgl;
    std::string failure;
    PyObject* function = stopCondition ? stopCondition->compileExpressionSource(expression, "<stopCondition>", { "loop", "loops" }, &failure) : NULL;
    PyObject* types = function ? PyImport_ImportModule("types") : NULL;
    PyObject* simpleNamespace = types ? PyObject_GetAttrString(types, "SimpleNamespace") : NULL;
    PyObject* noArgs = simpleNamespace ? PyTuple_New(0) : NULL;
    PyObject* enclosing = noArgs ? PyDict_New() : NULL;
    bool ok = (enclosing != NULL);

    // loops.<name> for every enclosing loop, from the context this loop cooks under.
    if (ok && context) {
        std::map<std::string, PyObject*> loopVariablesByName;
        for (const RenderContext::Entry& entry : context->entries()) {
            if ((entry.first.ns.size() <= kLoopsPrefix.size()) || (entry.first.ns.compare(0, kLoopsPrefix.size(), kLoopsPrefix) != 0)) {
                continue;
            }
            PyObject*& variables = loopVariablesByName[entry.first.ns.substr(kLoopsPrefix.size())];
            if (!variables) {
                variables = PyDict_New();
            }
            PyObject* value = PyRFV::toPython(entry.second);
            ok = ok && variables && value && (PyDict_SetItemString(variables, entry.first.name.c_str(), value) == 0);
            Py_XDECREF(value);
        }
        for (std::map<std::string, PyObject*>::const_iterator it = loopVariablesByName.begin(); it != loopVariablesByName.end(); ++it) {
            PyObject* outer = (ok && it->second) ? PyObject_Call(simpleNamespace, noArgs, it->second) : NULL;
            ok = outer && (PyDict_SetItemString(enclosing, it->first.c_str(), outer) == 0);
            Py_XDECREF(outer);
            Py_XDECREF(it->second);
        }
    }

    int k = 0;
    for (; ok && (k < count); ++k) {
        const Iteration& iteration = iterations[(std::size_t)k];
        const double t = (count > 1) ? (double)k / (double)(count - 1) : 0.;
        PyObject* variables = PyDict_New();
        PyObject* values[] = {
            PyLong_FromLongLong((long long)iteration.index),
            PyLong_FromLongLong((long long)count),
            PyLong_FromLongLong((long long)count),
            PyFloat_FromDouble(t),
            PyFloat_FromDouble(iteration.value),
            PyLong_FromLongLong((long long)iteration.ivalue),
            PyUnicode_FromStringAndSize(iteration.svalue.data(), (Py_ssize_t)iteration.svalue.size()),
        };
        ok = (variables != NULL);
        const std::vector<std::string>& names = loopVariables();
        for (std::size_t i = 0; i < names.size(); ++i) {
            ok = ok && values[i] && (PyDict_SetItemString(variables, names[i].c_str(), values[i]) == 0);
            Py_XDECREF(values[i]);
        }
        PyObject* loop = ok ? PyObject_Call(simpleNamespace, noArgs, variables) : NULL;
        Py_XDECREF(variables);
        PyObject* loopsVariables = loop ? PyDict_Copy(enclosing) : NULL;
        ok = loopsVariables && (PyDict_SetItemString(loopsVariables, loopName.c_str(), loop) == 0);
        PyObject* loops = ok ? PyObject_Call(simpleNamespace, noArgs, loopsVariables) : NULL;
        Py_XDECREF(loopsVariables);
        PyObject* arguments = loops ? PyTuple_Pack(2, loop, loops) : NULL;
        Py_XDECREF(loop);
        Py_XDECREF(loops);
        if (!arguments) {
            ok = false;
            break;
        }
        PyObject* result = NULL;
        const bool evaluated = stopCondition->callExpressionFunction(function, time, ViewIdx(0), arguments, &result, &failure);
        Py_DECREF(arguments);
        const int truth = evaluated ? PyObject_IsTrue(result) : -1;
        Py_XDECREF(result);
        if (truth != 0) {
            ok = (truth == 1);
            if (!ok && evaluated) {
                failure = "the result is not a truth value";
            }
            break;
        }
    }
    stopAt = std::min(k, count);
    if (!function || !ok) {
        if (failure.empty()) {
            failure = "the expression could not be evaluated";
        }
        PyErr_Clear();
        *error = tr("Stop Condition failed at iteration %1: %2").arg(stopAt).arg(QString::fromUtf8(failure.c_str())).toStdString();
        stopAt = -1;
    }
    Py_XDECREF(enclosing);
    Py_XDECREF(noArgs);
    Py_XDECREF(simpleNamespace);
    Py_XDECREF(types);
    Py_XDECREF(function);

    return stopAt;
} // GeoForEachEnd::evaluateStop

StatusEnum
GeoForEachEnd::renderGeometry(const GeoRenderActionArgs& args,
                              GeoDetailConstPtr* outputDetail,
                              std::string* errorMessage)
{
    const Settings s = readSettings();
    const std::string loopName = getLoopName();
    const bool pieces = (s.iteration == eGeoForEachIteratePieces);
    const RenderContextPtr base = getCurrentRenderContext();
    EffectInstancePtr body = getInput(0);

    if (!body || !body->producesGeometryData()) {
        return fail(tr("connect the loop body to Source.").toStdString(), errorMessage);
    }

    std::vector<Iteration> iterations;
    if (pieces) {
        GeoDetailConstPtr pieceSource = args.getInputDetail(1);
        if (!pieceSource) {
            // The input of the first GeoForEachBegin that extracts pieces.
            for (const NodePtr& node : getBegins()) {
                GeoForEachBegin* begin = dynamic_cast<GeoForEachBegin*>(node->getEffectInstance().get());
                if (!begin || (begin->getMethod() != eGeoForEachBeginExtractPiece)) {
                    continue;
                }
                EffectInstancePtr source = begin->getInput(0);
                if (!source || !source->producesGeometryData()) {
                    pieceSource = std::make_shared<const GeoDetail>();
                    break;
                }
                const RenderRoIRetCode code = source->evaluateGeometry(EvaluateGeometryArgs(args.time, args.view, args.byPassCache, base), &pieceSource, errorMessage);
                if (code != eRenderRoIRetCodeOk) {
                    return eStatusFailed;
                }
                break;
            }
        }
        if (!pieceSource) {
            return fail(tr("there is no geometry to split into pieces: connect the Piece input, or set a GeoForEachBegin "
                           "of this loop to Extract Piece or Point.")
                            .toStdString(),
                        errorMessage);
        }
        std::string partitionError;
        const GeoPiecePartitionConstPtr partition = GeoForEachPieces::partition(*pieceSource, s.pieces, &partitionError);
        if (!partition) {
            return fail(partitionError, errorMessage);
        }
        iterations.resize(partition->size());
        for (std::size_t k = 0; k < partition->size(); ++k) {
            Iteration& it = iterations[k];
            it.index = (std::int64_t)k;
            if (partition->hasStringValues()) {
                it.value = (double)k;
                it.ivalue = (std::int64_t)k;
                it.svalue = partition->stringValue(k);
                it.piece = it.svalue;
            } else {
                it.ivalue = partition->intValue(k);
                it.value = (double)it.ivalue;
                it.piece = it.ivalue;
            }
        }
    } else {
        iterations.resize((std::size_t)s.iterations);
        for (int k = 0; k < s.iterations; ++k) {
            Iteration& it = iterations[(std::size_t)k];
            it.index = k;
            it.value = s.startValue + (double)k * s.increment;
            it.ivalue = (std::int64_t)std::llround(it.value);
            it.piece = it.ivalue;
        }
    }

    if ((int)iterations.size() > s.maxIterations) {
        return fail(tr("%1 iterations is more than Max Iterations (%2).").arg(iterations.size()).arg(s.maxIterations).toStdString(), errorMessage);
    }
    const std::int64_t numIterations = (std::int64_t)iterations.size();

    int runCount = (int)iterations.size();
    if (!s.stopCondition.empty()) {
        std::string stopError;
        runCount = evaluateStop(iterations, loopName, args.time, &stopError);
        if (runCount < 0) {
            return fail(stopError, errorMessage);
        }
    }

    std::vector<std::size_t> run;
    if (s.singlePass && (runCount > 0)) {
        if (s.singlePassIteration >= runCount) {
            return fail(tr("Single Pass Iteration %1 is past the last iteration (%2 run).").arg(s.singlePassIteration).arg(runCount).toStdString(), errorMessage);
        }
        run.push_back((std::size_t)s.singlePassIteration);
    } else {
        for (int k = 0; k < runCount; ++k) {
            run.push_back((std::size_t)k);
        }
    }

    if (run.empty()) {
        postMessage(eMessageTypeInfo, getScriptName_mt_safe() + ": " + (pieces ? tr("there are no pieces to iterate over.") : tr("there are no iterations.")).toStdString());
        *outputDetail = std::make_shared<const GeoDetail>();

        return eStatusOK;
    }

    if ((s.gather == eGeoForEachGatherFeedback) && !s.singlePass) {
        GeoDetailConstPtr previous;
        // Each result stays findable while the next iteration cooks from it.
        std::vector<GeoDetailConstPtr> held;
        for (std::size_t k : run) {
            if (previous && !s.stopAttribute.empty()) {
                const GeoAttribute* stop = previous->findAttribute(eGeoAttribClassDetail, s.stopAttribute);
                if (stop && !stop->isArray() && (stop->size() > 0)) {
                    const bool stopped = (stop->storage() == eGeoStorageInt32) ? (stop->get<std::int32_t>(0) != 0) : (stop->storage() == eGeoStorageInt64) ? (stop->get<std::int64_t>(0) != 0)
                                                                                                                                                           : false;
                    if (stopped) {
                        break;
                    }
                }
            }
            const RenderContextPtr context = iterationContext(base, loopName, iterations[k], numIterations, pieces, previous);
            GeoDetailConstPtr result;
            std::string iterationError;
            const RenderRoIRetCode code = body->evaluateGeometry(EvaluateGeometryArgs(args.time, args.view, args.byPassCache, context), &result, &iterationError);
            if (code == eRenderRoIRetCodeAborted) {
                return eStatusFailed;
            }
            if ((code != eRenderRoIRetCodeOk) || !result) {
                return fail(tr("iteration %1 failed: %2").arg(k).arg(QString::fromUtf8(iterationError.c_str())).toStdString(), errorMessage);
            }
            held.push_back(result);
            previous = result;
            if (aborted()) {
                return eStatusFailed;
            }
        }
        postMessage(eMessageTypeInfo, std::string());
        *outputDetail = previous ? previous : std::make_shared<const GeoDetail>();

        return eStatusOK;
    }

    std::vector<RenderContextPtr> contexts(run.size());
    for (std::size_t j = 0; j < run.size(); ++j) {
        contexts[j] = iterationContext(base, loopName, iterations[run[j]], numIterations, pieces, GeoDetailConstPtr());
    }
    std::vector<GeoDetailConstPtr> results(run.size());
    std::vector<std::string> errors(run.size());
    std::vector<RenderRoIRetCode> codes(run.size(), eRenderRoIRetCodeOk);
    const auto cook = [&](std::size_t j) {
        codes[j] = body->evaluateGeometry(EvaluateGeometryArgs(args.time, args.view, args.byPassCache, contexts[j]), &results[j], &errors[j]);
    };

    // The first iteration cooks alone, so what every iteration shares upstream of the
    // GeoForEachBegins is cooked once before the others start.
    cook(0);
    if ((codes[0] == eRenderRoIRetCodeOk) && (run.size() > 1)) {
        QThread* const callingThread = QThread::currentThread();
        const FrameRenderContext* const frameContext = AppTLS::currentFrameContext();
        const IsolatedJob* const job = IsolatedJob::current();
        RenderCancellation cancel(this);
        parallelForCancellable((int)run.size() - 1, appPTR->getNCPUsAvailableForEffect(), cancel, [&](int i) {
            AppTLS::SpawnedThreadScope spawnedThreadTLS(callingThread, frameContext, job, AppTLS::eSpawnKindHostFrameThreading);
            const std::size_t j = (std::size_t)i + 1;
            if (cancel.check()) {
                codes[j] = eRenderRoIRetCodeAborted;

                return;
            }
            cook(j);
        });
    }
    if (aborted()) {
        return eStatusFailed;
    }
    for (std::size_t j = 0; j < run.size(); ++j) {
        if (codes[j] == eRenderRoIRetCodeAborted) {
            return eStatusFailed;
        }
        if ((codes[j] != eRenderRoIRetCodeOk) || !results[j]) {
            return fail(tr("iteration %1 failed: %2").arg(run[j]).arg(QString::fromUtf8(errors[j].c_str())).toStdString(), errorMessage);
        }
    }

    std::vector<std::string> warnings;
    GeoDetail merged = GeoMerge::merge(std::span<const GeoDetailConstPtr>(results), &warnings);
    if (warnings.empty()) {
        postMessage(eMessageTypeInfo, std::string());
    } else {
        postMessage(eMessageTypeWarning, getScriptName_mt_safe() + ": " + warnings.front());
    }
    *outputDetail = std::make_shared<const GeoDetail>(std::move(merged));

    return eStatusOK;
} // GeoForEachEnd::renderGeometry

NATRON_NAMESPACE_EXIT
