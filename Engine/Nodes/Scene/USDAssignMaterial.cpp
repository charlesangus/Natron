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

#include "USDAssignMaterial.h"

#include <string>
#include <vector>

#include <QStringList>

#include "Engine/AppInstance.h"
#include "Engine/ChoiceOption.h"
#include "Engine/ContentHasher.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_ENTER

namespace {
// The matches under one root prim, which gets one collection and binding for them.
struct RootMatches {
    PXR_NS::SdfPath root;
    std::vector<PXR_NS::SdfPath> paths;
    std::vector<PXR_NS::TfToken> existingBindings;
};

bool
isBlank(const std::string& text)
{
    return text.find_first_not_of(" \t\r\n") == std::string::npos;
}

PXR_NS::SdfPath
getRootPrim(const PXR_NS::SdfPath& path)
{
    PXR_NS::SdfPath root = path;

    while (!root.IsRootPrimPath() && !root.IsEmpty() && (root != PXR_NS::SdfPath::AbsoluteRootPath())) {
        root = root.GetParentPath();
    }

    return root;
}
} // anonymous namespace

USDAssignMaterial::USDAssignMaterial(NodePtr node)
    : NativeEffectBase(node)
{
}

USDAssignMaterial::~USDAssignMaterial()
{
}

const NodeSpec*
USDAssignMaterial::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
USDAssignMaterial::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobStringPtr primPattern = createKnob<KnobString>(tr("Prim Pattern"));
    primPattern->setName(kUSDAssignMaterialParamPrimPattern);
    primPattern->setHintToolTip(tr("The prims to bind the material to: absolute prim paths separated by spaces, whose names "
                                   "may use * (any run of characters) and ? (any one character), such as /Asset/geo/*. "
                                   "Their descendants take the material too, unless their own binding is stronger."));
    primPattern->setAnimationEnabled(false);
    page->addKnob(primPattern);
    _primPattern = primPattern;

    KnobStringPtr materialPath = createKnob<KnobString>(tr("Material Path"));
    materialPath->setName(kUSDAssignMaterialParamMaterialPath);
    materialPath->setHintToolTip(tr("The material already in the scene to bind, such as /materials/Metal. Unused while the "
                                    "Material input is connected: its first material is bound instead."));
    materialPath->setAnimationEnabled(false);
    page->addKnob(materialPath);
    _materialPath = materialPath;

    KnobChoicePtr purpose = createKnob<KnobChoice>(tr("Purpose"));
    purpose->setName(kUSDAssignMaterialParamPurpose);
    purpose->setHintToolTip(tr("Which renders the binding is for."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("all", tr("All").toStdString(),
                                       tr("Every render: the binding any purpose falls back to.").toStdString()));
        options.push_back(ChoiceOption("full", tr("Full").toStdString(),
                                       tr("Final-quality renders only.").toStdString()));
        options.push_back(ChoiceOption("preview", tr("Preview").toStdString(),
                                       tr("Interactive previews only, such as the Viewer.").toStdString()));
        purpose->populateChoices(options);
    }
    purpose->setDefaultValue((int)ePurposeAll);
    purpose->setAnimationEnabled(false);
    page->addKnob(purpose);
    _purpose = purpose;

    KnobChoicePtr strength = createKnob<KnobChoice>(tr("Strength"));
    strength->setName(kUSDAssignMaterialParamStrength);
    strength->setHintToolTip(tr("Whether the binding overrides the bindings the matched prims' descendants already have."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("strongerThanDescendants", tr("Stronger Than Descendants").toStdString(),
                                       tr("The material replaces every binding below the matched prims, so a later "
                                          "assign always wins.")
                                           .toStdString()));
        options.push_back(ChoiceOption("weakerThanDescendants", tr("Weaker Than Descendants").toStdString(),
                                       tr("Prims below the matches that have a binding of their own keep it; the material "
                                          "only fills in where there is none.")
                                           .toStdString()));
        strength->populateChoices(options);
    }
    strength->setDefaultValue((int)eStrengthStrongerThanDescendants);
    strength->setAnimationEnabled(false);
    page->addKnob(strength);
    _strength = strength;
} // USDAssignMaterial::initializeKnobs

