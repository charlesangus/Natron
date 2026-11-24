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

#include "SceneReparentNode.h"

#include <string>
#include <vector>

#include <QStringList>

#include "Engine/AppInstance.h"
#include "Engine/ChoiceOption.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/Project.h"
#include "Engine/SceneLoadSet.h"

#include <pxr/usd/usd/primRange.h>

NATRON_NAMESPACE_ENTER

namespace {
enum KindEnum {
    eKindNone = 0,
    eKindGroup,
    eKindAssembly,
    eKindComponent,
    eKindSubcomponent
};

PXR_NS::TfToken
getKindToken(int kind)
{
    switch ((KindEnum)kind) {
    case eKindGroup:
        return PXR_NS::KindTokens->group;
    case eKindAssembly:
        return PXR_NS::KindTokens->assembly;
    case eKindComponent:
        return PXR_NS::KindTokens->component;
    case eKindSubcomponent:
        return PXR_NS::KindTokens->subcomponent;
    case eKindNone:
        break;
    }

    return PXR_NS::TfToken();
}

// Where wrap mode defines its prim: beside wrapped, named name.
bool
getWrapPath(const PXR_NS::SdfPath& wrapped,
            const std::string& name,
            PXR_NS::SdfPath* wrapPath,
            std::string* errorMessage)
{
    if (!PXR_NS::SdfPath::IsValidIdentifier(name)) {
        *errorMessage = EffectInstance::tr("Wrap Name \"%1\" is not a valid prim name.").arg(QString::fromStdString(name)).toStdString();

        return false;
    }
    const PXR_NS::TfToken token(name);
    if (token == wrapped.GetNameToken()) {
        *errorMessage = EffectInstance::tr("Wrap Name must differ from the name of the prim it wraps, %1.").arg(QString::fromStdString(name)).toStdString();

        return false;
    }
    *wrapPath = wrapped.GetParentPath().AppendChild(token);

    return true;
}

// A relationship or attribute connection whose targets the move changes, as it reads afterwards.
struct WrapRetarget {
    PXR_NS::SdfPath property;
    bool isConnection = false;
    bool custom = false;
    PXR_NS::SdfVariability variability = PXR_NS::SdfVariabilityVarying;
    PXR_NS::SdfValueTypeName typeName;
    std::vector<PXR_NS::SdfPath> targets;
};

/**
 * @brief The links of stage the move of wrapped to moved changes: a target under wrapped, from a
 * property outside it, which the move would leave dangling; and a target outside wrapped from a
 * property under it, which a reference may not carry. Each is listed whole, with its property
 * and targets as they read after the move.
 **/
void
gatherWrapRetargets(const PXR_NS::UsdStageRefPtr& stage,
                    const PXR_NS::SdfPath& wrapped,
                    const PXR_NS::SdfPath& moved,
                    std::vector<WrapRetarget>* retargets)
{
    for (const PXR_NS::UsdPrim& prim : stage->Traverse()) {
        const bool inside = prim.GetPath().HasPrefix(wrapped);
        for (const PXR_NS::UsdProperty& property : prim.GetAuthoredProperties()) {
            WrapRetarget retarget;
            if (property.Is<PXR_NS::UsdRelationship>()) {
                const PXR_NS::UsdRelationship relationship = property.As<PXR_NS::UsdRelationship>();
                relationship.GetTargets(&retarget.targets);
                retarget.custom = relationship.IsCustom();
            } else if (property.Is<PXR_NS::UsdAttribute>()) {
                const PXR_NS::UsdAttribute attribute = property.As<PXR_NS::UsdAttribute>();
                if (!attribute.HasAuthoredConnections()) {
                    continue;
                }
                attribute.GetConnections(&retarget.targets);
                retarget.isConnection = true;
                retarget.custom = attribute.IsCustom();
                retarget.variability = attribute.GetVariability();
                retarget.typeName = attribute.GetTypeName();
            } else {
                continue;
            }
            bool changes = false;
            for (PXR_NS::SdfPath& target : retarget.targets) {
                if (target.HasPrefix(wrapped)) {
                    target = target.ReplacePrefix(wrapped, moved);
                    changes = changes || !inside;
                } else {
                    changes = changes || inside;
                }
            }
            if (changes) {
                retarget.property = inside ? property.GetPath().ReplacePrefix(wrapped, moved) : property.GetPath();
                retargets->push_back(retarget);
            }
        }
    }
}
} // anonymous namespace

