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

#include "SceneOps.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <set>

#include "Engine/ContentHasher.h"
#include "Engine/Nodes/Scene/SceneLayerMemo.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/SceneWriteBarrier.h"

NATRON_NAMESPACE_ENTER

namespace {
// A property under one of an input's roots whose composed targets leave that root.
struct InputLink {
    PXR_NS::SdfPath property;
    std::vector<PXR_NS::SdfPath> targets;
    bool isConnection = false;
    bool custom = false;
    PXR_NS::SdfVariability variability = PXR_NS::SdfVariabilityUniform;
    PXR_NS::SdfValueTypeName typeName;
};

// One input of a re-parenting node, as last grafted.
struct GraftSlot {
    ScenePayload input;
    // The input's root prim names, in the composed stage's order.
    std::vector<std::string> rootNames;
    // The properties of its link index that its stage composes with targets out of their root.
    std::vector<InputLink> links;
    // Sublayers input's stack; created once the input has roots.
    PXR_NS::SdfLayerRefPtr wrapper;
    std::string destinationPath;
    // The name each root is grafted under, index for index with rootNames.
    std::vector<std::string> childNames;
    // Null layer for an input without roots.
    SceneLayerEntry graft;
};
} // anonymous namespace

struct SceneGraftMemo::State {
    std::mutex mutex;
    std::string structurePath;
    std::string structureDestinationType;
    std::string structureAncestorType;
    SceneLayerEntry structure;
    SceneLayerMemo overrides;
    // The inputs of the last successful reparentRoots(), in input order.
    std::vector<GraftSlot> slots;
    ScenePayload output;
    U64 graftLayerAuthorCount = 0;
};

namespace {
// The root prim path lies under, or the empty path when it lies under none.
PXR_NS::SdfPath
rootPrimOf(const PXR_NS::SdfPath& path)
{
    PXR_NS::SdfPath prim = path.StripAllVariantSelections().GetPrimPath();

    if (!prim.IsAbsolutePath()) {
        return PXR_NS::SdfPath();
    }
    while (prim.IsPrimPath() && !prim.IsRootPrimPath()) {
        prim = prim.GetParentPath();
    }

    return prim.IsRootPrimPath() ? prim : PXR_NS::SdfPath();
}

bool
leavesRoot(const PXR_NS::SdfPath& property,
           const std::vector<PXR_NS::SdfPath>& targets)
{
    const PXR_NS::SdfPath root = rootPrimOf(property);

    for (const PXR_NS::SdfPath& target : targets) {
        if (root.IsEmpty() || (rootPrimOf(target) != root)) {
            return true;
        }
    }

    return false;
}

void
appendLayerLinks(const PXR_NS::SdfLayerRefPtr& layer,
                 SceneLinkIndex* links)
{
    layer->Traverse(PXR_NS::SdfPath::AbsoluteRootPath(), [&layer, links](const PXR_NS::SdfPath& path) {
        if (!path.IsPrimPropertyPath()) {
            return;
        }
        PXR_NS::SdfPathListOp listOp;
        if (!layer->HasField(path, PXR_NS::SdfFieldKeys->TargetPaths, &listOp) && !layer->HasField(path, PXR_NS::SdfFieldKeys->ConnectionPaths, &listOp)) {
            return;
        }
        const PXR_NS::SdfPath property = path.StripAllVariantSelections();
        std::vector<PXR_NS::SdfPath> targets = listOp.GetAppliedItems();
        for (PXR_NS::SdfPath& target : targets) {
            target = target.MakeAbsolutePath(property.GetPrimPath());
        }
        if (leavesRoot(property, targets)) {
            links->push_back(property);
        }
    });
}

void
sortLinks(SceneLinkIndex* links)
{
    std::sort(links->begin(), links->end());
    links->erase(std::unique(links->begin(), links->end()), links->end());
}

std::shared_ptr<const SceneLinkIndex>
shareLinks(SceneLinkIndex links)
{
    if (links.empty()) {
        return std::shared_ptr<const SceneLinkIndex>();
    }
    sortLinks(&links);

    return std::make_shared<const SceneLinkIndex>(std::move(links));
}
} // anonymous namespace

