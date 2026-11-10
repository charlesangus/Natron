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
#include "Engine/ChoiceOption.h"
#include "Engine/ContentHasher.h"
#include "Engine/GeoDetail.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Geometry/GeoSdfData.h"
#include "Engine/Nodes/Geometry/GeoSdfFileFormat.h"
#include "Engine/Nodes/Geometry/GeoUsdConvert.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
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

const char*
packedModeArgument(GeoUsdPackedModeEnum mode)
{
    switch (mode) {
    case eGeoUsdPackedNativeInstances:
        return "native";
    case eGeoUsdPackedUnpack:
        return "unpack";
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
isEmptyDetail(const GeoDetailConstPtr& detail)
{
    return !detail || ((detail->pointCount() == 0) && (detail->primitiveCount() == 0));
}

// One detail and the input scene it is converted against, at one time.
struct Gathered {
    double frame = 0.;
    GeoDetailConstPtr detail;
    GeoUsdInputSceneConstPtr inputScene;
    std::vector<std::string> missing;
    std::vector<std::string> collisions;
};

struct NodeSettings {
    GeoUsdConvertOptions options;
    bool copy = false;
    bool timeSamples = false;
};

U64
inputSceneHash(const GeoUsdInputSceneConstPtr& scene)
{
    return scene ? scene->hash : 0;
}

// The paths the detail names that only an input prim gives a meaning to and that prim is missing:
// packed USD sources, and absolute paths outside the root read from USD, which move under it. In
// *collisions, the absolute paths outside the root of geometry not read from USD that the input
// already holds, which move under it too.
std::vector<std::string>
missingSources(const GeoUsdDetailPaths& paths,
               const GeoUsdConvertOptions& options,
               const GeoUsdInputSceneConstPtr& scene,
               std::vector<std::string>* collisions)
{
    std::vector<std::string> missing;
    for (const PXR_NS::SdfPath& path : paths.sourcePaths) {
        if (!scene || !scene->find(path)) {
            missing.push_back(path.GetString());
        }
    }
    const PXR_NS::SdfPath root = (options.rootPath.IsAbsolutePath() && options.rootPath.IsPrimPath()) ? options.rootPath : PXR_NS::SdfPath("/geo");
    for (std::size_t i = 0; i < paths.paths.size(); ++i) {
        GeoUsdPlacementEnum placement = eGeoUsdPlacedAsRequested;
        geoUsdPlacedPath(paths.paths[i], root, scene.get(), paths.pathsFromUsd[i] != 0, &placement);
        if (placement == eGeoUsdPlacedSourceGone) {
            missing.push_back(paths.paths[i].GetString());
        } else if (placement == eGeoUsdPlacedCollision) {
            collisions->push_back(paths.paths[i].GetString());
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
     * The detail at frame and the Scene input captured where the detail names prims. inputScene,
     * when given, is the Scene input's payload, evaluated by a caller already reading scenes;
     * otherwise the input is evaluated here, under a ReadScope released on return.
     **/
    RenderRoIRetCode gather(GeoToUSD* node,
                            double frame,
                            ViewIdx view,
                            const ScenePayload* inputScene,
                            const GeoUsdConvertOptions& options,
                            Gathered* out,
                            std::string* errorMessage) const;

    // The natrongeo key: the node, its root and mode, the frame when timeKeyed, and the view
    // unless it is negative (layerView()).
    std::string layerKey(const GeoUsdConvertOptions& options,
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

    KnobStringPtr rootPath = createKnob<KnobString>(tr("Root Path"));
    rootPath->setName(kGeoToUSDParamRootPath);
    rootPath->setHintToolTip(tr("The prim primitives without a path attribute go under, and relative paths are "
                                "resolved against.")
                                 .toStdString());
    rootPath->setDefaultValue("/geo");
    rootPath->setAnimationEnabled(false);
    page->addKnob(rootPath);
    _rootPath = rootPath;

    KnobChoicePtr packed = createKnob<KnobChoice>(tr("Packed Primitives"));
    packed->setName(kGeoToUSDParamPackedPrimitives);
    packed->setHintToolTip(tr("What packed geometry primitives become. Packed USD primitives always override "
                              "the prim they were read from.")
                               .toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("instancer", tr("Point Instancer").toStdString(),
                                       tr("One point instancer per path, with one prototype per embedded geometry.").toStdString()));
        options.push_back(ChoiceOption("native", tr("Native Instances").toStdString(),
                                       tr("One instanceable prim per packed primitive, referencing a shared prototype.").toStdString()));
        options.push_back(ChoiceOption("unpack", tr("Unpack").toStdString(),
                                       tr("The embedded geometry, copied and transformed per packed primitive.").toStdString()));
        packed->populateChoices(options);
    }
    packed->setDefaultValue((int)ePackedPrimitivesPointInstancer);
    packed->setAnimationEnabled(false);
    page->addKnob(packed);
    _packedPrimitives = packed;

    KnobBoolPtr copy = createKnob<KnobBool>(tr("Copy Into Layer"));
    copy->setName(kGeoToUSDParamCopyIntoLayer);
    copy->setHintToolTip(tr("Off, the geometry is served to the scene as it is, converted only where the scene "
                            "reads it, and edits reach the scene without rebuilding it. On, the whole geometry is "
                            "converted and copied into the node's layer: slower, and every change copies it again.")
                             .toStdString());
    copy->setDefaultValue(false);
    copy->setAnimationEnabled(false);
    page->addKnob(copy);
    _copyIntoLayer = copy;

    KnobBoolPtr samples = createKnob<KnobBool>(tr("Author Time Samples"));
    samples->setName(kGeoToUSDParamAuthorTimeSamples);
    samples->setHintToolTip(tr("With Copy Into Layer: evaluates the geometry at every frame of the project range "
                               "and authors each value as a time sample. Off, the layer holds the current frame "
                               "only.")
                                .toStdString());
    samples->setDefaultValue(false);
    samples->setAnimationEnabled(false);
    page->addKnob(samples);
    _authorTimeSamples = samples;

    updateTimeSamplesEnabled();
}

void
GeoToUSD::updateTimeSamplesEnabled()
{
    const KnobBoolPtr copy = _copyIntoLayer.lock();
    const KnobBoolPtr samples = _authorTimeSamples.lock();
    if (copy && samples) {
        samples->setAllDimensionsEnabled(copy->getValue());
    }
}

void
GeoToUSD::onKnobsLoaded()
{
    updateTimeSamplesEnabled();
}

bool
GeoToUSD::knobChanged(KnobI* k,
                      ValueChangedReasonEnum /*reason*/,
                      ViewSpec /*view*/,
                      double /*time*/,
                      bool /*originatedFromMainThread*/)
{
    const KnobBoolPtr copy = _copyIntoLayer.lock();
    if (copy && (k == copy.get())) {
        updateTimeSamplesEnabled();

        return true;
    }

    return false;
}

bool
GeoToUSD::Implementation::readSettings(GeoToUSD* node,
                                       NodeSettings* settings,
                                       std::string* errorMessage) const
{
    const KnobStringPtr root = node->_rootPath.lock();
    const std::string rootText = root ? root->getValue() : std::string("/geo");
    const PXR_NS::SdfPath rootPath = PXR_NS::SdfPath::IsValidPathString(rootText) ? PXR_NS::SdfPath(rootText) : PXR_NS::SdfPath();
    if (rootPath.IsEmpty() || !rootPath.IsAbsolutePath() || !rootPath.IsPrimPath()) {
        *errorMessage = tr("Root Path \"%1\" is not an absolute prim path.").arg(QString::fromStdString(rootText)).toStdString();

        return false;
    }
    settings->options.rootPath = rootPath;

    const KnobChoicePtr packed = node->_packedPrimitives.lock();
    switch (packed ? packed->getValue() : (int)ePackedPrimitivesPointInstancer) {
    case ePackedPrimitivesNativeInstances:
        settings->options.packedMode = eGeoUsdPackedNativeInstances;
        break;
    case ePackedPrimitivesUnpack:
        settings->options.packedMode = eGeoUsdPackedUnpack;
        break;
    case ePackedPrimitivesPointInstancer:
    default:
        settings->options.packedMode = eGeoUsdPackedPointInstancer;
        break;
    }

    const KnobBoolPtr copy = node->_copyIntoLayer.lock();
    settings->copy = copy && copy->getValue();
    const KnobBoolPtr samples = node->_authorTimeSamples.lock();
    settings->timeSamples = settings->copy && samples && samples->getValue();

    return true;
}

EffectInstance::RenderRoIRetCode
GeoToUSD::Implementation::gather(GeoToUSD* node,
                                 double frame,
                                 ViewIdx view,
                                 const ScenePayload* inputScene,
                                 const GeoUsdConvertOptions& options,
                                 Gathered* out,
                                 std::string* errorMessage) const
{
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
    if (node->aborted()) {
        return eRenderRoIRetCodeAborted;
    }

    const GeoUsdDetailPaths paths = geoUsdDetailPaths(*out->detail, options);
    const EffectInstancePtr sceneInput = node->getInput(kGeoToUSDInputScene);
    if (paths.paths.empty() || !sceneInput || !sceneInput->producesSceneData()) {
        out->missing = missingSources(paths, options, out->inputScene, &out->collisions);

        return eRenderRoIRetCodeOk;
    }

    std::optional<SceneWriteBarrier::ReadScope> read;
    ScenePayload evaluated;
    if (!inputScene) {
        read.emplace();
        const RenderRoIRetCode code = sceneInput->evaluateScene(EvaluateSceneArgs(frame, view), &evaluated, errorMessage);
        if (code != eRenderRoIRetCodeOk) {
            return code;
        }
        inputScene = &evaluated;
    }
    if (!inputScene->isEmpty()) {
        SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(*inputScene, errorMessage);
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
        out->inputScene = geoUsdCaptureInputScene(stage, time, paths.paths, paths.sourcePaths);
    }
    out->missing = missingSources(paths, options, out->inputScene, &out->collisions);

    return node->aborted() ? eRenderRoIRetCodeAborted : eRenderRoIRetCodeOk;
} // GeoToUSD::Implementation::gather

std::string
GeoToUSD::Implementation::layerKey(const GeoUsdConvertOptions& options,
                                   bool timeKeyed,
                                   double frame,
                                   int view) const
{
    ContentHasher h;
    h.appendString(options.rootPath.GetString());
    h.appendI64((std::int64_t)options.packedMode);
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
    const RenderRoIRetCode code = _imp->gather(this, args.time, args.view, nullptr, settings.options, &gathered, &error);
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
    (void)_imp->serve(_imp->layerKey(settings.options, timeKeyed, args.time, layerView(*this, args.view)), settings.options, timeKeyed, gathered, true, &layer, &current, &error);

    return aborted() ? eRenderRoIRetCodeAborted : eRenderRoIRetCodeOk;
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
    std::vector<std::string> missing;
    std::vector<std::string> collisions;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        // The Scene input's payload is evaluated at args.time; other frames evaluate their own.
        const ScenePayload* given = (frames[i] == args.time) ? &upstream : nullptr;
        const RenderRoIRetCode code = _imp->gather(this, frames[i], args.view, given, settings.options, &gathered[i], errorMessage);
        if (code != eRenderRoIRetCodeOk) {
            if (code == eRenderRoIRetCodeFailed) {
                if (errorMessage->empty()) {
                    *errorMessage = tr("The geometry could not be evaluated.").toStdString();
                }
                setPersistentMessage(eMessageTypeError, *errorMessage);
            }

            return eStatusFailed;
        }
        for (const std::string& path : gathered[i].missing) {
            if (std::find(missing.begin(), missing.end(), path) == missing.end()) {
                missing.push_back(path);
            }
        }
        for (const std::string& path : gathered[i].collisions) {
            if (std::find(collisions.begin(), collisions.end(), path) == collisions.end()) {
                collisions.push_back(path);
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
        if (!_imp->serve(_imp->layerKey(settings.options, timeKeyed, args.time, layerView(*this, args.view)), settings.options, timeKeyed, gathered.front(), false, &geoLayer, &current, errorMessage)) {
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
        for (std::size_t i = 0; i < gathered.size(); ++i) {
            if (isEmptyDetail(gathered[i].detail)) {
                continue;
            }
            GeoUsdConvertOptions options = settings.options;
            options.inputScene = gathered[i].inputScene;
            converts[i].reset(new GeoUsdConvert(gathered[i].detail, options));
            noteSheared(*converts[i]);
        }
    } else if (const std::shared_ptr<const GeoUsdConvert> served = geoLayer ? geoLayerConvert(geoLayer) : std::shared_ptr<const GeoUsdConvert>()) {
        noteSheared(*served);
    }

    ContentHasher facts;
    facts.appendBool(copy);
    facts.appendString(identifier);
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

    const auto author = [&](const SceneLayerEdit& edit, std::string* error) {
        SceneNodeAuthoring::authorLayerMetadata(edit);
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

    if (missing.empty() && collisions.empty() && sheared.empty()) {
        clearPersistentMessage(false);

        return eStatusOK;
    }
    QString message;
    if (!missing.empty()) {
        message = tr("Not in the Scene input, so not overridden (unpacked geometry read from these paths goes "
                     "under the Root Path):");
        for (const std::string& path : missing) {
            message += QString::fromUtf8("\n  ") + QString::fromStdString(path);
        }
    }
    if (!collisions.empty()) {
        if (!message.isEmpty()) {
            message += QString::fromUtf8("\n");
        }
        message += tr("Already in the Scene input, and only geometry read from a prim overrides it, so this "
                      "geometry goes under the Root Path:");
        for (const std::string& path : collisions) {
            message += QString::fromUtf8("\n  ") + QString::fromStdString(path);
        }
    }
    if (!sheared.empty()) {
        if (!message.isEmpty()) {
            message += QString::fromUtf8("\n");
        }
        message += tr("Sheared packed geometry, written as native instances because a point instancer cannot hold "
                      "shear:");
        for (const std::string& path : sheared) {
            message += QString::fromUtf8("\n  ") + QString::fromStdString(path);
        }
    }
    setPersistentMessage(eMessageTypeWarning, message.toStdString());

    return eStatusOK;
} // GeoToUSD::renderScene

NATRON_NAMESPACE_EXIT
