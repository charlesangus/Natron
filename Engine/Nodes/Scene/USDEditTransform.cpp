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

#include "USDEditTransform.h"

#include <algorithm>
#include <list>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <QCoreApplication>
#include <QMetaObject>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/ContentHasher.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/Project.h"
#include <pxr/usd/usdGeom/modelAPI.h>

NATRON_NAMESPACE_ENTER

namespace {
const char* const kOpPrefix = "xformOp:transform:";

// The op name the node authors on prim: base, or base_2, base_3, ... if prim has an attribute of
// that name already.
std::string
getFreeOpName(const PXR_NS::UsdPrim& prim,
              const std::string& base)
{
    if (!prim || !prim.HasAttribute(PXR_NS::TfToken(base))) {
        return base;
    }
    for (int i = 2;; ++i) {
        const std::string candidate = base + "_" + std::to_string(i);
        if (!prim.HasAttribute(PXR_NS::TfToken(candidate))) {
            return candidate;
        }
    }
}

/**
 * @brief Whether order, the op order spec authors, is an Append-mode USDEditTransform's: it ends with
 * an op of this node's naming that spec's layer authors too, after the order the node saw, and
 * holds no reset.
 **/
bool
isAppendedTransformOrder(const PXR_NS::SdfPropertySpecHandle& spec,
                         const PXR_NS::VtTokenArray& order)
{
    if (order.empty() || (std::find(order.begin(), order.end(), PXR_NS::UsdGeomXformOpTypes->resetXformStack) != order.end())) {
        return false;
    }
    const std::string& last = order.back().GetString();
    if (last.compare(0, std::char_traits<char>::length(kOpPrefix), kOpPrefix) != 0) {
        return false;
    }
    const PXR_NS::SdfLayerHandle layer = spec->GetLayer();

    return layer && bool(layer->GetAttributeAtPath(spec->GetPath().GetPrimPath().AppendProperty(order.back())));
}

/**
 * @brief The op order an Append-mode transform of prim goes after. USD composes xformOpOrder as
 * one value, the strongest opinion's, so when a merge stacks two branches that each appended a
 * transform to the prim, the composed order holds only the stronger branch's. This adds the
 * opinions up instead, weakest first: an appended transform's opinion adds its ops to the order
 * so far, and any other opinion (a file's, a created prim's, a Replace-mode transform's) replaces
 * it, as it does in USD.
 **/
PXR_NS::VtTokenArray
getAppendBaseOrder(const PXR_NS::UsdPrim& prim)
{
    PXR_NS::VtTokenArray result;
    const PXR_NS::UsdAttribute attribute = prim ? PXR_NS::UsdGeomXformable(prim).GetXformOpOrderAttr() : PXR_NS::UsdAttribute();

    if (!attribute) {
        return result;
    }
    // Strongest first. xformOpOrder is uniform, so no value clip contributes at the default time.
    const PXR_NS::SdfPropertySpecHandleVector stack = attribute.GetPropertyStack(PXR_NS::UsdTimeCode::Default());
    for (PXR_NS::SdfPropertySpecHandleVector::const_reverse_iterator it = stack.rbegin(); it != stack.rend(); ++it) {
        const PXR_NS::SdfPropertySpecHandle& spec = *it;
        if (!spec || !spec->HasDefaultValue()) {
            continue;
        }
        const PXR_NS::VtValue value = spec->GetDefaultValue();
        if (value.IsHolding<PXR_NS::SdfValueBlock>()) {
            result = PXR_NS::VtTokenArray();
            continue;
        }
        if (!value.IsHolding<PXR_NS::VtTokenArray>()) {
            continue;
        }
        const PXR_NS::VtTokenArray& order = value.UncheckedGet<PXR_NS::VtTokenArray>();
        if (!isAppendedTransformOrder(spec, order)) {
            result = order;
            continue;
        }
        for (const PXR_NS::TfToken& op : order) {
            if (std::find(result.begin(), result.end(), op) == result.end()) {
                result.push_back(op);
            }
        }
    }

    return result;
} // getAppendBaseOrder
} // anonymous namespace

namespace {
// Evaluation contexts a result is kept for at once.
const std::size_t kContextResults = 4;
} // anonymous namespace

