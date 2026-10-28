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

#include "USDReference.h"

#include <memory>
#include <string>
#include <vector>

#include <QString>

#include "Engine/ChoiceOption.h"
#include "Engine/ContentHasher.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/ScenePayloadImpl.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

NATRON_NAMESPACE_ENTER

namespace {
// The prim path a Source input is referenced at: sourcePrim when set, else the input's default
// prim, else its first root prim. Empty with *errorMessage set when none is defined in stage.
PXR_NS::SdfPath
findInputSourcePrim(const PXR_NS::UsdStageRefPtr& stage,
                    const std::string& sourcePrim,
                    const std::string& defaultPrim,
                    std::string* errorMessage)
{
    PXR_NS::SdfPath path;

    if (!sourcePrim.empty()) {
        if (!PXR_NS::SdfPath::IsValidPathString(sourcePrim) || !PXR_NS::SdfPath(sourcePrim).IsAbsolutePath() || !PXR_NS::SdfPath(sourcePrim).IsPrimPath()) {
            *errorMessage = EffectInstance::tr("The source prim \"%1\" is not an absolute prim path such as /World/Prim.").arg(QString::fromStdString(sourcePrim)).toStdString();

            return PXR_NS::SdfPath();
        }
        path = PXR_NS::SdfPath(sourcePrim);
    } else if (!defaultPrim.empty() && PXR_NS::SdfPath::IsValidIdentifier(defaultPrim)) {
        path = PXR_NS::SdfPath::AbsoluteRootPath().AppendChild(PXR_NS::TfToken(defaultPrim));
    } else {
        const PXR_NS::UsdPrimSiblingRange roots = stage->GetPseudoRoot().GetChildren();
        if (roots.empty()) {
            *errorMessage = EffectInstance::tr("The Source input's scene has no prim to reference.").toStdString();

            return PXR_NS::SdfPath();
        }
        path = roots.front().GetPath();
    }
    const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(path);
    if (!prim || !prim.IsDefined()) {
        *errorMessage = EffectInstance::tr("The Source input's scene has no prim at %1.").arg(QString::fromStdString(path.GetString())).toStdString();

        return PXR_NS::SdfPath();
    }

    return path;
}

// Composes upstream, or returns null with *errorMessage set.
bool
borrowStage(const ScenePayload& payload,
            SceneQueryPool::Lease* lease,
            std::string* errorMessage)
{
    *lease = SceneQueryPool::instance().borrow(payload, errorMessage);
    if (lease->getStage()) {
        return true;
    }
    if (errorMessage->empty()) {
        *errorMessage = EffectInstance::tr("USD could not compose the input scene.").toStdString();
    }

    return false;
}
} // anonymous namespace

USDReference::USDReference(NodePtr node)
    : NativeEffectBase(node)
{
}

USDReference::~USDReference()
{
}

const NodeSpec*
USDReference::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
USDReference::onScriptNameChanged(const std::string& /*fullyQualifiedName*/)
{
    followSceneNodeRename(_primPath.lock(), getSceneNodeDefaultPrimPath(this, "Reference"));
}