SceneReparentNode::SceneReparentNode(NodePtr node)
    : NativeEffectBase(node)
{
}

SceneReparentNode::~SceneReparentNode()
{
}

void
SceneReparentNode::createReparentKnobs(const KnobPagePtr& page)
{
    const QString type = QString::fromStdString(getPrimTypeName());
    if (offersWrapMode()) {
        KnobChoicePtr mode = createKnob<KnobChoice>(tr("Mode"));
        mode->setName(kSceneReparentParamMode);
        mode->setHintToolTip(tr("Graft: defines the %1 at Prim Path and grafts the root prims of every input under it, so the "
                                "node graph's shape is the scene's.\n"
                                "Wrap: defines the %1 beside the input 0 prim at Prim Path, named Wrap Name, and moves "
                                "that prim and everything under it beneath the %1. The other inputs are not read.")
                                 .arg(type));
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("graft", tr("Graft").toStdString(), ""));
        options.push_back(ChoiceOption("wrap", tr("Wrap").toStdString(), ""));
        mode->populateChoices(options);
        mode->setDefaultValue((int)eSceneReparentModeGraft);
        mode->setAnimationEnabled(false);
        page->addKnob(mode);
        _mode = mode;
    }

    _primPath = createScenePrimPathKnob(this, page, getSceneNodeDefaultPrimPath(this, getPrimTypeName()),
                                        offersWrapMode()
                                            ? tr("Graft: the path of the prim the node defines. The root prims of every input are "
                                                 "grafted under it; missing ancestors of a nested path are defined as %1 prims.\n"
                                                 "Wrap: the path of the input 0 prim moved under the %1.")
                                                  .arg(type)
                                            : tr("The path of the prim the node defines. The root prims of every input are grafted "
                                                 "under it; missing ancestors of a nested path are defined as %1 prims.")
                                                  .arg(type));

    if (offersWrapMode()) {
        KnobStringPtr wrapName = createKnob<KnobString>(tr("Wrap Name"));
        wrapName->setName(kSceneReparentParamWrapName);
        wrapName->setHintToolTip(tr("Wrap: the name of the %1 the node defines beside the prim at Prim Path (by default the "
                                    "node's name, following renames until edited).")
                                     .arg(type));
        wrapName->setDefaultValue(getDefaultWrapName());
        wrapName->setAnimationEnabled(false);
        page->addKnob(wrapName);
        _wrapName = wrapName;
    }

    KnobChoicePtr kind = createKnob<KnobChoice>(tr("Kind"));
    kind->setName(kSceneReparentParamKind);
    kind->setHintToolTip(tr("The prim's model kind, which places it in the scene's model hierarchy. None authors no kind."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("none", tr("None").toStdString(), ""));
        options.push_back(ChoiceOption("group", "group", ""));
        options.push_back(ChoiceOption("assembly", "assembly", ""));
        options.push_back(ChoiceOption("component", "component", ""));
        options.push_back(ChoiceOption("subcomponent", "subcomponent", ""));
        kind->populateChoices(options);
    }
    kind->setDefaultValue((int)eKindNone);
    kind->setAnimationEnabled(false);
    page->addKnob(kind);
    _kind = kind;

    KnobBoolPtr autoRename = createKnob<KnobBool>(tr("Auto Rename"));
    autoRename->setName(kSceneReparentParamAutoRename);
    autoRename->setHintToolTip(tr("When two inputs have a root prim of the same name, gives the later input's the first free "
                                  "_1, _2, ... suffix instead of composing the two, the later input stronger."));
    autoRename->setDefaultValue(false);
    autoRename->setAnimationEnabled(false);
    page->addKnob(autoRename);
    _autoRename = autoRename;
} // SceneReparentNode::createReparentKnobs

void
SceneReparentNode::onScriptNameChanged(const std::string& /*fullyQualifiedName*/)
{
    followSceneNodeRename(_primPath.lock(), getSceneNodeDefaultPrimPath(this, getPrimTypeName()));
    followSceneNodeRename(_wrapName.lock(), getDefaultWrapName());
}