namespace SceneOps {
bool
checkUsdRuntime(std::string* errorMessage)
{
    static const std::string missing = []() {
        std::string names;
        PXR_NS::PlugRegistry& registry = PXR_NS::PlugRegistry::GetInstance();
        for (const char* name : { "ar", "sdf", "sdr", "usd", "usdGeom", "usdLux", "usdShade" }) {
            if (!registry.GetPluginWithName(name)) {
                names += names.empty() ? name : (std::string(", ") + name);
            }
        }

        return names;
    }();

    if (missing.empty()) {
        return true;
    }
    if (errorMessage) {
        static const char* const hint = ". The plugInfo.json files belong in the usd directory beside the OpenUSD libraries; "
                                        "run with TF_DEBUG=PLUG_INFO_SEARCH to see where OpenUSD looked.";
        *errorMessage = "OpenUSD is not usable: it found no plug-in registration for " + missing + hint;
    }

    return false;
}

PXR_NS::SdfLayerRefPtr
createNodeLayer(const std::string& tag)
{
    return PXR_NS::SdfLayer::CreateAnonymous(tag);
}

ScenePayload
appendLayer(const ScenePayload& base,
            const PXR_NS::SdfLayerRefPtr& layer,
            U64 layerHash)
{
    SceneLayerEntry entry;

    entry.layer = layer;
    entry.hash = layerHash;
    entry.links = shareLinks(findLayerLinks(layer));

    return appendEntry(base, entry);
}

SceneLinkIndex
findLayerLinks(const PXR_NS::SdfLayerRefPtr& layer)
{
    SceneLinkIndex links;

    if (layer) {
        appendLayerLinks(layer, &links);
    }
    sortLinks(&links);

    return links;
}

SceneLinkIndex
findLayerStackLinks(const PXR_NS::SdfLayerRefPtr& layer)
{
    SceneLinkIndex links;

    if (!layer) {
        return links;
    }
    std::set<std::string> visited;
    std::vector<PXR_NS::SdfLayerRefPtr> pending(1, layer);
    PXR_NS::TfErrorMark mark;
    while (!pending.empty()) {
        const PXR_NS::SdfLayerRefPtr current = pending.back();
        pending.pop_back();
        if (!visited.insert(current->GetIdentifier()).second) {
            continue;
        }
        appendLayerLinks(current, &links);
        for (const std::string& subLayerPath : current->GetSubLayerPaths()) {
            const PXR_NS::SdfLayerRefPtr subLayer = PXR_NS::SdfLayer::FindOrOpenRelativeToLayer(current, subLayerPath);
            if (subLayer) {
                pending.push_back(subLayer);
            }
        }
    }
    // A missing sublayer is the stage's error to report when it composes.
    mark.Clear();
    sortLinks(&links);

    return links;
}

SceneLinkIndex
getLinks(const ScenePayload& payload)
{
    SceneLinkIndex links;

    for (const ScenePayloadImpl* link = payload.getImpl().get(); link; link = link->base.get()) {
        if (link->top.links) {
            links.insert(links.end(), link->top.links->begin(), link->top.links->end());
        }
    }
    sortLinks(&links);

    return links;
}

ScenePayload
appendEntry(const ScenePayload& base,
            const SceneLayerEntry& entry)
{
    const PXR_NS::SdfLayerRefPtr& layer = entry.layer;
    const U64 layerHash = entry.hash;

    if (!layer) {
        return base;
    }

    std::shared_ptr<ScenePayloadImpl> imp = std::make_shared<ScenePayloadImpl>();
    imp->base = base.getImpl();
    imp->top = entry;
    imp->numLayers = base.getNumLayers() + 1;

    ContentHasher h;
    h.appendU64(base.getStackHash());
    h.appendU64(layerHash);
    imp->stackHash = h.finalize();
    // 0 stands for the empty stack.
    if (imp->stackHash == 0) {
        imp->stackHash = 1;
    }

    imp->metadata = readLayerMetadata(layer, base.getMetadata());

    return ScenePayload(imp);
}

std::vector<SceneLayerEntry>
getLayers(const ScenePayload& payload)
{
    std::vector<SceneLayerEntry> layers;

    layers.reserve(payload.getNumLayers());
    for (const ScenePayloadImpl* link = payload.getImpl().get(); link; link = link->base.get()) {
        layers.push_back(link->top);
    }
    std::reverse(layers.begin(), layers.end());

    return layers;
}

SceneMetadata
readLayerMetadata(const PXR_NS::SdfLayerRefPtr& layer,
                  const SceneMetadata& weaker)
{
    SceneMetadata metadata = weaker;

    if (!layer) {
        return metadata;
    }

    const PXR_NS::SdfPath& root = PXR_NS::SdfPath::AbsoluteRootPath();
    PXR_NS::TfToken upAxis;
    if (layer->HasField(root, PXR_NS::UsdGeomTokens->upAxis, &upAxis)) {
        if (upAxis == PXR_NS::UsdGeomTokens->z) {
            metadata.upAxis = SceneMetadata::eUpAxisZ;
        } else if (upAxis == PXR_NS::UsdGeomTokens->y) {
            metadata.upAxis = SceneMetadata::eUpAxisY;
        }
    }
    double metersPerUnit = 0.;
    if (layer->HasField(root, PXR_NS::UsdGeomTokens->metersPerUnit, &metersPerUnit) && (metersPerUnit > 0.)) {
        metadata.metersPerUnit = metersPerUnit;
    }
    if (layer->HasDefaultPrim()) {
        metadata.defaultPrim = layer->GetDefaultPrim().GetString();
    }
    if (layer->HasTimeCodesPerSecond()) {
        metadata.timeCodesPerSecond = layer->GetTimeCodesPerSecond();
    }

    return metadata;
}

std::vector<std::string>
getSubLayerPaths(const ScenePayload& payload)
{
    std::vector<std::string> subLayerPaths;

    subLayerPaths.reserve(payload.getNumLayers());
    for (const ScenePayloadImpl* link = payload.getImpl().get(); link; link = link->base.get()) {
        subLayerPaths.push_back(link->top.layer->GetIdentifier());
    }

    return subLayerPaths;
}

void
writeRootMetadata(const PXR_NS::SdfLayerRefPtr& root,
                  const SceneMetadata& metadata,
                  const SceneMetadata* applied)
{
    if (!root) {
        return;
    }

    // upAxis and metersPerUnit are plug-in metadata (UsdGeom's plugInfo.json), and the stage
    // scene index resyncs the whole stage when plug-in metadata changes on / (OpenUSD v26.08
    // usdImaging/stageSceneIndex.cpp, _OnUsdObjectsChanged), so unchanged fields are not
    // rewritten. Writing a field its current value sends no notice either, but skipping it costs
    // nothing.
    const PXR_NS::SdfPath& rootPath = PXR_NS::SdfPath::AbsoluteRootPath();
    if (!applied || (applied->upAxis != metadata.upAxis)) {
        root->SetField(rootPath, PXR_NS::UsdGeomTokens->upAxis,
                       (metadata.upAxis == SceneMetadata::eUpAxisZ) ? PXR_NS::UsdGeomTokens->z : PXR_NS::UsdGeomTokens->y);
    }
    if (!applied || (applied->metersPerUnit != metadata.metersPerUnit)) {
        root->SetField(rootPath, PXR_NS::UsdGeomTokens->metersPerUnit, metadata.metersPerUnit);
    }
    if (!applied || (applied->defaultPrim != metadata.defaultPrim)) {
        if (metadata.defaultPrim.empty()) {
            root->ClearDefaultPrim();
        } else {
            root->SetDefaultPrim(PXR_NS::TfToken(metadata.defaultPrim));
        }
    }
    if (!applied || (applied->timeCodesPerSecond != metadata.timeCodesPerSecond)) {
        root->SetTimeCodesPerSecond(metadata.timeCodesPerSecond);
    }
}

namespace {
    std::atomic<U64> stageOpenCount(0);
}

PXR_NS::UsdStageRefPtr
openStage(const PXR_NS::SdfLayerRefPtr& root)
{
    if (!root) {
        return PXR_NS::UsdStageRefPtr();
    }
    ++stageOpenCount;

    // The session layer is passed explicitly as null: the Open overload without one creates an
    // anonymous session layer (stage.h). Nothing edits these stages but their owner, through the
    // root layer.
    return PXR_NS::UsdStage::Open(root, PXR_NS::SdfLayerHandle(), PXR_NS::UsdStage::LoadAll);
}

U64
getStageOpenCount()
{
    return stageOpenCount.load();
}

PXR_NS::UsdStageRefPtr
compose(const ScenePayload& payload)
{
    if (!checkUsdRuntime(nullptr)) {
        return PXR_NS::UsdStageRefPtr();
    }

    PXR_NS::SdfLayerRefPtr root = PXR_NS::SdfLayer::CreateAnonymous("natron-scene-root");

    if (!root) {
        return PXR_NS::UsdStageRefPtr();
    }
    root->SetSubLayerPaths(getSubLayerPaths(payload));
    writeRootMetadata(root, payload.getMetadata(), nullptr);

    return openStage(root);
}

std::vector<PXR_NS::SdfLayerRefPtr>
getFileLayerClosure(const PXR_NS::SdfLayerRefPtr& layer)
{
    std::vector<PXR_NS::SdfLayerRefPtr> closure;

    if (!layer) {
        return closure;
    }

    // GetCompositionAssetDependencies lists one layer's own sublayer, reference and payload asset
    // paths, without recursing (sdf/layer.h), so the walk recurses itself.
    std::set<std::string> visited;
    std::vector<PXR_NS::SdfLayerRefPtr> pending(1, layer);
    PXR_NS::TfErrorMark mark;
    while (!pending.empty()) {
        const PXR_NS::SdfLayerRefPtr current = pending.back();
        pending.pop_back();
        if (!visited.insert(current->GetIdentifier()).second) {
            continue;
        }
        if (!current->IsAnonymous()) {
            closure.push_back(current);
        }
        for (const std::string& assetPath : current->GetCompositionAssetDependencies()) {
            if (assetPath.empty()) {
                continue;
            }
            // Resolves assetPath against current's location; FindOrOpenRelativeToLayer needs a
            // valid anchor, which current is.
            const PXR_NS::SdfLayerRefPtr dependency = PXR_NS::SdfLayer::FindOrOpenRelativeToLayer(current, assetPath);
            if (dependency) {
                pending.push_back(dependency);
            }
        }
    }
    // A missing or unreadable dependency is the stage's error to report when it composes, not
    // this walk's.
    mark.Clear();

    return closure;
}

namespace {
    std::atomic<std::uint64_t> fileLayerReloadCount(0);
} // anonymous namespace

bool
reloadFileLayers(const std::vector<PXR_NS::SdfLayerRefPtr>& layers,
                 std::string* errorMessage)
{
    std::set<PXR_NS::SdfLayerHandle> fileLayers;

    for (const PXR_NS::SdfLayerRefPtr& layer : layers) {
        // UsdStage::Reload documents that reloading an anonymous layer clears its content.
        if (layer && !layer->IsAnonymous()) {
            fileLayers.insert(layer);
        }
    }
    if (fileLayers.empty()) {
        return true;
    }

    bool reloaded = true;
    if (!SceneWriteBarrier::runExclusive([&fileLayers, &reloaded]() {
            // Only layers whose file changed since they were read reload (force = false compares
            // modification times, sdf/layer.h). All of them in one call: ReloadLayers wraps the
            // reloads in one change block (OpenUSD v26.08 sdf/layer.cpp), so each stage recomposes
            // once.
            reloaded = PXR_NS::SdfLayer::ReloadLayers(fileLayers, false);
            // Counted even when some layers failed: the others may have reloaded.
            fileLayerReloadCount.fetch_add(1);
        },
                                         errorMessage)) {
        return false;
    }
    if (!reloaded && errorMessage) {
        *errorMessage = "Some USD files could not be read again.";
    }

    return reloaded;
}

std::uint64_t
getFileLayerReloadCount()
{
    return fileLayerReloadCount.load();
}

double
frameToTimeCode(const ScenePayload& payload,
                double frame,
                double framesPerSecond)
{
    if (framesPerSecond <= 0.) {
        return frame;
    }

    return frame * payload.getMetadata().timeCodesPerSecond / framesPerSecond;
}

bool
parsePrimExpression(const std::string& text,
                    PXR_NS::SdfPathExpression* expression,
                    std::string* errorMessage)
{
    *expression = PXR_NS::SdfPathExpression();
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
        return true;
    }
    PXR_NS::SdfPathExpression parsed;
    {
        PXR_NS::TfErrorMark mark;
        parsed = PXR_NS::SdfPathExpression(text);
        mark.Clear();
    }
    if (!parsed.GetParseError().empty()) {
        *errorMessage = "\"" + text + "\" is not a valid prim selection: " + parsed.GetParseError();

        return false;
    }
    if (!parsed.IsAbsolute()) {
        *errorMessage = "\"" + text + "\" holds a relative path; prim selections name absolute paths such as /World/Geo or //*Light*.";

        return false;
    }
    if (parsed.ContainsExpressionReferences()) {
        *errorMessage = "\"" + text + "\" references a collection (%), which a prim selection cannot resolve.";

        return false;
    }
    *expression = parsed;

