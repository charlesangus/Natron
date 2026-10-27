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

#include "USDTransform.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

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

bool
isBlank(const std::string& s)
{
    return s.find_first_not_of(" \t\r\n") == std::string::npos;
}

/**
 * @brief Whether order, the op order spec authors, is an Append-mode USDTransform's: it ends with
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

struct USDTransform::Implementation {
    mutable std::mutex lock;
    // The matches of matchedPattern on the stack whose hash is matchedStack, the op name each gets
    // (from matchedOpName) and the op order an Append-mode op goes after on each, kept so that an
    // evaluation that changes none of them does not read the stage again.
    bool matchesValid = false;
    U64 matchedStack = 0;
    std::string matchedPattern;
    std::string matchedOpName;
    std::vector<PXR_NS::SdfPath> matches;
    std::vector<PXR_NS::TfToken> opNames;
    std::vector<PXR_NS::VtTokenArray> baseOrders;
    // From the last evaluation, for the handles and the centre pivot button.
    ScenePayload lastUpstream;
    std::string handlePrim;
    std::string handleOp;
};

USDTransform::USDTransform(NodePtr node)
    : NativeEffectBase(node)
    , _imp(new Implementation())
{
}

USDTransform::~USDTransform()
{
}

const NodeSpec*
USDTransform::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
USDTransform::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobStringPtr primPattern = createKnob<KnobString>(tr("Prim Pattern"));
    primPattern->setName(kUSDTransformParamPrimPattern);
    primPattern->setHintToolTip(tr("The prims to transform: absolute prim paths separated by spaces, whose names may use * "
                                   "(any run of characters) and ? (any one character), such as /World/Geo/*. "
                                   "Only prims that can hold a transform match."));
    primPattern->setAnimationEnabled(false);
    page->addKnob(primPattern);
    _primPattern = primPattern;

    KnobChoicePtr mode = createKnob<KnobChoice>(tr("Mode"));
    mode->setName(kUSDTransformParamMode);
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
    pivot->setName(kUSDTransformParamPivot);
    pivot->setHintToolTip(tr("The point the rotation and scale are about, in the space the transform applies in: the "
                             "prim's local space."));
    for (int d = 0; d < 3; ++d) {
        pivot->setDefaultValue(0., d);
    }
    page->addKnob(pivot);
    _pivot = pivot;

    KnobButtonPtr centrePivot = createKnob<KnobButton>(tr("Centre Pivot"));
    centrePivot->setName(kUSDTransformParamCentrePivot);
    centrePivot->setHintToolTip(tr("Sets the pivot to the centre of the matched prims' bounds in their local space, at the "
                                   "current frame."));
    page->addKnob(centrePivot);
    _centrePivot = centrePivot;
} // USDTransform::initializeKnobs

USDTransform::ModeEnum
USDTransform::getMode() const
{
    KnobChoicePtr mode = _mode.lock();

    return (mode && (mode->getValue() == (int)eModeReplace)) ? eModeReplace : eModeAppend;
}

KnobDoublePtr
USDTransform::getPivotKnob() const
{
    return _pivot.lock();
}

std::string
USDTransform::getBaseOpName() const
{
    return kOpPrefix + getScriptName_mt_safe();
}

void
USDTransform::onScriptNameChanged(const std::string& /*fullyQualifiedName*/)
{
    // The op name is the node's name: the layer must be authored again under the new one.
    invalidateSceneLayer();
    NodePtr node = getNode();
    if (node) {
        node->markHashDirty();
    }
}

void
USDTransform::appendNodeContentHash(ContentHasher& h) const
{
    h.appendString(getBaseOpName());
}

bool
USDTransform::knobChanged(KnobI* k,
                          ValueChangedReasonEnum /*reason*/,
                          ViewSpec /*view*/,
                          double /*time*/,
                          bool /*originatedFromMainThread*/)
{
    KnobButtonPtr button = _centrePivot.lock();
    if (!button || (k != button.get())) {
        return false;
    }
    std::string error;
    if (!centrePivot(&error)) {
        message(eMessageTypeWarning, error);
    }

    return true;
}