void
USDReference::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    _primPath = createScenePrimPathKnob(this, page, getSceneNodeDefaultPrimPath(this, "Reference"),
                                        tr("The prim the source prim composes at. It is defined when the input scene lacks it, "
                                           "its missing ancestors as Xforms."));

    KnobChoicePtr mode = createKnob<KnobChoice>(tr("Mode"));
    mode->setName(kUSDReferenceParamMode);
    mode->setHintToolTip(tr("The kind of composition arc the node authors on the prim."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("reference", tr("Reference").toStdString(),
                                       tr("The source prim always composes.").toStdString()));
        options.push_back(ChoiceOption("payload", tr("Payload").toStdString(),
                                       tr("The source prim composes while the payload is loaded, which Natron's scenes "
                                          "are by default.")
                                           .toStdString()));
        mode->populateChoices(options);
    }
    mode->setDefaultValue((int)eModeReference);
    mode->setAnimationEnabled(false);
    page->addKnob(mode);
    _mode = mode;

    KnobChoicePtr source = createKnob<KnobChoice>(tr("Source"));
    source->setName(kUSDReferenceParamSource);
    source->setHintToolTip(tr("Where the source prim comes from."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("file", tr("File").toStdString(), tr("A USD file.").toStdString()));
        options.push_back(ChoiceOption("input", tr("Input").toStdString(),
                                       tr("The scene connected to the Source input, every layer of it.").toStdString()));
        source->populateChoices(options);
    }
    source->setDefaultValue((int)eSourceFile);
    source->setAnimationEnabled(false);
    page->addKnob(source);
    _source = source;

    KnobFilePtr file = createKnob<KnobFile>(tr("File"));
    file->setName(kUSDReferenceParamFile);
    file->setHintToolTip(tr("The USD file (.usd, .usda, .usdc, .usdz) or Alembic file (.abc) to reference."));
    file->setFileExtensions({ "usd", "usda", "usdc", "usdz", "abc" });
    file->setAnimationEnabled(false);
    file->setAddNewLine(false);
    page->addKnob(file);
    _file = file;

    KnobButtonPtr reload = createKnob<KnobButton>(tr("Reload"));
    reload->setName(kUSDReferenceParamReload);
    reload->setHintToolTip(tr("Reads the file again, and every file it sublayers, references or loads as a payload, in "
                              "place: every scene using them shows their content on disk. Renders reading scenes are "
                              "stopped or waited for first."));
    page->addKnob(reload);
    _reload = reload;

    KnobStringPtr sourcePrim = createKnob<KnobString>(tr("Source Prim"));
    sourcePrim->setName(kUSDReferenceParamSourcePrim);
    sourcePrim->setHintToolTip(tr("The absolute path of the prim to reference in the source. Empty references the source's "
                                  "default prim; for the Source input, its first root prim when it has no default prim."));
    sourcePrim->setAnimationEnabled(false);
    page->addKnob(sourcePrim);
    _sourcePrim = sourcePrim;

    KnobBoolPtr instanceable = createKnob<KnobBool>(tr("Instanceable"));
    instanceable->setName(kUSDReferenceParamInstanceable);
    instanceable->setHintToolTip(tr("Marks the prim instanceable, so prims referencing the same source share one copy of "
                                    "it in USD and in renders. Its descendants can then not be edited one by one."));
    instanceable->setDefaultValue(false);
    instanceable->setAnimationEnabled(false);
    page->addKnob(instanceable);
    _instanceable = instanceable;

    refreshVisibility();
} // USDReference::initializeKnobs

USDReference::SourceEnum
USDReference::getSource() const
{
    KnobChoicePtr source = _source.lock();

    return (source && (source->getValue() == (int)eSourceInput)) ? eSourceInput : eSourceFile;
}

void
USDReference::refreshVisibility()
{
    const bool file = (getSource() == eSourceFile);

    setSceneKnobSecret(_file.lock(), !file);
    setSceneKnobSecret(_reload.lock(), !file);
}

void
USDReference::reloadFile()
{
    KnobFilePtr file = _file.lock();
    const std::string path = SceneNodeAuthoring::getAnchoredFilePath(file);
    PXR_NS::SdfLayerHandle open = path.empty() ? PXR_NS::SdfLayerHandle() : PXR_NS::SdfLayer::Find(path);
    std::string error;

    // A file no stage has open yet is read fresh when it is first composed.
    if (open && !SceneOps::reloadFileLayers(SceneOps::getFileLayerClosure(PXR_NS::SdfLayerRefPtr(open)), &error)) {
        setPersistentMessage(eMessageTypeError, tr("Reload failed: %1").arg(QString::fromStdString(error)).toStdString());
    } else {
        clearPersistentMessage(false);
    }
    if (file) {
        file->invalidateFileStamp();
    }
    invalidateSceneLayer();
    clearActionsCache();
    NodePtr node = getNode();
    if (node) {
        node->markHashDirty();
    }
}

