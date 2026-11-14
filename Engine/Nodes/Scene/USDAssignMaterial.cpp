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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
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
const int kMaxPathsNamed = 3;

struct RootMatches {
    PXR_NS::SdfPath root;
    std::vector<PXR_NS::SdfPath> paths;
    // Native instance roots below the paths whose upstream direct binding is kept.
    std::vector<PXR_NS::SdfPath> excludes;
    std::vector<PXR_NS::TfToken> existingBindings;
};

// A native instance root that gets a direct binding: Storm carries no collection binding from an
// ancestor onto the prims of a native instance.
struct InstanceBinding {
    PXR_NS::SdfPath root;
    // Matches below the root: the binding reaches the whole instance, not only them.
    std::vector<PXR_NS::SdfPath> narrowMatches;
    bool wholeInstanceMatched = false;
    bool alreadyBound = false;
};

// The outermost instance: a nested instance is itself an instance proxy, which USD cannot author on.
PXR_NS::SdfPath
getInstanceRoot(const PXR_NS::UsdPrim& prim)
{
    PXR_NS::UsdPrim current = prim;

    while (current && current.IsInstanceProxy()) {
        current = current.GetParent();
    }

    return (current && current.IsInstance()) ? current.GetPath() : PXR_NS::SdfPath();
}

// Sorted, a path's descendants follow it directly, so a walk of each path not under the last walked
// one reaches every prim once.
std::vector<PXR_NS::SdfPath>
getOutermostPaths(std::vector<PXR_NS::SdfPath> paths)
{
    std::sort(paths.begin(), paths.end());
    std::vector<PXR_NS::SdfPath> outermost;
    for (const PXR_NS::SdfPath& path : paths) {
        if (outermost.empty() || !path.HasPrefix(outermost.back())) {
            outermost.push_back(path);
        }
    }

    return outermost;
}

struct PlacedPrim {
    PXR_NS::SdfPath path;
    std::map<double, PXR_NS::GfMatrix4d> matrices;
    bool animated = false;
    // Empty unless its points move or are skinned.
    PXR_NS::VtVec3fArray referencePoints;
    U64 referenceDigest = 0;
};

// What a projection in Geometry space takes from the prims it is assigned to.
struct ProjectionPlacement {
    std::vector<PlacedPrim> prims;
    bool animated = false;
    // Native instance roots with prims inside that deform, or sit away from the root. The frame is
    // authored once, on the root, so they take it rigidly; a reference primvar there would be one
    // value for every instance of the prototype.
    std::vector<PXR_NS::SdfPath> instanceRigid;
    std::vector<PXR_NS::SdfPath> instanceOffsets;
    std::string pointInstanced;
    std::string topologyChanges;
    std::string cannotSkin;
    // Prims whose placement already differs under another projection at the same hold.
    std::vector<PXR_NS::SdfPath> clashes;
};

void
appendPath(const PXR_NS::SdfPath& path,
           std::string* list)
{
    *list += (list->empty() ? "" : ", ") + path.GetString();
}

// Prototypes need not lie below their instancer, so only its relationship says which prims they are.
PXR_NS::SdfPathSet
getPointInstancerPrototypes(const PXR_NS::UsdStageRefPtr& stage)
{
    PXR_NS::SdfPathSet prototypes;

    for (const PXR_NS::UsdPrim& prim : stage->Traverse()) {
        if (prim.IsA<PXR_NS::UsdGeomPointInstancer>()) {
            PXR_NS::SdfPathVector targets;
            PXR_NS::UsdGeomPointInstancer(prim).GetPrototypesRel().GetForwardedTargets(&targets);
            prototypes.insert(targets.begin(), targets.end());
        }
    }

    return prototypes;
}

bool
isUnderAny(const PXR_NS::SdfPath& path,
           const PXR_NS::SdfPathSet& roots)
{
    if (roots.empty()) {
        return false;
    }
    for (PXR_NS::SdfPath ancestor = path; !ancestor.IsAbsoluteRootPath() && !ancestor.IsEmpty(); ancestor = ancestor.GetParentPath()) {
        if (roots.count(ancestor)) {
            return true;
        }
    }

    return false;
}

// Reads of one placement can part by a few float rounding steps, which grow with the coordinate.
bool
isSamePlacementValue(double a,
                     double b)
{
    return std::abs(a - b) <= 1e-6 * std::max({ 1., std::abs(a), std::abs(b) });
}

