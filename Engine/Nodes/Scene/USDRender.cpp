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
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <QMetaObject>
#include <QStringList>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/ContentHasher.h"
#include "Engine/Format.h"
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
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderDelegate.h"
#include "Engine/Nodes/Scene/Utility/WavefrontGpu.h"

#include <pxr/usd/usd/primRange.h>

#include "Engine/Project.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_ENTER

namespace {
ImageLayerDesc
depthLayer()
{
    return ImageLayerDesc("depth", "depth", "", std::vector<std::string>(1, "Z"));
}

ImageLayerDesc
primIdLayer()
{
    return ImageLayerDesc("primId", "primId", "", std::vector<std::string>(1, "id"));
}

ImageLayerDesc
normalLayer()
{
    static const char* const channels[] = { "X", "Y", "Z" };

    return ImageLayerDesc("normal", "normal", "", std::vector<std::string>(channels, channels + 3));
}

std::string
primIdKey(std::uint32_t hash)
{
    char hex[9];
    std::snprintf(hex, sizeof(hex), "%08x", hash);

    return std::string(kUSDRenderMetadataPrimIdPrefix) + hex;
}
} // anon namespace

USDRender::USDRender(NodePtr node)
    : NativeEffectBase(node)
    , _settingsPrimMenuRefreshPending(false)
{
}

USDRender::~USDRender()
{
    HydraRender::release(this);
}