    return true;
}

bool
selectPrims(const PXR_NS::UsdStageRefPtr& stage,
            const std::string& expression,
            std::vector<PXR_NS::SdfPath>* paths,
            std::string* errorMessage)
{
    paths->clear();
    PXR_NS::SdfPathExpression parsed;
    if (!parsePrimExpression(expression, &parsed, errorMessage)) {
        return false;
    }
    if (!stage || parsed.IsEmpty()) {
        return true;
    }
    const PXR_NS::UsdObjectCollectionExpressionEvaluator evaluator(stage, parsed);
    PXR_NS::UsdObjectCollectionExpressionEvaluator::IncrementalSearcher searcher = evaluator.MakeIncrementalSearcher();
    PXR_NS::UsdPrimRange range = stage->Traverse();
    for (PXR_NS::UsdPrimRange::iterator it = range.begin(); it != range.end(); ++it) {
        const PXR_NS::SdfPredicateFunctionResult result = searcher.Next(it->GetPath());
        if (result.GetValue()) {
            paths->push_back(it->GetPath());
        } else if (result.IsConstant()) {
            // Nothing below can match: a literal path such as /World/Box costs its depth, not
            // the stage's size.
            it.PruneChildren();
        }
    }

    return true;
}

namespace {
    // The defined, non-abstract root prims of input's composed stage, and the properties under
    // them its link index lists whose composed targets leave their root. Pure overs define nothing
    // to graft, and classes stay where the input's inherits expect them.
    bool
    readInput(const ScenePayload& input,
              std::vector<std::string>* names,
              std::vector<InputLink>* links,
              std::string* errorMessage)
    {
        if (input.isEmpty()) {
            return true;
        }
        SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(input, errorMessage);
        if (!lease.isValid()) {
            return false;
        }
        const PXR_NS::UsdStageRefPtr stage = lease.getStage();
        for (const PXR_NS::UsdPrim& root : stage->GetPseudoRoot().GetFilteredChildren(PXR_NS::UsdPrimIsDefined && !PXR_NS::UsdPrimIsAbstract)) {
            names->push_back(root.GetName().GetString());
        }

        const std::set<std::string> roots(names->begin(), names->end());
        for (const PXR_NS::SdfPath& property : getLinks(input)) {
            const PXR_NS::SdfPath root = rootPrimOf(property);
            if (root.IsEmpty() || !roots.count(root.GetName())) {
                continue;
            }
            const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(property.GetPrimPath());
            if (!prim) {
                continue;
            }
            InputLink link;
            link.property = property;
            const PXR_NS::UsdProperty composed = prim.GetProperty(property.GetNameToken());
            if (composed.Is<PXR_NS::UsdRelationship>()) {
                const PXR_NS::UsdRelationship relationship = composed.As<PXR_NS::UsdRelationship>();
                relationship.GetTargets(&link.targets);
                link.custom = relationship.IsCustom();
            } else if (composed.Is<PXR_NS::UsdAttribute>()) {
                const PXR_NS::UsdAttribute attribute = composed.As<PXR_NS::UsdAttribute>();
                attribute.GetConnections(&link.targets);
                link.isConnection = true;
                link.custom = attribute.IsCustom();
                link.variability = attribute.GetVariability();
                link.typeName = attribute.GetTypeName();
            } else {
                continue;
            }
            if (leavesRoot(property, link.targets)) {
                links->push_back(link);
            }
        }

        return true;
    }