// A different material reading the primvar is what marks a different projection. This node's own
// layer is never in the scene it reads, so re-evaluating it finds nothing of its own; and one
// projection assigned twice is one material, which the later values place correctly.
bool
isBoundToAnotherProjection(const PXR_NS::UsdPrim& prim,
                           const SceneGeometryProjection& projection,
                           const std::string& materialPath)
{
    const PXR_NS::UsdShadeMaterialBindingAPI binding(prim);

    for (const PXR_NS::TfToken& purpose : { PXR_NS::UsdShadeTokens->allPurpose, PXR_NS::UsdShadeTokens->full, PXR_NS::UsdShadeTokens->preview }) {
        const PXR_NS::UsdShadeMaterial material = binding.ComputeBoundMaterial(purpose);
        SceneGeometryProjection bound;
        if (material && (material.GetPath().GetString() != materialPath) && SceneNodeAuthoring::readGeometryProjection(material.GetPrim(), &bound) && (bound.framePrimvar == projection.framePrimvar)) {
            return true;
        }
    }

    return false;
}

bool
clashesWithAnotherProjection(const PXR_NS::UsdPrim& prim,
                             const PlacedPrim& placed,
                             const SceneGeometryProjection& projection,
                             const std::string& materialPath)
{
    const PXR_NS::UsdGeomPrimvarsAPI primvars(prim);
    const PXR_NS::UsdAttribute frame = primvars.GetPrimvar(PXR_NS::TfToken(projection.framePrimvar)).GetAttr();
    const PXR_NS::UsdAttribute reference = (projection.referencePrimvar.empty() || placed.referencePoints.empty())
        ? PXR_NS::UsdAttribute()
        : primvars.GetPrimvar(PXR_NS::TfToken(projection.referencePrimvar)).GetAttr();
    const bool hasFrame = frame && frame.HasAuthoredValue();
    const bool hasReference = reference && reference.HasAuthoredValue();
    if ((!hasFrame && !hasReference) || !isBoundToAnotherProjection(prim, projection, materialPath)) {
        return false;
    }

    PXR_NS::GfMatrix4d matrix;
    if (hasFrame && !placed.matrices.empty() && frame.Get(&matrix)) {
        const double* const existing = matrix.data();
        const double* const placing = placed.matrices.begin()->second.data();
        for (int i = 0; i < 16; ++i) {
            if (!isSamePlacementValue(existing[i], placing[i])) {
                return true;
            }
        }
    }
    PXR_NS::VtVec3fArray points;
    if (hasReference && reference.Get(&points)) {
        if (points.size() != placed.referencePoints.size()) {
            return true;
        }
        const PXR_NS::GfVec3f* const existing = points.cdata();
        const PXR_NS::GfVec3f* const placing = placed.referencePoints.cdata();
        for (std::size_t i = 0; i < points.size(); ++i) {
            for (int axis = 0; axis < 3; ++axis) {
                if (!isSamePlacementValue(existing[i][axis], placing[i][axis])) {
                    return true;
                }
            }
        }
    }

    return false;
} // clashesWithAnotherProjection

