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

#include "GeoToUSD.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QString>

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/ContentHasher.h"
#include "Engine/Geo/GeoGroupOps.h"
#include "Engine/Geo/GeoOpUtils.h"
#include "Engine/GeoDetail.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Geometry/GeoSdfData.h"
#include "Engine/Nodes/Geometry/GeoSdfFileFormat.h"
#include "Engine/Nodes/Geometry/GeoUsdConvert.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/ScenePayloadImpl.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/SceneWriteBarrier.h"
#include "Engine/Project.h"
#include "Engine/ScenePayload.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

namespace {

// Time-keyed layers kept open per node: scrubbing back to a recent frame finds its layer as left.
const std::size_t kMaxTimeKeyedLayers = 8;

std::atomic<U64> nextInstanceId { 1 };

std::mutex frameGatheredHookMutex;
std::function<void(double)> frameGatheredHookStorage;

std::function<void(double)>
frameGatheredHook()
{
    std::lock_guard<std::mutex> lock(frameGatheredHookMutex);

    return frameGatheredHookStorage;
}

const char*
packedModeArgument(GeoUsdPackedModeEnum mode)
{
    switch (mode) {
    case eGeoUsdPackedNativeInstances:
        return "native";
    case eGeoUsdPackedUnpack:
        return "unpack";
    case eGeoUsdPackedCreateXforms:
        return "xforms";
    case eGeoUsdPackedPointInstancer:
    default:
        return "instancer";
    }
}

std::string
hexKey(U64 value)
{
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%016llx", (unsigned long long)value);

    return std::string(buffer);
}

// The view a layer is keyed on, or -1 for one layer shared by every view: upstream nodes can make
// different geometry per view only when the project has several.
int
layerView(const EffectInstance& node,
          ViewIdx view)
{
    const AppInstancePtr app = node.getApp();
    const bool several = app && app->getProject() && (app->getProject()->getProjectViewsCount() > 1);

    return several ? int(view) : -1;
}

// The frame's bit pattern, so that subframes that print alike still get layers of their own.
std::string
frameKey(double frame)
{
    // -0 and 0 are one frame with two bit patterns.
    if (frame == 0.) {
        frame = 0.;
    }
    U64 bits = 0;
    static_assert(sizeof(bits) == sizeof(frame), "a frame is a 64-bit double");
    std::memcpy(&bits, &frame, sizeof(bits));

    return hexKey(bits);
}

bool
isBlank(const std::string& text)
{
    return text.find_first_not_of(" \t\r\n") == std::string::npos;
}

constexpr GeoGroupTypeEnum kGroupTypes[] = { eGeoGroupTypePoints, eGeoGroupTypePrimitives, eGeoGroupTypeGuess };

bool
isEmptyDetail(const GeoDetailConstPtr& detail)
{
    return !detail || ((detail->pointCount() == 0) && (detail->primitiveCount() == 0));
}

// One detail and the input scene it is converted against, at one time.
struct Gathered {
    double frame = 0.;
    GeoDetailConstPtr detail;
    GeoUsdInputSceneConstPtr inputScene;
    // With Load As Reference: the Scene input's prims on the way to the prefix, which the
    // geometry is converted without.
    GeoUsdInputSceneConstPtr referenceScene;
    std::vector<std::string> missing;
};

struct NodeSettings {
    std::string group;
    GeoGroupTypeEnum groupType = eGeoGroupTypeGuess;
    GeoUsdConvertOptions options;
    bool copy = false;
    bool timeSamples = false;
    bool reference = false;
    GeoToUSD::ReferenceTypeEnum referenceType = GeoToUSD::eReferenceTypeWeakerPayload;
    GeoToUSD::ReferencePrimitiveEnum referencePrimitive = GeoToUSD::eReferencePrimitiveAutomatic;
    PXR_NS::SdfPath referencePrimitivePath;
    bool makeInstanceable = false;
    PXR_NS::TfToken parentPrimitiveKind;
};

U64
inputSceneHash(const GeoUsdInputSceneConstPtr& scene)
{
    return scene ? scene->hash : 0;
}

// The paths the detail names that only an input prim gives a meaning to and that prim is missing:
// packed USD sources, and, with Define, absolute paths outside the root read from USD, which move
// under it.
std::vector<std::string>
missingSources(const GeoUsdDetailPaths& paths,
               const GeoUsdConvertOptions& options,
               const GeoUsdInputSceneConstPtr& scene)
{
    std::vector<std::string> missing;
    for (const PXR_NS::SdfPath& path : paths.sourcePaths) {
        if (!scene || !scene->find(path)) {
            missing.push_back(path.GetString());
        }
    }
    if (options.otherPrimitives != eGeoUsdOtherDefine) {
        return missing;
    }
    const PXR_NS::SdfPath root = (options.rootPath.IsAbsolutePath() && options.rootPath.IsPrimPath()) ? options.rootPath : PXR_NS::SdfPath("/geo");
    for (std::size_t i = 0; i < paths.paths.size(); ++i) {
        GeoUsdPlacementEnum placement = eGeoUsdPlacedAsRequested;
        geoUsdPlacedPath(paths.paths[i], root, scene.get(), paths.pathsFromUsd[i] != 0, &placement);
        if (placement == eGeoUsdPlacedSourceGone) {
            missing.push_back(paths.paths[i].GetString());
        }
    }

    return missing;
}
} // anonymous namespace

struct GeoToUSD::Implementation {
    // The detail a natrongeo key serves, shared with the provider registered for it.
    struct Slot {
        std::mutex mutex;
        GeoDetailConstPtr detail;
    };

    struct Entry {
        std::shared_ptr<Slot> slot;
        std::string identifier;
        PXR_NS::SdfLayerRefPtr layer;
        // What the open layer last converted.
        U64 detailHash = 0;
        U64 sceneHash = 0;
        bool timeKeyed = false;
        U64 lastUse = 0;
    };

    const std::string instanceKey;
    std::mutex stateMutex;
    std::map<std::string, Entry> entries;
    U64 useCount = 0;
    // Held across a refresh, never inside a ReadScope, so a reader never waits on it.
    std::mutex refreshMutex;

    Implementation()
        : instanceKey("geotousd" + std::to_string(nextInstanceId.fetch_add(1)))
    {
    }

    ~Implementation()
    {
        for (const std::pair<const std::string, Entry>& entry : entries) {
            GeoLayers::removeProvider(entry.first);
        }
    }

    bool readSettings(GeoToUSD* node,
                      NodeSettings* settings,
                      std::string* errorMessage) const;

    /**
     * The part of the detail at frame the group selects and the Scene input captured where the detail names prims. inputScene,
     * when given, is the Scene input's payload, evaluated by a caller already reading scenes;
     * otherwise the input is evaluated here on inputContext, under a ReadScope released on
     * return. The capture loads inputContext's payloads and those on the way to the named
     * prims. Any thread of the render cancel was built for.
     **/
    RenderRoIRetCode gather(GeoToUSD* node,
                            double frame,
                            ViewIdx view,
                            const ScenePayload* inputScene,
                            const SceneEvaluationContext& inputContext,
                            const NodeSettings& settings,
                            RenderCancellation& cancel,
                            Gathered* out,
                            std::string* errorMessage) const;

    // The natrongeo key: the node, its conversion options and whether it is referenced, the
    // frame when timeKeyed, and the view unless it is negative (layerView()).
    std::string layerKey(const NodeSettings& settings,
                         bool timeKeyed,
                         double frame,
                         int view) const;

    /**
     * Serves gathered as the natrongeo layer under key and sets *layer to it. *current is false
     * when the layer was open already and still converts something older: it is brought up to
     * date only when mayRefresh, which a caller holding a ReadScope must not pass.
     **/
    bool serve(const std::string& key,
               const GeoUsdConvertOptions& options,
               bool timeKeyed,
               const Gathered& gathered,
               bool mayRefresh,
               PXR_NS::SdfLayerRefPtr* layer,
               bool* current,
               std::string* errorMessage);