struct USDEditTransform::Implementation {
    // What the last evaluation in one context found, for the handles, the centre pivot button
    // and the viewers asking which prims the node transforms.
    struct Result {
        U64 context = 0;
        ScenePayload upstream;
        // The matches of matchedPattern on the stack whose hash is matchedStack, the op name each
        // gets (from matchedOpName) and the op order an Append-mode op goes after on each, kept so
        // that an evaluation that changes none of them does not read the stage again.
        bool matchesValid = false;
        U64 matchedStack = 0;
        std::string matchedPattern;
        std::string matchedOpName;
        std::vector<PXR_NS::SdfPath> matches;
        std::vector<PXR_NS::TfToken> opNames;
        std::vector<PXR_NS::VtTokenArray> baseOrders;
        std::string handlePrim;
        std::string handleOp;
    };

    mutable std::mutex lock;
    // Most recently evaluated first.
    std::list<Result> results;
    U64 opReadCount = 0;
    PrimPatternCache patternMatches;
    // The request whose result the button applies. Under lock.
    std::shared_ptr<CentrePivotJob> pivotJob;
    SceneWorker pivotWorker;

    // The result of context, or null. Call with lock held.
    const Result* find(U64 context) const
    {
        for (const Result& result : results) {
            if (result.context == context) {
                return &result;
            }
        }

        return nullptr;
    }

    // The result of the latest evaluation in any context, or null. Call with lock held.
    const Result* latest() const
    {
        return results.empty() ? nullptr : &results.front();
    }
};

USDEditTransform::USDEditTransform(NodePtr node)
    : NativeEffectBase(node)
    , _imp(new Implementation())
{
}

USDEditTransform::~USDEditTransform()
{
    {
        std::lock_guard<std::mutex> l(_imp->lock);
        if (_imp->pivotJob) {
            _imp->pivotJob->cancel();
        }
    }
    _imp->pivotWorker.stop();
}

const NodeSpec*
USDEditTransform::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
USDEditTransform::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    _primPattern.create(this, page, kUSDEditTransformParamPrimPattern, tr("Prim Pattern"),
                        tr("The prims to transform. Only prims that can hold a transform match."));

    KnobChoicePtr mode = createKnob<KnobChoice>(tr("Mode"));
    mode->setName(kUSDEditTransformParamMode);
    mode->setHintToolTip(tr("How the transform combines with the transform each prim already has."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("append", tr("Append").toStdString(),
                                       tr("Adds the transform after the prim's existing ops, so it acts in the prim's local "
                                          "space, under the existing transform.")
                                           .toStdString()));
        options.push_back(ChoiceOption("replace", tr("Replace").toStdString(),
                                       tr("Discards the prim's existing ops and its parent's transform: the transform places "
                                          "the prim in world space.")
                                           .toStdString()));
        mode->populateChoices(options);
    }
    mode->setDefaultValue((int)eModeAppend);
    mode->setAnimationEnabled(false);
    page->addKnob(mode);
    _mode = mode;

    _transform.create(this, page);

    KnobDoublePtr pivot = createKnob<KnobDouble>(tr("Pivot"), 3);
    pivot->setName(kUSDEditTransformParamPivot);
    pivot->setHintToolTip(tr("The point the rotation and scale are about, in the space the transform applies in: the "
                             "prim's local space."));
    for (int d = 0; d < 3; ++d) {
        pivot->setDefaultValue(0., d);
    }
    page->addKnob(pivot);
    _pivot = pivot;

    KnobButtonPtr centrePivot = createKnob<KnobButton>(tr("Centre Pivot"));
    centrePivot->setName(kUSDEditTransformParamCentrePivot);
    centrePivot->setHintToolTip(tr("Sets the pivot to the centre of the matched prims' bounds in their local space, at the "
                                   "current frame."));
    page->addKnob(centrePivot);
    _centrePivot = centrePivot;
} // USDEditTransform::initializeKnobs

USDEditTransform::ModeEnum
USDEditTransform::getMode() const
{
    KnobChoicePtr mode = _mode.lock();

    return (mode && (mode->getValue() == (int)eModeReplace)) ? eModeReplace : eModeAppend;
}

KnobDoublePtr
USDEditTransform::getPivotKnob() const
{
    return _pivot.lock();
}

std::string
USDEditTransform::getBaseOpName() const
{
    return kOpPrefix + getScriptName_mt_safe();
}

