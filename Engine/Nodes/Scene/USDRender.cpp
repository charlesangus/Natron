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

#include "USDRender.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <QCoreApplication>
#include <QDebug>
#include <QMetaObject>
#include <QString>
#include <QStringList>
#include <QThread>

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/ContentHasher.h"
#include "Engine/Format.h"
#include "Engine/Hash64.h"
#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRuntime.h"
#endif
#include "Engine/GpuContext.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/Scene/AovLayers.h"
#include "Engine/Nodes/Scene/CyclesAovs.h"
#include "Engine/Nodes/Scene/HydraHandoff.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/LiveTextureStore.h"
#include "Engine/Nodes/Scene/MoonRayAovs.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/RenderSettingsPrim.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/ScenePayloadImpl.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/SceneWriteBarrier.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/Nodes/Scene/Utility/UtilityProfile.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderDelegate.h"
#include "Engine/Nodes/Scene/Utility/WavefrontGpu.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/PoolParallelFor.h"
#include "Engine/RenderContext.h"
#include "Engine/SceneLoadSet.h"

#include <pxr/imaging/hd/tokens.h>
#include <pxr/usd/usd/primRange.h>

#include "Engine/Project.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_ENTER

namespace {
using AovLayers::depthLayer;
using AovLayers::normalLayer;
using AovLayers::primIdLayer;

#ifdef NATRON_HAVE_GPU
// Placement asks for every frame, so each reason is logged only the first time it keeps a frame on the CPU.
bool
refuseGpu(const std::string& reason)
{
    static std::mutex mutex;
    static std::set<std::string> logged;
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (!logged.insert(reason).second) {
            return false;
        }
    }
    qInfo().noquote() << QString::fromLatin1("USDRender renders on the CPU: %1").arg(QString::fromStdString(reason));

    return false;
}
#endif

std::string
primIdKey(std::uint32_t hash)
{
    char hex[9];
    std::snprintf(hex, sizeof(hex), "%08x", hash);

    return std::string(kUSDRenderMetadataPrimIdPrefix) + hex;
}

bool
utilityUvUnwrap(const std::string& renderer,
                const RenderSettingsPrim::Values& values)
{
    if (renderer != kRenderDelegateUtilityId) {
        return false;
    }
    const RenderSettingsPrim::Values::const_iterator projection = values.find(kUtilitySettingProjection);

    return (projection != values.end()) && projection->second.IsHolding<PXR_NS::TfToken>() && (projection->second.UncheckedGet<PXR_NS::TfToken>() == kUtilityProjectionUvUnwrap);
}

// A UV Unwrap render's pixels are the texture's texels, so it has nothing beyond them to grow into.
int
utilityOverscan(const std::string& renderer,
                const RenderSettingsPrim::Values& values)
{
    if ((renderer != kRenderDelegateUtilityId) || utilityUvUnwrap(renderer, values)) {
        return 0;
    }
    const RenderSettingsPrim::Values::const_iterator found = values.find(kUtilitySettingOverscan);
    if (found == values.end()) {
        return 0;
    }
    const PXR_NS::VtValue pixels = PXR_NS::VtValue::Cast<int>(found->second);

    return pixels.IsHolding<int>() ? std::max(0, pixels.UncheckedGet<int>()) : 0;
}

// The overscan's pixels added to the canonical format; x is in pixels of the pixel aspect ratio.
RectD
withOverscan(const RectD& canonical,
             int overscan,
             double par)
{
    return RectD(canonical.x1 - overscan * par, canonical.y1 - overscan, canonical.x2 + overscan * par, canonical.y2 + overscan);
}

// How many frames' settings summaries are kept under one hash.
const std::size_t kMaxSettingsSummaries = 64;

bool
onMainThread()
{
    const QCoreApplication* app = QCoreApplication::instance();

    return app && (QThread::currentThread() == app->thread());
}
} // anon namespace

USDRender::USDRender(NodePtr node)
    : NativeEffectBase(node)
    , _settingsPrimMenuRefreshPending(false)
    , _settingsNothingContextId(SceneEvaluationContext::makeId())
    , _settingsPrimContextId(SceneEvaluationContext::makeId())
    , _settingsSummariesHash(0)
    , _hasLastSettingsSummary(false)
    , _settingsSummariesPosted(false)
    , _primIdTablesMutex()
    , _primIdTablesHash(0)
    , _primIdTables()
    , _primIdTablesMissed()
{
    _settingsPrimMenuRefresh.setRerun([this]() { return refreshSettingsPrimMenu(); });
}

USDRender::~USDRender()
{
    _settingsWorker.stop();
    EffectInstance::retireSceneContextId(_settingsNothingContextId);
    EffectInstance::retireSceneContextId(_settingsPrimContextId);
    HydraRender::release(this);
}

void
USDRender::purgeCaches()
{
    {
        const std::lock_guard<std::mutex> l(_renderSetupsMutex);
        _renderSetups.clear();
    }
    {
        std::lock_guard<std::mutex> locker(_settingsSummariesMutex);
        _settingsSummariesByContent.clear();
        _settingsSummariesByScope.clear();
    }
    HydraRender::release(this);
    clearSpecAovs();
}

const NodeSpec*
USDRender::nodeSpec() const
{
    return &kNodeSpec;
}

void
USDRender::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobChoicePtr settingsPrim = createKnob<KnobChoice>(tr("Settings Prim"));
    settingsPrim->setName(kUSDRenderParamSettingsPrim);
    settingsPrim->setHintToolTip(tr("The RenderSettings prim whose camera, resolution, aspect ratio conform policy, renderer "
                                    "and renderer settings the render follows: Stage Default is the one the stage's "
                                    "renderSettingsPrimPath metadata names (a USDRenderSettings node with Make Active on "
                                    "sets it), or one of the RenderSettings prims under /Render. Without one, the scene's "
                                    "first camera renders the project's format with the renderer's own settings. A chosen "
                                    "prim the scene no longer has is an error naming its path."));
    settingsPrim->setAnimationEnabled(false);
    settingsPrim->populateChoices(std::vector<ChoiceOption>(1, ChoiceOption(kUSDRenderSettingsPrimStageDefault, tr("Stage Default").toStdString(), "")));
    page->addKnob(settingsPrim);
    _settingsPrim = settingsPrim;

    KnobChoicePtr renderer = createKnob<KnobChoice>(tr("Renderer"));
    renderer->setName(kUSDRenderParamRenderer);
    renderer->setHintToolTip(tr("The Hydra render delegate that renders the scene. Scene Default is the one the render "
                                "settings prim names (its natron:renderer attribute), Storm when there is none or it "
                                "names none. Storm renders with OpenGL; the others render on the CPU. A renderer that "
                                "is not installed or cannot run here stays chosen, and the node is an error naming it "
                                "until it is installed or another is chosen."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kRenderDelegateSceneDefaultId, tr("Scene Default").toStdString(),
                                       tr("The renderer the render settings prim names, Storm if it names none.").toStdString()));
        for (const RenderDelegates::Delegate& delegate : RenderDelegates::list()) {
            if (delegate.available) {
                options.push_back(ChoiceOption(delegate.id, delegate.displayName, ""));
            } else {
                options.push_back(ChoiceOption(delegate.id,
                                               tr("%1 (unavailable)").arg(QString::fromStdString(delegate.displayName)).toStdString(),
                                               delegate.error));
            }
        }
        renderer->populateChoices(options);
        renderer->setDefaultValue(0);
    }
    renderer->setAnimationEnabled(false);
    page->addKnob(renderer);
    _renderer = renderer;

    KnobBoolPtr headlight = createKnob<KnobBool>(tr("Headlight"));
    headlight->setName(kUSDRenderParamHeadlight);
    headlight->setHintToolTip(tr("Adds a light at the camera to the scene's own lights. Off, only the scene's lights light "
                                 "it, and a scene with none renders its lit surfaces black."));
    headlight->setDefaultValue(false);
    headlight->setAnimationEnabled(false);
    page->addKnob(headlight);
    _headlight = headlight;

    KnobBoolPtr renderPurpose = createKnob<KnobBool>(tr("Render Purpose"));
    renderPurpose->setName(kUSDRenderParamRenderPurpose);
    renderPurpose->setHintToolTip(tr("Draws the prims whose USD purpose is render, the full-detail geometry meant for final "
                                     "images. Prims of the default purpose are always drawn."));
    renderPurpose->setDefaultValue(true);
    renderPurpose->setAnimationEnabled(false);
    page->addKnob(renderPurpose);
    _renderPurpose = renderPurpose;

    KnobBoolPtr proxyPurpose = createKnob<KnobBool>(tr("Proxy Purpose"));
    proxyPurpose->setName(kUSDRenderParamProxyPurpose);
    proxyPurpose->setHintToolTip(tr("Draws the prims whose USD purpose is proxy, the lightweight stand-ins for render "
                                    "geometry. With Render Purpose on too, both are drawn."));
    proxyPurpose->setDefaultValue(false);
    proxyPurpose->setAnimationEnabled(false);
    page->addKnob(proxyPurpose);
    _proxyPurpose = proxyPurpose;

    KnobBoolPtr guidePurpose = createKnob<KnobBool>(tr("Guide Purpose"));
    guidePurpose->setName(kUSDRenderParamGuidePurpose);
    guidePurpose->setHintToolTip(tr("Draws the prims whose USD purpose is guide, the helper geometry not meant to be "
                                    "seen in images."));
    guidePurpose->setDefaultValue(false);
    guidePurpose->setAnimationEnabled(false);
    page->addKnob(guidePurpose);
    _guidePurpose = guidePurpose;
} // USDRender::initializeKnobs

void
USDRender::onKnobsLoaded()
{
    NativeEffectBase::onKnobsLoaded();
    keepLoadedRenderer();
    scheduleSettingsPrimMenuRefresh();
}

bool
USDRender::knobChanged(KnobI* k,
                       ValueChangedReasonEnum /*reason*/,
                       ViewSpec /*view*/,
                       double /*time*/,
                       bool /*originatedFromMainThread*/)
{
    KnobChoicePtr renderer = _renderer.lock();
    KnobChoicePtr settingsPrim = _settingsPrim.lock();

    return (renderer && (k == renderer.get())) || (settingsPrim && (k == settingsPrim.get()));
}

void
USDRender::keepLoadedRenderer()
{
    KnobChoicePtr renderer = _renderer.lock();

    if (!renderer) {
        return;
    }
    // Loading an id the menu lacks leaves the index on the default; the menu gains the id so that
    // the knob, its hash and the next save all name it.
    const ChoiceOption active = renderer->getActiveEntry();
    if (active.id.empty() || renderer->isActiveEntryPresentInEntries()) {
        return;
    }
    renderer->appendChoice(ChoiceOption(active.id,
                                        tr("%1 (not installed)").arg(QString::fromStdString(active.id)).toStdString(),
                                        RenderDelegates::unavailableMessage(active.id)));
    renderer->setValue(renderer->getNumEntries() - 1);
}

std::string
USDRender::rendererOverride() const
{
    KnobChoicePtr renderer = _renderer.lock();
    const std::string id = renderer ? renderer->getActiveEntry().id : std::string();

    return id.empty() ? std::string(kRenderDelegateSceneDefaultId) : id;
}

std::string
USDRender::settingsPrimOverride() const
{
    KnobChoicePtr settingsPrim = _settingsPrim.lock();
    const std::string id = settingsPrim ? settingsPrim->getActiveEntry().id : std::string();

    return (id == kUSDRenderSettingsPrimStageDefault) ? std::string() : id;
}

void
USDRender::onInputChanged(int inputNo)
{
    NativeEffectBase::onInputChanged(inputNo);
    scheduleSettingsPrimMenuRefresh();
}

void
USDRender::onNodeHashChanged(U64 hash)
{
    clearSpecAovs();
    NativeEffectBase::onNodeHashChanged(hash);
    scheduleSettingsPrimMenuRefresh();
}

void
USDRender::scheduleSettingsPrimMenuRefresh()
{
    if (_settingsPrimMenuRefreshPending.exchange(true)) {
        return;
    }
    // The hooks that call this fire mid-recompute of a hash, before the upstream scene settled.
    QMetaObject::invokeMethod(this, [this]() {
        _settingsPrimMenuRefreshPending = false;
        refreshSettingsPrimMenu(); }, Qt::QueuedConnection);
}

std::shared_ptr<SceneMenuJob>
USDRender::refreshSettingsPrimMenu()
{
    bool noViewer = false;
    const SceneEvaluationContext context = _settingsPrimMenuRefresh.getContext(this, &noViewer);
    const EffectInstancePtr input = getInput(0);
    const double time = getCurrentTime();
    SceneMenuRefresh::WorkFunc work = [weakInput = EffectInstanceWPtr(input), time, context](const SceneMenuJob& job,
                                                                                             SceneMenuJob::Result* result) {
        result->lists.resize(1);
        // Render settings prims live under /Render, which is all this reads.
        if (context.reach) {
            context.reach->add(std::vector<std::string>(1, std::string("/Render")));
        }
        ScenePayload scene;
        SceneQueryPool::Lease lease;
        if (!SceneNodeAuthoring::borrowMenuScene(weakInput, time, context, job, &scene, &lease)) {
            return;
        }
        const PXR_NS::UsdPrim render = lease.getStage()->GetPrimAtPath(PXR_NS::SdfPath("/Render"));
        if (!render) {
            return;
        }
        // Unloaded prims are walked too, to count the payloads left unsearched.
        const PXR_NS::Usd_PrimFlagsPredicate predicate = PXR_NS::UsdPrimIsActive && PXR_NS::UsdPrimIsDefined && !PXR_NS::UsdPrimIsAbstract;
        PXR_NS::UsdPrimRange prims(render, predicate);
        for (PXR_NS::UsdPrimRange::iterator it = prims.begin(); it != prims.end(); ++it) {
            if (job.isCancelled()) {
                return;
            }
            if (it->HasPayload() && !it->IsLoaded()) {
                ++result->unsearched;
                it.PruneChildren();
                continue;
            }
            if (it->IsA<PXR_NS::UsdRenderSettings>()) {
                result->lists[0].push_back(it->GetPath().GetString());
            }
        }
    };

    return _settingsPrimMenuRefresh.start(this, context, noViewer, work, [this](const std::shared_ptr<SceneMenuJob>& job) { applySettingsPrimMenu(job); });
}