    // Drops the least recently used time-keyed entries beyond the limit. stateMutex held.
    void prune()
    {
        std::vector<std::pair<U64, std::string>> timeKeyed;
        for (const std::pair<const std::string, Entry>& entry : entries) {
            if (entry.second.timeKeyed) {
                timeKeyed.emplace_back(entry.second.lastUse, entry.first);
            }
        }
        if (timeKeyed.size() <= kMaxTimeKeyedLayers) {
            return;
        }
        std::sort(timeKeyed.begin(), timeKeyed.end());
        for (std::size_t i = 0; i + kMaxTimeKeyedLayers < timeKeyed.size(); ++i) {
            GeoLayers::removeProvider(timeKeyed[i].second);
            entries.erase(timeKeyed[i].second);
        }
    }
};

GeoToUSD::GeoToUSD(NodePtr node)
    : NativeEffectBase(node)
    , _imp(new Implementation)
{
    setPreparesSceneSources(true);
}

GeoToUSD::~GeoToUSD()
{
    setPreparesSceneSources(false);
}

const NodeSpec*
GeoToUSD::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
GeoToUSD::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls").toStdString());

    KnobStringPtr group = createKnob<KnobString>(tr("Group"));
    group->setName(kGeoToUSDParamGroup);
    group->setHintToolTip(tr("The part of the geometry to put on the scene: element numbers and ranges (0-10, 0-20:2), "
                             "group names or globs, * for everything, @attribute comparisons (@P.x<0), and the prefixes ! "
                             "(everything the term does not select) and ^ (remove the term's elements). Blank means all "
                             "of it. Prims keep the paths they have without a group.")
                              .toStdString());
    group->setAnimationEnabled(false);
    page->addKnob(group);
    _group = group;

    KnobChoicePtr groupType = createKnob<KnobChoice>(tr("Group Type"));
    groupType->setName(kGeoToUSDParamGroupType);
    groupType->setHintToolTip(tr("Whether the group pattern selects points or primitives.").toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("points", tr("Points").toStdString(),
                                       tr("The primitives whose points are all selected, and the selected points no "
                                          "primitive uses.")
                                           .toStdString()));
        options.push_back(ChoiceOption("primitives", tr("Primitives").toStdString(),
                                       tr("The selected primitives and the points they use.").toStdString()));
        options.push_back(ChoiceOption("guess", tr("Guess from Group").toStdString(),
                                       tr("The class the group pattern names: a group name means the class that has it, "
                                          "and plain numbers mean primitives.")
                                           .toStdString()));
        groupType->populateChoices(options);
    }
    groupType->setDefaultValue(2);
    groupType->setAnimationEnabled(false);
    page->addKnob(groupType);
    _groupType = groupType;

    KnobStringPtr pathAttributes = createKnob<KnobString>(tr("Path Attributes"));
    pathAttributes->setName(kGeoToUSDParamPathAttributes);
    pathAttributes->setHintToolTip(tr("The string attributes that name the prim each primitive becomes, separated by "
                                      "commas or spaces. A primitive takes the first one with a value; loose points "
                                      "read the point attributes of these names. Characters a prim name cannot hold "
                                      "become _.")
                                       .toStdString());
    pathAttributes->setDefaultValue("path,name");
    pathAttributes->setAnimationEnabled(false);
    page->addKnob(pathAttributes);
    _pathAttributes = pathAttributes;

    KnobStringPtr pathPrefix = createKnob<KnobString>(tr("Path Prefix"));
    pathPrefix->setName(kGeoToUSDParamPathPrefix);
    pathPrefix->setHintToolTip(tr("The prim relative paths are resolved against, and that geometry without a path "
                                  "goes under as mesh_0, curves_0, points_0 and so on. With Load As Reference, the "
                                  "prim that holds the reference. Defaults to the node's name, and follows a "
                                  "rename until it is edited.")
                                   .toStdString());
    pathPrefix->setDefaultValue(getSceneNodeDefaultPrimPath(this, PLUGINID_NATRON_GEOTOUSD));
    pathPrefix->setAnimationEnabled(false);
    page->addKnob(pathPrefix);
    _pathPrefix = pathPrefix;

    KnobBoolPtr prefixAbsolute = createKnob<KnobBool>(tr("Prefix Absolute Paths"));
    prefixAbsolute->setName(kGeoToUSDParamPrefixAbsolutePaths);
    prefixAbsolute->setHintToolTip(tr("On, absolute paths go under Path Prefix as well: /a/b becomes <prefix>/a/b. "
                                      "Off, they stay where they say.")
                                       .toStdString());
    prefixAbsolute->setDefaultValue(false);
    prefixAbsolute->setAnimationEnabled(false);
    page->addKnob(prefixAbsolute);
    _prefixAbsolutePaths = prefixAbsolute;

    KnobChoicePtr otherPrimitives = createKnob<KnobChoice>(tr("Other Primitives"));
    otherPrimitives->setName(kGeoToUSDParamOtherPrimitives);
    otherPrimitives->setHintToolTip(tr("What meshes, curves and points become. Packed geometry is always defined.").toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("define", tr("Define").toStdString(),
                                       tr("A def for each prim: on a path the Scene input holds, it redefines the "
                                          "prim with this geometry's type and properties.")
                                           .toStdString()));
        options.push_back(ChoiceOption("overlay", tr("Overlay").toStdString(),
                                       tr("An over for each prim, with the full geometry. It only edits a prim the "
                                          "Scene input holds; an over with nothing below it is not drawn.")
                                           .toStdString()));
        options.push_back(ChoiceOption("overlayTransforms", tr("Overlay Transforms").toStdString(),
                                       tr("An over for each prim that holds only its transform. Meshes, curves and "
                                          "points have none of their own, so this keeps the Scene input's transform.")
                                           .toStdString()));
        otherPrimitives->populateChoices(options);
    }
    otherPrimitives->setDefaultValue(0);
    otherPrimitives->setAnimationEnabled(false);
    page->addKnob(otherPrimitives);
    _otherPrimitives = otherPrimitives;

    KnobBoolPtr leafOnly = createKnob<KnobBool>(tr("Define Only Leaf Primitives"));
    leafOnly->setName(kGeoToUSDParamDefineOnlyLeafPrimitives);
    leafOnly->setHintToolTip(tr("On, the prims above the geometry's own prims are overs, so they define nothing the "
                                "Scene input does not already hold. Off, the ones the Scene input lacks are defined "
                                "with Parent Primitive Type.")
                                 .toStdString());
    leafOnly->setDefaultValue(false);
    leafOnly->setAnimationEnabled(false);
    page->addKnob(leafOnly);
    _defineOnlyLeafPrimitives = leafOnly;

    KnobChoicePtr packedUsd = createKnob<KnobChoice>(tr("Packed USD Primitives"));
    packedUsd->setName(kGeoToUSDParamPackedUsdPrimitives);
    packedUsd->setHintToolTip(tr("What packed USD primitives, which stand for a prim USDToGeo read without its "
                                 "geometry, become.")
                                  .toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("ignore", tr("Ignore").toStdString(),
                                       tr("Nothing: the prims they stand for keep their transforms.").toStdString()));
        options.push_back(ChoiceOption("overlayTransforms", tr("Overlay Transforms").toStdString(),
                                       tr("An over of the prim each one stands for, holding its transform.").toStdString()));
        options.push_back(ChoiceOption("overlayTransformsAndAttributes", tr("Overlay Transforms And Attributes").toStdString(),
                                       tr("An over of the prim each one stands for, holding its transform and its "
                                          "primitive and point attributes as constant primvars.")
                                           .toStdString()));
        packedUsd->populateChoices(options);
    }
    packedUsd->setDefaultValue(1);
    packedUsd->setAnimationEnabled(false);
    page->addKnob(packedUsd);
    _packedUsdPrimitives = packedUsd;

    KnobChoicePtr packed = createKnob<KnobChoice>(tr("Packed Primitives"));
    packed->setName(kGeoToUSDParamPackedPrimitives);
    packed->setHintToolTip(tr("What packed geometry primitives become.").toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("xforms", tr("Create Xforms").toStdString(),
                                       tr("One Xform per packed primitive, holding its transform and a copy of its "
                                          "geometry.")
                                           .toStdString()));
        options.push_back(ChoiceOption("native", tr("Native Instances").toStdString(),
                                       tr("One instanceable prim per packed primitive, referencing a shared prototype.").toStdString()));
        options.push_back(ChoiceOption("instancer", tr("Point Instancer").toStdString(),
                                       tr("One point instancer per path, with one prototype per embedded geometry.").toStdString()));
        options.push_back(ChoiceOption("unpack", tr("Unpack").toStdString(),
                                       tr("The embedded geometry, copied and transformed per packed primitive.").toStdString()));
        packed->populateChoices(options);
    }
    packed->setDefaultValue((int)ePackedPrimitivesPointInstancer);
    packed->setAnimationEnabled(false);
    page->addKnob(packed);
    _packedPrimitives = packed;

    KnobChoicePtr parentType = createKnob<KnobChoice>(tr("Parent Primitive Type"));
    parentType->setName(kGeoToUSDParamParentPrimitiveType);
    parentType->setHintToolTip(tr("The type of the prims defined above the geometry's own prims where the Scene "
                                  "input has none, Path Prefix included.")
                                   .toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("Xform", tr("Xform").toStdString(), std::string()));
        options.push_back(ChoiceOption("Scope", tr("Scope").toStdString(), std::string()));
        options.push_back(ChoiceOption("none", tr("None").toStdString(), tr("Typeless prims.").toStdString()));
        parentType->populateChoices(options);
    }
    parentType->setDefaultValue(0);
    parentType->setAnimationEnabled(false);
    page->addKnob(parentType);
    _parentPrimitiveType = parentType;

    KnobChoicePtr kinds = createKnob<KnobChoice>(tr("Kind Authoring"));
    kinds->setName(kGeoToUSDParamKindAuthoring);
    kinds->setHintToolTip(tr("The model kinds given to the prims defined above the geometry's own prims, so that "
                             "the scene's model hierarchy reaches them. Prims the Scene input holds keep theirs.")
                              .toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("component", tr("One Component").toStdString(),
                                       tr("The topmost defined prim is a component.").toStdString()));
        options.push_back(ChoiceOption("nested", tr("Nested Groups And Components").toStdString(),
                                       tr("The lowest defined prims are components, the ones above them groups.").toStdString()));
        options.push_back(ChoiceOption("assembly", tr("Nested Assembly, Groups, And Components").toStdString(),
                                       tr("As Nested Groups And Components, with the topmost prim an assembly.").toStdString()));
        options.push_back(ChoiceOption("none", tr("None").toStdString(), tr("No kinds.").toStdString()));
        kinds->populateChoices(options);
    }
    kinds->setDefaultValue(0);
    kinds->setAnimationEnabled(false);
    page->addKnob(kinds);
    _kindAuthoring = kinds;

    KnobBoolPtr adjust = createKnob<KnobBool>(tr("Adjust Transforms For Input Hierarchy"));
    adjust->setName(kGeoToUSDParamAdjustTransformsForInputHierarchy);
    adjust->setHintToolTip(tr("On, geometry keeps its world position under the Scene input's transformed prims: "
                              "their world transform is undone in the points and transforms written. Geometry "
                              "USDToGeo read in its prim's own space is written as it is. Off, the geometry is "
                              "written as it is, and moves with the prims above it.")
                               .toStdString());
    adjust->setDefaultValue(true);
    adjust->setAnimationEnabled(false);
    page->addKnob(adjust);
    _adjustTransformsForInputHierarchy = adjust;

    KnobBoolPtr asReference = createKnob<KnobBool>(tr("Load As Reference"));
    asReference->setName(kGeoToUSDParamLoadAsReference);
    asReference->setHintToolTip(tr("On, the geometry's layer is put on the Path Prefix prim as a reference or "
                                   "payload instead of a sublayer, so it can be loaded and unloaded or instanced. "
                                   "The geometry is then converted on its own, without the Scene input.")
                                    .toStdString());
    asReference->setDefaultValue(false);
    asReference->setAnimationEnabled(false);
    page->addKnob(asReference);
    _loadAsReference = asReference;

    KnobChoicePtr referenceType = createKnob<KnobChoice>(tr("Reference Type"));
    referenceType->setName(kGeoToUSDParamReferenceType);
    referenceType->setHintToolTip(tr("The arc that brings in the geometry's layer, and whether it is weaker or "
                                     "stronger than the arcs the prim already has.")
                                      .toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("weakerPayload", tr("Weaker Payload").toStdString(), std::string()));
        options.push_back(ChoiceOption("strongerPayload", tr("Stronger Payload").toStdString(), std::string()));
        options.push_back(ChoiceOption("weakerReference", tr("Weaker Reference").toStdString(), std::string()));
        options.push_back(ChoiceOption("strongerReference", tr("Stronger Reference").toStdString(), std::string()));
        referenceType->populateChoices(options);
    }
    referenceType->setDefaultValue((int)eReferenceTypeWeakerPayload);
    referenceType->setAnimationEnabled(false);
    page->addKnob(referenceType);
    _referenceType = referenceType;

    KnobChoicePtr referencePrim = createKnob<KnobChoice>(tr("Reference Primitive"));
    referencePrim->setName(kGeoToUSDParamReferencePrimitive);
    referencePrim->setHintToolTip(tr("The prim of the geometry's layer that is referenced.").toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("automatic", tr("Automatic").toStdString(),
                                       tr("The Path Prefix prim, else the layer's first root Xform, else its first root prim.").toStdString()));
        options.push_back(ChoiceOption("default", tr("Default").toStdString(),
                                       tr("The layer's default prim. The geometry's layer has none, so this "
                                          "references nothing until one is set downstream.")
                                           .toStdString()));
        options.push_back(ChoiceOption("specific", tr("Specific").toStdString(),
                                       tr("The prim Reference Primitive Path names.").toStdString()));
        referencePrim->populateChoices(options);
    }
    referencePrim->setDefaultValue((int)eReferencePrimitiveAutomatic);
    referencePrim->setAnimationEnabled(false);
    page->addKnob(referencePrim);
    _referencePrimitive = referencePrim;

    KnobStringPtr referencePath = createKnob<KnobString>(tr("Reference Primitive Path"));
    referencePath->setName(kGeoToUSDParamReferencePrimitivePath);
    referencePath->setHintToolTip(tr("With Reference Primitive Specific: the prim of the geometry's layer to reference.").toStdString());
    referencePath->setAnimationEnabled(false);
    page->addKnob(referencePath);
    _referencePrimitivePath = referencePath;

    KnobBoolPtr instanceable = createKnob<KnobBool>(tr("Make Instanceable"));
    instanceable->setName(kGeoToUSDParamMakeInstanceable);
    instanceable->setHintToolTip(tr("Marks the Path Prefix prim instanceable, so prims referencing the same geometry "
                                    "share it.")
                                     .toStdString());
    instanceable->setDefaultValue(false);
    instanceable->setAnimationEnabled(false);
    page->addKnob(instanceable);
    _makeInstanceable = instanceable;

    KnobChoicePtr parentKind = createKnob<KnobChoice>(tr("Parent Primitive Kind"));
    parentKind->setName(kGeoToUSDParamParentPrimitiveKind);
    parentKind->setHintToolTip(tr("With Load As Reference, the kind of the prims created above the Path Prefix prim, "
                                  "where the Scene input lacks them.")
                                   .toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("none", tr("None").toStdString(), tr("No kind.").toStdString()));
        options.push_back(ChoiceOption("component", tr("Component").toStdString(), std::string()));
        options.push_back(ChoiceOption("group", tr("Group").toStdString(), std::string()));
        options.push_back(ChoiceOption("assembly", tr("Assembly").toStdString(), std::string()));
        options.push_back(ChoiceOption("subcomponent", tr("Subcomponent").toStdString(), std::string()));
        parentKind->populateChoices(options);
    }
    parentKind->setDefaultValue(0);
    parentKind->setAnimationEnabled(false);
    page->addKnob(parentKind);
    _parentPrimitiveKind = parentKind;

    KnobChoicePtr layerStyle = createKnob<KnobChoice>(tr("Layer Style"));
    layerStyle->setName(kGeoToUSDParamLayerStyle);
    layerStyle->setHintToolTip(tr("How the geometry reaches the scene when it is not loaded as a reference.").toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("sublayer", tr("Sublayer").toStdString(),
                                       tr("The geometry is served as it is, converted only where the scene reads it, "
                                          "and edits reach the scene without rebuilding it.")
                                           .toStdString()));
        options.push_back(ChoiceOption("copy", tr("Copy Into Node Layer").toStdString(),
                                       tr("The whole geometry is converted and copied into the node's layer: slower, "
                                          "and every change copies it again.")
                                           .toStdString()));
        layerStyle->populateChoices(options);
    }
    layerStyle->setDefaultValue((int)eLayerStyleSublayer);
    layerStyle->setAnimationEnabled(false);
    page->addKnob(layerStyle);
    _layerStyle = layerStyle;

    KnobDoublePtr pointWidth = createKnob<KnobDouble>(tr("Default Point Width"));
    pointWidth->setName(kGeoToUSDParamDefaultPointWidth);
    pointWidth->setHintToolTip(tr("The width of points that have no pscale attribute. USD's own fallback is 1.0, "
                                  "which draws each point a unit wide.")
                                   .toStdString());
    pointWidth->setDefaultValue(0.01);
    pointWidth->setDisplayMinimum(0.);
    pointWidth->setDisplayMaximum(1.);
    pointWidth->setAnimationEnabled(false);
    page->addKnob(pointWidth);
    _defaultPointWidth = pointWidth;

    KnobBoolPtr samples = createKnob<KnobBool>(tr("Author Time Samples"));
    samples->setName(kGeoToUSDParamAuthorTimeSamples);
    samples->setHintToolTip(tr("With Layer Style Copy Into Node Layer: evaluates the geometry at every frame of the "
                               "project range and authors each value as a time sample. Off, the layer holds the "
                               "current frame only.")
                                .toStdString());
    samples->setDefaultValue(false);
    samples->setAnimationEnabled(false);
    page->addKnob(samples);
    _authorTimeSamples = samples;

    updateKnobsEnabled();
}