std::string
SceneReparentNode::getDefaultWrapName() const
{
    return getSceneNodeDefaultPrimPath(this, getPrimTypeName()).substr(1);
}

bool
SceneReparentNode::isWrapping() const
{
    KnobChoicePtr mode = _mode.lock();

    return mode && (mode->getValue() == (int)eSceneReparentModeWrap);
}

std::string
SceneReparentNode::getDefinedPrimPath() const
{
    KnobStringPtr primPath = _primPath.lock();
    const std::string value = primPath ? primPath->getValue() : std::string();

    if (!PXR_NS::SdfPath::IsValidPathString(value)) {
        return std::string();
    }
    const PXR_NS::SdfPath path(value);
    if (!path.IsAbsolutePath() || !path.IsPrimPath()) {
        return std::string();
    }
    if (!isWrapping()) {
        return path.GetString();
    }
    PXR_NS::SdfPath wrapPath;
    std::string error;
    KnobStringPtr wrapName = _wrapName.lock();

    return getWrapPath(path, wrapName ? wrapName->getValue() : std::string(), &wrapPath, &error) ? wrapPath.GetString() : std::string();
}

std::string
SceneReparentNode::mapToInputPath(const std::string& outputPath) const
{
    const std::string defined = getDefinedPrimPath();

    if (defined.empty() || !PXR_NS::SdfPath::IsValidPathString(outputPath)) {
        return std::string();
    }
    const PXR_NS::SdfPath path(outputPath);
    const PXR_NS::SdfPath definedPath(defined);
    if (!path.IsAbsolutePath() || !path.IsPrimPath() || (path == definedPath)) {
        return std::string();
    }
    if (!isWrapping()) {
        return path.HasPrefix(definedPath) ? path.ReplacePrefix(definedPath, PXR_NS::SdfPath::AbsoluteRootPath()).GetString() : std::string();
    }
    KnobStringPtr primPath = _primPath.lock();
    const PXR_NS::SdfPath wrapped(primPath->getValue());
    const PXR_NS::SdfPath moved = definedPath.AppendChild(wrapped.GetNameToken());
    if (path.HasPrefix(wrapped)) {
        return std::string();
    }

    return path.HasPrefix(moved) ? path.ReplacePrefix(moved, wrapped).GetString() : path.GetString();
}

bool
SceneReparentNode::authorPrimOverrides(const SceneLayerEdit& /*edit*/,
                                       const std::string& /*primPath*/,
                                       std::string* /*errorMessage*/) const
{
    return true;
}

bool
SceneReparentNode::authorOverrides(const SceneLayerEdit& edit,
                                   const std::string& primPath,
                                   int kind,
                                   std::string* errorMessage) const
{
    if (!edit.layer) {
        *errorMessage = tr("No layer to author.").toStdString();

        return false;
    }
    SceneNodeAuthoring::authorLayerMetadata(edit);
    if (!authorPrimOverrides(edit, primPath, errorMessage)) {
        return false;
    }
    const PXR_NS::TfToken kindToken = getKindToken(kind);
    if (!kindToken.IsEmpty()) {
        const PXR_NS::SdfPrimSpecHandle spec = PXR_NS::SdfCreatePrimInLayer(edit.layer, PXR_NS::SdfPath(primPath));
        if (!spec) {
            *errorMessage = tr("USD could not set the kind of %1.").arg(QString::fromStdString(primPath)).toStdString();

            return false;
        }
        spec->SetKind(kindToken);
    }

    return true;
}

bool
SceneReparentNode::getPlacement(bool* wrapping,
                                std::string* destination,
                                std::string* moved,
                                bool* autoRename) const
{
    return getPlacement(getCurrentTime(), ViewIdx(0), wrapping, destination, moved, autoRename);
}