void
USDEditTransform::onScriptNameChanged(const std::string& /*fullyQualifiedName*/)
{
    // The op name is the node's name: the layer must be authored again under the new one.
    invalidateSceneLayer();
    NodePtr node = getNode();
    if (node) {
        node->markHashDirty();
    }
}

void
USDEditTransform::appendNodeContentHash(ContentHasher& h) const
{
    h.appendString(getBaseOpName());
}

bool
USDEditTransform::knobChanged(KnobI* k,
                              ValueChangedReasonEnum /*reason*/,
                              ViewSpec /*view*/,
                              double /*time*/,
                              bool /*originatedFromMainThread*/)
{
    KnobButtonPtr button = _centrePivot.lock();
    if (!button || (k != button.get())) {
        return false;
    }
    SceneEvaluationContext context;
    if (SceneNodeAuthoring::getShowingViewerContext(this, &context)) {
        startCentrePivot(&context);
    } else {
        startCentrePivot(nullptr);
    }

    return true;
}

void
USDEditTransform::getMatchedPrims(std::vector<std::string>* primPaths,
                                  std::vector<std::string>* opNames) const
{
    std::lock_guard<std::mutex> l(_imp->lock);

    primPaths->clear();
    opNames->clear();
    const Implementation::Result* result = _imp->latest();
    for (std::size_t i = 0; result && (i < result->matches.size()) && (i < result->opNames.size()); ++i) {
        primPaths->push_back(result->matches[i].GetString());
        opNames->push_back(result->opNames[i].GetString());
    }
}

U64
USDEditTransform::getStageReadCount() const
{
    std::lock_guard<std::mutex> l(_imp->lock);

    return _imp->patternMatches.getQueryCount() + _imp->opReadCount;
}

bool
USDEditTransform::transformsPrim(const std::string& primPath,
                                 const SceneEvaluationContext& context) const
{
    const std::string pattern = _primPattern.getExpression();
    ScenePayload upstream;
    {
        std::lock_guard<std::mutex> l(_imp->lock);
        const Implementation::Result* result = _imp->find(context.id);
        if (!result) {
            return false;
        }
        if (result->matchesValid && (result->matchedPattern == pattern)) {
            for (const PXR_NS::SdfPath& path : result->matches) {
                if (path.GetString() == primPath) {
                    return true;
                }
            }

            return false;
        }
        upstream = result->upstream;
    }
    if (upstream.isEmpty() || PrimSelector::isBlank(pattern)) {
        return false;
    }
    std::string error;
    SceneQueryPool::Lease lease;
    SceneNodeAuthoring::PatternMatches found;
    const SceneNodeAuthoring::PatternFilter xformable = [](const PXR_NS::UsdPrim& prim) {
        return prim.IsA<PXR_NS::UsdGeomXformable>();
    };
    // The pattern cache is the one renderScene() fills, so a pattern already matched in this
    // context on this load set is taken without a query.
    SceneEvaluationContext asking(context.id, context.loadSet);
    if (!SceneNodeAuthoring::matchPattern(&_imp->patternMatches, upstream, pattern, asking, 0, xformable, &lease, &found, &error)) {
        return false;
    }
    for (const PXR_NS::SdfPath& path : found.paths) {
        if (path.GetString() == primPath) {
            return true;
        }
    }

    return false;
}

bool
USDEditTransform::getHandleTarget(std::string* primPath,
                                  std::string* opName) const
{
    std::lock_guard<std::mutex> l(_imp->lock);

    const Implementation::Result* result = _imp->latest();
    if (!result || result->handlePrim.empty()) {
        return false;
    }
    *primPath = result->handlePrim;
    *opName = result->handleOp;

    return true;
}

CentrePivotJob::StateEnum
CentrePivotJob::wait() const
{
    std::unique_lock<std::mutex> l(_lock);

    _done.wait(l, [this]() { return _state != eStateRunning; });

    return _state;
}

CentrePivotJob::StateEnum
CentrePivotJob::getState() const
{
    std::lock_guard<std::mutex> l(_lock);

    return _state;
}

void
CentrePivotJob::getCentre(double* x,
                          double* y,
                          double* z) const
{
    std::lock_guard<std::mutex> l(_lock);

    *x = _centre[0];
    *y = _centre[1];
    *z = _centre[2];
}

std::string
CentrePivotJob::getError() const
{
    std::lock_guard<std::mutex> l(_lock);

    return _error;
}