void
GeoToUSD::updateKnobsEnabled()
{
    const KnobBoolPtr asReference = _loadAsReference.lock();
    const KnobChoicePtr layerStyle = _layerStyle.lock();
    const KnobChoicePtr referencePrim = _referencePrimitive.lock();
    const bool reference = asReference && asReference->getValue();
    const bool copy = !reference && layerStyle && (layerStyle->getValue() == (int)eLayerStyleCopyIntoNodeLayer);
    const bool specific = referencePrim && (referencePrim->getValue() == (int)eReferencePrimitiveSpecific);
    const auto enable = [](const auto& weak, bool enabled) {
        if (const auto knob = weak.lock()) {
            knob->setAllDimensionsEnabled(enabled);
        }
    };
    enable(_referenceType, reference);
    enable(_referencePrimitive, reference);
    enable(_referencePrimitivePath, reference && specific);
    enable(_makeInstanceable, reference);
    enable(_parentPrimitiveKind, reference);
    // A referenced layer is converted without the Scene input and served, never copied.
    enable(_layerStyle, !reference);
    enable(_adjustTransformsForInputHierarchy, !reference);
    enable(_authorTimeSamples, copy);
}

void
GeoToUSD::onKnobsLoaded()
{
    updateKnobsEnabled();
}

void
GeoToUSD::onScriptNameChanged(const std::string& /*fullyQualifiedName*/)
{
    followSceneNodeRename(_pathPrefix.lock(), getSceneNodeDefaultPrimPath(this, PLUGINID_NATRON_GEOTOUSD));
}