    // Where path lies once slot's roots are grafted under destination; the empty path when it lies
    // under none of them.
    PXR_NS::SdfPath
    graftedPathOf(const GraftSlot& slot,
                  const PXR_NS::SdfPath& destination,
                  const PXR_NS::SdfPath& path)
    {
        const PXR_NS::SdfPath root = rootPrimOf(path);

        if (root.IsEmpty()) {
            return root;
        }
        for (std::size_t i = 0; i < slot.rootNames.size(); ++i) {
            if (slot.rootNames[i] == root.GetName()) {
                return path.ReplacePrefix(root, destination.AppendChild(PXR_NS::TfToken(slot.childNames[i])));
            }
        }

        return PXR_NS::SdfPath();
    }

    // Copies onto layer the stage metadata input's layers author, each field from the strongest
    // layer authoring it. Fields no layer authors stay unauthored, so a layer stack composing layer
    // falls back for them as it would for input's own layers: an unauthored timeCodesPerSecond
    // takes the composing stack's rate, and an unauthored upAxis does not override a stronger one
    // merged in later.
    void
    carryAuthoredMetadata(const ScenePayload& input,
                          const PXR_NS::SdfLayerRefPtr& layer)
    {
        const PXR_NS::SdfPath& root = PXR_NS::SdfPath::AbsoluteRootPath();
        bool hasUpAxis = false;
        bool hasMetersPerUnit = false;
        bool hasTimeCodesPerSecond = false;
        const std::vector<SceneLayerEntry> layers = getLayers(input);

        for (std::vector<SceneLayerEntry>::const_reverse_iterator it = layers.rbegin(); it != layers.rend(); ++it) {
            const PXR_NS::SdfLayerRefPtr& source = it->layer;
            PXR_NS::VtValue value;
            if (!hasUpAxis && source->HasField(root, PXR_NS::UsdGeomTokens->upAxis, &value)) {
                layer->SetField(root, PXR_NS::UsdGeomTokens->upAxis, value);
                hasUpAxis = true;
            }
            if (!hasMetersPerUnit && source->HasField(root, PXR_NS::UsdGeomTokens->metersPerUnit, &value)) {
                layer->SetField(root, PXR_NS::UsdGeomTokens->metersPerUnit, value);
                hasMetersPerUnit = true;
            }
            if (!hasTimeCodesPerSecond && source->HasTimeCodesPerSecond()) {
                layer->SetTimeCodesPerSecond(source->GetTimeCodesPerSecond());
                hasTimeCodesPerSecond = true;
            }
        }
    }