bool
USDRender::applySettingsPrimMenu(const std::shared_ptr<SceneMenuJob>& job)
{
    if (!_settingsPrimMenuRefresh.take(job)) {
        return false;
    }
    KnobChoicePtr settingsPrim = _settingsPrim.lock();
    if (!settingsPrim) {
        return false;
    }
    const SceneMenuJob::Result result = job->getResult();
    std::vector<ChoiceOption> options(1, ChoiceOption(kUSDRenderSettingsPrimStageDefault, tr("Stage Default").toStdString(), ""));
    if (!result.lists.empty()) {
        for (const std::string& path : result.lists[0]) {
            options.push_back(ChoiceOption(path, path, ""));
        }
    }
    if (result.unsearched > 0) {
        options.push_back(makeSceneUnsearchedOption(result.unsearched, job->hasNoViewer()));
    }
    // Repopulating an unchanged menu would move the hash and come back here.
    const std::vector<ChoiceOption> current = settingsPrim->getEntries_mt_safe();
    bool same = current.size() == options.size();
    for (std::size_t i = 0; same && (i < options.size()); ++i) {
        same = (current[i].id == options[i].id) && (current[i].label == options[i].label);
    }
    if (!same) {
        settingsPrim->populateChoices(options);
    }

    return true;
}

struct USDRender::ActiveSettings {
    ScenePayload scene;
    // Holds the stage; given back before the render, which renders the node's own stage.
    SceneQueryPool::Lease lease;
    PXR_NS::UsdStageRefPtr stage;
    PXR_NS::UsdTimeCode time;
    // Invalid when the stage has no active one.
    PXR_NS::UsdRenderSettings prim;
    RenderSettingsPrim::StandardAttributes standard;
    std::string renderer;
    // Set by evaluateSettingsPrimScene(): what scene is evaluated on, and the settings prim's path
    // when it names one.
    SceneLoadSetPtr loadSet;
    PXR_NS::SdfPath settingsPrimPath;
};

namespace {
std::atomic<std::uint64_t> gSettingsSummaryReads { 0 };
std::atomic<std::uint64_t> gSettingsSummaryEvaluations { 0 };
std::atomic<std::uint64_t> gSettingsSummaryReuses { 0 };
std::atomic<std::uint64_t> gSettingsSummaryJoins { 0 };

// The prim fields through which a prim composes opinions from specs other than those at its own
// path and its ancestors' in the layer stack.
const std::vector<PXR_NS::TfToken>&
arcFields()
{
    static const std::vector<PXR_NS::TfToken> fields = {
        PXR_NS::TfToken("references"),
        PXR_NS::TfToken("payload"),
        PXR_NS::TfToken("inheritPaths"),
        PXR_NS::TfToken("specializes"),
        PXR_NS::TfToken("variantSetNames"),
        PXR_NS::TfToken("variantSelection"),
        PXR_NS::TfToken("variantSetChildren"),
        PXR_NS::TfToken("clips"),
        PXR_NS::TfToken("clipSets"),
        PXR_NS::TfToken("relocates"),
        PXR_NS::TfToken("layerRelocates"),
    };

    return fields;
}

bool
hasArcField(const PXR_NS::SdfLayerHandle& layer,
            const PXR_NS::SdfPath& path)
{
    for (const PXR_NS::TfToken& field : arcFields()) {
        if (layer->HasField(path, field)) {
            return true;
        }
    }

    return false;
}

enum PrimSpecsEnum {
    ePrimSpecsNone = 0,
    ePrimSpecsSome,
    // An arc, or a sublayer not open, may bring opinions from elsewhere.
    ePrimSpecsUnknown
};

// Whether layer, or what it sublayers, has a spec at path or one of its ancestors.
PrimSpecsEnum
primSpecsIn(const PXR_NS::SdfLayerHandle& layer,
            const PXR_NS::SdfPath& path,
            std::set<const PXR_NS::SdfLayer*>* visited)
{
    // Handles are compared through their pointers: TfWeakPtr's operator== recurses under C++20.
    if (!visited->insert(PXR_NS::get_pointer(layer)).second) {
        return ePrimSpecsNone;
    }
    if (hasArcField(layer, PXR_NS::SdfPath::AbsoluteRootPath())) {
        return ePrimSpecsUnknown;
    }
    PrimSpecsEnum found = ePrimSpecsNone;
    for (PXR_NS::SdfPath at = path; !at.IsEmpty() && (at != PXR_NS::SdfPath::AbsoluteRootPath()); at = at.GetParentPath()) {
        if (!layer->HasSpec(at)) {
            continue;
        }
        if (hasArcField(layer, at)) {
            return ePrimSpecsUnknown;
        }
        found = ePrimSpecsSome;
    }
    for (const std::string& subLayerPath : layer->GetSubLayerPaths()) {
        const PXR_NS::SdfLayerHandle subLayer = PXR_NS::SdfLayer::FindRelativeToLayer(layer, subLayerPath);
        if (!subLayer) {
            return ePrimSpecsUnknown;
        }
        const PrimSpecsEnum below = primSpecsIn(subLayer, path, visited);
        if (below == ePrimSpecsUnknown) {
            return ePrimSpecsUnknown;
        }
        if (below == ePrimSpecsSome) {
            found = ePrimSpecsSome;
        }
    }

    return found;
}
} // anon namespace

std::uint64_t
USDRender::settingsSummaryReadCount()
{
    return gSettingsSummaryReads.load();
}

std::uint64_t
USDRender::settingsSummaryEvaluationCount()
{
    return gSettingsSummaryEvaluations.load();
}

bool
USDRender::settingsScopeKey(double time,
                            ViewIdx view,
                            U64* key) const
{
    const NodePtr node = getNode();
    const NodePtr input = node ? node->getInput(0) : NodePtr();
    const HashNamespacesConstPtr table = input ? input->getHashNamespaces() : HashNamespacesConstPtr();
    if (!table || table->settingsScopeUncertain) {
        return false;
    }
    // A context variable may switch what upstream authors, and the digest does not hold its value.
    const RenderContextReadSetConstPtr reads = input->getUpstreamContextReadSet();
    if (reads && !reads->empty()) {
        return false;
    }
    ContentHasher h;
    h.appendU64(table->settingsScope);
    h.appendBool(table->settingsScopeTimeVarying);
    if (table->settingsScopeTimeVarying) {
        h.appendDouble(time);
    }
    h.appendI64(view.value());
    h.appendString(settingsPrimOverride());
    h.appendString(rendererOverride());
    AppInstancePtr app = getApp();
    const ProjectPtr project = app ? app->getProject() : ProjectPtr();
    h.appendDouble(project ? project->getProjectFrameRate() : 24.);
    h.appendU64(project ? project->getContentDigest() : 0);
    *key = h.finalize();

    return true;
}

bool
USDRender::findScopedSettingsSummary(U64 key,
                                     SettingsSummary* summary) const
{
    std::lock_guard<std::mutex> locker(_settingsSummariesMutex);
    std::map<U64, SettingsSummary>::const_iterator found = _settingsSummariesByScope.find(key);
    if (found == _settingsSummariesByScope.end()) {
        return false;
    }
    ++gSettingsSummaryReuses;
    utilityProfile::count(utilityProfile::Counter::SettingsSummaryReuse);
    *summary = found->second;

    return true;
}

std::uint64_t
USDRender::settingsSummaryReuseCount()
{
    return gSettingsSummaryReuses.load();
}

std::uint64_t
USDRender::settingsSummaryJoinCount()
{
    return gSettingsSummaryJoins.load();
}

bool
USDRender::evaluateSettingsPrimScene(double time,
                                     ViewIdx view,
                                     ActiveSettings* active,
                                     std::string* error) const
{
    // Only the settings prim is read, as the stage metadata naming the active prim needs no payload.
    const SceneLoadSetPtr nothing = std::make_shared<const SceneLoadSet>(SceneLoadSet::none());
    active->loadSet = nothing;
    active->settingsPrimPath = PXR_NS::SdfPath();
    if (!evaluateInput(time, view, _settingsNothingContextId, nothing, active, error)) {
        return false;
    }
    std::string primPath = settingsPrimOverride();
    if (primPath.empty()) {
        primPath = active->scene.getMetadata().renderSettingsPrimPath;
    }
    // The summary reads the prim's own attributes; the camera it targets is for the render to open.
    if (primPath.empty() || !PXR_NS::SdfPath::IsValidPathString(primPath)) {
        return true;
    }
    const SceneLoadSetPtr primOnly = SceneLoadSet::withPaths(nothing, { primPath });
    if (!evaluateInput(time, view, _settingsPrimContextId, primOnly, active, error)) {
        return false;
    }
    active->loadSet = primOnly;
    active->settingsPrimPath = PXR_NS::SdfPath(primPath);

    return true;
}

U64
USDRender::settingsPrimDigest(const ActiveSettings& active) const
{
    ContentHasher h;
    for (const ScenePayloadImpl* link = active.scene.getImpl().get(); link; link = link->base.get()) {
        const SceneLayerEntry& entry = link->top;
        if (!entry.layer) {
            continue;
        }
        std::set<const PXR_NS::SdfLayer*> visited;
        const PrimSpecsEnum specs = active.settingsPrimPath.IsEmpty() ? ePrimSpecsNone : primSpecsIn(entry.layer, active.settingsPrimPath, &visited);
        if (specs == ePrimSpecsUnknown) {
            return 0;
        }
        if (specs == ePrimSpecsSome) {
            h.appendU64(entry.hash);
        }
    }
    const SceneMetadata& metadata = active.scene.getMetadata();
    h.appendString(metadata.renderSettingsPrimPath);
    h.appendDouble(metadata.timeCodesPerSecond);
    h.appendString(active.settingsPrimPath.GetString());
    h.appendString(settingsPrimOverride());
    h.appendString(rendererOverride());
    AppInstancePtr app = getApp();
    h.appendDouble((app && app->getProject()) ? app->getProject()->getProjectFrameRate() : 24.);

    return h.finalize();
}

bool
USDRender::evaluateInput(double time,
                         ViewIdx view,
                         U64 contextId,
                         const SceneLoadSetPtr& loadSet,
                         ActiveSettings* active,
                         std::string* error) const
{
    const SceneNodeAuthoring::EffectReleasedOnMainThread heldInput(getInput(0));
    const EffectInstancePtr& input = heldInput.get();
    if (!input) {
        *error = tr("USDRender needs a scene on its Scene input.").toStdString();

        return false;
    }
    const SceneEvaluationContext context = loadSet ? SceneEvaluationContext(contextId, loadSet) : SceneEvaluationContext();
    active->lease.release();
    active->stage.Reset();
    if (input->evaluateScene(EvaluateSceneArgs(time, view, context), &active->scene, error) != eRenderRoIRetCodeOk) {
        if (error->empty()) {
            *error = tr("The input scene could not be evaluated.").toStdString();
        }

        return false;
    }
    if (active->scene.isEmpty()) {
        *error = tr("The input scene is empty: there is no camera to render through.").toStdString();

        return false;
    }

    return true;
}

bool
USDRender::readActiveSettings(double time,
                              ViewIdx view,
                              SettingsLoadEnum load,
                              ActiveSettings* active,
                              std::string* error,
                              bool sceneEvaluated) const
{
    auto failWith = [error](const std::string& message) {
        *error = message;

        return false;
    };

    auto evaluate = [&](U64 id, const SceneLoadSetPtr& loadSet) {
        return evaluateInput(time, view, id, loadSet, active, error);
    };
    auto borrowStage = [active, error](const SceneLoadSetPtr& loadSet) {
        active->lease.release();
        active->lease = SceneQueryPool::instance().borrow(active->scene, loadSet, error);
        active->stage = active->lease.getStage();
        if (!active->stage && error->empty()) {
            *error = tr("USD could not compose the input scene.").toStdString();
        }

        return bool(active->stage);
    };
    const std::string chosenPrim = settingsPrimOverride();
    if (load == eSettingsLoadEverything) {
        // A render draws the whole scene.
        if ((!sceneEvaluated && !evaluate(0, SceneLoadSetPtr())) || !borrowStage(SceneLoadSet::everything())) {
            return false;
        }
    } else if (load == eSettingsLoadSettingsPrim) {
        if ((!sceneEvaluated && !evaluateSettingsPrimScene(time, view, active, error)) || !borrowStage(active->loadSet)) {
            return false;
        }
    } else {
        const SceneLoadSetPtr nothing = std::make_shared<const SceneLoadSet>(SceneLoadSet::none());
        if (!evaluate(_settingsNothingContextId, nothing) || !borrowStage(nothing)) {
            return false;
        }
    }
    AppInstancePtr app = getApp();
    const double fps = (app && app->getProject()) ? app->getProject()->getProjectFrameRate() : 24.;
    active->time = PXR_NS::UsdTimeCode(SceneOps::frameToTimeCode(active->scene, time, fps));

    std::string primError;
    active->prim = RenderSettingsPrim::activePrim(active->stage, chosenPrim, &primError);
    if (!active->prim && !primError.empty()) {
        return failWith(primError);
    }
    if (active->prim) {
        active->standard = RenderSettingsPrim::standardAttributes(active->prim);
    }

    // The node's renderer, else the prim's, else Storm.
    active->renderer = rendererOverride();
    if (active->renderer == kRenderDelegateSceneDefaultId) {
        active->renderer.clear();
        if (active->prim && !RenderSettingsPrim::rendererFor(active->prim.GetPrim(), &active->renderer, error)) {
            return false;
        }
        if (active->renderer.empty()) {
            active->renderer = kRenderDelegateStormId;
        }
    }

    return true;
} // USDRender::readActiveSettings