bool
USDReference::knobChanged(KnobI* k,
                          ValueChangedReasonEnum /*reason*/,
                          ViewSpec /*view*/,
                          double /*time*/,
                          bool /*originatedFromMainThread*/)
{
    KnobChoicePtr source = _source.lock();
    KnobButtonPtr reload = _reload.lock();

    if (source && (k == source.get())) {
        refreshVisibility();

        return true;
    }
    if (reload && (k == reload.get())) {
        reloadFile();

        return true;
    }

    return false;
}

void
USDReference::onKnobsLoaded()
{
    refreshVisibility();
}

StatusEnum
USDReference::renderScene(const SceneRenderActionArgs& args,
                          ScenePayload* outputScene,
                          std::string* errorMessage)
{
    const ScenePayload upstream = args.getInputScene(0);
    const SourceEnum sourceKind = getSource();
    KnobChoicePtr modeKnob = _mode.lock();
    const bool payload = modeKnob && (modeKnob->getValue() == (int)eModePayload);
    KnobBoolPtr instanceableKnob = _instanceable.lock();
    const bool instanceable = instanceableKnob && instanceableKnob->getValue();
    KnobStringPtr sourcePrimKnob = _sourcePrim.lock();
    const std::string sourcePrim = sourcePrimKnob ? sourcePrimKnob->getValue() : std::string();

    PXR_NS::SdfPath path;
    if (!SceneNodeAuthoring::getPrimPath(_primPath.lock(), &path, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }

    ContentHasher facts;
    SceneNodeAuthoring::appendUpstreamMetadata(upstream, &facts);

    // What the arc points at: a file, or the Source input's stack.
    std::string assetPath;
    PXR_NS::SdfPath sourcePath;
    ScenePayload source;
    if (sourceKind == eSourceFile) {
        assetPath = SceneNodeAuthoring::getAnchoredFilePath(_file.lock());
        if (!sourcePrim.empty()) {
            if (!PXR_NS::SdfPath::IsValidPathString(sourcePrim) || !PXR_NS::SdfPath(sourcePrim).IsAbsolutePath() || !PXR_NS::SdfPath(sourcePrim).IsPrimPath()) {
                *errorMessage = tr("The source prim \"%1\" is not an absolute prim path such as /World/Prim.").arg(QString::fromStdString(sourcePrim)).toStdString();
                setPersistentMessage(eMessageTypeError, *errorMessage);

                return eStatusFailed;
            }
            sourcePath = PXR_NS::SdfPath(sourcePrim);
        }
    } else {
        source = args.getInputScene(1);
        if (!source.isEmpty()) {
            SceneQueryPool::Lease lease;
            if (!borrowStage(source, &lease, errorMessage)) {
                setPersistentMessage(eMessageTypeError, *errorMessage);

                return eStatusFailed;
            }
            sourcePath = findInputSourcePrim(lease.getStage(), sourcePrim, source.getMetadata().defaultPrim, errorMessage);
            if (sourcePath.IsEmpty()) {
                setPersistentMessage(eMessageTypeError, *errorMessage);

                return eStatusFailed;
            }
        }
        facts.appendU64(source.getStackHash());
    }
    if ((sourceKind == eSourceFile) ? assetPath.empty() : source.isEmpty()) {
        *outputScene = upstream;
        setPersistentMessage(eMessageTypeWarning, ((sourceKind == eSourceFile) ? tr("Set a file to reference; the scene is passed through "
                                                                                    "unchanged.")
                                                                               : tr("Connect the Source input; the scene is passed through "
                                                                                    "unchanged."))
                                                      .toStdString());

        return eStatusOK;
    }
    facts.appendString(sourcePath.GetString());

    // Whether the input defines the prim and which of its ancestors it lacks.
    bool defined = false;
    std::vector<PXR_NS::SdfPath> missing;
    for (PXR_NS::SdfPath parent = path.GetParentPath(); !parent.IsEmpty() && !parent.IsAbsoluteRootPath(); parent = parent.GetParentPath()) {
        missing.insert(missing.begin(), parent);
    }
    if (!upstream.isEmpty()) {
        SceneQueryPool::Lease lease;
        if (!borrowStage(upstream, &lease, errorMessage)) {
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }
        const PXR_NS::UsdStageRefPtr stage = lease.getStage();
        const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(path);
        defined = prim && prim.IsDefined();
        std::vector<PXR_NS::SdfPath> lacking;
        for (const PXR_NS::SdfPath& ancestor : missing) {
            const PXR_NS::UsdPrim ancestorPrim = stage->GetPrimAtPath(ancestor);
            if (!ancestorPrim || !ancestorPrim.IsDefined()) {
                lacking.push_back(ancestor);
            }
        }
        missing.swap(lacking);
    }
    facts.appendBool(defined);
    facts.appendU64(missing.size());
    for (const PXR_NS::SdfPath& ancestor : missing) {
        facts.appendString(ancestor.GetString());
    }

    const std::string layerTag = getScriptName_mt_safe();
    SceneLayerMemo::AuthorFunc author = [path, defined, missing, assetPath, source, sourcePath, payload, instanceable, layerTag](const SceneLayerEdit& edit,
                                                                                                                                 std::string* error) {
        SceneNodeAuthoring::authorLayerMetadata(edit);
        std::string target = assetPath;
        if (!source.isEmpty()) {
            PXR_NS::SdfLayerRefPtr stackLayer = SceneOps::createStackLayer(source, layerTag + "-source");
            if (!stackLayer || !edit.dependencies) {
                *error = tr("USD could not create a layer for the Source input.").toStdString();

                return false;
            }
            std::shared_ptr<SceneLayerDependencies> dependencies = std::make_shared<SceneLayerDependencies>();
            dependencies->layers.push_back(stackLayer);
            dependencies->stacks.push_back(source);
            *edit.dependencies = dependencies;
            target = stackLayer->GetIdentifier();
        }
        PXR_NS::SdfPrimSpecHandle spec = PXR_NS::SdfCreatePrimInLayer(edit.layer, path);
        if (!spec) {
            *error = tr("USD could not author %1.").arg(QString::fromStdString(path.GetString())).toStdString();

            return false;
        }
        if (!defined) {
            spec->SetSpecifier(PXR_NS::SdfSpecifierDef);
        }
        for (const PXR_NS::SdfPath& ancestor : missing) {
            PXR_NS::SdfPrimSpecHandle ancestorSpec = edit.layer->GetPrimAtPath(ancestor);
            if (ancestorSpec) {
                ancestorSpec->SetSpecifier(PXR_NS::SdfSpecifierDef);
                ancestorSpec->SetTypeName("Xform");
            }
        }
        if (payload) {
            spec->GetPayloadList().Prepend(PXR_NS::SdfPayload(target, sourcePath));
        } else {
            spec->GetReferenceList().Prepend(PXR_NS::SdfReference(target, sourcePath));
        }
        if (instanceable) {
            spec->SetInstanceable(true);
        }

        return true;
    };

    // Of the input, the layer reads the metadata, whether the prim and its ancestors are
    // defined, and the whole Source stack, which its stack layer sublayers.
    SceneLayerReads reads;
    reads.upstream = SceneLayerMemo::Key::eUpstreamReadFacts;
    reads.factsDigest = facts.finalize();
    reads.frameRange = false;
    if (!emitSceneLayer(upstream, reads, author, outputScene, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }
    clearPersistentMessage(false);

    return eStatusOK;
} // USDReference::renderScene

NATRON_NAMESPACE_EXIT