bool
GeoToUSD::knobChanged(KnobI* k,
                      ValueChangedReasonEnum /*reason*/,
                      ViewSpec /*view*/,
                      double /*time*/,
                      bool /*originatedFromMainThread*/)
{
    const KnobI* watched[] = { _loadAsReference.lock().get(), _layerStyle.lock().get(), _referencePrimitive.lock().get() };
    if (k && (std::find(std::begin(watched), std::end(watched), k) != std::end(watched))) {
        updateKnobsEnabled();

        return true;
    }

    return false;
}

bool
GeoToUSD::Implementation::readSettings(GeoToUSD* node,
                                       NodeSettings* settings,
                                       std::string* errorMessage) const
{
    const KnobStringPtr root = node->_pathPrefix.lock();
    const std::string rootText = root ? root->getValue() : getSceneNodeDefaultPrimPath(node, PLUGINID_NATRON_GEOTOUSD);
    const PXR_NS::SdfPath rootPath = PXR_NS::SdfPath::IsValidPathString(rootText) ? PXR_NS::SdfPath(rootText) : PXR_NS::SdfPath();
    if (rootPath.IsEmpty() || !rootPath.IsAbsolutePath() || !rootPath.IsPrimPath()) {
        *errorMessage = tr("Path Prefix \"%1\" is not an absolute prim path.").arg(QString::fromStdString(rootText)).toStdString();

        return false;
    }
    GeoUsdConvertOptions& options = settings->options;
    options.rootPath = rootPath;

    const KnobStringPtr group = node->_group.lock();
    if (group) {
        settings->group = group->getValue();
    }
    const KnobChoicePtr groupType = node->_groupType.lock();
    const int typeIndex = groupType ? groupType->getValue() : -1;
    if ((typeIndex >= 0) && (typeIndex < (int)(sizeof(kGroupTypes) / sizeof(kGroupTypes[0])))) {
        settings->groupType = kGroupTypes[typeIndex];
    }

    options.pathAttributes.clear();
    if (const KnobStringPtr names = node->_pathAttributes.lock()) {
        const std::string text = names->getValue();
        std::size_t begin = 0;
        while (begin < text.size()) {
            const std::size_t end = std::min(text.find_first_of(", \t", begin), text.size());
            if (end > begin) {
                options.pathAttributes.push_back(text.substr(begin, end - begin));
            }
            begin = end + 1;
        }
    }
    const auto boolValue = [](const KnobBoolWPtr& weak, bool fallback) {
        const KnobBoolPtr knob = weak.lock();

        return knob ? knob->getValue() : fallback;
    };
    const auto choiceValue = [](const KnobChoiceWPtr& weak, int fallback) {
        const KnobChoicePtr knob = weak.lock();

        return knob ? knob->getValue() : fallback;
    };
    options.prefixAbsolutePaths = boolValue(node->_prefixAbsolutePaths, false);
    switch (choiceValue(node->_otherPrimitives, 0)) {
    case 1:
        options.otherPrimitives = eGeoUsdOtherOverlay;
        break;
    case 2:
        options.otherPrimitives = eGeoUsdOtherOverlayTransforms;
        break;
    default:
        options.otherPrimitives = eGeoUsdOtherDefine;
        break;
    }
    options.defineOnlyLeafPrimitives = boolValue(node->_defineOnlyLeafPrimitives, false);
    switch (choiceValue(node->_packedUsdPrimitives, 1)) {
    case 0:
        options.packedUsdPrimitives = eGeoUsdPackedUsdIgnore;
        break;
    case 2:
        options.packedUsdPrimitives = eGeoUsdPackedUsdOverlayTransformsAndAttributes;
        break;
    default:
        options.packedUsdPrimitives = eGeoUsdPackedUsdOverlayTransforms;
        break;
    }
    switch (choiceValue(node->_packedPrimitives, (int)ePackedPrimitivesPointInstancer)) {
    case ePackedPrimitivesCreateXforms:
        options.packedMode = eGeoUsdPackedCreateXforms;
        break;
    case ePackedPrimitivesNativeInstances:
        options.packedMode = eGeoUsdPackedNativeInstances;
        break;
    case ePackedPrimitivesUnpack:
        options.packedMode = eGeoUsdPackedUnpack;
        break;
    case ePackedPrimitivesPointInstancer:
    default:
        options.packedMode = eGeoUsdPackedPointInstancer;
        break;
    }
    switch (choiceValue(node->_parentPrimitiveType, 0)) {
    case 1:
        options.parentPrimitiveType = PXR_NS::TfToken("Scope");
        break;
    case 2:
        options.parentPrimitiveType = PXR_NS::TfToken();
        break;
    default:
        options.parentPrimitiveType = PXR_NS::TfToken("Xform");
        break;
    }
    switch (choiceValue(node->_kindAuthoring, 0)) {
    case 1:
        options.kindAuthoring = eGeoUsdKindNestedGroupsAndComponents;
        break;
    case 2:
        options.kindAuthoring = eGeoUsdKindNestedAssemblyGroupsAndComponents;
        break;
    case 3:
        options.kindAuthoring = eGeoUsdKindNone;
        break;
    default:
        options.kindAuthoring = eGeoUsdKindOneComponent;
        break;
    }

    const KnobDoublePtr width = node->_defaultPointWidth.lock();
    if (width) {
        options.defaultPointWidth = width->getValue();
    }

    settings->reference = boolValue(node->_loadAsReference, false);
    // A referenced layer is converted on its own, so nothing above the prefix moves it.
    options.adjustTransformsForInputHierarchy = !settings->reference && boolValue(node->_adjustTransformsForInputHierarchy, true);
    settings->copy = !settings->reference && (choiceValue(node->_layerStyle, (int)eLayerStyleSublayer) == (int)eLayerStyleCopyIntoNodeLayer);
    settings->timeSamples = settings->copy && boolValue(node->_authorTimeSamples, false);
    if (settings->reference) {
        const int type = choiceValue(node->_referenceType, (int)eReferenceTypeWeakerPayload);
        settings->referenceType = ((type >= 0) && (type <= (int)eReferenceTypeStrongerReference)) ? (ReferenceTypeEnum)type : eReferenceTypeWeakerPayload;
        const int prim = choiceValue(node->_referencePrimitive, (int)eReferencePrimitiveAutomatic);
        settings->referencePrimitive = ((prim >= 0) && (prim <= (int)eReferencePrimitiveSpecific)) ? (ReferencePrimitiveEnum)prim : eReferencePrimitiveAutomatic;
        if (settings->referencePrimitive == eReferencePrimitiveSpecific) {
            const KnobStringPtr path = node->_referencePrimitivePath.lock();
            const std::string text = path ? path->getValue() : std::string();
            settings->referencePrimitivePath = PXR_NS::SdfPath::IsValidPathString(text) ? PXR_NS::SdfPath(text) : PXR_NS::SdfPath();
            if (!settings->referencePrimitivePath.IsAbsolutePath() || !settings->referencePrimitivePath.IsPrimPath()) {
                *errorMessage = tr("Reference Primitive Path \"%1\" is not an absolute prim path.").arg(QString::fromStdString(text)).toStdString();

                return false;
            }
        }
        settings->makeInstanceable = boolValue(node->_makeInstanceable, false);
        const char* const kinds[] = { "", "component", "group", "assembly", "subcomponent" };
        const int kind = choiceValue(node->_parentPrimitiveKind, 0);
        settings->parentPrimitiveKind = PXR_NS::TfToken(((kind >= 0) && (kind < 5)) ? kinds[kind] : "");
    }

    return true;
}