USDRender::SettingsSummary
USDRender::summarizeSettings(double time,
                             ViewIdx view,
                             SettingsLoadEnum load) const
{
    const utilityProfile::Span timed("settings_summary");
    SettingsSummary summary;
    ActiveSettings active;

    U64 digest = 0;
    U64 scopeKey = 0;
    bool scoped = false;
    U64 hash = 0;
    if (load == eSettingsLoadSettingsPrim) {
        const NodePtr node = getNode();
        hash = node ? node->getHashValue() : 0;
        scoped = settingsScopeKey(time, view, &scopeKey);
        if (scoped && findScopedSettingsSummary(scopeKey, &summary)) {
            return summary;
        }
    }
    ++gSettingsSummaryEvaluations;
    utilityProfile::count(utilityProfile::Counter::SettingsSummaryEvaluation);
    if (load == eSettingsLoadSettingsPrim) {
        if (!evaluateSettingsPrimScene(time, view, &active, &summary.error)) {
            return summary;
        }
        digest = scoped ? 0 : settingsPrimDigest(active);
        if (digest) {
            std::lock_guard<std::mutex> locker(_settingsSummariesMutex);
            std::map<std::tuple<double, int, U64>, SettingsSummary>::const_iterator found = _settingsSummariesByContent.find(std::make_tuple(time, view.value(), digest));
            if (found != _settingsSummariesByContent.end()) {
                ++gSettingsSummaryReuses;
                utilityProfile::count(utilityProfile::Counter::SettingsSummaryReuse);

                return found->second;
            }
        }
    }
    ++gSettingsSummaryReads;
    utilityProfile::count(utilityProfile::Counter::SettingsSummaryRead);
    summary.read = readActiveSettings(time, view, load, &active, &summary.error, load == eSettingsLoadSettingsPrim);
    if (summary.read) {
        summary.error.clear();
        summary.renderer = active.renderer;
        summary.hasPrim = bool(active.prim);
        if (summary.hasPrim) {
            summary.width = active.standard.resolution[0];
            summary.height = active.standard.resolution[1];
            summary.pixelAspectRatio = active.standard.pixelAspectRatio;
            summary.overscan = utilityOverscan(active.renderer, RenderSettingsPrim::settings(active.prim.GetPrim(), active.renderer, active.time));
            const RenderSettingsPrim::Values values = RenderSettingsPrim::settings(active.prim.GetPrim(), kRenderDelegateUtilityId, active.time);
            const RenderSettingsPrim::Values::const_iterator device = values.find(kUtilitySettingDevice);
            if ((device != values.end()) && device->second.IsHolding<PXR_NS::TfToken>()) {
                summary.utilityDevice = device->second.UncheckedGet<PXR_NS::TfToken>().GetString();
            }
        }
    }
    // A failed read is tried again: what failed it, such as a layer that does not compose, may lie
    // outside the settings prim.
    if (digest && summary.read) {
        std::lock_guard<std::mutex> locker(_settingsSummariesMutex);
        if (_settingsSummariesByContent.size() >= kMaxSettingsSummaries) {
            _settingsSummariesByContent.clear();
        }
        _settingsSummariesByContent[std::make_tuple(time, view.value(), digest)] = summary;
    }
    if (scoped && summary.read) {
        // The graph may have moved while the scene was evaluated: the summary is of the scope
        // only if neither the scope nor the node did.
        U64 keyAfter = 0;
        const NodePtr node = getNode();
        if (node && (node->getHashValue() == hash) && settingsScopeKey(time, view, &keyAfter) && (keyAfter == scopeKey)) {
            std::lock_guard<std::mutex> locker(_settingsSummariesMutex);
            if (_settingsSummariesByScope.size() >= kMaxSettingsSummaries) {
                _settingsSummariesByScope.clear();
            }
            _settingsSummariesByScope[scopeKey] = summary;
        }
    }

    return summary;
}

USDRender::SettingsSummary
USDRender::getSettingsSummary(double time,
                              ViewIdx view) const
{
    const std::pair<double, int> key(time, view.value());
    const NodePtr node = getNode();
    const U64 hash = node ? node->getHashValue() : 0;
    const bool mainThread = onMainThread();
    U64 scopeKey = 0;
    const bool scoped = settingsScopeKey(time, view, &scopeKey);
    SettingsSummary provisional;
    bool post = false;
    {
        std::lock_guard<std::mutex> locker(_settingsSummariesMutex);
        if (hash != _settingsSummariesHash) {
            _settingsSummariesHash = hash;
            _settingsSummaries.clear();
            _settingsSummariesGiven.clear();
        }
        std::map<std::pair<double, int>, SettingsSummary>::const_iterator found = _settingsSummaries.find(key);
        if (found != _settingsSummaries.end()) {
            return found->second;
        }
    }
    // Known without evaluating the scene, so the main thread needs no provisional summary.
    SettingsSummary scopedSummary;
    if (scoped && findScopedSettingsSummary(scopeKey, &scopedSummary)) {
        storeSettingsSummary(hash, time, view, scopedSummary);

        return scopedSummary;
    }
    {
        std::lock_guard<std::mutex> locker(_settingsSummariesMutex);
        if (hash == _settingsSummariesHash) {
            std::map<std::pair<double, int>, SettingsSummary>::const_iterator found = _settingsSummaries.find(key);
            if (found != _settingsSummaries.end()) {
                return found->second;
            }
        }
        if (mainThread) {
            std::map<std::pair<double, int>, GivenSettingsSummary>::const_iterator given = _settingsSummariesGiven.find(key);
            if ((given != _settingsSummariesGiven.end()) && (given->second.hash == hash)) {
                provisional = given->second.summary;
            } else {
                if (_hasLastSettingsSummary) {
                    provisional = _lastSettingsSummary;
                } else {
                    // The project's format, as outputFormat() gives without a prim.
                    provisional.read = true;
                    provisional.renderer = kRenderDelegateStormId;
                }
                GivenSettingsSummary entry;
                entry.hash = hash;
                entry.summary = provisional;
                _settingsSummariesGiven[key] = entry;
            }
            post = !_settingsSummariesPosted;
            _settingsSummariesPosted = true;
        }
    }
    if (!mainThread) {
        return readSettingsSummary(hash, time, view);
    }
    const_cast<USDRender*>(this)->markOutputMetadataProvisional();
    if (post) {
        postSettingsRead(hash);
    }

    return provisional;
} // USDRender::getSettingsSummary

void
USDRender::postSettingsRead(U64 hash) const
{
    // Never more than one posted job: one that replaced another would drop it, and the drop
    // would be taken for the node's end.
    const std::weak_ptr<const EffectInstance> weakThis = weak_from_this();
    _settingsWorker.post(getApp(), [weakThis]() {
        std::shared_ptr<const EffectInstance> effect = weakThis.lock();
        if (const USDRender* render = dynamic_cast<const USDRender*>(effect.get())) {
            render->readGivenSettingsSummaries();
            render->readWantedSpecAovs();
        }
        SceneNodeAuthoring::releaseOnMainThread(&effect); }, [weakThis, hash]() {
        std::shared_ptr<const EffectInstance> effect = weakThis.lock();
        if (const USDRender* render = dynamic_cast<const USDRender*>(effect.get())) {
            bool dropped = false;
            {
                std::lock_guard<std::mutex> locker(render->_settingsSummariesMutex);
                render->_settingsSummariesPosted = false;
                for (std::map<std::pair<double, int>, GivenSettingsSummary>::iterator it = render->_settingsSummariesGiven.begin(); it != render->_settingsSummariesGiven.end();) {
                    if (it->second.hash == hash) {
                        it = render->_settingsSummariesGiven.erase(it);
                        dropped = true;
                    } else {
                        ++it;
                    }
                }
            }
            {
                std::lock_guard<std::mutex> l(render->_specMutex);
                render->_specAovsWanted.clear();
            }
            // The readers given a provisional summary ask again rather than keep it.
            if (dropped && QCoreApplication::instance()) {
                const std::weak_ptr<const EffectInstance> weak = weakThis;
                QMetaObject::invokeMethod(QCoreApplication::instance(), [weak]() {
                    const std::shared_ptr<const EffectInstance> self = weak.lock();
                    if (const USDRender* r = dynamic_cast<const USDRender*>(self.get())) {
                        const_cast<USDRender*>(r)->invalidateOutputMetadata();
                    } }, Qt::QueuedConnection);
            }
        }
        SceneNodeAuthoring::releaseOnMainThread(&effect); });
}

void
USDRender::readGivenSettingsSummaries() const
{
    {
        std::lock_guard<std::mutex> locker(_settingsSummariesMutex);
        _settingsSummariesPosted = false;
    }
    for (;;) {
        U64 hash = 0;
        bool hasNode = false;
        {
            const NodePtr node = getNode();
            hasNode = bool(node);
            hash = node ? node->getHashValue() : 0;
        }
        bool found = false;
        std::pair<double, int> key;
        {
            std::lock_guard<std::mutex> locker(_settingsSummariesMutex);
            for (std::map<std::pair<double, int>, GivenSettingsSummary>::iterator it = _settingsSummariesGiven.begin(); it != _settingsSummariesGiven.end();) {
                // Given under a hash the node left: its readers ask again under the new one.
                if (it->second.hash != hash) {
                    it = _settingsSummariesGiven.erase(it);
                    continue;
                }
                if (!found) {
                    key = it->first;
                    found = true;
                }
                ++it;
            }
        }
        if (!found || !hasNode) {
            return;
        }
        (void)readSettingsSummary(hash, key.first, ViewIdx(key.second));
    }
}

USDRender::SettingsSummary
USDRender::readSettingsSummary(U64 hash,
                               double time,
                               ViewIdx view) const
{
    // Past this the other read is taken to be held up behind this thread, and the summary is read here.
    const std::chrono::milliseconds kJoinLimit(5000);
    const std::tuple<U64, double, int> reading(hash, time, view.value());
    const std::pair<double, int> key(time, view.value());
    // The other read may wait on a layer reload, which waits for the read scope this thread holds.
    const bool mayWait = !SceneWriteBarrier::isReadingOnThisThread();
    const std::chrono::steady_clock::time_point giveUp = std::chrono::steady_clock::now() + kJoinLimit;
    bool owns = false;
    {
        std::unique_lock<std::mutex> locker(_settingsSummariesMutex);
        bool waited = false;
        while (mayWait && _settingsSummariesReading.count(reading) && (std::chrono::steady_clock::now() < giveUp)) {
            waited = true;
            _settingsSummaryReadDone.wait_until(locker, giveUp);
        }
        if (waited && (hash == _settingsSummariesHash)) {
            std::map<std::pair<double, int>, SettingsSummary>::const_iterator found = _settingsSummaries.find(key);
            if (found != _settingsSummaries.end()) {
                ++gSettingsSummaryJoins;
                utilityProfile::count(utilityProfile::Counter::SettingsSummaryJoin);

                return found->second;
            }
        }
        owns = _settingsSummariesReading.insert(reading).second;
    }
    struct ReadingGuard {
        const USDRender* render;
        std::tuple<U64, double, int> reading;
        bool owns;

        ~ReadingGuard()
        {
            if (!owns) {
                return;
            }
            {
                std::lock_guard<std::mutex> l(render->_settingsSummariesMutex);
                render->_settingsSummariesReading.erase(reading);
            }
            render->_settingsSummaryReadDone.notify_all();
        }
    } guard { this, reading, owns };

    const SettingsSummary summary = summarizeSettings(time, view, eSettingsLoadSettingsPrim);
    storeSettingsSummary(hash, time, view, summary);

    return summary;
}