bool
SceneReparentNode::getPlacement(double time,
                                ViewIdx view,
                                bool* wrapping,
                                std::string* destination,
                                std::string* moved,
                                bool* autoRename) const
{
    PXR_NS::SdfPath path;
    std::string error;
    if (!SceneNodeAuthoring::getPrimPath(_primPath.lock(), time, view, &path, &error)) {
        return false;
    }
    KnobChoicePtr mode = _mode.lock();
    *wrapping = mode && (mode->getValueAtTime(time, 0, view) == (int)eSceneReparentModeWrap);
    *destination = path.GetString();
    moved->clear();
    KnobBoolPtr autoRenameKnob = _autoRename.lock();
    *autoRename = autoRenameKnob && autoRenameKnob->getValueAtTime(time, 0, view);
    if (!*wrapping) {
        return true;
    }
    KnobStringPtr wrapNameKnob = _wrapName.lock();
    PXR_NS::SdfPath wrapPath;
    if (!getWrapPath(path, wrapNameKnob ? wrapNameKnob->getValueAtTime(time, 0, view) : std::string(), &wrapPath, &error)) {
        return false;
    }
    *moved = wrapPath.AppendChild(path.GetNameToken()).GetString();

    return true;
}

std::string
SceneReparentNode::getPassThroughRoot() const
{
    AppInstancePtr app = getApp();

    return app ? app->getProject()->getProjectMaterialsScope() : std::string();
}

SceneEvaluationContext
SceneReparentNode::getSceneInputContext(int inputNb,
                                        const SceneEvaluationContext& ctx,
                                        double time,
                                        ViewIdx view) const
{
    bool wrapping = false;
    std::string destination;
    std::string moved;
    bool autoRename = false;
    if ((inputNb < 0) || (inputNb >= kSceneReparentInputCount) || !getPlacement(time, view, &wrapping, &destination, &moved, &autoRename)) {
        return ctx;
    }
    if (wrapping) {
        return (inputNb == 0) ? SceneEvaluationContext(ctx.id, SceneOps::mapWrapLoadSet(ctx.loadSet, destination, moved)) : ctx;
    }

    AppInstancePtr app = getApp();

    return SceneEvaluationContext(ctx.id, SceneOps::mapGraftLoadSet(ctx.loadSet, destination, app ? app->getProject()->getProjectMaterialsScope(time, view) : std::string(), autoRename));
}

void
SceneReparentNode::mapSceneInputReach(int inputNb,
                                      double time,
                                      ViewIdx view,
                                      const std::vector<std::string>& in,
                                      std::vector<std::string>* out) const
{
    bool wrapping = false;
    std::string destination;
    std::string moved;
    bool autoRename = false;
    if ((inputNb < 0) || (inputNb >= kSceneReparentInputCount) || !getPlacement(time, view, &wrapping, &destination, &moved, &autoRename)) {
        *out = in;

        return;
    }
    if (wrapping) {
        if (inputNb == 0) {
            SceneOps::mapWrapReach(destination, moved, in, out);
        } else {
            *out = in;
        }

        return;
    }
    SceneOps::mapGraftReach(&_graftMemo, inputNb, destination, getPassThroughRoot(), in, out);
}

