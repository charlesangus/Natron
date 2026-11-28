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
#include <cstdio>
#include <cstring>
#include <limits>
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
#include "Engine/Nodes/Scene/CyclesAovs.h"
#include "Engine/Nodes/Scene/HydraHandoff.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/LiveTextureStore.h"
#include "Engine/Nodes/Scene/MoonRayAovs.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/RenderSettingsPrim.h"
#include "Engine/Nodes/Scene/SceneNodeAuthoring.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/Nodes/Scene/Utility/UtilityProfile.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderDelegate.h"
#include "Engine/Nodes/Scene/Utility/WavefrontGpu.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/PoolParallelFor.h"
#include "Engine/SceneLoadSet.h"

#include <pxr/usd/usd/primRange.h>

#include "Engine/Project.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_ENTER

namespace {
ImageLayerDesc
depthLayer()
{
    return ImageLayerDesc("depth", std::vector<std::string>(1, "Z"));
}

ImageLayerDesc
primIdLayer()
{
    return ImageLayerDesc("primId", std::vector<std::string>(1, "id"));
}

ImageLayerDesc
normalLayer()
{
    static const char* const channels[] = { "x", "y", "z" };

    return ImageLayerDesc("normal", std::vector<std::string>(channels, channels + 3));
}

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
    HydraRender::release(this);
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
};

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
    } else {
        // Only the settings prim is read, as the stage metadata naming the active prim needs no payload.
        const SceneLoadSetPtr nothing = std::make_shared<const SceneLoadSet>(SceneLoadSet::none());
        std::string primPath = chosenPrim;
        if (!evaluate(_settingsNothingContextId, nothing) || !borrowStage(nothing)) {
            return false;
        }
        if (primPath.empty() && active->stage->HasAuthoredMetadata(PXR_NS::UsdRenderTokens->renderSettingsPrimPath)) {
            active->stage->GetMetadata(PXR_NS::UsdRenderTokens->renderSettingsPrimPath, &primPath);
        }
        // The summary reads the prim's own attributes; the camera it targets is for the render to open.
        if ((load == eSettingsLoadSettingsPrim) && !primPath.empty() && PXR_NS::SdfPath::IsValidPathString(primPath)) {
            const SceneLoadSetPtr primOnly = SceneLoadSet::withPaths(nothing, { primPath });
            if (!evaluate(_settingsPrimContextId, primOnly) || !borrowStage(primOnly)) {
                return false;
            }
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
    SettingsSummary summary;
    ActiveSettings active;

    summary.read = readActiveSettings(time, view, load, &active, &summary.error);
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
        const SettingsSummary summary = summarizeSettings(time, view, eSettingsLoadSettingsPrim);
        storeSettingsSummary(hash, time, view, summary);

        return summary;
    }
    const_cast<USDRender*>(this)->markOutputMetadataProvisional();
    if (!post) {
        return provisional;
    }
    // Never more than one posted job: one that replaced another would drop it, and the drop
    // would be taken for the node's end.
    const std::weak_ptr<const EffectInstance> weakThis = weak_from_this();
    _settingsWorker.post([weakThis]() {
        std::shared_ptr<const EffectInstance> effect = weakThis.lock();
        if (const USDRender* render = dynamic_cast<const USDRender*>(effect.get())) {
            render->readGivenSettingsSummaries();
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

    return provisional;
} // USDRender::getSettingsSummary

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
        storeSettingsSummary(hash, key.first, ViewIdx(key.second), summarizeSettings(key.first, ViewIdx(key.second), eSettingsLoadSettingsPrim));
    }
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
    for (const std::string& ns : RenderSettingsPrim::namespaces(renderer)) {
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
    std::string renderer;
    std::string error;
    if (!activeRenderer(time, view, &renderer, &error)) {
        renderer = kRenderDelegateStormId;
    }
    if (!RenderDelegates::isStorm(renderer) && (renderer != kRenderDelegateUtilityId)) {
        produced.push_back(normalLayer());
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
    const U64 hash = primIdTableHash();
    const bool mainThread = onMainThread();

    if (findPrimIdTable(hash, time, view, mainThread, &metadata)) {
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
    metadata = primIdTableOf(stage, primIdPurposesOf(_renderPurpose, _proxyPurpose, _guidePurpose));
    // A table of a scene that moved while it was read must not be served under the newer hash.
    if (primIdTableHash() == hash) {
        storePrimIdTable(hash, time, view, metadata);
    }

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

bool
USDRender::RenderSetupKey::operator==(const RenderSetupKey& other) const
{
    return (sceneHash == other.sceneHash) && (time == other.time) && (view == other.view) && (level == other.level) && (renderer == other.renderer) && (knobs == other.knobs) && (project == other.project);
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
    if (keyed) {
        std::shared_ptr<const RenderSetup> kept;
        {
            const std::lock_guard<std::mutex> l(_renderSetupsMutex);
            for (std::list<KeptRenderSetup>::iterator it = _renderSetups.begin(); it != _renderSetups.end(); ++it) {
                if (it->first == key) {
                    kept = it->second;
                    _renderSetups.splice(_renderSetups.begin(), _renderSetups, it);
                    break;
                }
            }
        }
        // The primId table is stored by a render that reads the stage, which a reused setup does not.
        ImageMetadata primIds;
        if (kept && (!primIdPlane || findPrimIdTable(primIdTableHash(), time, view, false, &primIds))) {
            ++gRenderSetupReuses;
            *setup = *kept;
            setup->request.owner = this;
            setup->request.aborted = [this]() { return aborted(); };
            setup->request.scene = active.scene;
            prepareLiveTextures(active, time, setup);

            return true;
        }
    }
    ++gRenderSetupReads;
    if (!readRenderSetup(time, view, level, primIdPlane, &active, setup, error)) {
        return false;
    }
    // Stored only when nothing the key covers moved during the read, so that it is never kept under
    // a key it was not read for.
    RenderSetupKey after;
    if (keyed && renderSetupKey(time, view, level, active, &after) && (after == key)) {
        std::shared_ptr<RenderSetup> keep = std::make_shared<RenderSetup>(*setup);
        keep->request.aborted = std::function<bool()>();
        keep->request.scene = ScenePayload();
        const std::lock_guard<std::mutex> l(_renderSetupsMutex);
        _renderSetups.remove_if([&key](const KeptRenderSetup& kept) { return kept.first == key; });
        _renderSetups.emplace_front(key, std::move(keep));
        while (_renderSetups.size() > kRenderSetupsKept) {
            _renderSetups.pop_back();
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
    if (cameraPath.IsEmpty()) {
        setup->request.frustum = PXR_NS::GfFrustum();

        return true;
    }
    RenderSettingsPrim::addDomeLightCameraVisibility(stage, RenderSettingsPrim::domeLights(stage), active.time, &setup->request.settings);
    const double aspect = (format.width() * par) / format.height();
    if (!HydraRender::cameraFrustum(stage, cameraPath, active.time, aspect, fit, &setup->request.frustum, error)) {
        return false;
    }
    const PXR_NS::UsdGeomCamera camera(stage->GetPrimAtPath(cameraPath));
    double shutterOpen = 0.;
    double shutterClose = 0.;
    (void)camera.GetShutterOpenAttr().Get(&shutterOpen, active.time);
    (void)camera.GetShutterCloseAttr().Get(&shutterClose, active.time);
    setup->request.shutterOpen = shutterOpen;
    setup->request.shutterClose = shutterClose;
    if (!active.time.IsDefault() && HydraRender::cameraMotionBlur(setup->request)) {
        // Between these the delegate interpolates the camera, so a few follow a curved move closely.
        const int kCameraMotionSamples = 9;
        const PXR_NS::GfVec2d shutter = HydraRender::shutterInterval(setup->request);
        for (int i = 0; i < kCameraMotionSamples; ++i) {
            const double t = shutter[0] + (shutter[1] - shutter[0]) * i / (kCameraMotionSamples - 1);
            PXR_NS::GfFrustum frustum;
            if (!HydraRender::cameraFrustum(stage, cameraPath, PXR_NS::UsdTimeCode(active.time.GetValue() + t), aspect, fit, &frustum, error)) {
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
        const PlaneEnum plane = it->first.isColorLayer() ? ePlaneColor : (layerID == "depth") ? ePlaneDepth
            : (layerID == "primId")                                                           ? ePlanePrimId
            : (layerID == "normal")                                                           ? ePlaneNormal
                                                                                              : ePlaneOther;
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
                        break;
                    }
                    out[c] = value;
                }
            }
        });
    }
    showRenderMessages(setup.warning, result.warning, result.errors, result.device);

    return eStatusOK;
} // USDRender::render

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