void
USDRender::purgeCaches()
{
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

void
USDRender::refreshSettingsPrimMenu()
{
    KnobChoicePtr settingsPrim = _settingsPrim.lock();

    if (!settingsPrim) {
        return;
    }
    std::vector<ChoiceOption> options(1, ChoiceOption(kUSDRenderSettingsPrimStageDefault, tr("Stage Default").toStdString(), ""));
    EffectInstancePtr input = getInput(0);
    ScenePayload scene;
    std::string error;
    if (input && (input->evaluateScene(EvaluateSceneArgs(getCurrentTime(), ViewIdx(0)), &scene, &error) == eRenderRoIRetCodeOk) && !scene.isEmpty()) {
        SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(scene, &error);
        const PXR_NS::UsdStageRefPtr stage = lease.getStage();
        const PXR_NS::UsdPrim render = stage ? stage->GetPrimAtPath(PXR_NS::SdfPath("/Render")) : PXR_NS::UsdPrim();
        if (render) {
            for (const PXR_NS::UsdPrim& prim : PXR_NS::UsdPrimRange(render)) {
                if (prim.IsA<PXR_NS::UsdRenderSettings>()) {
                    const std::string path = prim.GetPath().GetString();
                    options.push_back(ChoiceOption(path, path, ""));
                }
            }
        }
    }
    // Repopulating an unchanged menu would move the hash and come back here.
    const std::vector<ChoiceOption> current = settingsPrim->getEntries_mt_safe();
    bool same = current.size() == options.size();
    for (std::size_t i = 0; same && (i < options.size()); ++i) {
        same = current[i].id == options[i].id;
    }
    if (!same) {
        settingsPrim->populateChoices(options);
    }
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
USDRender::readActiveSettings(double time,
                              ViewIdx view,
                              ActiveSettings* active,
                              std::string* error) const
{
    auto failWith = [error](const std::string& message) {
        *error = message;

        return false;
    };

    EffectInstancePtr input = getInput(0);
    if (!input) {
        return failWith(tr("USDRender needs a scene on its Scene input.").toStdString());
    }
    if (input->evaluateScene(EvaluateSceneArgs(time, view), &active->scene, error) != eRenderRoIRetCodeOk) {
        if (error->empty()) {
            *error = tr("The input scene could not be evaluated.").toStdString();
        }

        return false;
    }
    if (active->scene.isEmpty()) {
        return failWith(tr("The input scene is empty: there is no camera to render through.").toStdString());
    }
    active->lease = SceneQueryPool::instance().borrow(active->scene, error);
    active->stage = active->lease.getStage();
    if (!active->stage) {
        if (error->empty()) {
            *error = tr("USD could not compose the input scene.").toStdString();
        }

        return false;
    }
    AppInstancePtr app = getApp();
    const double fps = (app && app->getProject()) ? app->getProject()->getProjectFrameRate() : 24.;
    active->time = PXR_NS::UsdTimeCode(SceneOps::frameToTimeCode(active->scene, time, fps));

    std::string primError;
    active->prim = RenderSettingsPrim::activePrim(active->stage, settingsPrimOverride(), &primError);
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
    ActiveSettings active;
    if (!readActiveSettings(time, view, &active, error)) {
        return false;
    }
    *rendererId = active.renderer;

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
                        double* par) const
{
    ActiveSettings active;
    std::string error;

    if (readActiveSettings(time, view, &active, &error) && active.prim) {
        const int width = active.standard.resolution[0];
        const int height = active.standard.resolution[1];
        const double aspect = active.standard.pixelAspectRatio;
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
    metadata.setComponentsType(-1, kNatronColorLayerID);
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

    outputFormat(time, view, &format, &par);
    *rod = Format(format, par).toCanonicalFormat();

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

ImageMetadata
USDRender::deriveOutputMetadata(double time,
                                ViewIdx view)
{
    // Every prim Storm can draw, so that each primId.id value a render gives has its entry.
    ImageMetadata metadata;
    EffectInstancePtr input = getInput(0);
    ScenePayload scene;
    std::string error;

    if (!input || (input->evaluateScene(EvaluateSceneArgs(time, view), &scene, &error) != eRenderRoIRetCodeOk) || scene.isEmpty()) {
        return metadata;
    }
    SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(scene, &error);
    PXR_NS::UsdStageRefPtr stage = lease.getStage();
    if (!stage) {
        return metadata;
    }
    // Point instancer prototypes are commonly authored as class or over prims, which the default traversal skips but Storm draws
    // through the instancer, so every prim is visited.
    const auto isOn = [](const KnobBoolWPtr& weak) {
        const KnobBoolPtr knob = weak.lock();

        return knob && knob->getValue();
    };
    const bool showRender = isOn(_renderPurpose);
    const bool showProxy = isOn(_proxyPurpose);
    const bool showGuides = isOn(_guidePurpose);
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
        metadata.setString(primIdKey(HydraRender::primPathHash(path)), path);
    }

    return metadata;
}

struct USDRender::RenderSetup {
    HydraRender::Request request;
    // The render's pixels in the output image.
    RectI pixels;
    // What renders but not as asked, shown once the render succeeds.
    std::string warning;
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

bool
USDRender::prepareRender(double time,
                         ViewIdx view,
                         unsigned int level,
                         RenderSetup* setup,
                         std::string* error)
{
    auto failWith = [error](const std::string& message) {
        *error = message;

        return false;
    };

    ActiveSettings active;
    if (!readActiveSettings(time, view, &active, error)) {
        return false;
    }
    const PXR_NS::UsdStageRefPtr& stage = active.stage;
    const QString primPath = active.prim ? QString::fromStdString(active.prim.GetPath().GetString()) : QString();

    // Hydra reads live textures from the store and never renders them: render the frame's first.
    LiveTextureStore::instance().prepare(SceneOps::getLiveTextures(active.scene), time);
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
    setup->pixels = Format(format, par).toCanonicalFormat().toPixelEnclosing(level, par);
    if (setup->pixels.isNull()) {
        return failWith(tr("The format is empty.").toStdString());
    }

    PXR_NS::SdfPath cameraPath = active.prim ? active.standard.camera : PXR_NS::SdfPath();
    if (cameraPath.IsEmpty()) {
        const std::vector<PXR_NS::SdfPath> cameras = HydraRender::cameraPaths(stage);
        if (cameras.empty()) {
            return failWith(tr("The input scene has no camera to render through.").toStdString());
        }
        cameraPath = cameras.front();
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
    if (active.prim) {
        setup->request.settings = RenderSettingsPrim::settings(active.prim.GetPrim(), active.renderer, active.time);
    }
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
} // USDRender::prepareRender

StatusEnum
USDRender::render(const RenderActionArgs& args)
{
    auto fail = [this](const std::string& message) {
        setPersistentMessage(eMessageTypeError, message);

        return eStatusFailed;
    };

    RenderSetup setup;
    std::string error;
    if (!prepareRender(args.time, args.view, args.mappedScale.toMipmapLevel(), &setup, &error)) {
        return fail(error);
    }
    HydraRender::Request& request = setup.request;
    const RectI& pixels = setup.pixels;
    request.aborted = [this]() { return aborted(); };
    request.threads = appPTR->getNCPUsAvailableForEffect();

    HydraRender::Result result;
    if (!HydraRender::render(request, &result, &error)) {
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
        for (int y = roi.y1; y < roi.y2; ++y) {
            float* row = reinterpret_cast<float*>(access.pixelAt(roi.x1, y));
            if (!row) {
                continue;
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
        }
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
        setPersistentMessage(eMessageTypeInfo, rendered);
    } else {
        showWarning(warning);
    }
}

std::string
USDRender::utilityDevice(double time,
                         ViewIdx view) const
{
    ActiveSettings active;
    std::string error;
    if (!readActiveSettings(time, view, &active, &error) || !active.prim) {
        return kUtilityDeviceAuto;
    }
    const RenderSettingsPrim::Values values = RenderSettingsPrim::settings(active.prim.GetPrim(), kRenderDelegateUtilityId, active.time);
    const RenderSettingsPrim::Values::const_iterator found = values.find(kUtilitySettingDevice);
    if ((found == values.end()) || !found->second.IsHolding<PXR_NS::TfToken>()) {
        return kUtilityDeviceAuto;
    }

    return found->second.UncheckedGet<PXR_NS::TfToken>().GetString();
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
    if ((query.bitDepth != eImageBitDepthFloat) || (!query.plane.isColorLayer() && (layerID != "depth") && (layerID != "primId"))) {
        return false;
    }
    std::string renderer;
    std::string error;
    if (!activeRenderer(query.time, query.view, &renderer, &error)) {
        return false;
    }
    // Natron Utility keeps a GPU frame's colour on the device, where renderGpu() copies it from.
    if (renderer == kRenderDelegateUtilityId) {
        if (!query.plane.isColorLayer() || (utilityDevice(query.time, query.view) == kUtilityDeviceCpu)) {
            return false;
        }
        // Asked while the frame is being planned, so it neither waits for the GPU nor recovers a lost one:
        // the render itself does both, and a frame asked before then renders on the CPU.
        GpuContext* context = appPTR ? appPTR->getGpuContext() : nullptr;
        if (!context || context->isInitializing() || !context->isAvailable()) {
            return false;
        }
        const std::shared_ptr<void> hold = context->holdRuntime();
        gpu::GpuRuntime* runtime = hold ? context->runtime() : nullptr;
        if (!runtime || runtime->isLost() || !WavefrontGpu::missingSupport(*runtime).empty()) {
            return false;
        }

        return !HydraRender::deviceFrameFellBack(this, context->generation(), utilityGpuBudget(), utilityDevice(query.time, query.view));
    }
    // Only Storm renders into GL; the other delegates' host pixels are read back by render().
    if (!RenderDelegates::isStorm(renderer)) {
        return false;
    }

    return HydraHandoff::instance().path() != gpu::GlHandoffPath::Readback;
#else
    Q_UNUSED(query);

    return false;
#endif
}

StatusEnum
USDRender::renderGpu(const GpuRenderArgs& args)
{
#ifdef NATRON_HAVE_GPU
    // A failure here leaves no message: the frame renders again on the CPU, which reports it.
    RenderSetup setup;
    std::string error;
    if (!prepareRender(args.query.time, args.query.view, args.query.mappedScale.toMipmapLevel(), &setup, &error)) {
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
        HydraRender::Result info;
        if (!HydraRender::recordDeviceWindow(setup.request, window, args.context, args.output, args.outputRect, &info, &error)) {
            return eStatusFailed;
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