void
USDRender::storeSettingsSummary(U64 hash,
                                double time,
                                ViewIdx view,
                                const SettingsSummary& summary) const
{
    const std::pair<double, int> key(time, view.value());
    bool differs = false;
    {
        std::lock_guard<std::mutex> locker(_settingsSummariesMutex);
        std::map<std::pair<double, int>, GivenSettingsSummary>::iterator given = _settingsSummariesGiven.find(key);
        const bool givenForHash = (given != _settingsSummariesGiven.end()) && (given->second.hash == hash);
        // A summary of a scene that moved while it was read must not be served under the newer hash.
        const NodePtr node = getNode();
        if (!node || (node->getHashValue() != hash) || (hash != _settingsSummariesHash)) {
            if (givenForHash) {
                _settingsSummariesGiven.erase(given);
            }

            return;
        }
        if (_settingsSummaries.size() >= kMaxSettingsSummaries) {
            _settingsSummaries.clear();
        }
        _settingsSummaries[key] = summary;
        _lastSettingsSummary = summary;
        _hasLastSettingsSummary = true;
        if (givenForHash) {
            differs = !(given->second.summary == summary);
            _settingsSummariesGiven.erase(given);
        }
    }
    if (differs && QCoreApplication::instance()) {
        const std::weak_ptr<const EffectInstance> weakThis = weak_from_this();
        QMetaObject::invokeMethod(QCoreApplication::instance(), [weakThis]() {
            const std::shared_ptr<const EffectInstance> self = weakThis.lock();
            if (const USDRender* render = dynamic_cast<const USDRender*>(self.get())) {
                const_cast<USDRender*>(render)->invalidateOutputMetadata();
            } }, Qt::QueuedConnection);
    }
}

bool
USDRender::activeRenderer(double time,
                          ViewIdx view,
                          std::string* rendererId,
                          std::string* error) const
{
    const std::string chosen = rendererOverride();

    if (chosen != kRenderDelegateSceneDefaultId) {
        *rendererId = chosen;

        return true;
    }
    const SettingsSummary summary = getSettingsSummary(time, view);
    if (!summary.read) {
        *error = summary.error;

        return false;
    }
    *rendererId = summary.renderer;

    return true;
}

struct USDRender::SpecAovs {
    struct Entry {
        HydraRender::Aov aov;
        PXR_NS::SdfPath path;
        PXR_NS::TfToken dataType;
        ImageLayerDesc layer;
        // The channels of the renderer's format the layer was declared with, 0 when unknown then.
        int formatChannels = 0;
        // False while the renderer gives no buffer for it: it is asked for, and has no layer.
        bool declared = true;
    };

    std::string renderer;
    std::vector<Entry> aovs;
    // The layer of each RenderVar that names an AOV every render has, by AOV.
    std::map<PXR_NS::TfToken, std::string> builtInLayers;
    // Those AOVs, with the settings their RenderVars author for the delegate, if any.
    std::vector<HydraRender::Aov> builtInAovs;
    std::vector<RenderSettingsPrim::RejectedVar> rejected;
    std::string warning;
    std::string error;
};

namespace {
// The layer of the AOVs every render of renderer has, which a RenderVar naming one renders to; empty for any other AOV.
std::string
builtInLayer(const PXR_NS::TfToken& aov,
             const std::string& renderer)
{
    const bool moonRay = MoonRayAovs::isMoonRay(renderer);

    if (aov == PXR_NS::HdAovTokens->color) {
        return ImageLayerDesc::getRGBAComponents().getLayerID();
    } else if ((aov == PXR_NS::HdAovTokens->depth) || (moonRay && (aov == MoonRayAovs::depthAov()))) {
        return depthLayer().getLayerID();
    } else if (aov == PXR_NS::HdAovTokens->primId) {
        return primIdLayer().getLayerID();
    } else if (!RenderDelegates::isStorm(renderer) && ((aov == PXR_NS::HdAovTokens->normal) || (moonRay && (aov == MoonRayAovs::normalAov())))) {
        return normalLayer().getLayerID();
    }

    return std::string();
}

// Whether the var authors an attribute for the delegate, besides Natron's own.
bool
authorsAovSettings(const RenderSettingsPrim::AovRequest& request)
{
    static const std::string natron("natron:");

    for (const std::pair<const std::string, PXR_NS::VtValue>& entry : request.settings) {
        if (entry.first.compare(0, natron.size(), natron) != 0) {
            return true;
        }
    }

    return false;
}

void
setMetadataValue(ImageMetadata* metadata,
                 const std::string& key,
                 const PXR_NS::VtValue& value)
{
    if (value.IsHolding<std::string>()) {
        metadata->setString(key, value.UncheckedGet<std::string>());
    } else if (value.IsHolding<PXR_NS::TfToken>()) {
        metadata->setString(key, value.UncheckedGet<PXR_NS::TfToken>().GetString());
    } else if (value.IsHolding<bool>()) {
        metadata->setInt(key, value.UncheckedGet<bool>() ? 1 : 0);
    } else if (value.IsHolding<int>()) {
        metadata->setInt(key, value.UncheckedGet<int>());
    } else if (value.IsHolding<float>()) {
        metadata->setDouble(key, value.UncheckedGet<float>());
    } else if (value.IsHolding<double>()) {
        metadata->setDouble(key, value.UncheckedGet<double>());
    } else if (value.IsHolding<PXR_NS::VtIntArray>()) {
        const PXR_NS::VtIntArray& array = value.UncheckedGet<PXR_NS::VtIntArray>();
        metadata->setIntVector(key, std::vector<int>(array.cbegin(), array.cend()));
    } else if (value.IsHolding<PXR_NS::VtDoubleArray>()) {
        const PXR_NS::VtDoubleArray& array = value.UncheckedGet<PXR_NS::VtDoubleArray>();
        metadata->setDoubleVector(key, std::vector<double>(array.cbegin(), array.cend()));
    } else if (value.IsHolding<PXR_NS::VtFloatArray>()) {
        const PXR_NS::VtFloatArray& array = value.UncheckedGet<PXR_NS::VtFloatArray>();
        metadata->setDoubleVector(key, std::vector<double>(array.cbegin(), array.cend()));
    } else {
        std::ostringstream text;
        text << value;
        metadata->setString(key, text.str());
    }
}
} // anon namespace

std::shared_ptr<const USDRender::SpecAovs>
USDRender::specAovs(double time,
                    ViewIdx view,
                    const ActiveSettings* active,
                    bool* provisionalOut) const
{
    const FrameKey frame(time, (int)view);
    const U64 hash = getRenderHash();
    const HashedFrameKey key(hash, frame);
    // A render on the main thread, as a background render's, reads it: nothing would refresh it.
    const bool provisional = !active && onMainThread() && !getFrameArgs();
    if (provisionalOut) {
        *provisionalOut = false;
    }
    U64 generation = 0;
    std::set<std::pair<std::string, std::string>> refused;
    std::shared_ptr<const SpecAovs> given;
    {
        std::lock_guard<std::mutex> l(_specMutex);
        const std::shared_ptr<const SpecAovs>* found = _specAovs.find(key);
        if (found) {
            return *found;
        }
        if (provisional) {
            given = _lastSpecAovs;
            WantedSpecAovs& wanted = _specAovsWanted[frame];
            wanted.hash = getHash();
            wanted.given = given;
        } else {
            generation = _specGeneration;
            refused = _refusedAovs;
        }
    }
    if (provisional) {
        bool post = false;
        {
            std::lock_guard<std::mutex> locker(_settingsSummariesMutex);
            post = !_settingsSummariesPosted;
            _settingsSummariesPosted = true;
        }
        if (post) {
            postSettingsRead(getHash());
        }
        if (provisionalOut) {
            *provisionalOut = true;
        }

        return given;
    }
    ActiveSettings read;
    if (!active) {
        std::string error;
        if (!readActiveSettings(time, view, eSettingsLoadSettingsPrim, &read, &error)) {
            return std::shared_ptr<const SpecAovs>();
        }
        active = &read;
    }
    std::shared_ptr<SpecAovs> spec = std::make_shared<SpecAovs>();
    spec->renderer = active->renderer;
    RenderSettingsPrim::AovRequests requests;
    if (active->prim && !RenderSettingsPrim::aovRequests(active->prim, active->renderer, active->time, &requests, &spec->error)) {
        requests = RenderSettingsPrim::AovRequests();
    }
    spec->rejected = requests.rejected;
    std::set<std::string> taken = { ImageLayerDesc::getRGBAComponents().getLayerID(), depthLayer().getLayerID(), primIdLayer().getLayerID() };
    if (!RenderDelegates::isStorm(active->renderer)) {
        taken.insert(normalLayer().getLayerID());
    }
    auto addWarning = [&spec](const std::string& warning) {
        spec->warning = spec->warning.empty() ? warning : spec->warning + "\n" + warning;
    };
    if (!requests.warning.empty()) {
        addWarning(requests.warning);
    }
    // Two vars can name one AOV only once mapped to the delegate's names ("cycles:normal" and "normal").
    std::set<PXR_NS::TfToken> asked;
    for (const RenderSettingsPrim::AovRequest& request : requests.requests) {
        PXR_NS::TfToken aovName(RenderSettingsPrim::delegateAovName(request.name.GetString(), active->renderer));
        std::string builtIn = builtInLayer(aovName, active->renderer);
        if (builtIn.empty()) {
            // The delegate's own pass of an AOV every render has ("cycles:normal") is that AOV, whose layer it would collide with.
            const PXR_NS::TfToken bare(RenderSettingsPrim::withoutOwnNamespace(aovName.GetString(), active->renderer));
            builtIn = builtInLayer(bare, active->renderer);
            if (!builtIn.empty()) {
                aovName = bare;
            }
        }
        if (!asked.insert(aovName).second) {
            spec->rejected.push_back({ request.path, "another RenderVar already asks for the AOV " + aovName.GetString() });
            continue;
        }
        if (!builtIn.empty()) {
            spec->builtInLayers[aovName] = builtIn;
            if (!request.layer.empty() && (request.layer != builtIn)) {
                addWarning(tr("The RenderVar %1 names the AOV %2, which renders to the layer %3: its layer %4 is ignored.")
                               .arg(QString::fromStdString(request.path.GetString()))
                               .arg(QString::fromStdString(aovName.GetString()))
                               .arg(QString::fromStdString(builtIn))
                               .arg(QString::fromStdString(request.layer))
                               .toStdString());
            }
            HydraRender::Aov aov;
            aov.name = aovName;
            if (authorsAovSettings(request)) {
                aov.settings = RenderSettingsPrim::outputSettings(request);
            }
            spec->builtInAovs.push_back(aov);
            continue;
        }
        std::string formatWarning;
        SpecAovs::Entry entry;
        entry.aov.name = aovName;
        entry.aov.settings = RenderSettingsPrim::outputSettings(request);
        entry.path = request.path;
        entry.dataType = request.dataType;
        entry.formatChannels = AovLayers::formatChannels(active->renderer, aovName.GetString());
        entry.layer = AovLayers::layer(active->renderer, aovName.GetString(), request.dataType.GetString(), request.layer, entry.formatChannels, &formatWarning);
        if (!formatWarning.empty()) {
            addWarning(tr("The RenderVar %1: %2").arg(QString::fromStdString(request.path.GetString())).arg(QString::fromStdString(formatWarning)).toStdString());
        }
        if (!taken.insert(entry.layer.getLayerID()).second) {
            addWarning(tr("The RenderVar %1 renders to the layer %2, which another layer of the render has: it is not rendered.")
                           .arg(QString::fromStdString(request.path.GetString()))
                           .arg(QString::fromStdString(entry.layer.getLayerID()))
                           .toStdString());
            continue;
        }
        entry.declared = !refused.count(std::make_pair(active->renderer, aovName.GetString()));
        spec->aovs.push_back(entry);
    }
    // A spec of a scene that moved, or of refused AOVs that changed, while it was read must not be
    // served under the newer state.
    const bool current = (getRenderHash() == hash);
    std::lock_guard<std::mutex> l(_specMutex);
    if (current && (generation == _specGeneration)) {
        _specAovs.get(key) = spec;
        _lastSpecAovs = spec;
    }

    return spec;
} // USDRender::specAovs

void
USDRender::clearSpecAovs()
{
    std::lock_guard<std::mutex> l(_specMutex);

    _specAovs.clear();
    ++_specGeneration;
}

void
USDRender::readWantedSpecAovs() const
{
    // What getComponentsNeededAndProduced() takes from a spec: its renderer and declared layers.
    const auto declaredLayers = [](const std::shared_ptr<const SpecAovs>& spec) {
        std::vector<std::string> layers;
        if (!spec) {
            return layers;
        }
        layers.push_back(spec->renderer);
        for (std::size_t i = 0; spec->error.empty() && (i < spec->aovs.size()); ++i) {
            if (spec->aovs[i].declared) {
                layers.push_back(spec->aovs[i].layer.getLayerID());
                const std::vector<std::string>& channels = spec->aovs[i].layer.getChannels();
                layers.insert(layers.end(), channels.begin(), channels.end());
            }
        }

        return layers;
    };
    for (;;) {
        const U64 hash = getHash();
        FrameKey frame;
        std::shared_ptr<const SpecAovs> given;
        bool found = false;
        {
            std::lock_guard<std::mutex> l(_specMutex);
            for (std::map<FrameKey, WantedSpecAovs>::iterator it = _specAovsWanted.begin(); it != _specAovsWanted.end();) {
                // Given under a hash the node left: its readers ask again under the new one.
                if (!found && (it->second.hash == hash)) {
                    frame = it->first;
                    given = it->second.given;
                    found = true;
                }
                it = _specAovsWanted.erase(it);
                if (found) {
                    break;
                }
            }
        }
        if (!found || !getNode()) {
            return;
        }
        const std::shared_ptr<const SpecAovs> spec = specAovs(frame.first, ViewIdx(frame.second), nullptr);
        if (spec && (getHash() == hash) && (declaredLayers(spec) != declaredLayers(given)) && QCoreApplication::instance()) {
            const std::weak_ptr<const EffectInstance> weakThis = weak_from_this();
            QMetaObject::invokeMethod(QCoreApplication::instance(), [weakThis]() {
                const std::shared_ptr<const EffectInstance> self = weakThis.lock();
                if (const USDRender* render = dynamic_cast<const USDRender*>(self.get())) {
                    USDRender* r = const_cast<USDRender*>(render);
                    r->invalidateOutputMetadata();
                    // The layers moved under an unchanged hash, so nothing else re-lists the layer menus downstream.
                    r->refreshMetadataSideEffects();
                } }, Qt::QueuedConnection);
        }
    }
} // USDRender::readWantedSpecAovs