void
CentrePivotJob::finish(StateEnum state,
                       const double* centre,
                       const std::string& error)
{
    {
        std::lock_guard<std::mutex> l(_lock);
        _state = state;
        if (centre) {
            std::copy(centre, centre + 3, _centre);
        }
        _error = error;
    }
    _done.notify_all();
}

namespace {
struct CentreSnapshot {
    ScenePayload upstream;
    std::vector<PXR_NS::SdfPath> matches;
    SceneLoadSetPtr loadSet;
    double timeCode = 0.;
    std::string failure;
};

// Adds to range the extentsHint of every prim at or under prim whose payload stage left
// unloaded, in prim's local space; counts the ones with no hint in *unmeasured.
void
addUnloadedHints(const PXR_NS::UsdPrim& prim,
                 const PXR_NS::UsdTimeCode time,
                 const CentrePivotJob& job,
                 PXR_NS::GfRange3d* range,
                 int* unmeasured)
{
    PXR_NS::UsdGeomXformCache xforms(time);
    // The default predicate leaves unloaded prims out, and they are the ones wanted.
    const PXR_NS::Usd_PrimFlagsPredicate predicate = PXR_NS::UsdPrimIsActive && PXR_NS::UsdPrimIsDefined && !PXR_NS::UsdPrimIsAbstract;
    PXR_NS::UsdPrimRange prims(prim, predicate);
    for (PXR_NS::UsdPrimRange::iterator it = prims.begin(); it != prims.end(); ++it) {
        if (job.isCancelled()) {
            return;
        }
        const PXR_NS::UsdPrim& at = *it;
        if (!at.HasPayload() || at.IsLoaded()) {
            continue;
        }
        it.PruneChildren();
        PXR_NS::VtVec3fArray hint;
        const PXR_NS::UsdAttribute attribute = PXR_NS::UsdGeomModelAPI(at).GetExtentsHintAttr();
        // Pairs of min and max, one per purpose: default, render, proxy, guide.
        if (!attribute || !attribute.Get(&hint, time) || (hint.size() < 2)) {
            ++*unmeasured;
            continue;
        }
        bool resets = false;
        const PXR_NS::GfMatrix4d toPrim = (at.GetPath() == prim.GetPath()) ? PXR_NS::GfMatrix4d(1.) : xforms.ComputeRelativeTransform(at, prim, &resets);
        for (std::size_t i = 0; (i + 1 < hint.size()) && (i < 6); i += 2) {
            const PXR_NS::GfRange3d box(PXR_NS::GfVec3d(hint[i]), PXR_NS::GfVec3d(hint[i + 1]));
            range->UnionWith(PXR_NS::GfBBox3d(box, toPrim).ComputeAlignedRange());
        }
    }
}

void
computeCentre(const CentreSnapshot& snapshot,
              CentrePivotJob* job,
              bool noViewer)
{
    const std::string note = noViewer ? QCoreApplication::translate("USDEditTransform", "No viewer shows this node, so its unloaded branches were not searched.").toStdString() : std::string();
    const auto fail = [&](const std::string& message) {
        job->finish(CentrePivotJob::eStateFailed, nullptr, note.empty() ? message : (message + " " + note));
    };
    if (!snapshot.failure.empty()) {
        fail(snapshot.failure);

        return;
    }
    std::string borrowError;
    SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(snapshot.upstream, snapshot.loadSet, &borrowError);
    PXR_NS::UsdStageRefPtr stage = lease.getStage();
    if (!stage) {
        fail(borrowError);

        return;
    }
    const PXR_NS::UsdTimeCode time(snapshot.timeCode);
    PXR_NS::UsdGeomBBoxCache bboxes(time, { PXR_NS::UsdGeomTokens->default_, PXR_NS::UsdGeomTokens->render, PXR_NS::UsdGeomTokens->proxy }, true);
    PXR_NS::GfRange3d range;
    int unmeasured = 0;
    // Several matches can sit under one unloaded payload; its hint is counted once.
    std::set<PXR_NS::SdfPath> hintedAncestors;
    for (const PXR_NS::SdfPath& path : snapshot.matches) {
        if (job->isCancelled()) {
            job->finish(CentrePivotJob::eStateFailed, nullptr, std::string());

            return;
        }
        // A match inside a payload the load set leaves unloaded is not on the stage.
        const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(path);
        if (!prim) {
            PXR_NS::UsdPrim ancestor;
            for (PXR_NS::SdfPath at = path.GetParentPath(); !at.IsEmpty() && !ancestor; at = at.GetParentPath()) {
                ancestor = stage->GetPrimAtPath(at);
            }
            // The payload holding the match bounds it; nothing nearer is on the stage.
            if (!ancestor || !ancestor.HasPayload() || ancestor.IsLoaded()) {
                ++unmeasured;
            } else if (hintedAncestors.insert(ancestor.GetPath()).second) {
                addUnloadedHints(ancestor, time, *job, &range, &unmeasured);
            }
            continue;
        }
        if (!prim.HasPayload() || prim.IsLoaded()) {
            range.UnionWith(bboxes.ComputeUntransformedBound(prim).ComputeAlignedRange());
        }
        addUnloadedHints(prim, time, *job, &range, &unmeasured);
    }
    if (job->isCancelled()) {
        job->finish(CentrePivotJob::eStateFailed, nullptr, std::string());

        return;
    }
    // Given back before the job ends: the pool must not hold a stage for a result nobody uses.
    lease.release();
    if (range.IsEmpty()) {
        std::string message = QCoreApplication::translate("USDEditTransform", "The matched prims have no bounds to centre the pivot on.").toStdString();
        if (unmeasured > 0) {
            message += " " + QCoreApplication::translate("USDEditTransform", "%1 matches could not be measured: they are unloaded and author no extentsHint.").arg(unmeasured).toStdString();
        }
        fail(message);

        return;
    }
    const PXR_NS::GfVec3d centre = range.GetMidpoint();
    const double values[3] = { centre[0], centre[1], centre[2] };
    job->finish(CentrePivotJob::eStateDone, values, std::string());
}
} // anonymous namespace