    // The root layer of the layer stack a graft layer references: it sublayers input's stack,
    // strongest first, as a stage composing input on its own would.
    PXR_NS::SdfLayerRefPtr
    createWrapper(const ScenePayload& input,
                  const std::string& tag)
    {
        PXR_NS::SdfLayerRefPtr wrapper = PXR_NS::SdfLayer::CreateAnonymous(tag + "-wrapper");

        if (!wrapper) {
            return wrapper;
        }
        wrapper->SetSubLayerPaths(getSubLayerPaths(input));
        carryAuthoredMetadata(input, wrapper);
        wrapper->SetPermissionToEdit(false);

        return wrapper;
    }

    PXR_NS::SdfLayerRefPtr
    authorStructureLayer(const PXR_NS::SdfPath& destination,
                         const std::string& destinationType,
                         const std::string& ancestorType,
                         const std::string& tag)
    {
        PXR_NS::SdfLayerRefPtr layer = createNodeLayer(tag + "-structure");

        if (!layer) {
            return layer;
        }
        for (const PXR_NS::SdfPath& prefix : destination.GetPrefixes()) {
            const PXR_NS::SdfPrimSpecHandle spec = PXR_NS::SdfCreatePrimInLayer(layer, prefix);
            if (!spec) {
                return PXR_NS::SdfLayerRefPtr();
            }
            spec->SetSpecifier(PXR_NS::SdfSpecifierDef);
            const std::string& typeName = (prefix == destination) ? destinationType : ancestorType;
            if (!typeName.empty()) {
                spec->SetTypeName(typeName);
            }
        }
        layer->SetPermissionToEdit(false);

        return layer;
    }

    // Defines destination/<child name> for each of slot's roots, referencing the root in slot's
    // wrapper. Everything above the children is an "over": the structure layer defines it, and an
    // over sent or withdrawn with this layer changes no prim's definition, so swapping this layer
    // resyncs only the grafted roots.
    PXR_NS::SdfLayerRefPtr
    authorGraftLayer(const GraftSlot& slot,
                     const PXR_NS::SdfPath& destination,
                     const std::string& tag)
    {
        PXR_NS::SdfLayerRefPtr layer = createNodeLayer(tag + "-graft");

        if (!layer) {
            return layer;
        }
        // Graft layers sit in the consumer's stack, which scales each one's time samples by its
        // timeCodesPerSecond against the stage's; carrying the input's keeps the wrapper's samples
        // at the rate they were authored for.
        carryAuthoredMetadata(slot.input, layer);
        const std::string& wrapperIdentifier = slot.wrapper->GetIdentifier();
        for (std::size_t i = 0; i < slot.rootNames.size(); ++i) {
            const PXR_NS::SdfPrimSpecHandle child = PXR_NS::SdfCreatePrimInLayer(layer, destination.AppendChild(PXR_NS::TfToken(slot.childNames[i])));
            if (!child) {
                return PXR_NS::SdfLayerRefPtr();
            }
            child->SetSpecifier(PXR_NS::SdfSpecifierDef);
            child->GetReferenceList().Prepend(PXR_NS::SdfReference(wrapperIdentifier, PXR_NS::SdfPath::AbsoluteRootPath().AppendChild(PXR_NS::TfToken(slot.rootNames[i]))));
        }
        // The reference drops the targets that leave the root; an explicit list here, stronger
        // than the reference, stands in for its whole list.
        for (const InputLink& link : slot.links) {
            const PXR_NS::SdfPath property = graftedPathOf(slot, destination, link.property);
            const PXR_NS::SdfPrimSpecHandle owner = property.IsEmpty() ? PXR_NS::SdfPrimSpecHandle() : PXR_NS::SdfCreatePrimInLayer(layer, property.GetPrimPath());
            if (!owner) {
                return PXR_NS::SdfLayerRefPtr();
            }
            std::vector<PXR_NS::SdfPath> targets;
            for (const PXR_NS::SdfPath& target : link.targets) {
                const PXR_NS::SdfPath grafted = graftedPathOf(slot, destination, target);
                if (!grafted.IsEmpty()) {
                    targets.push_back(grafted);
                }
            }
            if (link.isConnection) {
                if (!PXR_NS::SdfAttributeSpec::New(owner, property.GetName(), link.typeName, link.variability, link.custom)) {
                    return PXR_NS::SdfLayerRefPtr();
                }
                layer->SetField(property, PXR_NS::SdfFieldKeys->ConnectionPaths, PXR_NS::SdfPathListOp::CreateExplicit(targets));
            } else {
                if (!PXR_NS::SdfRelationshipSpec::New(owner, property.GetName(), link.custom)) {
                    return PXR_NS::SdfLayerRefPtr();
                }
                layer->SetField(property, PXR_NS::SdfFieldKeys->TargetPaths, PXR_NS::SdfPathListOp::CreateExplicit(targets));
            }
        }
        layer->SetPermissionToEdit(false);

        return layer;
    }