void
USDRender::appendNodeContentHash(ContentHasher& h) const
{
    // Of the render settings knobs upstream, which every other node folds whole, only those the
    // render reads are folded in (consumesHashNamespaces()): the active prim's in the namespaces
    // of the renderer it renders with. When the active prim is none a node authors (a file's, or not known here),
    // or the table is uncertain about it or its renderer, every one is.
    NodePtr node = getNode();
    NodePtr input = node ? node->getInput(0) : NodePtr();
    const HashNamespacesConstPtr table = input ? input->getHashNamespaces() : HashNamespacesConstPtr();

    h.appendString("renderSettings");
    if (!table) {
        h.appendBool(false);

        return;
    }
    const std::string chosenPrim = settingsPrimOverride();
    const std::string path = chosenPrim.empty() ? table->active : chosenPrim;
    std::map<std::string, HashNamespaces::Scope>::const_iterator scope = path.empty() ? table->scopes.end() : table->scopes.find(path);
    // An uncertain table knows only the prims and the active one a node over all of it settled.
    const bool known = !table->uncertain || ((scope != table->scopes.end()) && scope->second.settled && (!chosenPrim.empty() || table->activeSettled));
    if ((scope == table->scopes.end()) || !known) {
        h.appendBool(false);
        table->appendAll(h);

        return;
    }
    std::string renderer = rendererOverride();
    if (renderer == kRenderDelegateSceneDefaultId) {
        renderer = scope->second.defaultRenderer.empty() ? std::string(kRenderDelegateStormId) : scope->second.defaultRenderer;
    }
    h.appendBool(true);
    h.appendString(path);
    h.appendString(renderer);
    // The RenderVars' AOV settings and layers are authored in these, besides the renderer's.
    std::vector<std::string> spaces = RenderSettingsPrim::namespaces(renderer);
    spaces.insert(spaces.end(), { "driver", "natron", "parameters" });
    for (const std::string& ns : spaces) {
        std::map<std::string, U64>::const_iterator digest = scope->second.digests.find(ns);
        h.appendString(ns);
        h.appendU64((digest == scope->second.digests.end()) ? 0 : digest->second);
    }
} // USDRender::appendNodeContentHash

void
USDRender::outputFormat(double time,
                        ViewIdx view,
                        RectI* format,
                        double* par,
                        int* overscan) const
{
    const SettingsSummary summary = getSettingsSummary(time, view);

    if (overscan) {
        *overscan = 0;
    }
    if (summary.read && summary.hasPrim) {
        const int width = summary.width;
        const int height = summary.height;
        const double aspect = summary.pixelAspectRatio;
        if (overscan) {
            *overscan = summary.overscan;
        }
        if ((width > 0) && (height > 0) && (aspect > 0.)) {
            *format = RectI(0, 0, width, height);
            *par = aspect;

            return;
        }
    }
    Format projectFormat;
    getApp()->getProject()->getProjectDefaultFormat(&projectFormat);
    *format = projectFormat;
    *par = projectFormat.getPixelAspectRatio();
}

StatusEnum
USDRender::getPreferredMetadataAt(double time,
                                  ViewIdx view,
                                  NodeMetadata& metadata)
{
    metadata.setBitDepth(-1, eImageBitDepthFloat);
    metadata.setComponentsType(-1, eImageComponentsTypeColor);
    metadata.setNComps(-1, 4);
    metadata.setIsFrameVarying(true);
    RectI format;
    double par;
    outputFormat(time, view, &format, &par);
    metadata.setOutputFormat(format);
    metadata.setPixelAspectRatio(-1, par);

    return eStatusOK;
}

StatusEnum
USDRender::getRegionOfDefinition(U64 /*hash*/,
                                 double time,
                                 const RenderScale& /*scale*/,
                                 ViewIdx view,
                                 RectD* rod)
{
    RectI format;
    double par;
    int overscan;

    outputFormat(time, view, &format, &par, &overscan);
    *rod = withOverscan(Format(format, par).toCanonicalFormat(), overscan, par);

    return eStatusOK;
}

void
USDRender::getComponentsNeededAndProduced(double time,
                                          ViewIdx view,
                                          EffectInstance::ComponentsNeededMap* comps,
                                          double* passThroughTime,
                                          int* passThroughView,
                                          int* passThroughInputNb)
{
    EffectInstance::getComponentsNeededAndProduced(time, view, comps, passThroughTime, passThroughView, passThroughInputNb);
    // Nothing is fetched from the scene input as an image.
    comps->erase(0);
    std::list<ImageLayerDesc>& produced = (*comps)[-1];
    produced.clear();
    produced.push_back(ImageLayerDesc::getRGBAComponents());
    produced.push_back(depthLayer());
    produced.push_back(primIdLayer());
    // Storm and Natron Utility render no normal AOV.
    bool provisional = false;
    const std::shared_ptr<const SpecAovs> spec = specAovs(time, view, nullptr, &provisional);
    if (provisional) {
        markComponentsProvisional();
    }
    std::string renderer;
    std::string error;
    if (spec) {
        renderer = spec->renderer;
    } else if (!activeRenderer(time, view, &renderer, &error)) {
        renderer = kRenderDelegateStormId;
    }
    if (!RenderDelegates::isStorm(renderer) && (renderer != kRenderDelegateUtilityId)) {
        produced.push_back(normalLayer());
    }
    for (std::size_t i = 0; spec && spec->error.empty() && (i < spec->aovs.size()); ++i) {
        if (spec->aovs[i].declared) {
            produced.push_back(spec->aovs[i].layer);
        }
    }
}

FramesNeededMap
USDRender::getFramesNeeded(double /*time*/,
                           ViewIdx /*view*/)
{
    return FramesNeededMap();
}

void
USDRender::getRegionsOfInterest(double /*time*/,
                                const RenderScale& /*scale*/,
                                const RectD& /*outputRoD*/,
                                const RectD& /*renderWindow*/,
                                ViewIdx /*view*/,
                                RoIMap* /*ret*/)
{
}

namespace {
// How many frames' primId tables are kept under one hash.
const std::size_t kMaxPrimIdTables = 64;

// The purposes a render draws besides the default one.
enum PrimIdPurposeEnum {
    ePrimIdPurposeRender = 1,
    ePrimIdPurposeProxy = 2,
    ePrimIdPurposeGuide = 4,
};

unsigned
primIdPurposesOf(const KnobBoolWPtr& render,
                 const KnobBoolWPtr& proxy,
                 const KnobBoolWPtr& guide)
{
    const auto isOn = [](const KnobBoolWPtr& weak) {
        const KnobBoolPtr knob = weak.lock();

        return knob && knob->getValue();
    };

    return (isOn(render) ? ePrimIdPurposeRender : 0) | (isOn(proxy) ? ePrimIdPurposeProxy : 0) | (isOn(guide) ? ePrimIdPurposeGuide : 0);
}

// Every prim Storm can draw with the purposes given, so that each primId.id value a render gives has its entry.
ImageMetadata
primIdTableOf(const PXR_NS::UsdStageRefPtr& stage,
              unsigned purposes)
{
    ImageMetadata table;
    // Point instancer prototypes are commonly authored as class or over prims, which the default traversal skips but Storm draws
    // through the instancer, so every prim is visited.
    const bool showRender = (purposes & ePrimIdPurposeRender) != 0;
    const bool showProxy = (purposes & ePrimIdPurposeProxy) != 0;
    const bool showGuides = (purposes & ePrimIdPurposeGuide) != 0;
    const PXR_NS::UsdPrimRange all(stage->GetPseudoRoot(), PXR_NS::UsdTraverseInstanceProxies(PXR_NS::UsdPrimAllPrimsPredicate));

    for (const PXR_NS::UsdPrim& prim : all) {
        if (!prim.IsA<PXR_NS::UsdGeomGprim>()) {
            continue;
        }
        const PXR_NS::TfToken purpose = PXR_NS::UsdGeomImageable(prim).ComputePurpose();
        if (((purpose == PXR_NS::UsdGeomTokens->render) && !showRender) || ((purpose == PXR_NS::UsdGeomTokens->proxy) && !showProxy)
            || ((purpose == PXR_NS::UsdGeomTokens->guide) && !showGuides)) {
            continue;
        }
        const std::string path = prim.GetPath().GetString();
        table.setString(primIdKey(HydraRender::primPathHash(path)), path);
    }

    return table;
}
} // anon namespace

U64
USDRender::primIdTableHash() const
{
    const EffectInstancePtr input = getInput(0);

    if (!input) {
        return 0;
    }
    // The purpose knobs decide which prims the table names.
    Hash64 hash;
    hash.append(input->getHash());
    hash.append(primIdPurposesOf(_renderPurpose, _proxyPurpose, _guidePurpose));
    hash.computeHash();

    return hash.value();
}

bool
USDRender::findPrimIdTable(U64 hash,
                           double time,
                           ViewIdx view,
                           bool noteMiss,
                           ImageMetadata* table)
{
    const std::pair<double, int> key(time, view.value());
    std::lock_guard<std::mutex> locker(_primIdTablesMutex);

    if (hash == _primIdTablesHash) {
        std::map<std::pair<double, int>, ImageMetadata>::const_iterator found = _primIdTables.find(key);
        if (found != _primIdTables.end()) {
            *table = found->second;

            return true;
        }
    } else if (noteMiss) {
        _primIdTablesHash = hash;
        _primIdTables.clear();
        _primIdTablesMissed.clear();
    }
    if (noteMiss) {
        _primIdTablesMissed.insert(key);
    }

    return false;
}

void
USDRender::storePrimIdTable(U64 hash,
                            double time,
                            ViewIdx view,
                            const ImageMetadata& table)
{
    const std::pair<double, int> key(time, view.value());
    bool missed = false;
    {
        std::lock_guard<std::mutex> locker(_primIdTablesMutex);
        if (hash != _primIdTablesHash) {
            // The main-thread readers given the provisional answer under the hash the node left
            // never ask again under the new one unless told to, as nothing else changed for them.
            missed = !_primIdTablesMissed.empty();
            _primIdTablesHash = hash;
            _primIdTables.clear();
            _primIdTablesMissed.clear();
        }
        if (_primIdTables.size() >= kMaxPrimIdTables) {
            _primIdTables.clear();
        }
        _primIdTables[key] = table;
        missed = (_primIdTablesMissed.erase(key) > 0) || missed;
    }
    if (missed) {
        QMetaObject::invokeMethod(this, [this]() { invalidateOutputMetadata(); }, Qt::QueuedConnection);
    }
}

ImageMetadata
USDRender::deriveOutputMetadata(double time,
                                ViewIdx view)
{
    // The table names every drawable prim, which needs every payload of the input scene open: that
    // is never done on the main thread, which is served the table a render or another thread stored
    // and, until one has, an empty provisional answer.
    ImageMetadata metadata;
    {
        std::lock_guard<std::mutex> l(_specMutex);
        const ImageMetadata* found = _aovMetadata.find(HashedFrameKey(getRenderHash(), FrameKey(time, (int)view)));
        if (found) {
            metadata = *found;
        }
    }
    ImageMetadata table;
    const U64 hash = primIdTableHash();
    const bool mainThread = onMainThread();

    if (findPrimIdTable(hash, time, view, mainThread, &table)) {
        metadata.merge(table, ImageMetadata::eMergePreferOther);

        return metadata;
    }
    if (mainThread) {
        markOutputMetadataProvisional();

        return metadata;
    }
    EffectInstancePtr input = getInput(0);
    ScenePayload scene;
    std::string error;
    if (!input || (input->evaluateScene(EvaluateSceneArgs(time, view), &scene, &error) != eRenderRoIRetCodeOk) || scene.isEmpty()) {
        return metadata;
    }
    // Built off the main thread for the render output, which has no viewer to take a set from.
    SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(scene, SceneLoadSet::everything(), &error);
    PXR_NS::UsdStageRefPtr stage = lease.getStage();
    if (!stage) {
        return metadata;
    }
    table = primIdTableOf(stage, primIdPurposesOf(_renderPurpose, _proxyPurpose, _guidePurpose));
    // A table of a scene that moved while it was read must not be served under the newer hash.
    if (primIdTableHash() == hash) {
        storePrimIdTable(hash, time, view, table);
    }
    metadata.merge(table, ImageMetadata::eMergePreferOther);

    return metadata;
}

struct USDRender::RenderSetup {
    HydraRender::Request request;
    // The render's pixels in the output image.
    RectI pixels;
    // What renders but not as asked, shown once the render succeeds.
    std::string warning;
    // The URIs of the live textures the render reads, at its time.
    std::vector<std::string> liveTextureUris;
    // What the camera is read again from: its path, empty without one, the aspect ratio and fit of
    // its frustum, and what else the setup read that a camera-only change must not touch.
    PXR_NS::SdfPath cameraPath;
    double cameraAspect = 1.;
    HydraRender::FitModeEnum fit = HydraRender::eFitModeExpand;
    PXR_NS::SdfPathVector domes;
    PXR_NS::SdfPath settingsPrimPath;
    std::shared_ptr<const SpecAovs> spec;
};