void
USDAssignMaterial::onKnobsLoaded()
{
    NativeEffectBase::onKnobsLoaded();
    refreshMaterialPathEnabled();
}

void
USDAssignMaterial::onInputChanged(int inputNo)
{
    NativeEffectBase::onInputChanged(inputNo);
    refreshMaterialPathEnabled();
}

void
USDAssignMaterial::refreshMaterialPathEnabled()
{
    KnobStringPtr materialPath = _materialPath.lock();

    if (materialPath) {
        materialPath->setAllDimensionsEnabled(!getInput(kUSDAssignMaterialInputMaterial));
    }
}

StatusEnum
USDAssignMaterial::renderScene(const SceneRenderActionArgs& args,
                               ScenePayload* outputScene,
                               std::string* errorMessage)
{
    const ScenePayload upstream = args.getInputScene(kUSDAssignMaterialInputScene);
    KnobStringPtr patternKnob = _primPattern.lock();
    KnobStringPtr materialPathKnob = _materialPath.lock();
    KnobChoicePtr purposeKnob = _purpose.lock();
    KnobChoicePtr strengthKnob = _strength.lock();
    AppInstancePtr app = getApp();
    if (!patternKnob || !materialPathKnob || !purposeKnob || !strengthKnob || !app || !app->getProject()) {
        *errorMessage = tr("The node is being destroyed").toStdString();

        return eStatusFailed;
    }
    const std::string pattern = patternKnob->getValue();
    const std::string scope = app->getProject()->getProjectMaterialsScope();
    const bool materialConnected = args.inputScenes.count(kUSDAssignMaterialInputMaterial) > 0;
    QStringList warnings;

    // The scene the binding layer stacks on: the input, plus the grafted material when there is one.
    ScenePayload base = upstream;
    std::string materialPath;
    if (materialConnected) {
        SceneMaterialGraft graft;
        if (!SceneNodeAuthoring::graftSharedMaterial(args.getInputScene(kUSDAssignMaterialInputMaterial), scope, getScriptName_mt_safe(), &_graftMemo, &graft, errorMessage)) {
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }
        if (graft.materialPath.empty()) {
            *errorMessage = tr("The Material input holds no material.").toStdString();
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }
        for (const SceneLayerEntry& entry : SceneOps::getLayers(graft.payload)) {
            base = SceneOps::appendEntry(base, entry);
        }
        materialPath = graft.materialPath;
        if (!graft.unmappedLinks.empty()) {
            QString warning = tr("These relationship targets and connections of the material lie under no root prim of the "
                                 "Material input, so the grafted properties are authored without them:");
            for (const SceneUnmappedLink& link : graft.unmappedLinks) {
                warning += QLatin1Char('\n') + tr("%1 -> %2").arg(QString::fromStdString(link.propertyPath)).arg(QString::fromStdString(link.targetPath));
            }
            warnings += warning;
        }
    } else {
        materialPath = materialPathKnob->getValue();
        if (!isBlank(materialPath)) {
            PXR_NS::SdfPath path;
            if (!SceneNodeAuthoring::getPrimPath(materialPathKnob, &path, errorMessage)) {
                setPersistentMessage(eMessageTypeError, *errorMessage);

                return eStatusFailed;
            }
            materialPath = path.GetString();
        }
    }

    std::vector<RootMatches> roots;
    if (!upstream.isEmpty() && !isBlank(pattern)) {
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
        QStringList skipped;
        for (const PXR_NS::SdfPath& path : found) {
            const PXR_NS::SdfPath root = getRootPrim(path);
            if (!scope.empty() && (root.GetName() == scope)) {
                skipped += QString::fromStdString(path.GetString());
                continue;
            }
            std::vector<RootMatches>::iterator it = roots.begin();
            while ((it != roots.end()) && (it->root != root)) {
                ++it;
            }
            if (it == roots.end()) {
                RootMatches matches;
                matches.root = root;
                matches.existingBindings = SceneNodeAuthoring::getCollectionBindingNames(stage->GetPrimAtPath(root));
                it = roots.insert(roots.end(), matches);
            }
            it->paths.push_back(path);
        }
        if (!skipped.isEmpty()) {
            warnings += tr("These matches lie inside the Materials Scope /%1 and are skipped: %2")
                            .arg(QString::fromStdString(scope))
                            .arg(skipped.join(QLatin1String(", ")));
        }
        if (roots.empty()) {
            warnings += tr("The prim pattern \"%1\" matches no prim to bind the material to; the scene is passed through "
                           "unchanged.")
                            .arg(QString::fromStdString(pattern));
        } else if (isBlank(materialPath)) {
            warnings += tr("There is no material to bind: connect the Material input or set Material Path. The scene is "
                           "passed through unchanged.");
            roots.clear();
        } else if (!materialConnected) {
            const PXR_NS::UsdPrim materialPrim = stage->GetPrimAtPath(PXR_NS::SdfPath(materialPath));
            if (!materialPrim || !materialPrim.IsA<PXR_NS::UsdShadeMaterial>()) {
                warnings += tr("%1 is not a material in the input scene; the binding is authored anyway.").arg(QString::fromStdString(materialPath));
            }
        }
    }

    if (roots.empty()) {
        *outputScene = upstream;
        if (warnings.isEmpty()) {
            clearPersistentMessage(false);
        } else {
            setPersistentMessage(eMessageTypeWarning, warnings.join(QLatin1Char('\n')).toStdString());
        }

        return eStatusOK;
    }

    SceneNodeAuthoring::SceneCollectionBinding binding;
    binding.materialPath = materialPath;
    binding.bindingName = PXR_NS::TfMakeValidIdentifier(getFullyQualifiedName());
    switch (purposeKnob->getValue()) {
    case ePurposeFull:
        binding.purpose = PXR_NS::UsdShadeTokens->full;
        break;
    case ePurposePreview:
        binding.purpose = PXR_NS::UsdShadeTokens->preview;
        break;
    default:
        binding.purpose = PXR_NS::UsdShadeTokens->allPurpose;
        break;
    }
    binding.strength = (strengthKnob->getValue() == (int)eStrengthWeakerThanDescendants) ? PXR_NS::UsdShadeTokens->weakerThanDescendants : PXR_NS::UsdShadeTokens->strongerThanDescendants;

    SceneLayerMemo::AuthorFunc author = [this, roots, binding](const SceneLayerEdit& edit,
                                                               std::string* error) {
        PXR_NS::UsdStageRefPtr stage = SceneNodeAuthoring::openLayerStage(edit, error);
        if (!stage) {
            if (error->empty()) {
                *error = tr("USD could not open the node's layer.").toStdString();
            }

            return false;
        }
        for (const RootMatches& matches : roots) {
            SceneNodeAuthoring::SceneCollectionBinding rootBinding = binding;
            rootBinding.includes = matches.paths;
            rootBinding.existingBindings = matches.existingBindings;
            if (!SceneNodeAuthoring::authorCollectionBinding(stage->OverridePrim(matches.root), rootBinding, error)) {
                return false;
            }
        }

        return true;
    };

    // Of the scene, the layer reads only the matches, the bindings already on their roots and the
    // metadata; of the material, only where it landed.
    ContentHasher facts;
    SceneNodeAuthoring::appendUpstreamMetadata(base, &facts);
    facts.appendString(binding.bindingName);
    facts.appendString(binding.materialPath);
    facts.appendU64(roots.size());
    for (const RootMatches& matches : roots) {
        facts.appendString(matches.root.GetString());
        facts.appendU64(matches.paths.size());
        for (const PXR_NS::SdfPath& path : matches.paths) {
            facts.appendString(path.GetString());
        }
        facts.appendU64(matches.existingBindings.size());
        for (const PXR_NS::TfToken& name : matches.existingBindings) {
            facts.appendString(name.GetString());
        }
    }
    SceneLayerReads reads;
    reads.upstream = SceneLayerMemo::Key::eUpstreamReadFacts;
    reads.factsDigest = facts.finalize();
    reads.frameRange = false;
    if (!emitSceneLayer(base, reads, author, outputScene, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }
    if (warnings.isEmpty()) {
        clearPersistentMessage(false);
    } else {
        setPersistentMessage(eMessageTypeWarning, warnings.join(QLatin1Char('\n')).toStdString());
    }

    return eStatusOK;
} // USDAssignMaterial::renderScene

NATRON_NAMESPACE_EXIT