bool
USDTransform::getHandleTarget(std::string* primPath,
                              std::string* opName) const
{
    std::lock_guard<std::mutex> l(_imp->lock);

    if (_imp->handlePrim.empty()) {
        return false;
    }
    *primPath = _imp->handlePrim;
    *opName = _imp->handleOp;

    return true;
}

bool
USDTransform::centrePivot(std::string* errorMessage)
{
    ScenePayload upstream;
    std::vector<PXR_NS::SdfPath> matches;
    {
        std::lock_guard<std::mutex> l(_imp->lock);
        upstream = _imp->lastUpstream;
        if (_imp->matchesValid) {
            matches = _imp->matches;
        }
    }
    KnobDoublePtr pivot = _pivot.lock();
    std::string borrowError;
    SceneQueryPool::Lease lease;
    if (!upstream.isEmpty() && !matches.empty()) {
        lease = SceneQueryPool::instance().borrow(upstream, &borrowError);
    }
    PXR_NS::UsdStageRefPtr stage = lease.getStage();
    if (!pivot || !stage || matches.empty()) {
        *errorMessage = tr("The pattern matches no prim to centre the pivot on.").toStdString();

        return false;
    }
    AppInstancePtr app = getApp();
    const double fps = (app && app->getProject()) ? app->getProject()->getProjectFrameRate() : 24.;
    const double timeCode = SceneOps::frameToTimeCode(upstream, getCurrentTime(), fps);
    PXR_NS::UsdGeomBBoxCache bboxes(PXR_NS::UsdTimeCode(timeCode), { PXR_NS::UsdGeomTokens->default_, PXR_NS::UsdGeomTokens->render, PXR_NS::UsdGeomTokens->proxy }, true);
    PXR_NS::GfRange3d range;
    for (const PXR_NS::SdfPath& path : matches) {
        const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(path);
        if (prim) {
            range.UnionWith(bboxes.ComputeUntransformedBound(prim).ComputeAlignedRange());
        }
    }
    // Given back before the knobs change: setting them may evaluate the graph on this thread.
    lease.release();
    if (range.IsEmpty()) {
        *errorMessage = tr("The matched prims have no bounds to centre the pivot on.").toStdString();

        return false;
    }
    const PXR_NS::GfVec3d centre = range.GetMidpoint();
    // One undoable edit for the three values.
    setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOnCreateNewCommand);
    for (int d = 0; d < 3; ++d) {
        pivot->setValueFromPlugin(centre[d], ViewSpec::all(), d);
    }
    setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOff);

    return true;
} // USDTransform::centrePivot