namespace {
// The fit that does what an aspectRatioConformPolicy asks, false with the policy's fallback if
// none does.
bool
fitModeForPolicy(const PXR_NS::TfToken& policy,
                 HydraRender::FitModeEnum* fit)
{
    *fit = HydraRender::eFitModeExpand;
    if (policy.IsEmpty() || (policy == PXR_NS::UsdRenderTokens->expandAperture)) {
        return true;
    }
    if (policy == PXR_NS::UsdRenderTokens->cropAperture) {
        *fit = HydraRender::eFitModeFill;

        return true;
    }
    // The aperture's width moves to the image's aspect, its height kept.
    if (policy == PXR_NS::UsdRenderTokens->adjustApertureWidth) {
        *fit = HydraRender::eFitModeVertical;

        return true;
    }
    if (policy == PXR_NS::UsdRenderTokens->adjustApertureHeight) {
        *fit = HydraRender::eFitModeHorizontal;

        return true;
    }

    // adjustPixelAspectRatio, or an unknown token.
    return false;
}
} // anon namespace

namespace {
std::atomic<std::uint64_t> gRenderSetupReads { 0 };
std::atomic<std::uint64_t> gRenderSetupReuses { 0 };
std::atomic<std::uint64_t> gRenderSetupCameraReads { 0 };

// Whether layer authors nothing but prim specs on camera and its ancestors, none of them in avoid,
// with no arc or sublayer: *h then gets every field of those specs, their properties' left out.
bool
authorsOnlyOnCamera(const PXR_NS::SdfLayerHandle& layer,
                    const PXR_NS::SdfPath& camera,
                    const PXR_NS::SdfPathVector& avoid,
                    ContentHasher* h)
{
    static const PXR_NS::TfToken primChildren("primChildren");
    if (!layer->GetSubLayerPaths().empty()) {
        return false;
    }
    for (PXR_NS::SdfPath at = PXR_NS::SdfPath::AbsoluteRootPath();;) {
        if (hasArcField(layer, at) || (std::find(avoid.begin(), avoid.end(), at) != avoid.end())) {
            return false;
        }
        h->appendString(at.GetString());
        for (const PXR_NS::TfToken& field : layer->ListFields(at)) {
            h->appendString(field.GetString());
            h->appendU64(layer->GetField(at, field).GetHash());
        }
        const PXR_NS::TfTokenVector children = layer->GetFieldAs<PXR_NS::TfTokenVector>(at, primChildren);
        if (children.empty()) {
            return true;
        }
        if (children.size() != 1) {
            return false;
        }
        const PXR_NS::SdfPath child = at.AppendChild(children.front());
        if (!camera.HasPrefix(child)) {
            return false;
        }
        at = child;
    }
}

// The scene's stack hash with each layer that authors only on setup's camera and its ancestors
// standing for its prim specs alone: equal for two scenes that differ only in properties of those
// prims, which the setup reads nothing from but the camera. 0 when the setup has no camera.
U64
cameraStructureDigest(const ScenePayload& scene,
                      const PXR_NS::SdfPath& camera,
                      const PXR_NS::SdfPathVector& domes,
                      const PXR_NS::SdfPath& settingsPrim)
{
    if (camera.IsEmpty()) {
        return 0;
    }
    PXR_NS::SdfPathVector avoid = domes;
    if (!settingsPrim.IsEmpty()) {
        avoid.push_back(settingsPrim);
    }
    ContentHasher h;
    for (const ScenePayloadImpl* link = scene.getImpl().get(); link; link = link->base.get()) {
        const SceneLayerEntry& entry = link->top;
        if (!entry.layer) {
            continue;
        }
        ContentHasher fields;
        const bool cameraOnly = !entry.deferredProjections && !entry.liveTextures && authorsOnlyOnCamera(entry.layer, camera, avoid, &fields);
        h.appendBool(cameraOnly);
        h.appendU64(cameraOnly ? fields.finalize() : entry.hash);
    }
    const SceneMetadata& metadata = scene.getMetadata();
    h.appendI64((std::int64_t)metadata.upAxis);
    h.appendDouble(metadata.metersPerUnit);
    h.appendString(metadata.defaultPrim);
    h.appendDouble(metadata.timeCodesPerSecond);
    h.appendString(metadata.renderSettingsPrimPath);
    h.appendString(camera.GetString());

    return h.finalize();
}

// Past this many, the setup least recently used is dropped: a viewer, its other planes and a Write
// rendering another frame at once each keep theirs.
const std::size_t kRenderSetupsKept = 4;
} // anon namespace

std::uint64_t
USDRender::renderSetupReadCount()
{
    return gRenderSetupReads.load();
}

std::uint64_t
USDRender::renderSetupReuseCount()
{
    return gRenderSetupReuses.load();
}

std::uint64_t
USDRender::renderSetupCameraReadCount()
{
    return gRenderSetupCameraReads.load();
}

bool
USDRender::RenderSetupKey::operator==(const RenderSetupKey& other) const
{
    return (sceneHash == other.sceneHash) && sameSettings(other);
}

bool
USDRender::RenderSetupKey::sameSettings(const RenderSetupKey& other) const
{
    return (time == other.time) && (view == other.view) && (level == other.level) && (renderer == other.renderer) && (knobs == other.knobs) && (project == other.project);
}

bool
USDRender::renderSetupKey(double time,
                          ViewIdx view,
                          unsigned int level,
                          const ActiveSettings& active,
                          RenderSetupKey* key) const
{
    const NodePtr node = getNode();
    if (!node) {
        return false;
    }
    // An expression may read what no digest here covers, such as another node's knob.
    bool hasExpression = false;
    key->knobs = node->getKnobsContentDigest(&hasExpression);
    if (hasExpression) {
        return false;
    }
    ContentHasher namespaced;
    for (const std::pair<const std::string, U64>& digest : node->getKnobsNamespaceDigests()) {
        namespaced.appendString(digest.first);
        namespaced.appendU64(digest.second);
    }
    namespaced.appendU64(key->knobs);
    key->knobs = namespaced.finalize();
    key->sceneHash = active.scene.getStackHash();
    key->time = time;
    key->view = view.value();
    key->level = level;
    key->renderer = rendererOverride();
    // Without a settings prim the render takes the project's format, and the frame's time code
    // follows the project's frame rate.
    ContentHasher project;
    AppInstancePtr app = getApp();
    if (app && app->getProject()) {
        Format format;
        app->getProject()->getProjectDefaultFormat(&format);
        project.appendI64(format.x1);
        project.appendI64(format.y1);
        project.appendI64(format.x2);
        project.appendI64(format.y2);
        project.appendDouble(format.getPixelAspectRatio());
        project.appendDouble(app->getProject()->getProjectFrameRate());
    }
    key->project = project.finalize();

    return true;
}

void
USDRender::prepareLiveTextures(const ActiveSettings& active,
                               double time,
                               RenderSetup* setup)
{
    // Hydra reads live textures from the store and never renders them: render the frame's first.
    const LiveTextureSourceList liveTextures = SceneOps::getLiveTextures(active.scene);
    LiveTextureStore::instance().prepare(liveTextures, time, [this]() { return aborted(); });
    setup->liveTextureUris.clear();
    for (const LiveTextureSource& source : liveTextures) {
        setup->liveTextureUris.push_back(source.getUri(time));
    }
}

bool
USDRender::prepareRender(double time,
                         ViewIdx view,
                         unsigned int level,
                         bool primIdPlane,
                         RenderSetup* setup,
                         std::string* error)
{
    const utilityProfile::Span timed("prepare_render");
    ActiveSettings active;
    if (!evaluateInput(time, view, 0, SceneLoadSetPtr(), &active, error)) {
        return false;
    }
    RenderSetupKey key;
    const bool keyed = renderSetupKey(time, view, level, active, &key);
    // Stored only when nothing the key covers moved during the read, so that it is never kept under
    // a key it was not read for.
    auto keep = [&]() {
        RenderSetupKey after;
        if (!keyed || !renderSetupKey(time, view, level, active, &after) || !(after == key)) {
            return;
        }
        after.structure = cameraStructureDigest(active.scene, setup->cameraPath, setup->domes, setup->settingsPrimPath);
        std::shared_ptr<RenderSetup> kept = std::make_shared<RenderSetup>(*setup);
        kept->request.aborted = std::function<bool()>();
        kept->request.scene = ScenePayload();
        const std::lock_guard<std::mutex> l(_renderSetupsMutex);
        _renderSetups.remove_if([&after](const KeptRenderSetup& entry) { return entry.first == after; });
        _renderSetups.emplace_front(after, std::move(kept));
        while (_renderSetups.size() > kRenderSetupsKept) {
            _renderSetups.pop_back();
        }
    };
    if (keyed) {
        std::shared_ptr<const RenderSetup> kept;
        std::vector<KeptRenderSetup> others;
        {
            const std::lock_guard<std::mutex> l(_renderSetupsMutex);
            for (std::list<KeptRenderSetup>::iterator it = _renderSetups.begin(); it != _renderSetups.end(); ++it) {
                if (it->first == key) {
                    kept = it->second;
                    _renderSetups.splice(_renderSetups.begin(), _renderSetups, it);
                    break;
                }
                if (it->first.structure && it->first.sameSettings(key)) {
                    others.push_back(*it);
                }
            }
        }
        const bool sameScene = bool(kept);
        for (std::size_t i = 0; !kept && (i < others.size()); ++i) {
            const RenderSetup& other = *others[i].second;
            if (cameraStructureDigest(active.scene, other.cameraPath, other.domes, other.settingsPrimPath) == others[i].first.structure) {
                kept = others[i].second;
            }
        }
        // The primId table is stored by a render that reads the stage, which a reused setup does not.
        ImageMetadata primIds;
        if (kept && (!primIdPlane || findPrimIdTable(primIdTableHash(), time, view, false, &primIds))) {
            ++gRenderSetupReuses;
            utilityProfile::count(utilityProfile::Counter::RenderSetupReuse);
            *setup = *kept;
            setup->request.owner = this;
            setup->request.aborted = [this]() { return aborted(); };
            setup->request.scene = active.scene;
            prepareLiveTextures(active, time, setup);
            if (sameScene) {
                return true;
            }
            const utilityProfile::Span camera("setup_camera");
            ++gRenderSetupCameraReads;
            utilityProfile::count(utilityProfile::Counter::RenderSetupCameraRead);
            if (!readActiveSettings(time, view, eSettingsLoadEverything, &active, error, true) || !readSetupCamera(&active, setup, error)) {
                return false;
            }
            keep();

            return true;
        }
    }
    ++gRenderSetupReads;
    utilityProfile::count(utilityProfile::Counter::RenderSetupRead);
    {
        const utilityProfile::Span read("setup_read");
        if (!readRenderSetup(time, view, level, primIdPlane, &active, setup, error)) {
            return false;
        }
    }
    keep();

    return true;
}

bool
USDRender::readSetupCamera(ActiveSettings* active,
                           RenderSetup* setup,
                           std::string* error) const
{
    const PXR_NS::UsdStageRefPtr& stage = active->stage;
    const PXR_NS::SdfPath& cameraPath = setup->cameraPath;
    setup->request.time = active->time;
    setup->request.frustum = PXR_NS::GfFrustum();
    setup->request.shutterOpen = 0.;
    setup->request.shutterClose = 0.;
    setup->request.cameraMotion.clear();
    if (cameraPath.IsEmpty()) {
        return true;
    }
    const double aspect = setup->cameraAspect;
    const HydraRender::FitModeEnum fit = setup->fit;
    if (!HydraRender::cameraFrustum(stage, cameraPath, active->time, aspect, fit, &setup->request.frustum, error)) {
        return false;
    }
    const PXR_NS::UsdGeomCamera camera(stage->GetPrimAtPath(cameraPath));
    double shutterOpen = 0.;
    double shutterClose = 0.;
    (void)camera.GetShutterOpenAttr().Get(&shutterOpen, active->time);
    (void)camera.GetShutterCloseAttr().Get(&shutterClose, active->time);
    setup->request.shutterOpen = shutterOpen;
    setup->request.shutterClose = shutterClose;
    if (!active->time.IsDefault() && HydraRender::cameraMotionBlur(setup->request)) {
        // Between these the delegate interpolates the camera, so a few follow a curved move closely.
        const int kCameraMotionSamples = 9;
        const PXR_NS::GfVec2d shutter = HydraRender::shutterInterval(setup->request);
        for (int i = 0; i < kCameraMotionSamples; ++i) {
            const double t = shutter[0] + (shutter[1] - shutter[0]) * i / (kCameraMotionSamples - 1);
            PXR_NS::GfFrustum frustum;
            if (!HydraRender::cameraFrustum(stage, cameraPath, PXR_NS::UsdTimeCode(active->time.GetValue() + t), aspect, fit, &frustum, error)) {
                return false;
            }
            setup->request.cameraMotion.push_back(std::make_pair(t, frustum));
        }
        std::vector<std::pair<double, PXR_NS::GfFrustum>>& motion = setup->request.cameraMotion;
        // The delegate moves the camera it is given by how these differ from the frame's, so the
        // frame's is among them even when the shutter misses the frame.
        if ((shutter[0] > 0.) || (shutter[1] < 0.)) {
            motion.insert((shutter[0] > 0.) ? motion.begin() : motion.end(), std::make_pair(0., setup->request.frustum));
        }
        if (std::all_of(motion.begin(), motion.end(), [&motion](const std::pair<double, PXR_NS::GfFrustum>& s) { return s.second == motion.front().second; })) {
            motion.clear();
        }
    }

    return true;
}