std::shared_ptr<CentrePivotJob>
USDEditTransform::startCentrePivot(const SceneEvaluationContext* context)
{
    std::shared_ptr<CentrePivotJob> job = std::make_shared<CentrePivotJob>();
    auto snapshot = std::make_shared<CentreSnapshot>();
    {
        std::lock_guard<std::mutex> l(_imp->lock);
        if (_imp->pivotJob) {
            _imp->pivotJob->cancel();
        }
        _imp->pivotJob = job;
        // The viewer's result when it evaluated the node, else the latest: the matches are
        // what the load set was searched for, and the same stage serves any context.
        const Implementation::Result* result = context ? _imp->find(context->id) : nullptr;
        if (!result) {
            result = _imp->latest();
        }
        if (result) {
            snapshot->upstream = result->upstream;
            if (result->matchesValid) {
                snapshot->matches = result->matches;
            }
        }
    }
    if (context) {
        snapshot->loadSet = context->loadSet;
    } else {
        snapshot->loadSet = std::make_shared<const SceneLoadSet>(SceneLoadSet::none());
    }
    if (snapshot->upstream.isEmpty() || snapshot->matches.empty()) {
        snapshot->failure = tr("The pattern matches no prim to centre the pivot on.").toStdString();
    } else {
        AppInstancePtr app = getApp();
        const double fps = (app && app->getProject()) ? app->getProject()->getProjectFrameRate() : 24.;
        snapshot->timeCode = SceneOps::frameToTimeCode(snapshot->upstream, getCurrentTime(), fps);
    }

    const bool noViewer = !context;
    NodeWPtr weakNode = getNode();
    _imp->pivotWorker.post([job, snapshot, noViewer, weakNode]() {
        try {
            computeCentre(*snapshot, job.get(), noViewer);
        } catch (...) {
            job->finish(CentrePivotJob::eStateFailed, nullptr, std::string());
        }
        if (QCoreApplication::instance()) {
            QMetaObject::invokeMethod(QCoreApplication::instance(), [job, weakNode]() {
                NodePtr node = weakNode.lock();
                USDEditTransform* effect = node ? dynamic_cast<USDEditTransform*>(node->getEffectInstance().get()) : nullptr;
                if (effect) {
                    effect->applyCentrePivot(job);
                } }, Qt::QueuedConnection);
        } }, [job]() { job->finish(CentrePivotJob::eStateFailed, nullptr, std::string()); });

    return job;
}