StatusEnum
USDTransform::renderScene(const SceneRenderActionArgs& args,
                          ScenePayload* outputScene,
                          std::string* errorMessage)
{
    const ScenePayload upstream = args.getInputScene(0);
    KnobStringPtr patternKnob = _primPattern.lock();
    const std::string pattern = patternKnob ? patternKnob->getValue() : std::string();
    const std::string baseOpName = getBaseOpName();

    // What the layer reads of the input: the matches, the op name each gets and the order an
    // Append-mode op goes after. Read here, so the layer is keyed on them alone.
    std::vector<PXR_NS::SdfPath> matches;
    std::vector<PXR_NS::TfToken> opNames;
    std::vector<PXR_NS::VtTokenArray> baseOrders;
    if (!upstream.isEmpty() && !isBlank(pattern)) {
        bool cached = false;
        {
            std::lock_guard<std::mutex> l(_imp->lock);
            if (_imp->matchesValid && (_imp->matchedStack == upstream.getStackHash()) && (_imp->matchedPattern == pattern) && (_imp->matchedOpName == baseOpName)) {
                matches = _imp->matches;
                opNames = _imp->opNames;
                baseOrders = _imp->baseOrders;
                cached = true;
            }
        }
        if (!cached) {
            SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(upstream, errorMessage);
            PXR_NS::UsdStageRefPtr stage = lease.getStage();
            if (!stage) {
                if (errorMessage->empty()) {
                    *errorMessage = tr("USD could not compose the input scene.").toStdString();
                }
                setPersistentMessage(eMessageTypeError, *errorMessage);

                return eStatusFailed;
            }
            std::vector<PXR_NS::SdfPath> found;
            if (!SceneNodeAuthoring::matchPrims(stage, pattern, &found, errorMessage)) {
                setPersistentMessage(eMessageTypeError, *errorMessage);

                return eStatusFailed;
            }
            for (const PXR_NS::SdfPath& path : found) {
                const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(path);
                if (prim.IsA<PXR_NS::UsdGeomXformable>()) {
                    matches.push_back(path);
                    opNames.push_back(PXR_NS::TfToken(getFreeOpName(prim, baseOpName)));
                    baseOrders.push_back(getAppendBaseOrder(prim));
                }
            }
        }
    }

    {
        std::lock_guard<std::mutex> l(_imp->lock);
        _imp->lastUpstream = upstream;
        _imp->matchesValid = !upstream.isEmpty() && !isBlank(pattern);
        _imp->matchedStack = upstream.getStackHash();
        _imp->matchedPattern = pattern;
        _imp->matchedOpName = baseOpName;
        _imp->matches = matches;
        _imp->opNames = opNames;
        _imp->baseOrders = baseOrders;
        _imp->handlePrim = matches.empty() ? std::string() : matches.front().GetString();
        _imp->handleOp = opNames.empty() ? std::string() : opNames.front().GetString();
    }

    if (matches.empty()) {
        // Nothing to transform: the scene goes through untouched.
        *outputScene = upstream;
        if (!upstream.isEmpty() && !isBlank(pattern)) {
            setPersistentMessage(eMessageTypeWarning, tr("The prim pattern \"%1\" matches no prim that can hold a transform; "
                                                         "the scene is passed through unchanged.")
                                                          .arg(QString::fromStdString(pattern))
                                                          .toStdString());
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
        KnobDoublePtr translate = _transform.translate.lock();
        KnobDoublePtr rotate = _transform.rotate.lock();
        KnobDoublePtr scale = _transform.scale.lock();
        KnobDoublePtr uniformScale = _transform.uniformScale.lock();
        KnobChoicePtr rotateOrder = _transform.rotateOrder.lock();
        KnobDoublePtr pivot = _pivot.lock();
        if (!translate || !rotate || !scale || !uniformScale || !rotateOrder || !pivot) {
            *error = tr("The node is being destroyed").toStdString();

            return false;
        }
        using SceneNodeAuthoring::isAnimated;
        const bool animated = isAnimated(translate) || isAnimated(rotate) || isAnimated(scale) || isAnimated(uniformScale) || isAnimated(pivot);
        const SceneRotateOrderEnum order = (SceneRotateOrderEnum)rotateOrder->getValue();
        auto matrixAt = [&](double frame) {
            const double uniform = uniformScale->getValueAtTime(frame, 0);

            return SceneNodeAuthoring::composeTransformMatrix(PXR_NS::GfVec3d(translate->getValueAtTime(frame, 0), translate->getValueAtTime(frame, 1), translate->getValueAtTime(frame, 2)),
                                                              PXR_NS::GfVec3d(rotate->getValueAtTime(frame, 0), rotate->getValueAtTime(frame, 1), rotate->getValueAtTime(frame, 2)),
                                                              PXR_NS::GfVec3d(scale->getValueAtTime(frame, 0) * uniform, scale->getValueAtTime(frame, 1) * uniform, scale->getValueAtTime(frame, 2) * uniform),
                                                              PXR_NS::GfVec3d(pivot->getValueAtTime(frame, 0), pivot->getValueAtTime(frame, 1), pivot->getValueAtTime(frame, 2)),
                                                              order);
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
} // USDTransform::renderScene

NATRON_NAMESPACE_EXIT