bool
USDRender::readRenderSetup(double time,
                           ViewIdx view,
                           unsigned int level,
                           bool primIdPlane,
                           ActiveSettings* activeSettings,
                           RenderSetup* setup,
                           std::string* error)
{
    auto failWith = [error](const std::string& message) {
        *error = message;

        return false;
    };

    // The render draws every prim, and the camera fallback looks through all of them.
    const NodePtr node = getNode();
    ActiveSettings& active = *activeSettings;
    if (!readActiveSettings(time, view, eSettingsLoadEverything, &active, error, true)) {
        return false;
    }
    // The output's primId table, which the main thread is never given the whole stage to read. Only
    // a render of the plane it names stores it: it walks every prim, which an interactive colour
    // render would pay on every edit of the scene for nothing it draws.
    ImageMetadata primIds;
    const U64 primIdHash = primIdTableHash();
    if (node && primIdPlane && !findPrimIdTable(primIdHash, time, view, false, &primIds)) {
        primIds = primIdTableOf(active.stage, primIdPurposesOf(_renderPurpose, _proxyPurpose, _guidePurpose));
        if (primIdTableHash() == primIdHash) {
            storePrimIdTable(primIdHash, time, view, primIds);
        }
    }
    const PXR_NS::UsdStageRefPtr& stage = active.stage;
    const QString primPath = active.prim ? QString::fromStdString(active.prim.GetPath().GetString()) : QString();

    prepareLiveTextures(active, time, setup);
    auto addWarning = [setup](const std::string& warning) {
        setup->warning = setup->warning.empty() ? warning : setup->warning + "\n" + warning;
    };
    for (const SceneProjectionCameras::Unresolved& projection : active.lease.getUnresolvedProjections()) {
        addWarning(tr("%1 projects through %2, which the scene has no camera at: it projects nothing.")
                       .arg(QString::fromStdString(projection.shader.GetString()))
                       .arg(QString::fromStdString(projection.camera))
                       .toStdString());
    }

    RectI format;
    double par;
    if (active.prim) {
        const PXR_NS::GfVec2i& resolution = active.standard.resolution;
        if ((resolution[0] <= 0) || (resolution[1] <= 0) || (active.standard.pixelAspectRatio <= 0.f)) {
            return failWith(tr("The render settings prim at %1 asks for a %2x%3 image with a pixel aspect ratio of %4.")
                                .arg(primPath)
                                .arg(resolution[0])
                                .arg(resolution[1])
                                .arg(active.standard.pixelAspectRatio)
                                .toStdString());
        }
        format = RectI(0, 0, resolution[0], resolution[1]);
        par = active.standard.pixelAspectRatio;
    } else {
        Format projectFormat;
        getApp()->getProject()->getProjectDefaultFormat(&projectFormat);
        format = projectFormat;
        par = projectFormat.getPixelAspectRatio();
        addWarning(tr("The input scene has no active render settings prim: its first camera renders the project's "
                      "format with the renderer's own settings.")
                       .toStdString());
    }
    if (active.prim) {
        setup->request.settings = RenderSettingsPrim::settings(active.prim.GetPrim(), active.renderer, active.time);
    }
    const RectD canonical = Format(format, par).toCanonicalFormat();
    const RectI shown = canonical.toPixelEnclosing(level, par);
    setup->pixels = withOverscan(canonical, utilityOverscan(active.renderer, setup->request.settings), par).toPixelEnclosing(level, par);
    if (shown.isNull()) {
        return failWith(tr("The format is empty.").toStdString());
    }
    setup->request.overscanLeft = shown.x1 - setup->pixels.x1;
    setup->request.overscanBottom = shown.y1 - setup->pixels.y1;
    setup->request.overscanRight = setup->pixels.x2 - shown.x2;
    setup->request.overscanTop = setup->pixels.y2 - shown.y2;

    // A bake's texels look at the surface along its normals, so it needs no camera: one the scene
    // lacks is left out rather than failing an asset that has none.
    const bool bake = utilityUvUnwrap(active.renderer, setup->request.settings);
    PXR_NS::SdfPath cameraPath = active.prim ? active.standard.camera : PXR_NS::SdfPath();
    if (cameraPath.IsEmpty()) {
        const std::vector<PXR_NS::SdfPath> cameras = HydraRender::cameraPaths(stage);
        if (!cameras.empty()) {
            cameraPath = cameras.front();
        } else if (!bake) {
            return failWith(tr("The input scene has no camera to render through.").toStdString());
        }
    }
    if (bake && !cameraPath.IsEmpty() && !PXR_NS::UsdGeomCamera(stage->GetPrimAtPath(cameraPath))) {
        cameraPath = PXR_NS::SdfPath();
    }
    HydraRender::FitModeEnum fit = HydraRender::eFitModeExpand;
    if (active.prim && !fitModeForPolicy(active.standard.aspectRatioConformPolicy, &fit)) {
        addWarning(tr("The render settings prim at %1 asks for the %2 aspect ratio conform policy, which "
                      "the render does not do: the aperture is expanded instead (expandAperture).")
                       .arg(primPath)
                       .arg(QString::fromStdString(active.standard.aspectRatioConformPolicy.GetString()))
                       .toStdString());
    }
    setup->request.owner = this;
    setup->request.view = (int)view;
    setup->request.aborted = [this]() { return aborted(); };
    setup->request.scene = active.scene;
    setup->request.time = active.time;
    setup->request.width = setup->pixels.width();
    setup->request.height = setup->pixels.height();
    KnobBoolPtr headlightKnob = _headlight.lock();
    setup->request.headlight = headlightKnob && headlightKnob->getValue();
    KnobBoolPtr renderPurposeKnob = _renderPurpose.lock();
    KnobBoolPtr proxyPurposeKnob = _proxyPurpose.lock();
    KnobBoolPtr guidePurposeKnob = _guidePurpose.lock();
    setup->request.showRender = renderPurposeKnob && renderPurposeKnob->getValue();
    setup->request.showProxy = proxyPurposeKnob && proxyPurposeKnob->getValue();
    setup->request.showGuides = guidePurposeKnob && guidePurposeKnob->getValue();
    setup->request.renderer = active.renderer;
    setup->cameraPath = cameraPath;
    setup->cameraAspect = (format.width() * par) / format.height();
    setup->fit = fit;
    setup->settingsPrimPath = active.prim ? active.prim.GetPath() : PXR_NS::SdfPath();
    if (cameraPath.IsEmpty()) {
        setup->request.frustum = PXR_NS::GfFrustum();

        return true;
    }
    setup->spec = specAovs(time, view, &active);
    if (setup->spec && !setup->spec->error.empty()) {
        return failWith(setup->spec->error);
    }
    for (std::size_t i = 0; setup->spec && (i < setup->spec->aovs.size()); ++i) {
        setup->request.aovs.push_back(setup->spec->aovs[i].aov);
    }
    if (setup->spec) {
        setup->request.builtInAovs = setup->spec->builtInAovs;
    }
    if (setup->spec && !setup->spec->warning.empty()) {
        addWarning(setup->spec->warning);
    }
    setup->domes = RenderSettingsPrim::domeLights(stage);
    RenderSettingsPrim::addDomeLightCameraVisibility(stage, setup->domes, active.time, &setup->request.settings);

    return readSetupCamera(&active, setup, error);
} // USDRender::readRenderSetup

StatusEnum
USDRender::render(const RenderActionArgs& args)
{
    const utilityProfile::Span timed("usdrender_region");
    auto fail = [this](const std::string& message) {
        setPersistentMessage(eMessageTypeError, message);

        return eStatusFailed;
    };

    RenderSetup setup;
    std::string error;
    bool primIdPlane = false;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        primIdPlane = primIdPlane || (it->second && (it->first.getLayerID() == "primId"));
    }
    if (!prepareRender(args.time, args.view, args.mappedScale.toMipmapLevel(), primIdPlane, &setup, &error)) {
        return fail(error);
    }
    HydraRender::Request& request = setup.request;
    request.aovPixels = std::set<PXR_NS::TfToken>();
    for (std::size_t i = 0; setup.spec && (i < setup.spec->aovs.size()); ++i) {
        for (const std::pair<ImageLayerDesc, ImagePtr>& output : args.outputLayers) {
            if (output.second && (output.first.getLayerID() == setup.spec->aovs[i].layer.getLayerID())) {
                request.aovPixels->insert(setup.spec->aovs[i].aov.name);
            }
        }
    }
    const RectI& pixels = setup.pixels;
    const std::vector<std::string> liveTextureUris = setup.liveTextureUris;
    // A texture that cannot reach the renderer for want of room in shared memory stops the render
    // as soon as the delegate asks for it, rather than at its end.
    const auto noRoom = [liveTextureUris]() {
        for (const std::string& uri : liveTextureUris) {
            const std::string message = LiveTextureStore::instance().getNoRoom(uri);
            if (!message.empty()) {
                return message;
            }
        }

        return std::string();
    };
    request.aborted = [this, noRoom]() { return aborted() || !noRoom().empty(); };
    request.interactive = rendersForViewer();
    request.threads = appPTR->getNCPUsAvailableForEffect();
    request.planes = 0;
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        if (!it->second) {
            continue;
        }
        const std::string& layerID = it->first.getLayerID();
        request.planes |= it->first.isColorLayer() ? (1u << HydraRender::ePlaneColor) : (layerID == "depth") ? (1u << HydraRender::ePlaneDepth)
            : (layerID == "primId")                                                                          ? (1u << HydraRender::ePlanePrimId)
            : (layerID == "normal")                                                                          ? (1u << HydraRender::ePlaneNormal)
                                                                                                             : 0u;
    }

    HydraRender::Result result;
    const bool rendered = HydraRender::render(request, &result, &error);
    const std::string noRoomMessage = noRoom();
    if (!noRoomMessage.empty()) {
        return fail(noRoomMessage);
    }
    if (!rendered) {
        // A cancelled render is no error of the node's.
        if (aborted()) {
            return eStatusFailed;
        }

        return fail(error);
    }

    const std::size_t width = (std::size_t)pixels.width();
    const float infinity = std::numeric_limits<float>::infinity();
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        const ImagePtr& image = it->second;
        if (!image) {
            continue;
        }
        if (image->getBitDepth() != eImageBitDepthFloat) {
            return eStatusFailed;
        }
        enum PlaneEnum {
            ePlaneColor,
            ePlaneDepth,
            ePlanePrimId,
            ePlaneNormal,
            ePlaneOther
        };
        const std::string& layerID = it->first.getLayerID();
        // A RenderVar can render to a layer named like a fixed plane its renderer does not give (Storm's normal).
        bool specLayer = false;
        for (std::size_t i = 0; setup.spec && !specLayer && (i < setup.spec->aovs.size()); ++i) {
            specLayer = (setup.spec->aovs[i].layer.getLayerID() == layerID);
        }
        const PlaneEnum plane = it->first.isColorLayer() ? ePlaneColor : specLayer ? ePlaneOther
            : (layerID == "depth")                                                 ? ePlaneDepth
            : (layerID == "primId")                                                ? ePlanePrimId
            : (layerID == "normal")                                                ? ePlaneNormal
                                                                                   : ePlaneOther;
        const HydraRender::AovPlane* aov = nullptr;
        for (std::size_t i = 0; setup.spec && (plane == ePlaneOther) && !aov && (i < setup.spec->aovs.size()); ++i) {
            if (setup.spec->aovs[i].layer.getLayerID() != layerID) {
                continue;
            }
            for (const HydraRender::AovPlane& rendered : result.aovs) {
                if (rendered.name == setup.spec->aovs[i].aov.name) {
                    aov = &rendered;
                }
            }
        }
        const std::size_t aovChannels = aov ? (std::size_t)aov->channels : 0;
        const int nComps = (int)image->getComponentsCount();
        const RectI roi = args.roi.intersect(image->getBounds());
        if (roi.isNull()) {
            continue;
        }
        Image::WriteAccess access(image.get());
        // The part of the window's rows the render covers, which an RGBA colour plane takes as it is.
        const int insideX1 = std::max(roi.x1, pixels.x1);
        const int insideX2 = std::min(roi.x2, pixels.x2);
        const bool asIs = (plane == ePlaneColor) && (nComps == 4) && (insideX1 < insideX2);
        parallelForOnGlobalPool(roi.y2 - roi.y1, std::max(1, request.threads), [&](int r) {
            const int y = roi.y1 + r;
            float* row = reinterpret_cast<float*>(access.pixelAt(roi.x1, y));
            if (!row) {
                return;
            }
            if (asIs && (y >= pixels.y1) && (y < pixels.y2)) {
                const std::size_t rowStart = (std::size_t)(y - pixels.y1) * width;
                std::fill(row, row + (std::size_t)(insideX1 - roi.x1) * 4, 0.f);
                std::memcpy(row + (std::size_t)(insideX1 - roi.x1) * 4,
                            result.rgba.data() + (rowStart + (std::size_t)(insideX1 - pixels.x1)) * 4,
                            (std::size_t)(insideX2 - insideX1) * 4 * sizeof(float));
                std::fill(row + (std::size_t)(insideX2 - roi.x1) * 4, row + (std::size_t)(roi.x2 - roi.x1) * 4, 0.f);

                return;
            }
            for (int x = roi.x1; x < roi.x2; ++x) {
                float* out = row + (std::size_t)(x - roi.x1) * (std::size_t)nComps;
                const bool inside = pixels.contains(x, y);
                const std::size_t index = inside ? (std::size_t)(y - pixels.y1) * width + (std::size_t)(x - pixels.x1) : 0;
                for (int c = 0; c < nComps; ++c) {
                    float value = 0.f;
                    switch (plane) {
                    case ePlaneColor: {
                        // A one-channel colour plane is alpha.
                        const int channel = (nComps == 1) ? 3 : c;
                        value = (inside && (channel < 4)) ? result.rgba[index * 4 + (std::size_t)channel] : 0.f;
                        break;
                    }
                    case ePlaneDepth:
                        value = inside ? result.depth[index] : infinity;
                        break;
                    case ePlanePrimId:
                        value = inside ? result.primId[index] : 0.f;
                        break;
                    case ePlaneNormal:
                        value = (inside && (c < 3) && !result.normal.empty()) ? result.normal[index * 3 + (std::size_t)c] : 0.f;
                        break;
                    case ePlaneOther:
                        value = (inside && aov && ((std::size_t)c < aovChannels)) ? aov->pixels[index * aovChannels + (std::size_t)c] : 0.f;
                        break;
                    }
                    out[c] = value;
                }
            }
        });
    }
    std::string warning = setup.warning;
    const std::string aovWarning = followRenderedAovs(getRenderHash(), args.time, args.view, setup, result);
    if (!aovWarning.empty()) {
        warning = warning.empty() ? aovWarning : warning + "\n" + aovWarning;
    }
    showRenderMessages(warning, result.warning, result.errors, result.device);

    return eStatusOK;
} // USDRender::render