EffectInstance::RenderRoIRetCode
GeoToUSD::Implementation::gather(GeoToUSD* node,
                                 double frame,
                                 ViewIdx view,
                                 const ScenePayload* inputScene,
                                 const SceneEvaluationContext& inputContext,
                                 const NodeSettings& settings,
                                 RenderCancellation& cancel,
                                 Gathered* out,
                                 std::string* errorMessage) const
{
    const GeoUsdConvertOptions& options = settings.options;
    out->frame = frame;
    const EffectInstancePtr geometry = node->getInput(kGeoToUSDInputGeometry);
    if (geometry && geometry->producesGeometryData()) {
        const RenderRoIRetCode code = geometry->evaluateGeometry(EvaluateGeometryArgs(frame, view), &out->detail, errorMessage);
        if (code != eRenderRoIRetCodeOk) {
            return code;
        }
    }
    if (!out->detail) {
        out->detail = std::make_shared<const GeoDetail>();
    }
    if (cancel.check()) {
        return eRenderRoIRetCodeAborted;
    }
    if (!selectGeometry(out->detail, settings.group, settings.groupType, &out->detail, errorMessage)) {
        return eRenderRoIRetCodeFailed;
    }
    if (cancel.check()) {
        return eRenderRoIRetCodeAborted;
    }

    // A referenced layer is converted without the Scene input, which only tells what is on
    // the way to the prefix.
    const GeoUsdDetailPaths paths = settings.reference ? GeoUsdDetailPaths() : geoUsdDetailPaths(*out->detail, options);
    const EffectInstancePtr sceneInput = node->getInput(kGeoToUSDInputScene);
    if ((!settings.reference && paths.paths.empty()) || !sceneInput || !sceneInput->producesSceneData()) {
        if (!settings.reference) {
            out->missing = missingSources(paths, options, out->inputScene);
        }

        return eRenderRoIRetCodeOk;
    }

    std::optional<SceneWriteBarrier::ReadScope> read;
    ScenePayload evaluated;
    if (!inputScene) {
        read.emplace();
        const RenderRoIRetCode code = sceneInput->evaluateScene(EvaluateSceneArgs(frame, view, inputContext), &evaluated, errorMessage);
        if (code != eRenderRoIRetCodeOk) {
            return code;
        }
        inputScene = &evaluated;
    }
    if (!inputScene->isEmpty()) {
        std::vector<std::string> named;
        named.reserve(paths.paths.size() + paths.sourcePaths.size());
        for (const PXR_NS::SdfPath& path : paths.paths) {
            named.push_back(path.GetString());
        }
        for (const PXR_NS::SdfPath& path : paths.sourcePaths) {
            named.push_back(path.GetString());
        }
        if (settings.reference) {
            named.push_back(options.rootPath.GetString());
        }
        SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(*inputScene, SceneLoadSet::withPaths(inputContext.loadSet, named), errorMessage);
        const PXR_NS::UsdStageRefPtr stage = lease.getStage();
        if (!stage) {
            if (errorMessage->empty()) {
                *errorMessage = tr("USD could not compose the Scene input.").toStdString();
            }

            return eRenderRoIRetCodeFailed;
        }
        const AppInstancePtr app = node->getApp();
        const double fps = (app && app->getProject()) ? app->getProject()->getProjectFrameRate() : 24.;
        const PXR_NS::UsdTimeCode time(SceneOps::frameToTimeCode(*inputScene, frame, fps));
        if (settings.reference) {
            out->referenceScene = geoUsdCaptureInputScene(stage, time, std::vector<PXR_NS::SdfPath>(), std::vector<PXR_NS::SdfPath> { options.rootPath });
        } else {
            out->inputScene = geoUsdCaptureInputScene(stage, time, paths.paths, paths.sourcePaths);
        }
    }
    if (!settings.reference) {
        out->missing = missingSources(paths, options, out->inputScene);
    }

    return cancel.check() ? eRenderRoIRetCodeAborted : eRenderRoIRetCodeOk;
} // GeoToUSD::Implementation::gather

std::string
GeoToUSD::Implementation::layerKey(const NodeSettings& settings,
                                   bool timeKeyed,
                                   double frame,
                                   int view) const
{
    // Every option is in the key: a layer is built once with the options registered for it.
    const GeoUsdConvertOptions& options = settings.options;
    ContentHasher h;
    h.appendString(options.rootPath.GetString());
    h.appendI64((std::int64_t)options.pathAttributes.size());
    for (const std::string& name : options.pathAttributes) {
        h.appendString(name);
    }
    h.appendBool(options.prefixAbsolutePaths);
    h.appendI64((std::int64_t)options.packedMode);
    h.appendI64((std::int64_t)options.otherPrimitives);
    h.appendBool(options.defineOnlyLeafPrimitives);
    h.appendI64((std::int64_t)options.packedUsdPrimitives);
    h.appendString(options.parentPrimitiveType.GetString());
    h.appendI64((std::int64_t)options.kindAuthoring);
    h.appendBool(options.adjustTransformsForInputHierarchy);
    h.appendDouble(options.defaultPointWidth);
    h.appendBool(settings.reference);
    std::string key = instanceKey + "-" + hexKey(h.finalize());
    if (timeKeyed) {
        key += "-f" + frameKey(frame);
    }
    if (view >= 0) {
        key += "-v" + std::to_string(view);
    }

    return key;
}