    // Whether layer authors nothing but "over" prim specs.
    bool
    authorsOnlyOvers(const PXR_NS::SdfLayerRefPtr& layer)
    {
        bool onlyOvers = true;

        layer->Traverse(PXR_NS::SdfPath::AbsoluteRootPath(), [&layer, &onlyOvers](const PXR_NS::SdfPath& path) {
            if (onlyOvers && path.IsPrimPath()) {
                const PXR_NS::SdfPrimSpecHandle spec = layer->GetPrimAtPath(path);
                onlyOvers = !spec || (spec->GetSpecifier() == PXR_NS::SdfSpecifierOver);
            }
        });

        return onlyOvers;
    }

    // The first slot not yet consumed that matches, which it marks consumed, so two inputs
    // connected to the same stack each keep a slot of their own.
    const GraftSlot*
    findSlot(const std::vector<GraftSlot>& slots,
             std::vector<bool>* consumed,
             const ScenePayload& input,
             const std::string* destinationPath,
             const std::vector<std::string>* childNames)
    {
        for (std::size_t i = 0; i < slots.size(); ++i) {
            const GraftSlot& slot = slots[i];
            if ((*consumed)[i] || !slot.input.isSameStack(input)) {
                continue;
            }
            if (destinationPath && ((slot.destinationPath != *destinationPath) || !slot.graft.layer)) {
                continue;
            }
            if (childNames && (slot.childNames != *childNames)) {
                continue;
            }
            (*consumed)[i] = true;

            return &slot;
        }

        return NULL;
    }
} // anonymous namespace