std::string
USDRender::followRenderedAovs(U64 hash,
                              double time,
                              ViewIdx view,
                              const RenderSetup& setup,
                              const HydraRender::Result& result)
{
    const std::shared_ptr<const SpecAovs>& spec = setup.spec;
    if (!spec) {
        return std::string();
    }
    QStringList rejected;
    for (const RenderSettingsPrim::RejectedVar& var : spec->rejected) {
        rejected.push_back(tr("%1 (%2)").arg(QString::fromStdString(var.path.GetString())).arg(QString::fromStdString(var.reason)));
    }
    QStringList formatWarnings;
    bool layersChanged = false;
    std::map<PXR_NS::TfToken, std::string> layers = spec->builtInLayers;
    {
        std::lock_guard<std::mutex> l(_specMutex);
        for (const SpecAovs::Entry& entry : spec->aovs) {
            const std::pair<std::string, std::string> key(spec->renderer, entry.aov.name.GetString());
            const bool refused = std::find(result.rejectedAovs.begin(), result.rejectedAovs.end(), entry.aov.name) != result.rejectedAovs.end();
            if (refused) {
                rejected.push_back(tr("%1 (the renderer gives no such AOV)").arg(QString::fromStdString(entry.path.GetString())));
                layersChanged = _refusedAovs.insert(key).second || layersChanged;
                continue;
            }
            layersChanged = (_refusedAovs.erase(key) > 0) || layersChanged;
            layers[entry.aov.name] = entry.layer.getLayerID();
            for (const HydraRender::AovPlane& plane : result.aovs) {
                if (plane.name != entry.aov.name) {
                    continue;
                }
                // The layer was declared before the renderer's format was known; the format can leave it as it is (texCoord2f's two channels of Cycles' three).
                // A spec declared with this format already warns of a dataType it overrides.
                std::string formatWarning;
                const AovLayers::Layout rendered = AovLayers::layout(spec->renderer, entry.aov.name.GetString(), entry.dataType.GetString(), plane.channels, (plane.channels != entry.formatChannels) ? &formatWarning : nullptr);
                if (!formatWarning.empty()) {
                    formatWarnings.push_back(tr("The RenderVar %1: %2").arg(QString::fromStdString(entry.path.GetString())).arg(QString::fromStdString(formatWarning)));
                }
                if (AovLayers::channelNames(entry.aov.name.GetString(), rendered) != entry.layer.getChannels()) {
                    layersChanged = true;
                }
            }
        }
    }
    ImageMetadata metadata;
    for (const std::pair<const PXR_NS::TfToken, PXR_NS::VtDictionary>& aov : result.aovMetadata) {
        std::map<PXR_NS::TfToken, std::string>::const_iterator layer = layers.find(aov.first);
        if (layer == layers.end()) {
            continue;
        }
        for (const std::pair<const std::string, PXR_NS::VtValue>& value : aov.second) {
            setMetadataValue(&metadata, std::string(kUSDRenderMetadataAovPrefix) + layer->second + "/" + value.first, value.second);
        }
    }
    bool metadataChanged = false;
    {
        std::lock_guard<std::mutex> l(_specMutex);
        ImageMetadata& stored = _aovMetadata.get(HashedFrameKey(hash, FrameKey(time, (int)view)));
        if (stored != metadata) {
            stored = metadata;
            metadataChanged = true;
        }
    }
    if (layersChanged) {
        clearSpecAovs();
        // A kept setup holds the spec it was read with, which a reuse would render again under the
        // same key: it would declare the old layers and be found changed on every render after.
        const std::lock_guard<std::mutex> l(_renderSetupsMutex);
        _renderSetups.clear();
    }
    // The layers and metadata change under the same hash, and the readers are refreshed on the main thread.
    if (layersChanged || metadataChanged) {
        QMetaObject::invokeMethod(this, [this, layersChanged]() {
            invalidateOutputMetadata();
            if (layersChanged) {
                // The layers moved under an unchanged hash, so nothing else re-lists the layer menus downstream, the viewers' among them.
                refreshMetadataSideEffects();
            } }, Qt::QueuedConnection);
    }
    if (!rejected.isEmpty()) {
        formatWarnings.push_back(tr("No layer renders for the RenderVars %1.").arg(rejected.join(QString::fromUtf8(", "))));
    }

    return formatWarnings.join(QString::fromUtf8("\n")).toStdString();
} // USDRender::followRenderedAovs

void
USDRender::showRenderMessages(const std::string& setupWarning,
                              const std::string& resultWarning,
                              const std::string& errors,
                              const std::string& device)
{
    // The render stands; what the delegate could not render, or logged as an error naming no prim
    // of the scene, stays on the node until a render without it.
    std::string warning = setupWarning;
    if (!resultWarning.empty()) {
        warning = warning.empty() ? resultWarning : warning + "\n" + resultWarning;
    }
    const std::string rendered = device.empty() ? std::string() : "Rendered on " + device + ".";
    if (!errors.empty()) {
        setPersistentMessage(eMessageTypeError, warning.empty() ? errors : errors + "\n" + warning);
    } else if (!warning.empty()) {
        showWarning(rendered.empty() ? warning : warning + "\n" + rendered);
    } else if (!rendered.empty()) {
        // A posted information message only reaches the log in the GUI and leaves the last
        // render's warning standing; the quiet one replaces it.
        if (const NodePtr node = getNode()) {
            node->setQuietInfoMessage(rendered);
        }
    } else {
        showWarning(warning);
    }
}

std::string
USDRender::utilityDevice(double time,
                         ViewIdx view) const
{
    const SettingsSummary summary = getSettingsSummary(time, view);

    return (summary.read && !summary.utilityDevice.empty()) ? summary.utilityDevice : std::string(kUtilityDeviceAuto);
}

void
USDRender::showWarning(const std::string& warning)
{
    if (warning.empty()) {
        clearPersistentMessage(false);
    } else {
        setPersistentMessage(eMessageTypeWarning, warning);
    }
}

std::optional<OpKind>
USDRender::opKind() const
{
    return OpKind::Custom;
}

bool
USDRender::readsGpuInput(int /*inputNb*/,
                         const ImageLayerDesc& /*plane*/,
                         double /*time*/,
                         ViewIdx /*view*/) const
{
    // The scene is evaluated by renderGpu() itself, not handed over as an image.
    return false;
}

bool
USDRender::canRenderOnGpu(const GpuQuery& query) const
{
#ifdef NATRON_HAVE_GPU
    const std::string& layerID = query.plane.getLayerID();
    if (query.bitDepth != eImageBitDepthFloat) {
        return refuseGpu("a frame was asked at a depth other than float");
    }
    if (!query.plane.isColorLayer() && (layerID != "depth") && (layerID != "primId")) {
        return refuseGpu("the " + layerID + " layer has no GPU path");
    }
    std::string renderer;
    std::string error;
    if (!activeRenderer(query.time, query.view, &renderer, &error)) {
        return false;
    }
    // Natron Utility keeps a GPU frame's colour, depth and ids on the device, where renderGpu() copies them from.
    if (renderer == kRenderDelegateUtilityId) {
        if (utilityDevice(query.time, query.view) == kUtilityDeviceCpu) {
            return false;
        }
        // Asked while the frame is being planned, so it neither waits for the GPU nor recovers a lost one:
        // the render itself does both, and a frame asked before then renders on the CPU.
        GpuContext* context = appPTR ? appPTR->getGpuContext() : nullptr;
        if (!context || context->isInitializing() || !context->isAvailable()) {
            return refuseGpu("Natron Utility found GPU compute unavailable or still starting");
        }
        const std::shared_ptr<void> hold = context->holdRuntime();
        gpu::GpuRuntime* runtime = hold ? context->runtime() : nullptr;
        if (!runtime || runtime->isLost()) {
            return refuseGpu("Natron Utility found no GPU runtime, or a lost one");
        }
        const std::string missing = WavefrontGpu::missingSupport(*runtime);
        if (!missing.empty()) {
            return refuseGpu("Natron Utility's GPU lacks " + missing);
        }
        if (HydraRender::deviceFrameFellBack(this, context->generation(), utilityGpuBudget(), utilityDevice(query.time, query.view))) {
            return refuseGpu("Natron Utility's last GPU frame fell back to the CPU on this device and budget");
        }

        return true;
    }
    // Only Storm renders into GL; the other delegates' host pixels are read back by render().
    if (!RenderDelegates::isStorm(renderer)) {
        return refuseGpu("the " + renderer + " renderer has no GPU path");
    }
    if (HydraHandoff::instance().path() == gpu::GlHandoffPath::Readback) {
        return refuseGpu("Storm's frames are handed over by readback");
    }

    return true;
#else
    Q_UNUSED(query);

    return false;
#endif
}

GpuWeight
USDRender::gpuWeight(double time,
                     ViewIdx view,
                     unsigned int mipmapLevel) const
{
    std::string renderer;
    std::string error;
    if (activeRenderer(time, view, &renderer, &error) && (renderer == kRenderDelegateUtilityId)) {
        return GpuWeight::resident();
    }

    return NativeEffectBase::gpuWeight(time, view, mipmapLevel);
}

bool
USDRender::rendersForViewer() const
{
    const ParallelRenderArgsPtr args = getFrameArgs();

    if (!args) {
        return false;
    }

    const NodePtr root = args->treeRoot;

    return args->isRenderResponseToUserInteraction || (root && root->isEffectViewer());
}

StatusEnum
USDRender::renderGpu(const GpuRenderArgs& args)
{
#ifdef NATRON_HAVE_GPU
    const utilityProfile::Span timed("usdrender_region");
    // A failure here leaves no message: the frame renders again on the CPU, which reports it.
    RenderSetup setup;
    std::string error;
    if (!prepareRender(args.query.time, args.query.view, args.query.mappedScale.toMipmapLevel(), args.query.plane.getLayerID() == "primId", &setup, &error)) {
        return eStatusFailed;
    }
    const ImageLayerDesc& plane = args.query.plane;
    HydraRender::PlaneWindow window;
    window.plane = plane.isColorLayer() ? HydraRender::ePlaneColor : (plane.getLayerID() == "depth") ? HydraRender::ePlaneDepth
                                                                                                     : HydraRender::ePlanePrimId;
    window.nComps = args.output.nComps;
    window.x1 = args.outputRect.x1 - setup.pixels.x1;
    window.y1 = args.outputRect.y1 - setup.pixels.y1;
    window.x2 = args.outputRect.x2 - setup.pixels.x1;
    window.y2 = args.outputRect.y2 - setup.pixels.y1;
    if (setup.request.renderer == kRenderDelegateUtilityId) {
        setup.request.aborted = [this]() { return aborted(); };
        setup.request.threads = appPTR->getNCPUsAvailableForEffect();
        setup.request.interactive = rendersForViewer();
        if (args.refinement) {
            setup.request.refine = setup.request.interactive;
            setup.request.sequence = args.refinement->getRenderSequence();
        }
        HydraRender::Result info;
        if (!HydraRender::recordDeviceWindow(setup.request, window, args.context, args.output, args.outputRect, &info, &error)) {
            return eStatusFailed;
        }
        if (info.partial && args.refinement) {
            args.refinement->markRefining();
        }
        showRenderMessages(setup.warning, info.warning, info.errors, info.device);

        return eStatusOK;
    }
    if (!HydraHandoff::instance().record(setup.request, window, args.context, args.output, args.outputRect, &error)) {
        return eStatusFailed;
    }
    showWarning(setup.warning);

    return eStatusOK;
#else
    Q_UNUSED(args);

    return eStatusFailed;
#endif
}

NATRON_NAMESPACE_EXIT