bool
GeoToUSD::Implementation::serve(const std::string& key,
                                const GeoUsdConvertOptions& options,
                                bool timeKeyed,
                                const Gathered& gathered,
                                bool mayRefresh,
                                PXR_NS::SdfLayerRefPtr* layer,
                                bool* current,
                                std::string* errorMessage)
{
    const std::string identifier = GeoLayers::identifier(key, options.rootPath.GetString(), packedModeArgument(options.packedMode));
    const U64 detailHash = gathered.detail->contentHash();
    const U64 sceneHash = inputSceneHash(gathered.inputScene);
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        Entry& entry = entries[key];
        entry.timeKeyed = timeKeyed;
        entry.lastUse = ++useCount;
        if (!entry.slot) {
            entry.slot = std::make_shared<Slot>();
            GeoLayers::setConvertOptions(key, options);
            const std::weak_ptr<Slot> weakSlot = entry.slot;
            GeoLayers::setProvider(key, [weakSlot]() {
                const std::shared_ptr<Slot> slot = weakSlot.lock();
                if (!slot) {
                    return GeoDetailConstPtr();
                }
                std::lock_guard<std::mutex> slotLock(slot->mutex);

                return slot->detail;
            });
        }
        {
            std::lock_guard<std::mutex> slotLock(entry.slot->mutex);
            entry.slot->detail = gathered.detail;
        }
        const GeoUsdInputSceneConstPtr served = GeoLayers::currentInputScene(key);
        if ((!served != !gathered.inputScene) || (inputSceneHash(served) != sceneHash)) {
            GeoLayers::setInputScene(key, gathered.inputScene);
        }

        if (!entry.layer) {
            entry.identifier = identifier;
            // Open already when a payload outlived this node's entry: what it converts is unknown.
            entry.layer = PXR_NS::SdfLayer::Find(identifier);
            if (!entry.layer) {
                entry.layer = PXR_NS::SdfLayer::FindOrOpen(identifier);
                if (!entry.layer) {
                    *errorMessage = tr("USD could not open the geometry layer %1.").arg(QString::fromStdString(identifier)).toStdString();
                    entries.erase(key);
                    GeoLayers::removeProvider(key);

                    return false;
                }
                entry.detailHash = detailHash;
                entry.sceneHash = sceneHash;
                *current = true;
            } else {
                *current = false;
            }
        } else {
            *current = (entry.detailHash == detailHash) && (entry.sceneHash == sceneHash);
        }
        *layer = entry.layer;
        prune();
    }
    if (*current || !mayRefresh) {
        return true;
    }

    std::lock_guard<std::mutex> refreshLock(refreshMutex);
    bool refreshed = false;
    std::string refreshError;
    U64 servedDetailHash = 0;
    U64 servedSceneHash = 0;
    const bool ran = SceneWriteBarrier::runExclusive([&]() {
        refreshed = GeoLayers::refresh(identifier, &refreshError);
        const GeoDetailConstPtr served = GeoLayers::currentDetail(key);
        servedDetailHash = served ? served->contentHash() : 0;
        servedSceneHash = inputSceneHash(GeoLayers::currentInputScene(key));
    },
                                                     errorMessage);
    if (!ran) {
        return false;
    }
    if (!refreshed) {
        *errorMessage = refreshError;

        return false;
    }
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        std::map<std::string, Entry>::iterator found = entries.find(key);
        if (found != entries.end()) {
            found->second.detailHash = servedDetailHash;
            found->second.sceneHash = servedSceneHash;
        }
    }
    *current = (servedDetailHash == detailHash) && (servedSceneHash == sceneHash);

    return true;
} // GeoToUSD::Implementation::serve

EffectInstance::RenderRoIRetCode
GeoToUSD::prepareSceneSources(const EvaluateSceneArgs& args,
                              std::string* /*errorMessage*/)
{
    const NodePtr node = getNode();
    NodeSettings settings;
    std::string error;
    if (!node || node->isNodeDisabled() || !_imp->readSettings(this, &settings, &error) || settings.copy) {
        return eRenderRoIRetCodeOk;
    }

    Gathered gathered;
    RenderCancellation cancel(this);
    const RenderRoIRetCode code = _imp->gather(this, args.time, args.view, nullptr, getSceneInputContext_public(kGeoToUSDInputScene, args.context, args.time, args.view), settings, cancel, &gathered, &error);
    if (code == eRenderRoIRetCodeAborted) {
        return code;
    }
    // renderScene() reports what failed here, and copies a layer this could not refresh.
    if ((code != eRenderRoIRetCodeOk) || isEmptyDetail(gathered.detail)) {
        return eRenderRoIRetCodeOk;
    }
    const bool timeKeyed = isFrameVaryingOrAnimated_Recursive();
    PXR_NS::SdfLayerRefPtr layer;
    bool current = false;
    (void)_imp->serve(_imp->layerKey(settings, timeKeyed, args.time, layerView(*this, args.view)), settings.options, timeKeyed, gathered, true, &layer, &current, &error);

    return aborted() ? eRenderRoIRetCodeAborted : eRenderRoIRetCodeOk;
}

bool
GeoToUSD::selectGeometry(const GeoDetailConstPtr& detail,
                         const std::string& group,
                         GeoGroupTypeEnum groupType,
                         GeoDetailConstPtr* selected,
                         std::string* error)
{
    *selected = detail;
    if (!detail || isBlank(group)) {
        return true;
    }
    const GeoDetail& d = *detail;
    if (groupType == eGeoGroupTypeGuess) {
        const GeoGroupTypeEnum allowed[] = { eGeoGroupTypePoints, eGeoGroupTypePrimitives };
        if (!geoGuessGroupType(d, group, allowed, eGeoGroupTypePrimitives, &groupType, error)) {
            return false;
        }
    }
    if ((groupType != eGeoGroupTypePoints) && (groupType != eGeoGroupTypePrimitives)) {
        if (error) {
            *error = tr("The group selects points or primitives only.").toStdString();
        }

        return false;
    }

    GeoElementMask primitives;
    GeoElementMask points;
    if (groupType == eGeoGroupTypePrimitives) {
        primitives.assign(d.elementOffsetSize(eGeoAttribClassPrimitive), 0);
        if (!geoSelectElements(d, eGeoAttribClassPrimitive, group, &primitives, error)) {
            return false;
        }
        points = geoPromotePrimitivesToPoints(d, primitives);
    } else {
        GeoElementMask picked(d.elementOffsetSize(eGeoAttribClassPoint), 0);
        if (!geoSelectElements(d, eGeoAttribClassPoint, group, &picked, error)) {
            return false;
        }
        primitives = geoPromotePointsToPrimitives(d, picked, true);
        points = geoPromotePrimitivesToPoints(d, primitives);
        // A selected point of a primitive that is dropped would otherwise come out as a loose point.
        GeoElementMask used(d.elementOffsetSize(eGeoAttribClassPoint), 0);
        d.vertexMap().forEachLive([&](GeoIndex /*index*/, GeoOffset v) {
            const GeoOffset p = d.vertexPoint(v);
            if ((p >= 0) && (std::size_t(p) < used.size())) {
                used[std::size_t(p)] = 1;
            }
        });
        points.resize(picked.size(), 0);
        for (std::size_t p = 0; p < picked.size(); ++p) {
            if (picked[p] && !used[p]) {
                points[p] = 1;
            }
        }
    }

    const auto dropped = [](const GeoIndexMap& map, const GeoElementMask& kept) {
        std::vector<GeoOffset> offsets;
        map.forEachLive([&](GeoIndex /*index*/, GeoOffset offset) {
            if ((std::size_t(offset) >= kept.size()) || !kept[std::size_t(offset)]) {
                offsets.push_back(offset);
            }
        });

        return offsets;
    };
    const std::vector<GeoOffset> droppedPrimitives = dropped(d.primitiveMap(), primitives);
    const std::vector<GeoOffset> droppedPoints = dropped(d.pointMap(), points);
    if (droppedPrimitives.empty() && droppedPoints.empty()) {
        return true;
    }
    GeoDetailBuilder builder(d);
    if (!droppedPrimitives.empty()) {
        builder.deletePrimitives(droppedPrimitives, false);
    }
    if (!droppedPoints.empty()) {
        builder.deletePoints(droppedPoints, eGeoPointDeleteKeepReferenced);
    }
    *selected = std::make_shared<const GeoDetail>(builder.finish());

    return true;
} // GeoToUSD::selectGeometry