bool
reparentRoots(const SceneReparentArgs& args,
              SceneGraftMemo* memo,
              ScenePayload* output,
              std::vector<SceneRootGraft>* grafts,
              std::vector<SceneUnmappedLink>* unmappedLinks,
              std::string* errorMessage)
{
    if (!memo || !output) {
        return false;
    }
    if (!checkUsdRuntime(errorMessage)) {
        return false;
    }
    if (!PXR_NS::SdfPath::IsValidPathString(args.destinationPath)) {
        if (errorMessage) {
            *errorMessage = "\"" + args.destinationPath + "\" is not a valid prim path.";
        }

        return false;
    }
    const PXR_NS::SdfPath destination(args.destinationPath);
    if (!destination.IsAbsolutePath() || !destination.IsPrimPath()) {
        if (errorMessage) {
            *errorMessage = "\"" + args.destinationPath + "\" is not an absolute prim path.";
        }

        return false;
    }

    // Reading the inputs' stages and layers: no layer reload may run meanwhile. Taken before the
    // memo's mutex, so a thread waiting on the mutex holds no write off.
    SceneWriteBarrier::ReadScope read;
    SceneGraftMemo::State& state = memo->getState();
    std::lock_guard<std::mutex> lock(state.mutex);

    // An input seen last time keeps its root names and wrapper: the same stack composes the same.
    std::vector<GraftSlot> slots(args.inputs.size());
    std::vector<bool> readConsumed(state.slots.size(), false);
    for (std::size_t i = 0; i < args.inputs.size(); ++i) {
        GraftSlot& slot = slots[i];
        slot.input = args.inputs[i];
        slot.destinationPath = args.destinationPath;
        if (const GraftSlot* previous = findSlot(state.slots, &readConsumed, slot.input, NULL, NULL)) {
            slot.rootNames = previous->rootNames;
            slot.links = previous->links;
            slot.wrapper = previous->wrapper;
        } else if (!readInput(slot.input, &slot.rootNames, &slot.links, errorMessage)) {
            return false;
        }
        if (destination.IsRootPrimPath()) {
            for (const std::string& root : slot.rootNames) {
                if (root == destination.GetName()) {
                    if (errorMessage) {
                        *errorMessage = "Input " + std::to_string(i + 1) + " has a root prim at " + args.destinationPath + ", the path its roots are grafted under: it would be grafted inside itself.";
                    }

                    return false;
                }
            }
        }
    }

    // Names in input order: with autoRename, a root whose name an earlier input took gets the
    // first "_<n>" suffix free of every name taken so far and of its own input's root names.
    std::set<std::string> taken;
    for (GraftSlot& slot : slots) {
        const std::set<std::string> own(slot.rootNames.begin(), slot.rootNames.end());
        for (const std::string& root : slot.rootNames) {
            std::string name = root;
            if (args.autoRename && taken.count(name)) {
                for (U64 n = 1;; ++n) {
                    const std::string candidate = root + "_" + std::to_string(n);
                    if (!taken.count(candidate) && !own.count(candidate)) {
                        name = candidate;
                        break;
                    }
                }
            }
            slot.childNames.push_back(name);
            taken.insert(name);
        }
    }

    SceneLayerEntry structure = state.structure;
    const bool structureChanged = !structure.layer || (state.structurePath != args.destinationPath) || (state.structureDestinationType != args.destinationType) || (state.structureAncestorType != args.ancestorType);
    if (structureChanged) {
        structure = SceneLayerEntry();
        structure.layer = authorStructureLayer(destination, args.destinationType, args.ancestorType, args.layerTag);
        if (!structure.layer) {
            if (errorMessage) {
                *errorMessage = "Could not author the prim " + args.destinationPath + ".";
            }

            return false;
        }
        ContentHasher h;
        h.appendString("structure");
        h.appendString(args.destinationPath);
        h.appendString(args.destinationType);
        h.appendString(args.ancestorType);
        structure.hash = h.finalize();
    }

    U64 graftLayersAuthored = 0;
    std::vector<bool> graftConsumed(state.slots.size(), false);
    for (GraftSlot& slot : slots) {
        if (slot.rootNames.empty()) {
            continue;
        }
        if (const GraftSlot* previous = findSlot(state.slots, &graftConsumed, slot.input, &slot.destinationPath, &slot.childNames)) {
            slot.wrapper = previous->wrapper;
            slot.graft = previous->graft;
            continue;
        }
        if (!slot.wrapper) {
            slot.wrapper = createWrapper(slot.input, args.layerTag);
        }
        PXR_NS::SdfLayerRefPtr layer = slot.wrapper ? authorGraftLayer(slot, destination, args.layerTag) : PXR_NS::SdfLayerRefPtr();
        if (!layer) {
            if (errorMessage) {
                *errorMessage = "Could not author the grafts under " + args.destinationPath + ".";
            }

            return false;
        }
        ++graftLayersAuthored;

        std::shared_ptr<SceneLayerDependencies> dependencies = std::make_shared<SceneLayerDependencies>();
        dependencies->layers.push_back(slot.wrapper);
        dependencies->stacks.push_back(slot.input);

        ContentHasher h;
        h.appendString("graft");
        h.appendU64(slot.input.getStackHash());
        h.appendString(slot.destinationPath);
        for (std::size_t i = 0; i < slot.rootNames.size(); ++i) {
            h.appendString(slot.rootNames[i]);
            h.appendString(slot.childNames[i]);
        }
        slot.graft.layer = layer;
        slot.graft.hash = h.finalize();
        slot.graft.dependencies = dependencies;
    }

    // Last, as the overrides memo commits what it authors: everything that can fail before it has.
    SceneLayerEntry overrides;
    if (args.authorOverrides) {
        SceneLayerMemo::Key key = args.overridesKey;
        key.upstreamRead = SceneLayerMemo::Key::eUpstreamReadNothing;
        const SceneLayerMemo::AuthorFunc& author = args.authorOverrides;
        const SceneLayerMemo::AuthorFunc checkedAuthor = [&author, &args](const SceneLayerEdit& edit,
                                                                          std::string* authorError) {
            if (!author(edit, authorError)) {
                return false;
            }
            // A def or class here would make every swap of this layer resync the whole graft.
            if (!authorsOnlyOvers(edit.layer)) {
                if (authorError) {
                    *authorError = "The overrides of " + args.destinationPath + " may only author \"over\" prims.";
                }

                return false;
            }

            return true;
        };
        ScenePayload produced;
        if (!state.overrides.produce(key, ScenePayload(), args.layerTag + "-overrides", checkedAuthor, &produced, errorMessage)) {
            return false;
        }
        overrides = produced.getImpl()->top;
    }

    // A payload of its own: the inputs are reached only through the graft layers' references.
    ScenePayload result = appendEntry(ScenePayload(), structure);
    if (overrides.layer) {
        result = appendEntry(result, overrides);
    }
    for (const GraftSlot& slot : slots) {
        if (slot.graft.layer) {
            result = appendEntry(result, slot.graft);
        }
    }
    if (result.isSameStack(state.output)) {
        result = state.output;
    }

    state.structurePath = args.destinationPath;
    state.structureDestinationType = args.destinationType;
    state.structureAncestorType = args.ancestorType;
    state.structure = structure;
    state.slots.swap(slots);
    state.output = result;
    state.graftLayerAuthorCount += graftLayersAuthored;

    if (grafts) {
        grafts->clear();
        for (std::size_t i = 0; i < state.slots.size(); ++i) {
            const GraftSlot& slot = state.slots[i];
            for (std::size_t j = 0; j < slot.rootNames.size(); ++j) {
                SceneRootGraft graft;
                graft.inputIndex = i;
                graft.sourcePath = "/" + slot.rootNames[j];
                graft.graftedPath = destination.AppendChild(PXR_NS::TfToken(slot.childNames[j])).GetString();
                grafts->push_back(graft);
            }
        }
    }
    if (unmappedLinks) {
        unmappedLinks->clear();
        for (std::size_t i = 0; i < state.slots.size(); ++i) {
            const GraftSlot& slot = state.slots[i];
            for (const InputLink& link : slot.links) {
                for (const PXR_NS::SdfPath& target : link.targets) {
                    if (graftedPathOf(slot, destination, target).IsEmpty()) {
                        SceneUnmappedLink unmapped;
                        unmapped.inputIndex = i;
                        unmapped.propertyPath = graftedPathOf(slot, destination, link.property).GetString();
                        unmapped.targetPath = target.GetString();
                        unmappedLinks->push_back(unmapped);
                    }
                }
            }
        }
    }
    *output = result;

    return true;
} // reparentRoots

} // namespace SceneOps