bool
USDEditTransform::applyCentrePivot(const std::shared_ptr<CentrePivotJob>& job)
{
    {
        std::lock_guard<std::mutex> l(_imp->lock);
        if (job->isCancelled() || (_imp->pivotJob != job)) {
            return false;
        }
        _imp->pivotJob.reset();
    }
    KnobDoublePtr pivot = _pivot.lock();
    if (!pivot || (job->getState() == CentrePivotJob::eStateRunning)) {
        return false;
    }
    if (job->getState() == CentrePivotJob::eStateFailed) {
        message(eMessageTypeWarning, job->getError());

        return false;
    }
    double centre[3];
    job->getCentre(&centre[0], &centre[1], &centre[2]);
    // One undoable edit for the three values.
    setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOnCreateNewCommand);
    for (int d = 0; d < 3; ++d) {
        pivot->setValueFromPlugin(centre[d], ViewSpec::all(), d);
    }
    setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOff);

    return true;
}

StatusEnum
USDEditTransform::renderScene(const SceneRenderActionArgs& args,
                              ScenePayload* outputScene,
                              std::string* errorMessage)
{
    const ScenePayload upstream = args.getInputScene(0);
    const std::string pattern = _primPattern.getExpression();
    const std::string baseOpName = getBaseOpName();
    if (!PrimSelector::validate(pattern, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }

    // What the layer reads of the input: the matches, the op name each gets and the order an
    // Append-mode op goes after. Read here, so the layer is keyed on them alone.
    std::vector<PXR_NS::SdfPath> matches;
    std::vector<PXR_NS::TfToken> opNames;
    std::vector<PXR_NS::VtTokenArray> baseOrders;
    SceneNodeAuthoring::PatternMatches noMatch;
    if (!upstream.isEmpty() && !PrimSelector::isBlank(pattern)) {
        SceneQueryPool::Lease lease;
        SceneNodeAuthoring::PatternMatches found;
        const SceneNodeAuthoring::PatternFilter xformable = [](const PXR_NS::UsdPrim& prim) {
            return prim.IsA<PXR_NS::UsdGeomXformable>();
        };
        if (!SceneNodeAuthoring::matchPattern(&_imp->patternMatches, upstream, pattern, args.context, 0, xformable, &lease, &found, errorMessage)) {
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }
        matches = found.paths;
        noMatch = found;
        bool cached = false;
        {
            std::lock_guard<std::mutex> l(_imp->lock);
            // The same prims of the same stack hold the same ops whatever else is loaded.
            const Implementation::Result* previous = _imp->find(args.context.id);
            if (previous && previous->matchesValid && (previous->matchedStack == upstream.getStackHash()) && (previous->matchedOpName == baseOpName) && (previous->matches == matches)) {
                opNames = previous->opNames;
                baseOrders = previous->baseOrders;
                cached = true;
            }
        }
        if (!cached && !matches.empty()) {
            if (!lease.isValid()) {
                lease = SceneQueryPool::instance().borrow(upstream, args.context.loadSet, errorMessage);
            }
            PXR_NS::UsdStageRefPtr stage = lease.getStage();
            {
                std::lock_guard<std::mutex> l(_imp->lock);
                ++_imp->opReadCount;
            }
            if (!stage) {
                if (errorMessage->empty()) {
                    *errorMessage = tr("USD could not compose the input scene.").toStdString();
                }
                setPersistentMessage(eMessageTypeError, *errorMessage);

                return eStatusFailed;
            }
            for (const PXR_NS::SdfPath& path : matches) {
                const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(path);
                opNames.push_back(PXR_NS::TfToken(getFreeOpName(prim, baseOpName)));
                baseOrders.push_back(getAppendBaseOrder(prim));
            }
        }
    }

    {
        std::lock_guard<std::mutex> l(_imp->lock);
        Implementation::Result result;
        result.context = args.context.id;
        result.upstream = upstream;
        result.matchesValid = !upstream.isEmpty() && !PrimSelector::isBlank(pattern);
        result.matchedStack = upstream.getStackHash();
        result.matchedPattern = pattern;
        result.matchedOpName = baseOpName;
        result.matches = matches;
        result.opNames = opNames;
        result.baseOrders = baseOrders;
        result.handlePrim = matches.empty() ? std::string() : matches.front().GetString();
        result.handleOp = opNames.empty() ? std::string() : opNames.front().GetString();
        for (std::list<Implementation::Result>::iterator it = _imp->results.begin(); it != _imp->results.end(); ++it) {
            if (it->context == result.context) {
                _imp->results.erase(it);
                break;
            }
        }
        _imp->results.push_front(std::move(result));
        while (_imp->results.size() > kContextResults) {
            _imp->results.pop_back();
        }
    }

    if (matches.empty()) {
        // Nothing to transform: the scene goes through untouched.
        *outputScene = upstream;
        if (!upstream.isEmpty() && !PrimSelector::isBlank(pattern)) {
            setPersistentMessage(eMessageTypeWarning,
                                 SceneNodeAuthoring::describeNoMatch(pattern, noMatch,
                                                                     tr("only prims that can hold a transform match, and the scene is "
                                                                        "passed through unchanged")
                                                                         .toStdString()));
        } else {
            clearPersistentMessage(false);
        }

        return eStatusOK;
    }

    const ModeEnum mode = getMode();
    ContentHasher facts;
    SceneNodeAuthoring::appendUpstreamMetadata(upstream, &facts);
    facts.appendU64(matches.size());
    for (std::size_t i = 0; i < matches.size(); ++i) {
        facts.appendString(matches[i].GetString());
        facts.appendString(opNames[i].GetString());
        if (mode == eModeAppend) {
            facts.appendU64(baseOrders[i].size());
            for (const PXR_NS::TfToken& op : baseOrders[i]) {
                facts.appendString(op.GetString());
            }
        }
    }
    SceneLayerReads reads;
    reads.upstream = SceneLayerMemo::Key::eUpstreamReadFacts;
    reads.factsDigest = facts.finalize();

    SceneLayerMemo::AuthorFunc author = [this, matches, opNames, baseOrders, mode](const SceneLayerEdit& edit,
                                                                                   std::string* error) {
        PXR_NS::UsdStageRefPtr stage = SceneNodeAuthoring::openLayerStage(edit, error);
        if (!stage) {
            if (error->empty()) {
                *error = tr("USD could not open the node's layer.").toStdString();
            }

            return false;
        }
        KnobDoublePtr pivot = _pivot.lock();
        if (!pivot) {
            *error = tr("The node is being destroyed").toStdString();

            return false;
        }
        const bool animated = SceneNodeAuthoring::isTransformAnimated(_transform, pivot);
        auto matrixAt = [this, &pivot](double frame) {
            return SceneNodeAuthoring::getTransformMatrixAt(_transform, pivot, frame);
        };

        for (std::size_t i = 0; i < matches.size(); ++i) {
            const PXR_NS::SdfPath& path = matches[i];
            const PXR_NS::TfToken& opName = opNames[i];
            PXR_NS::UsdPrim prim = stage->OverridePrim(path);
            if (!prim) {
                *error = tr("USD could not author over %1.").arg(QString::fromStdString(path.GetString())).toStdString();

                return false;
            }
            const PXR_NS::UsdAttribute op = prim.CreateAttribute(opName, PXR_NS::SdfValueTypeNames->Matrix4d, false);
            if (!SceneNodeAuthoring::authorValue(op, edit, animated, matrixAt)) {
                *error = tr("USD could not author the transform of %1.").arg(QString::fromStdString(path.GetString())).toStdString();

                return false;
            }
            PXR_NS::VtTokenArray opOrder;
            if (mode == eModeAppend) {
                opOrder = baseOrders[i];
            } else {
                opOrder.push_back(PXR_NS::UsdGeomXformOpTypes->resetXformStack);
            }
            opOrder.push_back(opName);
            const PXR_NS::UsdAttribute orderAttr = prim.CreateAttribute(PXR_NS::UsdGeomTokens->xformOpOrder, PXR_NS::SdfValueTypeNames->TokenArray, false, PXR_NS::SdfVariabilityUniform);
            if (!orderAttr || !orderAttr.Set(opOrder)) {
                *error = tr("USD could not author the op order of %1.").arg(QString::fromStdString(path.GetString())).toStdString();

                return false;
            }
        }

        return true;
    };

    if (!emitSceneLayer(upstream, reads, author, outputScene, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }
    clearPersistentMessage(false);

    return eStatusOK;
} // USDEditTransform::renderScene

NATRON_NAMESPACE_EXIT