StatusEnum
SceneReparentNode::renderWrap(const SceneRenderActionArgs& args,
                              const std::string& wrappedPath,
                              int kind,
                              ScenePayload* outputScene,
                              std::string* errorMessage)
{
    const PXR_NS::SdfPath wrapped(wrappedPath);
    KnobStringPtr wrapNameKnob = _wrapName.lock();
    PXR_NS::SdfPath wrapPath;
    if (!getWrapPath(wrapped, wrapNameKnob ? wrapNameKnob->getValue() : std::string(), &wrapPath, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }
    const PXR_NS::SdfPath moved = wrapPath.AppendChild(wrapped.GetNameToken());

    const ScenePayload upstream = args.getInputScene(0);
    const SceneEvaluationContext& inputContext = args.getInputContext(0);
    std::vector<WrapRetarget> retargets;
    {
        // The existence checks load only the two paths; the retargets below read the whole stage.
        SceneQueryPool::Lease lease = upstream.isEmpty() ? SceneQueryPool::Lease()
                                                         : SceneQueryPool::instance().borrow(upstream, SceneLoadSet::withPaths(inputContext.loadSet, { wrappedPath, wrapPath.GetString() }), errorMessage);
        const PXR_NS::UsdStageRefPtr stage = lease.isValid() ? lease.getStage() : PXR_NS::UsdStageRefPtr();
        if (!upstream.isEmpty() && !stage) {
            if (errorMessage->empty()) {
                *errorMessage = tr("USD could not compose the input scene.").toStdString();
            }
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }
        const PXR_NS::UsdPrim prim = stage ? stage->GetPrimAtPath(wrapped) : PXR_NS::UsdPrim();
        if (!prim || !prim.IsDefined() || prim.IsAbstract()) {
            *errorMessage = tr("Input 0 has no prim at %1 to wrap.").arg(QString::fromStdString(wrappedPath)).toStdString();
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }
        if (stage->GetPrimAtPath(wrapPath)) {
            *errorMessage = tr("Input 0 already has a prim at %1; give Wrap Name another name.").arg(QString::fromStdString(wrapPath.GetString())).toStdString();
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }
    }
    if (!upstream.isEmpty()) {
        // A link can sit in any prim, so a payload that loads later may add one.
        if (inputContext.reach) {
            inputContext.reach->add({ std::string("/") });
        }
        SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(upstream, inputContext.loadSet, errorMessage);
        const PXR_NS::UsdStageRefPtr stage = lease.getStage();
        if (!stage) {
            if (errorMessage->empty()) {
                *errorMessage = tr("USD could not compose the input scene.").toStdString();
            }
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }
        gatherWrapRetargets(stage, wrapped, moved, &retargets);
    }

    const std::string typeName = getPrimTypeName();
    SceneLayerMemo::AuthorFunc author = [this, wrapped, wrapPath, moved, retargets, kind, typeName](const SceneLayerEdit& edit,
                                                                                                    std::string* error) {
        // The overrides first: they open a stage on the layer, which would warn about the
        // reference below, whose target only the upstream layers hold.
        if (!authorOverrides(edit, wrapPath.GetString(), kind, error)) {
            return false;
        }
        const PXR_NS::SdfLayerRefPtr& layer = edit.layer;
        const PXR_NS::SdfPrimSpecHandle wrapSpec = PXR_NS::SdfCreatePrimInLayer(layer, wrapPath);
        const PXR_NS::SdfPrimSpecHandle movedSpec = PXR_NS::SdfCreatePrimInLayer(layer, moved);
        const PXR_NS::SdfPrimSpecHandle wrappedSpec = PXR_NS::SdfCreatePrimInLayer(layer, wrapped);
        if (!wrapSpec || !movedSpec || !wrappedSpec) {
            *error = tr("USD could not author the move of %1 under %2.").arg(QString::fromStdString(wrapped.GetString()), QString::fromStdString(wrapPath.GetString())).toStdString();

            return false;
        }
        wrapSpec->SetSpecifier(PXR_NS::SdfSpecifierDef);
        wrapSpec->SetTypeName(typeName);
        // An internal reference composes the prim from the whole layer stack, this layer's
        // deactivation of it included; the local opinion re-activates the moved copy.
        movedSpec->SetSpecifier(PXR_NS::SdfSpecifierDef);
        movedSpec->GetReferenceList().Prepend(PXR_NS::SdfReference(std::string(), wrapped));
        movedSpec->SetActive(true);
        wrappedSpec->SetActive(false);
        // An explicit list, stronger than the reference, stands in for the property's whole list.
        for (const WrapRetarget& retarget : retargets) {
            const PXR_NS::SdfPrimSpecHandle owner = PXR_NS::SdfCreatePrimInLayer(layer, retarget.property.GetPrimPath());
            if (!owner) {
                *error = tr("USD could not retarget %1.").arg(QString::fromStdString(retarget.property.GetString())).toStdString();

                return false;
            }
            const PXR_NS::TfToken name = retarget.property.GetNameToken();
            if (retarget.isConnection) {
                if (!layer->GetAttributeAtPath(retarget.property) && !PXR_NS::SdfAttributeSpec::New(owner, name.GetString(), retarget.typeName, retarget.variability, retarget.custom)) {
                    *error = tr("USD could not retarget %1.").arg(QString::fromStdString(retarget.property.GetString())).toStdString();

                    return false;
                }
                layer->SetField(retarget.property, PXR_NS::SdfFieldKeys->ConnectionPaths, PXR_NS::SdfPathListOp::CreateExplicit(retarget.targets));
            } else {
                if (!layer->GetRelationshipAtPath(retarget.property) && !PXR_NS::SdfRelationshipSpec::New(owner, name.GetString(), retarget.custom)) {
                    *error = tr("USD could not retarget %1.").arg(QString::fromStdString(retarget.property.GetString())).toStdString();

                    return false;
                }
                layer->SetField(retarget.property, PXR_NS::SdfFieldKeys->TargetPaths, PXR_NS::SdfPathListOp::CreateExplicit(retarget.targets));
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
} // SceneReparentNode::renderWrap

StatusEnum
SceneReparentNode::renderScene(const SceneRenderActionArgs& args,
                               ScenePayload* outputScene,
                               std::string* errorMessage)
{
    PXR_NS::SdfPath path;
    if (!SceneNodeAuthoring::getPrimPath(_primPath.lock(), &path, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }
    KnobChoicePtr kindKnob = _kind.lock();
    KnobBoolPtr autoRenameKnob = _autoRename.lock();
    const int kind = kindKnob ? kindKnob->getValue() : (int)eKindNone;
    if (isWrapping()) {
        return renderWrap(args, path.GetString(), kind, outputScene, errorMessage);
    }

    SceneReparentArgs reparent;
    for (int i = 0; i < kSceneReparentInputCount; ++i) {
        reparent.inputs.push_back(args.getInputScene(i));
        reparent.inputContexts.push_back(args.getInputContext(i));
    }
    reparent.destinationPath = path.GetString();
    reparent.destinationType = getPrimTypeName();
    reparent.ancestorType = reparent.destinationType;
    reparent.autoRename = autoRenameKnob && autoRenameKnob->getValue();
    reparent.layerTag = getScriptName_mt_safe();

    // The overrides layer reads the knobs alone, so it is keyed as a layer that reads no upstream.
    SceneLayerReads reads;
    reads.upstream = SceneLayerMemo::Key::eUpstreamReadNothing;
    reads.frameRange = overridesReadFrameRange();
    if (!getSceneLayerKey(reads, &reparent.overridesKey, errorMessage)) {
        return eStatusFailed;
    }
    // Shared materials stay at the top of the scene, where bindings from every graft resolve.
    reparent.passThroughRoot = reparent.overridesKey.materialsScope;
    const std::string primPath = reparent.destinationPath;
    reparent.authorOverrides = [this, primPath, kind](const SceneLayerEdit& edit, std::string* error) {
        return authorOverrides(edit, primPath, kind, error);
    };

    std::vector<SceneRootGraft> grafts;
    std::vector<SceneUnmappedLink> unmapped;
    if (!SceneOps::reparentRoots(reparent, &_graftMemo, outputScene, &grafts, &unmapped, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }
    // Every layer of the result is this node's: its inputs come in through references.
    SceneOps::setLayerOwner(*outputScene, getNode(), false);
    QStringList warnings;
    for (const SceneRootGraft& graft : grafts) {
        if (graft.holdsGprim) {
            warnings += tr("Input %1's %2 holds geometry, but it is the project's Materials Scope: it stays at %2 instead "
                           "of moving under %3.")
                            .arg(graft.inputIndex + 1)
                            .arg(QString::fromStdString(graft.graftedPath))
                            .arg(QString::fromStdString(primPath));
        }
    }
    if (!unmapped.empty()) {
        QString warning = tr("These relationship targets and connections lie under no root prim their input grafts, so the "
                             "grafted properties are authored without them:");
        for (const SceneUnmappedLink& link : unmapped) {
            warning += QLatin1Char('\n') + tr("input %1: %2 -> %3").arg(link.inputIndex).arg(QString::fromStdString(link.propertyPath)).arg(QString::fromStdString(link.targetPath));
        }
        warnings += warning;
    }
    if (warnings.isEmpty()) {
        clearPersistentMessage(false);

        return eStatusOK;
    }
    setPersistentMessage(eMessageTypeWarning, warnings.join(QLatin1Char('\n')).toStdString());

    return eStatusOK;
} // SceneReparentNode::renderScene

NATRON_NAMESPACE_EXIT