struct SceneLayerMemo::Implementation {
    std::mutex mutex;
    bool valid = false;
    U64 layerHash = 0;
    ScenePayload output;
    U64 authorCount = 0;
};

SceneLayerMemo::SceneLayerMemo()
    : _imp(new Implementation)
{
}

SceneLayerMemo::~SceneLayerMemo()
{
}

bool
SceneLayerMemo::produce(const Key& key,
                        const ScenePayload& upstream,
                        const std::string& layerTag,
                        const AuthorFunc& author,
                        ScenePayload* output,
                        std::string* errorMessage)
{
    ContentHasher h;
    h.appendU64(key.nodeDigest);
    h.appendI64((std::int64_t)key.upstreamRead);
    switch (key.upstreamRead) {
    case Key::eUpstreamReadWholeStack:
        h.appendU64(upstream.getStackHash());
        break;
    case Key::eUpstreamReadFacts:
        h.appendU64(key.factsDigest);
        break;
    case Key::eUpstreamReadNothing:
        break;
    }
    h.appendBool(key.readsFrameRange);
    if (key.readsFrameRange) {
        h.appendDouble(key.firstFrame);
        h.appendDouble(key.lastFrame);
    }
    // Always: time codes are authored per second of the project rate.
    h.appendDouble(key.framesPerSecond);
    h.appendI64((std::int64_t)key.upAxis);
    h.appendDouble(key.metersPerUnit);
    const U64 layerHash = h.finalize();

    if (!SceneOps::checkUsdRuntime(errorMessage)) {
        return false;
    }

    // Authoring reads the upstream layers and opens files: no layer reload may run meanwhile.
    // Taken before the memo's mutex, so a thread waiting on the mutex holds no write off.
    SceneWriteBarrier::ReadScope read;
    // Held while authoring: two threads evaluating the same node author one layer, not two.
    std::lock_guard<std::mutex> lock(_imp->mutex);
    if (_imp->valid && (_imp->layerHash == layerHash)) {
        // An upstream re-authored to the same content hashes the same but holds other layers:
        // the memoized layer goes on top of the stack actually handed in.
        const ScenePayload memoizedBase(_imp->output.getImpl()->base);
        if (!memoizedBase.isSameStack(upstream)) {
            _imp->output = SceneOps::appendEntry(upstream, _imp->output.getImpl()->top);
        }
        *output = _imp->output;

        return true;
    }

    SceneLayerEdit edit;
    edit.layer = SceneOps::createNodeLayer(layerTag);
    // A node that reads nothing upstream is not handed it, so it cannot read what its key misses.
    if (key.upstreamRead != Key::eUpstreamReadNothing) {
        edit.upstream = upstream;
    }
    edit.firstFrame = key.firstFrame;
    edit.lastFrame = key.lastFrame;
    edit.framesPerSecond = key.framesPerSecond;
    edit.upAxis = key.upAxis;
    edit.metersPerUnit = key.metersPerUnit;
    SceneLinkIndex sublayerLinks;
    edit.sublayerLinks = &sublayerLinks;
    if (!edit.layer) {
        if (errorMessage) {
            *errorMessage = "Could not create a layer for " + layerTag;
        }

        return false;
    }

    std::string authorError;
    if (!author || !author(edit, &authorError)) {
        if (errorMessage) {
            *errorMessage = authorError.empty() ? ("Could not author the layer of " + layerTag) : authorError;
        }

        return false;
    }
    ++_imp->authorCount;
    edit.layer->SetPermissionToEdit(false);

    SceneLayerEntry entry;
    entry.layer = edit.layer;
    entry.hash = layerHash;
    SceneLinkIndex links = SceneOps::findLayerLinks(edit.layer);
    links.insert(links.end(), sublayerLinks.begin(), sublayerLinks.end());
    entry.links = shareLinks(std::move(links));
    _imp->output = SceneOps::appendEntry(upstream, entry);
    _imp->layerHash = layerHash;
    _imp->valid = true;
    *output = _imp->output;

    return true;
} // SceneLayerMemo::produce

void
SceneLayerMemo::invalidate()
{
    std::lock_guard<std::mutex> lock(_imp->mutex);

    _imp->valid = false;
    _imp->output = ScenePayload();
}

U64
SceneLayerMemo::getAuthorCount() const
{
    std::lock_guard<std::mutex> lock(_imp->mutex);

    return _imp->authorCount;
}

SceneGraftMemo::SceneGraftMemo()
    : _state(new State)
{
}

SceneGraftMemo::~SceneGraftMemo()
{
}

void
SceneGraftMemo::invalidate()
{
    std::lock_guard<std::mutex> lock(_state->mutex);

    _state->structure = SceneLayerEntry();
    _state->structurePath.clear();
    _state->overrides.invalidate();
    _state->slots.clear();
    _state->output = ScenePayload();
}

U64
SceneGraftMemo::getGraftLayerAuthorCount() const
{
    std::lock_guard<std::mutex> lock(_state->mutex);

    return _state->graftLayerAuthorCount;
}

SceneGraftMemo::State&
SceneGraftMemo::getState()
{
    return *_state;
}

NATRON_NAMESPACE_EXIT
