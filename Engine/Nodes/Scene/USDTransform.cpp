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
#include "Engine/Nodes/Scene/StageCache.h"
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
} // anonymous namespace

struct USDTransform::Implementation {
    mutable std::mutex lock;
    // The matches of matchedPattern on the stack whose hash is matchedStack, kept so that an
    // evaluation that changes neither does not traverse the stage again.
    bool matchesValid = false;
    U64 matchedStack = 0;
    std::string matchedPattern;
    std::vector<PXR_NS::SdfPath> matches;
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
    PXR_NS::UsdStageRefPtr stage = upstream.isEmpty() ? PXR_NS::UsdStageRefPtr() : StageCache::instance().getStage(upstream);
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

    std::vector<PXR_NS::SdfPath> matches;
    if (!upstream.isEmpty() && !isBlank(pattern)) {
        bool cached = false;
        {
            std::lock_guard<std::mutex> l(_imp->lock);
            if (_imp->matchesValid && (_imp->matchedStack == upstream.getStackHash()) && (_imp->matchedPattern == pattern)) {
                matches = _imp->matches;
                cached = true;
            }
        }
        if (!cached) {
            PXR_NS::UsdStageRefPtr stage = StageCache::instance().getStage(upstream);
            if (!stage) {
                *errorMessage = tr("USD could not compose the input scene.").toStdString();
                setPersistentMessage(eMessageTypeError, *errorMessage);

                return eStatusFailed;
            }
            std::vector<PXR_NS::SdfPath> found;
            if (!SceneNodeAuthoring::matchPrims(stage, pattern, &found, errorMessage)) {
                setPersistentMessage(eMessageTypeError, *errorMessage);

                return eStatusFailed;
            }
            for (const PXR_NS::SdfPath& path : found) {
                if (stage->GetPrimAtPath(path).IsA<PXR_NS::UsdGeomXformable>()) {
                    matches.push_back(path);
                }
            }
        }
    }

    std::string handleOp;
    if (!matches.empty()) {
        PXR_NS::UsdStageRefPtr stage = StageCache::instance().getStage(upstream);
        handleOp = getFreeOpName(stage ? stage->GetPrimAtPath(matches.front()) : PXR_NS::UsdPrim(), getBaseOpName());
    }
    {
        std::lock_guard<std::mutex> l(_imp->lock);
        _imp->lastUpstream = upstream;
        _imp->matchesValid = !upstream.isEmpty() && !isBlank(pattern);
        _imp->matchedStack = upstream.getStackHash();
        _imp->matchedPattern = pattern;
        _imp->matches = matches;
        _imp->handlePrim = matches.empty() ? std::string() : matches.front().GetString();
        _imp->handleOp = handleOp;
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
    const std::string baseOpName = getBaseOpName();
    SceneLayerMemo::AuthorFunc author = [this, matches, mode, baseOpName](const SceneLayerEdit& edit,
                                                                          std::string* error) {
        PXR_NS::UsdStageRefPtr upstreamStage = StageCache::instance().getStage(edit.upstream);
        PXR_NS::UsdStageRefPtr stage = SceneNodeAuthoring::openLayerStage(edit, error);
        if (!upstreamStage || !stage) {
            if (error->empty()) {
                *error = tr("USD could not compose the input scene.").toStdString();
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

        for (const PXR_NS::SdfPath& path : matches) {
            const PXR_NS::UsdPrim source = upstreamStage->GetPrimAtPath(path);
            const PXR_NS::TfToken opName(getFreeOpName(source, baseOpName));
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
                if (source) {
                    PXR_NS::UsdGeomXformable(source).GetXformOpOrderAttr().Get(&opOrder);
                }
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

    if (!emitSceneLayer(upstream, author, outputScene, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }
    clearPersistentMessage(false);

    return eStatusOK;
} // USDTransform::renderScene

NATRON_NAMESPACE_EXIT
