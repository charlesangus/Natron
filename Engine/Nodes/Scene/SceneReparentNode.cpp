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

#include "Engine/ChoiceOption.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

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
    _primPath = createScenePrimPathKnob(this, page, getSceneNodeDefaultPrimPath(this, getPrimTypeName()),
                                        tr("The path of the prim the node defines. The root prims of every input are grafted "
                                           "under it; missing ancestors of a nested path are defined as %1 prims.")
                                            .arg(QString::fromStdString(getPrimTypeName())));

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

    SceneReparentArgs reparent;
    for (int i = 0; i < kSceneReparentInputCount; ++i) {
        reparent.inputs.push_back(args.getInputScene(i));
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