void
GeoToUSD::setFrameGatheredHookForTests(const std::function<void(double frame)>& hook)
{
    std::lock_guard<std::mutex> lock(frameGatheredHookMutex);
    frameGatheredHookStorage = hook;
}

StatusEnum
GeoToUSD::renderScene(const SceneRenderActionArgs& args,
                      ScenePayload* outputScene,
                      std::string* errorMessage)
{
    NodeSettings settings;
    if (!_imp->readSettings(this, &settings, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }
    const ScenePayload upstream = args.getInputScene(kGeoToUSDInputScene);

    std::vector<double> frames;
    if (settings.timeSamples) {
        const AppInstancePtr app = getApp();
        double first = args.time;
        double last = args.time;
        if (app && app->getProject()) {
            app->getProject()->getFrameRange(&first, &last);
        }
        SceneLayerEdit range;
        range.firstFrame = first;
        range.lastFrame = last;
        frames = SceneNodeAuthoring::getSampleFrames(range);
    } else {
        frames.push_back(args.time);
    }

    std::vector<Gathered> gathered(frames.size());
    std::vector<RenderRoIRetCode> codes(frames.size(), eRenderRoIRetCodeOk);
    std::vector<std::string> errors(frames.size());
    RenderCancellation cancel(this);
    std::atomic<bool> failed(false);
    // This render's evaluation holds a ReadScope that the helpers read under.
    const bool reading = SceneWriteBarrier::isReadingOnThisThread();
    const std::function<void(double)> hook = frameGatheredHook();
    const SceneEvaluationContext& inputContext = args.getInputContext(kGeoToUSDInputScene);
    const auto gatherFrame = [&](int i) {
        if (failed.load(std::memory_order_relaxed)) {
            return;
        }
        SceneWriteBarrier::SharedReadScope shared(reading);
        // The Scene input's payload is evaluated at args.time; other frames evaluate their own.
        const ScenePayload* given = (frames[i] == args.time) ? &upstream : nullptr;
        codes[i] = _imp->gather(this, frames[i], args.view, given, inputContext, settings, cancel, &gathered[i], &errors[i]);
        if (codes[i] != eRenderRoIRetCodeOk) {
            failed = true;
        } else if (hook) {
            hook(frames[i]);
        }
    };
    {
        // An upstream expression a helper evaluates needs the GIL, which a script evaluating this
        // node may hold while it waits for the helpers.
        std::optional<PythonUnlockedWhileWaiting> unlocked;
        if (frames.size() > 1) {
            unlocked.emplace();
        }
        parallelForCancellable((int)frames.size(), appPTR ? appPTR->getNCPUsAvailableForEffect() : 1, cancel, gatherFrame);
    }
    if (cancel.check()) {
        return eStatusFailed;
    }
    std::vector<std::string> missing;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        if (codes[i] != eRenderRoIRetCodeOk) {
            if (codes[i] == eRenderRoIRetCodeFailed) {
                *errorMessage = errors[i];
                if (errorMessage->empty()) {
                    *errorMessage = tr("The geometry could not be evaluated.").toStdString();
                }
                setPersistentMessage(eMessageTypeError, *errorMessage);
            }

            return eStatusFailed;
        }
    }
    for (std::size_t i = 0; i < frames.size(); ++i) {
        for (const std::string& path : gathered[i].missing) {
            if (std::find(missing.begin(), missing.end(), path) == missing.end()) {
                missing.push_back(path);
            }
        }
    }

    // After the gather: rendering the geometry can tell its nodes that it does not vary after all.
    const bool timeKeyed = isFrameVaryingOrAnimated_Recursive();
    bool copy = settings.copy;
    PXR_NS::SdfLayerRefPtr geoLayer;
    std::string identifier;
    if (!copy && !isEmptyDetail(gathered.front().detail)) {
        bool current = false;
        if (!_imp->serve(_imp->layerKey(settings, timeKeyed, args.time, layerView(*this, args.view)), settings.options, timeKeyed, gathered.front(), false, &geoLayer, &current, errorMessage)) {
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }
        if (current) {
            identifier = geoLayer->GetIdentifier();
        } else {
            // Opened by an evaluation that skipped the refresh (one called while a scene was
            // being read): this evaluation copies rather than serve a stale layer.
            copy = true;
            geoLayer = PXR_NS::SdfLayerRefPtr();
        }
    }
    // A reference needs a layer to point at: a stale served layer is replaced by a copy in an
    // anonymous layer of its own.
    const bool referenceCopy = settings.reference && copy && !isEmptyDetail(gathered.front().detail);

    std::vector<std::unique_ptr<const GeoUsdConvert>> converts;
    std::vector<std::string> sheared;
    const auto noteSheared = [&](const GeoUsdConvert& convert) {
        for (const PXR_NS::SdfPath& path : convert.shearedInstances()) {
            if (std::find(sheared.begin(), sheared.end(), path.GetString()) == sheared.end()) {
                sheared.push_back(path.GetString());
            }
        }
    };
    if (copy) {
        converts.resize(gathered.size());
        parallelForCancellable((int)gathered.size(), appPTR ? appPTR->getNCPUsAvailableForEffect() : 1, cancel, [&](int i) {
            if (isEmptyDetail(gathered[i].detail)) {
                return;
            }
            GeoUsdConvertOptions options = settings.options;
            options.inputScene = gathered[i].inputScene;
            converts[i].reset(new GeoUsdConvert(gathered[i].detail, options));
        });
        if (cancel.check()) {
            return eStatusFailed;
        }
        for (const std::unique_ptr<const GeoUsdConvert>& convert : converts) {
            if (convert) {
                noteSheared(*convert);
            }
        }
    }
    const std::shared_ptr<const GeoUsdConvert> served = geoLayer ? geoLayerConvert(geoLayer) : std::shared_ptr<const GeoUsdConvert>();
    if (served) {
        noteSheared(*served);
    }
    // The conversion the node message describes: the served one, or the copy at the current frame.
    const GeoUsdConvert* reported = served.get();
    for (std::size_t i = 0; !reported && (i < converts.size()); ++i) {
        if (converts[i] && (gathered[i].frame == args.time)) {
            reported = converts[i].get();
        }
    }
    if (!reported && !converts.empty()) {
        reported = converts.front().get();
    }
    if (referenceCopy && converts.front()) {
        geoLayer = PXR_NS::SdfLayer::CreateAnonymous("geotousd");
        if (!converts.front()->author(geoLayer, PXR_NS::UsdTimeCode::Default(), errorMessage)) {
            setPersistentMessage(eMessageTypeError, *errorMessage);

            return eStatusFailed;
        }
    }

    ContentHasher facts;
    facts.appendBool(copy);
    facts.appendString(identifier);
    facts.appendBool(settings.reference);
    if (settings.reference) {
        facts.appendI64((std::int64_t)settings.referenceType);
        facts.appendI64((std::int64_t)settings.referencePrimitive);
        facts.appendString(settings.referencePrimitivePath.GetString());
        facts.appendBool(settings.makeInstanceable);
        facts.appendString(settings.parentPrimitiveKind.GetString());
        facts.appendString(settings.options.parentPrimitiveType.GetString());
        facts.appendString(settings.options.rootPath.GetString());
        facts.appendU64(gathered.front().referenceScene ? gathered.front().referenceScene->hash : 0);
    }
    if (copy) {
        for (const Gathered& g : gathered) {
            facts.appendDouble(g.frame);
            facts.appendU64(g.detail->contentHash());
            facts.appendU64(inputSceneHash(g.inputScene));
        }
    }
    SceneLayerReads reads;
    reads.upstream = SceneLayerMemo::Key::eUpstreamReadFacts;
    reads.factsDigest = facts.finalize();
    reads.frameRange = settings.timeSamples;

    std::string referenceWarning;
    const auto authorReference = [&](const SceneLayerEdit& edit) {
        const PXR_NS::SdfPath& prefix = settings.options.rootPath;
        const GeoUsdInputScene* scene = gathered.front().referenceScene.get();
        PXR_NS::SdfPrimSpecHandle prim = PXR_NS::SdfCreatePrimInLayer(edit.layer, prefix);
        if (!prim) {
            return false;
        }
        // The prims on the way that the Scene input lacks are defined; the ones it holds stay overs.
        for (PXR_NS::SdfPath at = prefix; at != PXR_NS::SdfPath::AbsoluteRootPath(); at = at.GetParentPath()) {
            if (scene && scene->find(at)) {
                continue;
            }
            PXR_NS::SdfPrimSpecHandle spec = edit.layer->GetPrimAtPath(at);
            spec->SetSpecifier(PXR_NS::SdfSpecifierDef);
            if (at != prefix) {
                if (!settings.options.parentPrimitiveType.IsEmpty()) {
                    spec->SetTypeName(settings.options.parentPrimitiveType.GetString());
                }
                if (!settings.parentPrimitiveKind.IsEmpty()) {
                    spec->SetKind(settings.parentPrimitiveKind);
                }
            }
        }
        if (settings.makeInstanceable) {
            prim->SetInstanceable(true);
        }
        PXR_NS::SdfPath target;
        switch (settings.referencePrimitive) {
        case eReferencePrimitiveSpecific:
            target = settings.referencePrimitivePath;
            break;
        case eReferencePrimitiveDefault:
            if (!geoLayer->HasDefaultPrim()) {
                referenceWarning = tr("Reference Primitive is Default, but the geometry's layer has no default prim, so the "
                                      "reference brings nothing in.")
                                       .toStdString();
            }
            break;
        case eReferencePrimitiveAutomatic: {
            // The geometry is converted under the prefix, so the prefix prim is the branch to
            // bring in; a root prim of a nested prefix would nest it a second time below itself.
            if (geoLayer->HasDefaultPrim()) {
                break;
            }
            if (geoLayer->GetPrimAtPath(prefix)) {
                target = prefix;
                break;
            }
            for (const PXR_NS::SdfPrimSpecHandle& root : geoLayer->GetRootPrims()) {
                if (target.IsEmpty()) {
                    target = root->GetPath();
                }
                if (root->GetTypeName() == PXR_NS::TfToken("Xform")) {
                    target = root->GetPath();
                    break;
                }
            }
            break;
        }
        }
        const std::string asset = geoLayer->GetIdentifier();
        const bool stronger = (settings.referenceType == eReferenceTypeStrongerPayload) || (settings.referenceType == eReferenceTypeStrongerReference);
        if ((settings.referenceType == eReferenceTypeWeakerPayload) || (settings.referenceType == eReferenceTypeStrongerPayload)) {
            const PXR_NS::SdfPayload payload(asset, target);
            if (stronger) {
                prim->GetPayloadList().Prepend(payload);
            } else {
                prim->GetPayloadList().Append(payload);
            }
        } else {
            const PXR_NS::SdfReference reference(asset, target);
            if (stronger) {
                prim->GetReferenceList().Prepend(reference);
            } else {
                prim->GetReferenceList().Append(reference);
            }
        }
        if (edit.dependencies) {
            std::shared_ptr<SceneLayerDependencies> dependencies = std::make_shared<SceneLayerDependencies>();
            dependencies->layers.push_back(geoLayer);
            *edit.dependencies = dependencies;
        }

        return true;
    };
    const auto author = [&](const SceneLayerEdit& edit, std::string* error) {
        SceneNodeAuthoring::authorLayerMetadata(edit);
        if (settings.reference) {
            if (geoLayer && !authorReference(edit)) {
                *error = tr("The prim %1 could not be authored.").arg(QString::fromStdString(settings.options.rootPath.GetString())).toStdString();

                return false;
            }

            return true;
        }
        if (!copy) {
            if (geoLayer) {
                edit.layer->InsertSubLayerPath(identifier);
                if (edit.dependencies) {
                    std::shared_ptr<SceneLayerDependencies> dependencies = std::make_shared<SceneLayerDependencies>();
                    dependencies->layers.push_back(geoLayer);
                    *edit.dependencies = dependencies;
                }
            }

            return true;
        }
        for (std::size_t i = 0; i < gathered.size(); ++i) {
            if (!converts[i]) {
                continue;
            }
            const PXR_NS::UsdTimeCode time = settings.timeSamples ? SceneNodeAuthoring::frameToTimeCode(edit, gathered[i].frame) : PXR_NS::UsdTimeCode::Default();
            if (!converts[i]->author(edit.layer, time, error)) {
                return false;
            }
        }

        return true;
    };
    if (!emitSceneLayer(upstream, reads, author, outputScene, errorMessage)) {
        setPersistentMessage(eMessageTypeError, *errorMessage);

        return eStatusFailed;
    }

    const std::vector<PXR_NS::SdfPath> noPaths;
    const std::vector<PXR_NS::SdfPath>& overlaysWithoutPrim = reported ? reported->overlaysWithoutPrim() : noPaths;
    const bool warn = !missing.empty() || !sheared.empty() || !overlaysWithoutPrim.empty() || !referenceWarning.empty();
    QString message;
    const auto section = [&](const QString& heading) {
        if (!message.isEmpty()) {
            message += QString::fromUtf8("\n");
        }
        message += heading;
    };
    if (reported && (reported->partitionCount() > 0)) {
        // Where the geometry went, so a path or mode mistake shows without inspecting the stage.
        const std::size_t kListed = 20;
        const std::size_t count = reported->partitionCount();
        section(tr("Prims written:"));
        for (std::size_t i = 0; (i < count) && (i < kListed); ++i) {
            message += QString::fromUtf8("\n  ") + QString::fromStdString(reported->partitionPath(i).GetString()) + QString::fromUtf8("  ") + (reported->isPartitionOverlay(i) ? tr("overlaid") : tr("defined"));
        }
        if (count > kListed) {
            message += QString::fromUtf8("\n  ") + tr("and %1 more").arg((qulonglong)(count - kListed));
        }
    }
    if (!overlaysWithoutPrim.empty()) {
        section(tr("Overlaid, but not in the Scene input, so not drawn:"));
        for (const PXR_NS::SdfPath& path : overlaysWithoutPrim) {
            message += QString::fromUtf8("\n  ") + QString::fromStdString(path.GetString());
        }
    }
    if (!missing.empty()) {
        if (settings.options.otherPrimitives == eGeoUsdOtherDefine) {
            section(tr("Not in the Scene input, so not overridden (geometry read from these paths goes under "
                       "the Path Prefix):"));
        } else {
            section(tr("Not in the Scene input, so not overridden:"));
        }
        for (const std::string& path : missing) {
            message += QString::fromUtf8("\n  ") + QString::fromStdString(path);
        }
    }
    if (!sheared.empty()) {
        section(tr("Sheared packed geometry, written as native instances because a point instancer cannot hold "
                   "shear:"));
        for (const std::string& path : sheared) {
            message += QString::fromUtf8("\n  ") + QString::fromStdString(path);
        }
    }
    if (!referenceWarning.empty()) {
        section(QString::fromStdString(referenceWarning));
    }
    if (message.isEmpty()) {
        clearPersistentMessage(false);

        return eStatusOK;
    }
    if (warn) {
        setPersistentMessage(eMessageTypeWarning, message.toStdString());
    } else if (const NodePtr node = getNode()) {
        // A posted info message opens a dialog in the GUI instead of staying on the node.
        node->setQuietInfoMessage(message.toStdString());
    }

    return eStatusOK;
} // GeoToUSD::renderScene

NATRON_NAMESPACE_EXIT