ProjectionPlacement
readProjectionPlacement(const PXR_NS::UsdStageRefPtr& stage,
                        const std::vector<RootMatches>& roots,
                        const std::vector<InstanceBinding>& instances,
                        const PXR_NS::SdfPathSet& prototypes,
                        const SceneGeometryProjection& projection,
                        const std::string& materialPath,
                        const ScenePayload& scene,
                        const SceneLayerEdit& range,
                        double framesPerSecond)
{
    ProjectionPlacement placement;
    std::vector<PXR_NS::UsdPrim> gprims;
    std::vector<PXR_NS::SdfPath> matched;
    for (const RootMatches& matches : roots) {
        matched.insert(matched.end(), matches.paths.begin(), matches.paths.end());
    }
    for (const PXR_NS::SdfPath& path : getOutermostPaths(matched)) {
        PXR_NS::UsdPrimRange range(stage->GetPrimAtPath(path));
        for (PXR_NS::UsdPrimRange::iterator it = range.begin(); it != range.end(); ++it) {
            // Every instance below a match is noted with the instances, which place it through its
            // root, or keep its upstream binding and take no frame.
            if (it->IsInstance()) {
                it.PruneChildren();
                continue;
            }
            if (!it->IsA<PXR_NS::UsdGeomGprim>()) {
                continue;
            }
            if (isUnderAny(it->GetPath(), prototypes)) {
                appendPath(it->GetPath(), &placement.pointInstanced);
            }
            gprims.push_back(*it);
        }
    }

    const auto cacheAt = [&scene, framesPerSecond](double frame) {
        return PXR_NS::UsdGeomXformCache(PXR_NS::UsdTimeCode(SceneOps::frameToTimeCode(scene, frame, framesPerSecond)));
    };
    const double fixedFrame = projection.held ? projection.holdFrame : range.firstFrame;
    PXR_NS::UsdGeomXformCache fixedCache = cacheAt(fixedFrame);
    SceneReferenceContext referenceContext;
    referenceContext.holdTime = SceneOps::frameToTimeCode(scene, fixedFrame, framesPerSecond);
    referenceContext.firstTime = SceneOps::frameToTimeCode(scene, range.firstFrame, framesPerSecond);
    referenceContext.lastTime = SceneOps::frameToTimeCode(scene, range.lastFrame, framesPerSecond);
    const PXR_NS::GfMatrix4d identity(1.);
    for (const InstanceBinding& instance : instances) {
        const PXR_NS::UsdPrim root = stage->GetPrimAtPath(instance.root);
        if (instance.alreadyBound || !root) {
            continue;
        }
        gprims.push_back(root);
        const PXR_NS::GfMatrix4d rootInverse = fixedCache.GetLocalToWorldTransform(root).GetInverse();
        bool offsetFromRoot = false;
        // Points that move, change topology or cannot be skinned all need a reference primvar of
        // their own, which one shared by every instance cannot be.
        bool deforms = false;
        const bool checkDeforms = !projection.referencePrimvar.empty();
        for (const PXR_NS::UsdPrim& prim : PXR_NS::UsdPrimRange(root, PXR_NS::UsdTraverseInstanceProxies())) {
            if (offsetFromRoot && (deforms || !checkDeforms)) {
                break;
            }
            if (!prim.IsInstanceProxy() || !prim.IsA<PXR_NS::UsdGeomGprim>()) {
                continue;
            }
            if (!offsetFromRoot) {
                const PXR_NS::GfMatrix4d offset = fixedCache.GetLocalToWorldTransform(prim) * rootInverse;
                for (int i = 0; i < 16; ++i) {
                    offsetFromRoot = offsetFromRoot || !isSamePlacementValue(offset.data()[i], identity.data()[i]);
                }
            }
            if (checkDeforms && !deforms) {
                deforms = SceneNodeAuthoring::readReferencePoints(prim, &referenceContext, &fixedCache).status != SceneReferencePoints::eStatusStill;
            }
        }
        if (offsetFromRoot) {
            placement.instanceOffsets.push_back(instance.root);
        }
        if (deforms) {
            placement.instanceRigid.push_back(instance.root);
        }
    }
    for (const PXR_NS::UsdPrim& prim : gprims) {
        PlacedPrim placed;
        placed.path = prim.GetPath();
        placed.animated = !projection.held && SceneProjectionCameras::transformMightVary(prim);
        if (placed.animated) {
            placement.animated = true;
        } else {
            placed.matrices[fixedFrame] = fixedCache.GetLocalToWorldTransform(prim);
        }
        if (!projection.referencePrimvar.empty()) {
            const SceneReferencePoints reference = SceneNodeAuthoring::readReferencePoints(prim, &referenceContext, &fixedCache);
            if (reference.status == SceneReferencePoints::eStatusRead) {
                placed.referencePoints = reference.points;
                ContentHasher hasher;
                hasher.appendBytes(reference.points.cdata(), reference.points.size() * sizeof(PXR_NS::GfVec3f));
                placed.referenceDigest = hasher.finalize();
            } else if (reference.status == SceneReferencePoints::eStatusTopologyChanges) {
                appendPath(prim.GetPath(), &placement.topologyChanges);
            } else if (reference.status == SceneReferencePoints::eStatusCannotSkin) {
                appendPath(prim.GetPath(), &placement.cannotSkin);
            }
        }
        if (projection.held && clashesWithAnotherProjection(prim, placed, projection, materialPath)) {
            placement.clashes.push_back(prim.GetPath());
        }
        placement.prims.push_back(placed);
    }
    if (placement.animated) {
        for (double frame : SceneNodeAuthoring::getSampleFrames(range)) {
            PXR_NS::UsdGeomXformCache cache = cacheAt(frame);
            for (std::size_t i = 0; i < gprims.size(); ++i) {
                if (placement.prims[i].animated) {
                    placement.prims[i].matrices[frame] = cache.GetLocalToWorldTransform(gprims[i]);
                }
            }
        }
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
    std::shared_ptr<const ProjectionPlacement> placement;
    // Finding them walks the whole stage, so only a new input scene pays for it, not each change to
    // this node's own controls or the frame range.
    bool prototypesValid = false;
    U64 prototypesKey = 0;
    std::shared_ptr<const PXR_NS::SdfPathSet> prototypes;
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
    const auto nameSome = [](const std::vector<PXR_NS::SdfPath>& paths) {
        const int count = (int)paths.size();
        QStringList named;
        for (int i = 0; i < std::min(count, kMaxPathsNamed); ++i) {
            named += QString::fromStdString(paths[i].GetString());
        }
        QString list = named.join(QLatin1String(", "));
        if (count > kMaxPathsNamed) {
            list = tr("%1 and %2 more").arg(list).arg(count - kMaxPathsNamed);
        }

        return list;
    };

    ScenePayload base = upstream;
    std::string materialPath;
    SceneGeometryProjection projection;
    std::shared_ptr<const ProjectionPlacement> placement = std::make_shared<ProjectionPlacement>();
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
        projection = graft.projection;
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

    PXR_NS::TfToken bindingPurpose;
    switch (purposeKnob->getValue()) {
    case ePurposeFull:
        bindingPurpose = PXR_NS::UsdShadeTokens->full;
        break;
    case ePurposePreview:
        bindingPurpose = PXR_NS::UsdShadeTokens->preview;
        break;
    default:
        bindingPurpose = PXR_NS::UsdShadeTokens->allPurpose;
        break;
    }
    const PXR_NS::TfToken bindingStrength = (strengthKnob->getValue() == (int)eStrengthWeakerThanDescendants) ? PXR_NS::UsdShadeTokens->weakerThanDescendants : PXR_NS::UsdShadeTokens->strongerThanDescendants;

    std::vector<RootMatches> roots;
    std::vector<InstanceBinding> instances;
    if (!isBlank(pattern)) {
        SceneQueryPool::Lease lease;
        PXR_NS::UsdStageRefPtr stage;
        if (!upstream.isEmpty()) {
            lease = SceneQueryPool::instance().borrow(upstream, errorMessage);
            stage = lease.getStage();
            if (!stage) {
                if (errorMessage->empty()) {
                    *errorMessage = tr("USD could not compose the input scene.").toStdString();
                }
                setPersistentMessage(eMessageTypeError, *errorMessage);

                return eStatusFailed;
            }
        }
        std::vector<PXR_NS::SdfPath> found;
        if (!SceneOps::selectPrims(stage, pattern, &found, errorMessage, true)) {
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }
        std::unordered_map<PXR_NS::SdfPath, std::size_t, PXR_NS::SdfPath::Hash> rootIndices;
        std::unordered_map<PXR_NS::SdfPath, std::size_t, PXR_NS::SdfPath::Hash> instanceIndices;
        const auto noteInstance = [&](const PXR_NS::SdfPath& instanceRoot, const PXR_NS::SdfPath& narrowMatch) {
            const std::pair<std::unordered_map<PXR_NS::SdfPath, std::size_t, PXR_NS::SdfPath::Hash>::iterator, bool> added = instanceIndices.emplace(instanceRoot, instances.size());
            if (added.second) {
                InstanceBinding instance;
                instance.root = instanceRoot;
                const PXR_NS::UsdRelationship direct = PXR_NS::UsdShadeMaterialBindingAPI(stage->GetPrimAtPath(instanceRoot)).GetDirectBindingRel(bindingPurpose);
                instance.alreadyBound = direct && direct.HasAuthoredTargets();
                instances.push_back(instance);
            }
            InstanceBinding& instance = instances[added.first->second];
            if (narrowMatch.IsEmpty()) {
                instance.wholeInstanceMatched = true;
            } else {
                instance.narrowMatches.push_back(narrowMatch);
            }
        };
        QStringList skipped;
        std::vector<PXR_NS::SdfPath> unproxied;
        for (const PXR_NS::SdfPath& path : found) {
            const PXR_NS::SdfPath root = getRootPrim(path);
            if (!scope.empty() && (root.GetName() == scope)) {
                skipped += QString::fromStdString(path.GetString());
                continue;
            }
            const PXR_NS::UsdPrim matched = stage->GetPrimAtPath(path);
            if (matched.IsInstanceProxy()) {
                noteInstance(getInstanceRoot(matched), path);
                continue;
            }
            unproxied.push_back(path);
        }
        for (const PXR_NS::SdfPath& path : getOutermostPaths(unproxied)) {
            PXR_NS::UsdPrimRange range(stage->GetPrimAtPath(path));
            for (PXR_NS::UsdPrimRange::iterator it = range.begin(); it != range.end(); ++it) {
                if (it->IsInstance()) {
                    noteInstance(it->GetPath(), PXR_NS::SdfPath());
                    it.PruneChildren();
                }
            }
        }
        for (const PXR_NS::SdfPath& path : unproxied) {
            const PXR_NS::SdfPath root = getRootPrim(path);
            if (stage->GetPrimAtPath(path).IsInstance() && instances[instanceIndices[path]].alreadyBound) {
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
        PXR_NS::SdfPathSet collected;
        for (const RootMatches& matches : roots) {
            collected.insert(matches.paths.begin(), matches.paths.end());
        }
        for (InstanceBinding& instance : instances) {
            if (instance.alreadyBound) {
                // A stronger collection binding from an ancestor would win over the kept direct binding.
                const std::unordered_map<PXR_NS::SdfPath, std::size_t, PXR_NS::SdfPath::Hash>::const_iterator root = rootIndices.find(getRootPrim(instance.root));
                if ((root != rootIndices.end()) && isUnderAny(instance.root, collected)) {
                    roots[root->second].excludes.push_back(instance.root);
                }
                continue;
            }
            if (instance.wholeInstanceMatched) {
                continue;
            }
            const PXR_NS::SdfPathSet narrowMatches(instance.narrowMatches.begin(), instance.narrowMatches.end());
            bool everyGprimMatched = true;
            PXR_NS::UsdPrimRange range(stage->GetPrimAtPath(instance.root), PXR_NS::UsdTraverseInstanceProxies());
            for (PXR_NS::UsdPrimRange::iterator it = range.begin(); it != range.end(); ++it) {
                // A match covers all below it, so any gprim the walk still reaches is left out.
                if (narrowMatches.count(it->GetPath())) {
                    it.PruneChildren();
                } else if (it->IsA<PXR_NS::UsdGeomGprim>()) {
                    everyGprimMatched = false;
                    break;
                }
            }
            // Binding the whole instance then reaches nothing the pattern left out.
            instance.wholeInstanceMatched = everyGprimMatched;
        }
        if (!skipped.isEmpty()) {
            warnings += tr("These matches lie inside the Materials Scope /%1 and are skipped: %2")
                            .arg(QString::fromStdString(scope))
                            .arg(skipped.join(QLatin1String(", ")));
        }
        if (roots.empty() && instances.empty()) {
            warnings += tr("The prim pattern \"%1\" matches no prim to bind the material to; the scene is passed through "
                           "unchanged.")
                            .arg(QString::fromStdString(pattern));
        } else if (isBlank(materialPath)) {
            warnings += tr("There is no material to bind: connect the Material input or set Material Path. The scene is "
                           "passed through unchanged.");
            roots.clear();
            instances.clear();
        } else if (!materialConnected && stage) {
            const PXR_NS::UsdPrim materialPrim = stage->GetPrimAtPath(PXR_NS::SdfPath(materialPath));
            if (!materialPrim || !materialPrim.IsA<PXR_NS::UsdShadeMaterial>()) {
                warnings += tr("%1 is not a material in the input scene; the binding is authored anyway.").arg(QString::fromStdString(materialPath));
            }
            SceneNodeAuthoring::readGeometryProjection(materialPrim, &projection);
        }
        if ((!roots.empty() || !instances.empty()) && !projection.framePrimvar.empty()) {
            SceneLayerEdit range;
            app->getProject()->getFrameRange(&range.firstFrame, &range.lastFrame);
            const double framesPerSecond = app->getProject()->getProjectFrameRate();
            ContentHasher upstreamHasher;
            upstreamHasher.appendU64(upstream.getStackHash());
            SceneNodeAuthoring::appendUpstreamMetadata(upstream, &upstreamHasher);
            const U64 upstreamKey = upstreamHasher.finalize();
            ContentHasher keyHasher;
            keyHasher.appendU64(upstreamKey);
            keyHasher.appendString(materialPath);
            keyHasher.appendString(projection.framePrimvar);
            keyHasher.appendString(projection.referencePrimvar);
            keyHasher.appendBool(projection.held);
            keyHasher.appendDouble(projection.holdFrame);
            keyHasher.appendDouble(range.firstFrame);
            keyHasher.appendDouble(range.lastFrame);
            keyHasher.appendDouble(framesPerSecond);
            keyHasher.appendU64(instances.size());
            for (const InstanceBinding& instance : instances) {
                keyHasher.appendString(instance.root.GetString());
                keyHasher.appendBool(instance.alreadyBound);
            }
            for (const RootMatches& matches : roots) {
                keyHasher.appendU64(matches.paths.size());
                for (const PXR_NS::SdfPath& path : matches.paths) {
                    keyHasher.appendString(path.GetString());
                }
                keyHasher.appendU64(matches.excludes.size());
                for (const PXR_NS::SdfPath& path : matches.excludes) {
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
                std::shared_ptr<const PXR_NS::SdfPathSet> prototypes;
                {
                    std::lock_guard<std::mutex> l(_placementMemo->lock);
                    if (_placementMemo->prototypesValid && (_placementMemo->prototypesKey == upstreamKey)) {
                        prototypes = _placementMemo->prototypes;
                    }
                }
                if (!prototypes) {
                    prototypes = std::make_shared<const PXR_NS::SdfPathSet>(getPointInstancerPrototypes(stage));
                    std::lock_guard<std::mutex> l(_placementMemo->lock);
                    _placementMemo->prototypesValid = true;
                    _placementMemo->prototypesKey = upstreamKey;
                    _placementMemo->prototypes = prototypes;
                }
                placement = std::make_shared<const ProjectionPlacement>(readProjectionPlacement(stage, roots, instances, *prototypes, projection, materialPath, upstream, range, framesPerSecond));
                std::lock_guard<std::mutex> l(_placementMemo->lock);
                _placementMemo->valid = true;
                _placementMemo->key = placementKey;
                _placementMemo->placement = placement;
            }
            if (!placement->instanceRigid.empty()) {
                warnings += tr("These native instances hold prims that deform: the projection is placed once per "
                               "instance, so they take it rigidly and slide through the image. %1")
                                .arg(nameSome(placement->instanceRigid));
            }
            if (!placement->instanceOffsets.empty()) {
                warnings += tr("These native instances hold prims at an offset from the instance's root, from where the "
                               "projection is placed: it falls on them shifted by that offset. %1")
                                .arg(nameSome(placement->instanceOffsets));
            }
            if (!placement->pointInstanced.empty()) {
                warnings += tr("These matches are prototypes of a point instancer, whose instances share one placement: "
                               "each shows the image as it falls on the prototype. %1")
                                .arg(QString::fromStdString(placement->pointInstanced));
            }
            if (!placement->topologyChanges.empty()) {
                warnings += tr("The topology of these matches changes over the project's frame range, so their points at "
                               "the Frame Hold cannot apply: they take the projection rigidly and slide through the image. %1")
                                .arg(QString::fromStdString(placement->topologyChanges));
            }
            if (!placement->cannotSkin.empty()) {
                warnings += tr("USD cannot skin these matches, which are bound to a skeleton: they take the projection "
                               "rigidly and slide through the image. %1")
                                .arg(QString::fromStdString(placement->cannotSkin));
            }
            if (!placement->clashes.empty()) {
                warnings += tr("Another projection at Frame Hold %1 has placed these prims differently, %2 in all: %3. "
                               "Projections at one Frame Hold share a prim's placement, so they cannot both be correct: "
                               "this node's placement wins.")
                                .arg(projection.holdFrame)
                                .arg((int)placement->clashes.size())
                                .arg(nameSome(placement->clashes));
            }
        }
    }

    std::vector<PXR_NS::SdfPath> narrow;
    std::vector<PXR_NS::SdfPath> alreadyBound;
    std::vector<PXR_NS::SdfPath> instanceRoots;
    for (const InstanceBinding& instance : instances) {
        if (instance.alreadyBound) {
            alreadyBound.push_back(instance.root);
            continue;
        }
        instanceRoots.push_back(instance.root);
        if (!instance.wholeInstanceMatched) {
            narrow.push_back(instance.root);
        }
    }
    if (!narrow.empty()) {
        warnings += tr("Only some prims of these native instances match, but an instance takes a material only as a "
                       "whole, including the instances nested in it, so every prim of it takes the material: %1")
                        .arg(nameSome(narrow));
    }
    if (!alreadyBound.empty()) {
        warnings += tr("These native instances already have a direct material binding from upstream, which is kept: %1")
                        .arg(nameSome(alreadyBound));
    }

    if (roots.empty() && instanceRoots.empty()) {
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
    binding.purpose = bindingPurpose;
    binding.strength = bindingStrength;

    SceneLayerMemo::AuthorFunc author = [this, roots, instanceRoots, binding, projection, placement](const SceneLayerEdit& edit,
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
            rootBinding.excludes = matches.excludes;
            rootBinding.existingBindings = matches.existingBindings;
            if (!SceneNodeAuthoring::authorCollectionBinding(stage->OverridePrim(matches.root), rootBinding, error)) {
                return false;
            }
        }
        for (const PXR_NS::SdfPath& instanceRoot : instanceRoots) {
            if (!SceneNodeAuthoring::authorMaterialBinding(stage->OverridePrim(instanceRoot), binding.materialPath, error, binding.purpose, binding.strength)) {
                return false;
            }
        }
        for (const PlacedPrim& placed : placement->prims) {
            const auto frameAt = [&placed](double frame) {
                const std::map<double, PXR_NS::GfMatrix4d>::const_iterator found = placed.matrices.find(frame);

                return (found != placed.matrices.end()) ? found->second : placed.matrices.begin()->second;
            };
            if (!SceneNodeAuthoring::authorProjectionFrame(stage->OverridePrim(placed.path), edit, projection, placed.animated, frameAt)) {
                *error = tr("USD could not author the projection's placement on %1.").arg(QString::fromStdString(placed.path.GetString())).toStdString();

                return false;
            }
            if (!placed.referencePoints.empty() && !SceneNodeAuthoring::authorReferencePoints(stage->OverridePrim(placed.path), projection, placed.referencePoints)) {
                *error = tr("USD could not author the projection's reference points on %1.").arg(QString::fromStdString(placed.path.GetString())).toStdString();

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
        facts.appendU64(matches.excludes.size());
        for (const PXR_NS::SdfPath& path : matches.excludes) {
            facts.appendString(path.GetString());
        }
        facts.appendU64(matches.existingBindings.size());
        for (const PXR_NS::TfToken& name : matches.existingBindings) {
            facts.appendString(name.GetString());
        }
    }
    facts.appendU64(instanceRoots.size());
    for (const PXR_NS::SdfPath& instanceRoot : instanceRoots) {
        facts.appendString(instanceRoot.GetString());
    }
    facts.appendString(binding.purpose.GetString());
    facts.appendString(binding.strength.GetString());
    facts.appendU64(placement->prims.size());
    if (!placement->prims.empty()) {
        facts.appendString(projection.framePrimvar);
        facts.appendString(projection.referencePrimvar);
        facts.appendBool(projection.held);
        facts.appendDouble(projection.holdFrame);
        for (const PlacedPrim& placed : placement->prims) {
            facts.appendString(placed.path.GetString());
            facts.appendBool(placed.animated);
            facts.appendU64(placed.referenceDigest);
            for (const std::pair<const double, PXR_NS::GfMatrix4d>& sample : placed.matrices) {
                facts.appendDouble(sample.first);
                facts.appendBytes(sample.second.data(), 16 * sizeof(double));
            }
        }
    }
    SceneLayerReads reads;
    reads.upstream = SceneLayerMemo::Key::eUpstreamReadFacts;
    reads.factsDigest = facts.finalize();
    reads.frameRange = placement->animated;
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
