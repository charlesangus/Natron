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

#include <cstdint>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <QStringList>

#include "Engine/AppInstance.h"
#include "Engine/ChoiceOption.h"
#include "Engine/ContentHasher.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneProjectionCameras.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/Project.h"

NATRON_NAMESPACE_ENTER

namespace {
struct RootMatches {
    PXR_NS::SdfPath root;
    std::vector<PXR_NS::SdfPath> paths;
    std::vector<PXR_NS::TfToken> existingBindings;
};

// What a projection in Geometry space takes from the prims it is assigned to. Matrices is empty
// when the matches hold no geometry.
struct ProjectionPlacement {
    std::map<double, PXR_NS::GfMatrix4d> matrices;
    bool animated = false;
    std::string followed;
    std::string othersAt;
};

ProjectionPlacement
readProjectionPlacement(const PXR_NS::UsdStageRefPtr& stage,
                        const std::vector<RootMatches>& roots,
                        const SceneMaterialGraft& projection,
                        const ScenePayload& scene,
                        const SceneLayerEdit& range,
                        double framesPerSecond)
{
    ProjectionPlacement placement;
    std::vector<PXR_NS::UsdPrim> gprims;
    PXR_NS::SdfPathSet seen;
    for (const RootMatches& matches : roots) {
        for (const PXR_NS::SdfPath& path : matches.paths) {
            for (const PXR_NS::UsdPrim& prim : PXR_NS::UsdPrimRange(stage->GetPrimAtPath(path))) {
                if (prim.IsA<PXR_NS::UsdGeomGprim>() && seen.insert(prim.GetPath()).second) {
                    gprims.push_back(prim);
                }
            }
        }
    }
    if (gprims.empty()) {
        return placement;
    }
    placement.followed = gprims.front().GetPath().GetString();

    std::vector<double> frames;
    if (projection.projectionHeld) {
        frames.push_back(projection.projectionHoldFrame);
    } else {
        for (const PXR_NS::UsdPrim& prim : gprims) {
            placement.animated = placement.animated || SceneProjectionCameras::transformMightVary(prim);
        }
        if (placement.animated) {
            frames = SceneNodeAuthoring::getSampleFrames(range);
        } else {
            frames.push_back(range.firstFrame);
        }
    }
    PXR_NS::SdfPathSet elsewhere;
    for (double frame : frames) {
        PXR_NS::UsdGeomXformCache cache(PXR_NS::UsdTimeCode(SceneOps::frameToTimeCode(scene, frame, framesPerSecond)));
        const PXR_NS::GfMatrix4d matrix = cache.GetLocalToWorldTransform(gprims.front());
        placement.matrices[frame] = matrix;
        for (const PXR_NS::UsdPrim& prim : gprims) {
            if (!PXR_NS::GfIsClose(matrix, cache.GetLocalToWorldTransform(prim), 1e-9)) {
                elsewhere.insert(prim.GetPath());
            }
        }
    }
    for (const PXR_NS::SdfPath& path : elsewhere) {
        placement.othersAt += (placement.othersAt.empty() ? "" : ", ") + path.GetString();
    }

    return placement;
} // readProjectionPlacement

std::string
hexSuffix(const std::string& text)
{
    // FNV-1a, so the name is the same in every process and every session.
    std::uint64_t hash = 1469598103934665603ULL;

    for (const char c : text) {
        hash = (hash ^ (std::uint64_t)(unsigned char)c) * 1099511628211ULL;
    }
    char buffer[17];
    std::snprintf(buffer, sizeof(buffer), "%016llx", (unsigned long long)hash);

    return std::string(buffer);
}

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

struct USDAssignMaterial::PlacementMemo {
    std::mutex lock;
    bool valid = false;
    U64 key = 0;
    ProjectionPlacement placement;
};

USDAssignMaterial::USDAssignMaterial(NodePtr node)
    : NativeEffectBase(node)
    , _placementMemo(new PlacementMemo)
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
    primPattern->setHintToolTip(tr("The prims to bind the material to: a USD path expression, such as /Asset/geo/* or "
                                   "/World//*Light*, where // spans any depth and spaces join several paths. "
                                   "Their descendants take the material too, unless their own binding is stronger."));
    primPattern->setAnimationEnabled(false);
    primPattern->setPrimPathHint(KnobString::ePrimPathHintPattern);
    page->addKnob(primPattern);
    _primPattern = primPattern;

    KnobStringPtr materialPath = createKnob<KnobString>(tr("Material Path"));
    materialPath->setName(kUSDAssignMaterialParamMaterialPath);
    materialPath->setHintToolTip(tr("The material already in the scene to bind, such as /materials/Metal. Unused while the "
                                    "Material input is connected: its first material is bound instead."));
    materialPath->setAnimationEnabled(false);
    materialPath->setPrimPathHint(KnobString::ePrimPathHintPath);
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

SceneEvaluationContext
USDAssignMaterial::getSceneInputContext(int inputNb,
                                        const SceneEvaluationContext& ctx,
                                        double time,
                                        ViewIdx view) const
{
    if (inputNb != kUSDAssignMaterialInputMaterial) {
        return ctx;
    }
    AppInstancePtr app = getApp();

    return SceneNodeAuthoring::getSharedMaterialContext(ctx, app ? app->getProject()->getProjectMaterialsScope(time, view) : std::string());
}

void
USDAssignMaterial::mapSceneInputReach(int inputNb,
                                      double time,
                                      ViewIdx view,
                                      const std::vector<std::string>& in,
                                      std::vector<std::string>* out) const
{
    if (inputNb != kUSDAssignMaterialInputMaterial) {
        *out = in;

        return;
    }
    AppInstancePtr app = getApp();

    SceneNodeAuthoring::mapSharedMaterialReach(&_graftMemo, app ? app->getProject()->getProjectMaterialsScope(time, view) : std::string(), in, out);
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

    ScenePayload base = upstream;
    std::string materialPath;
    SceneMaterialGraft projection;
    ProjectionPlacement placement;
    if (materialConnected) {
        SceneMaterialGraft graft;
        if (!SceneNodeAuthoring::graftSharedMaterial(args.getInputScene(kUSDAssignMaterialInputMaterial), args.getInputContext(kUSDAssignMaterialInputMaterial), scope, getScriptName_mt_safe(), &_graftMemo, &graft, errorMessage)) {
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
        projection.geometryProjectionShader = graft.geometryProjectionShader;
        projection.projectionHeld = graft.projectionHeld;
        projection.projectionHoldFrame = graft.projectionHoldFrame;
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
    if (!isBlank(pattern)) {
        SceneQueryPool::Lease lease;
        SceneNodeAuthoring::PatternMatches matched;
        if (!SceneNodeAuthoring::matchPattern(&_patternMatches, upstream, pattern, args.context, 0, SceneNodeAuthoring::PatternFilter(), &lease, &matched, errorMessage)) {
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }
        const std::vector<PXR_NS::SdfPath>& found = matched.paths;
        PXR_NS::UsdStageRefPtr stage;
        if (!upstream.isEmpty()) {
            // The bindings already on the matches' roots and the material are read on the stage.
            if (!lease.isValid()) {
                lease = SceneQueryPool::instance().borrow(upstream, args.context.loadSet, errorMessage);
            }
            stage = lease.getStage();
            if (!stage) {
                if (errorMessage->empty()) {
                    *errorMessage = tr("USD could not compose the input scene.").toStdString();
                }
                setPersistentMessage(eMessageTypeError, *errorMessage);

                return eStatusFailed;
            }
        }
        std::unordered_map<PXR_NS::SdfPath, std::size_t, PXR_NS::SdfPath::Hash> rootIndices;
        QStringList skipped;
        for (const PXR_NS::SdfPath& path : found) {
            const PXR_NS::SdfPath root = getRootPrim(path);
            if (!scope.empty() && (root.GetName() == scope)) {
                skipped += QString::fromStdString(path.GetString());
                continue;
            }
            const std::pair<std::unordered_map<PXR_NS::SdfPath, std::size_t, PXR_NS::SdfPath::Hash>::iterator, bool> inserted = rootIndices.emplace(root, roots.size());
            if (inserted.second) {
                RootMatches matches;
                matches.root = root;
                matches.existingBindings = SceneNodeAuthoring::getCollectionBindingNames(stage->GetPrimAtPath(root));
                roots.push_back(matches);
            }
            roots[inserted.first->second].paths.push_back(path);
        }
        if (!skipped.isEmpty()) {
            warnings += tr("These matches lie inside the Materials Scope /%1 and are skipped: %2")
                            .arg(QString::fromStdString(scope))
                            .arg(skipped.join(QLatin1String(", ")));
        }
        if (roots.empty()) {
            warnings += QString::fromStdString(SceneNodeAuthoring::describeNoMatch(pattern, matched,
                                                                                   tr("the scene is passed through unchanged")
                                                                                       .toStdString()));
        } else if (isBlank(materialPath)) {
            warnings += tr("There is no material to bind: connect the Material input or set Material Path. The scene is "
                           "passed through unchanged.");
            roots.clear();
        } else if (!materialConnected && stage) {
            const PXR_NS::UsdPrim materialPrim = stage->GetPrimAtPath(PXR_NS::SdfPath(materialPath));
            if (!materialPrim || !materialPrim.IsA<PXR_NS::UsdShadeMaterial>()) {
                warnings += tr("%1 is not a material in the input scene; the binding is authored anyway.").arg(QString::fromStdString(materialPath));
            }
        }
        if (!roots.empty() && !projection.geometryProjectionShader.empty()) {
            SceneLayerEdit range;
            app->getProject()->getFrameRange(&range.firstFrame, &range.lastFrame);
            const double framesPerSecond = app->getProject()->getProjectFrameRate();
            ContentHasher keyHasher;
            keyHasher.appendU64(upstream.getStackHash());
            SceneNodeAuthoring::appendUpstreamMetadata(upstream, &keyHasher);
            keyHasher.appendString(projection.geometryProjectionShader);
            keyHasher.appendBool(projection.projectionHeld);
            keyHasher.appendDouble(projection.projectionHoldFrame);
            keyHasher.appendDouble(range.firstFrame);
            keyHasher.appendDouble(range.lastFrame);
            keyHasher.appendDouble(framesPerSecond);
            for (const RootMatches& matches : roots) {
                keyHasher.appendU64(matches.paths.size());
                for (const PXR_NS::SdfPath& path : matches.paths) {
                    keyHasher.appendString(path.GetString());
                }
            }
            const U64 placementKey = keyHasher.finalize();
            bool memoized = false;
            {
                std::lock_guard<std::mutex> l(_placementMemo->lock);
                if (_placementMemo->valid && (_placementMemo->key == placementKey)) {
                    placement = _placementMemo->placement;
                    memoized = true;
                }
            }
            if (!memoized) {
                placement = readProjectionPlacement(stage, roots, projection, upstream, range, framesPerSecond);
                std::lock_guard<std::mutex> l(_placementMemo->lock);
                _placementMemo->valid = true;
                _placementMemo->key = placementKey;
                _placementMemo->placement = placement;
            }
            if (!placement.othersAt.empty()) {
                warnings += tr("A projection in Geometry space takes one placement per material, and the matches lie at "
                               "several: it follows %1, so %2 shows it offset. Project them in World space, or through a "
                               "projection of their own.")
                                .arg(QString::fromStdString(placement.followed))
                                .arg(QString::fromStdString(placement.othersAt));
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
    binding.bindingName = getBindingName(getFullyQualifiedName());
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

    SceneLayerMemo::AuthorFunc author = [this, roots, binding, projection, placement](const SceneLayerEdit& edit,
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
        if (placement.matrices.empty()) {
            return true;
        }
        const auto frameAt = [&placement](double frame) {
            const std::map<double, PXR_NS::GfMatrix4d>::const_iterator found = placement.matrices.find(frame);

            return (found != placement.matrices.end()) ? found->second : placement.matrices.begin()->second;
        };
        if (!SceneNodeAuthoring::authorProjectionGeometryFrame(stage, edit, projection, placement.animated, frameAt)) {
            *error = tr("USD could not author the projection's geometry frame.").toStdString();

            return false;
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
    facts.appendU64(placement.matrices.size());
    if (!placement.matrices.empty()) {
        facts.appendString(projection.geometryProjectionShader);
        facts.appendBool(projection.projectionHeld);
        facts.appendBool(placement.animated);
        for (const std::pair<const double, PXR_NS::GfMatrix4d>& sample : placement.matrices) {
            facts.appendDouble(sample.first);
            facts.appendBytes(sample.second.data(), 16 * sizeof(double));
        }
    }
    SceneLayerReads reads;
    reads.upstream = SceneLayerMemo::Key::eUpstreamReadFacts;
    reads.factsDigest = facts.finalize();
    reads.frameRange = placement.animated;
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

// TfMakeValidIdentifier maps "A.B" and "A_B" to one name; the hash of the original keeps the
// bindings of such nodes apart.
std::string
USDAssignMaterial::getBindingName(const std::string& fullyQualifiedName)
{
    return PXR_NS::TfMakeValidIdentifier(fullyQualifiedName) + "_" + hexSuffix(fullyQualifiedName);
}

NATRON_NAMESPACE_EXIT
