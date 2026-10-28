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

#include <cstdio>
#include <limits>
#include <string>
#include <vector>

#include <QMetaObject>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/Scene/HydraHandoff.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"

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
    , _cameraMenuRefreshPending(false)
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

    KnobChoicePtr camera = createKnob<KnobChoice>(tr("Camera"));
    camera->setName(kUSDRenderParamCamera);
    camera->setHintToolTip(tr("The camera to render through: one of the input scene's cameras, by path. Until one is "
                              "chosen the scene's first camera is used. A chosen camera the scene no longer has is an "
                              "error naming its path."));
    camera->setAnimationEnabled(false);
    page->addKnob(camera);
    _camera = camera;

    const ExtentKnobs::FormatKnobSet formatKnobs = ExtentKnobs::createFormatKnobs(this, page);
    _format = formatKnobs.choice;
    _formatSize = formatKnobs.size;
    _formatPar = formatKnobs.par;

    KnobChoicePtr fitMode = createKnob<KnobChoice>(tr("Fit Mode"));
    fitMode->setName(kUSDRenderParamFitMode);
    fitMode->setHintToolTip(tr("How the camera's aperture is conformed to the format when their shapes differ."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("fill", tr("Fill").toStdString(),
                                       tr("The aperture fills the format: the side that does not fit is cropped.").toStdString()));
        options.push_back(ChoiceOption("horizontal", tr("Horizontal").toStdString(),
                                       tr("The aperture's width fills the format's; its height is cropped or extended.").toStdString()));
        options.push_back(ChoiceOption("vertical", tr("Vertical").toStdString(),
                                       tr("The aperture's height fills the format's; its width is cropped or extended.").toStdString()));
        fitMode->populateChoices(options);
    }
    fitMode->setDefaultValue((int)HydraRender::eFitModeFill);
    fitMode->setAnimationEnabled(false);
    page->addKnob(fitMode);
    _fitMode = fitMode;

    KnobChoicePtr renderer = createKnob<KnobChoice>(tr("Renderer"));
    renderer->setName(kUSDRenderParamRenderer);
    renderer->setHintToolTip(tr("The Hydra render delegate that renders the scene. Storm renders with OpenGL; the "
                                "others render on the CPU. A renderer that is not installed or cannot run here stays "
                                "chosen, and the node is an error naming it until it is installed or another is "
                                "chosen."));
    {
        std::vector<ChoiceOption> options;
        int stormIndex = 0;
        for (const RenderDelegates::Delegate& delegate : RenderDelegates::list()) {
            if (RenderDelegates::isStorm(delegate.id)) {
                stormIndex = (int)options.size();
            }
            if (delegate.available) {
                options.push_back(ChoiceOption(delegate.id, delegate.displayName, ""));
            } else {
                options.push_back(ChoiceOption(delegate.id,
                                               tr("%1 (unavailable)").arg(QString::fromStdString(delegate.displayName)).toStdString(),
                                               delegate.error));
            }
        }
        renderer->populateChoices(options);
        renderer->setDefaultValue(stormIndex);
    }
    renderer->setAnimationEnabled(false);
    page->addKnob(renderer);
    _renderer = renderer;
} // USDRender::initializeKnobs

void
USDRender::onKnobsLoaded()
{
    NativeEffectBase::onKnobsLoaded();
    keepLoadedRenderer();
    scheduleCameraMenuRefresh();
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
USDRender::rendererId() const
{
    KnobChoicePtr renderer = _renderer.lock();
    const std::string id = renderer ? renderer->getActiveEntry().id : std::string();

    return id.empty() ? std::string(kRenderDelegateStormId) : id;
}

void
USDRender::onInputChanged(int inputNo)
{
    NativeEffectBase::onInputChanged(inputNo);
    scheduleCameraMenuRefresh();
}

void
USDRender::onNodeHashChanged(U64 hash)
{
    NativeEffectBase::onNodeHashChanged(hash);
    scheduleCameraMenuRefresh();
}

void
USDRender::scheduleCameraMenuRefresh()
{
    if (_cameraMenuRefreshPending.exchange(true)) {
        return;
    }
    // The hooks that call this fire mid-recompute of a hash, before the upstream scene settled.
    QMetaObject::invokeMethod(this, [this]() {
        _cameraMenuRefreshPending = false;
        refreshCameraMenu(); }, Qt::QueuedConnection);
}

void
USDRender::refreshCameraMenu()
{
    KnobChoicePtr camera = _camera.lock();

    if (!camera) {
        return;
    }
    std::vector<ChoiceOption> options;
    EffectInstancePtr input = getInput(0);
    ScenePayload scene;
    std::string error;
    if (input && (input->evaluateScene(EvaluateSceneArgs(getCurrentTime(), ViewIdx(0)), &scene, &error) == eRenderRoIRetCodeOk) && !scene.isEmpty()) {
        SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(scene, &error);
        if (lease.isValid()) {
            for (const PXR_NS::SdfPath& path : HydraRender::cameraPaths(lease.getStage())) {
                options.push_back(ChoiceOption(path.GetString(), path.GetString(), ""));
            }
        }
    }
    // Repopulating an unchanged menu would move the hash and come back here.
    const std::vector<ChoiceOption> current = camera->getEntries_mt_safe();
    bool same = current.size() == options.size();
    for (std::size_t i = 0; same && (i < options.size()); ++i) {
        same = current[i].id == options[i].id;
    }
    if (!same) {
        camera->populateChoices(options);
    }
}

void
USDRender::outputFormat(double time,
                        ViewIdx view,
                        RectI* format,
                        double* par) const
{
    KnobIntPtr size = _formatSize.lock();
    KnobDoublePtr formatPar = _formatPar.lock();

    if (size && formatPar) {
        const int width = size->getValueAtTime(time, 0, view);
        const int height = size->getValueAtTime(time, 1, view);
        const double aspect = formatPar->getValueAtTime(time, 0, view);
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
    // Storm renders no normal AOV.
    if (!RenderDelegates::isStorm(rendererId())) {
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
    // through the instancer, so every prim is visited. The render draws the default and proxy purposes only (HydraRender leaves
    // showGuides and showRender off), so guide- and render-purpose prims get no entry.
    const PXR_NS::UsdPrimRange all(stage->GetPseudoRoot(), PXR_NS::UsdTraverseInstanceProxies(PXR_NS::UsdPrimAllPrimsPredicate));
    for (const PXR_NS::UsdPrim& prim : all) {
        if (!prim.IsA<PXR_NS::UsdGeomGprim>()) {
            continue;
        }
        const PXR_NS::TfToken purpose = PXR_NS::UsdGeomImageable(prim).ComputePurpose();
        if ((purpose != PXR_NS::UsdGeomTokens->default_) && (purpose != PXR_NS::UsdGeomTokens->proxy)) {
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
};

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

    RectI format;
    double par;
    outputFormat(time, view, &format, &par);
    setup->pixels = Format(format, par).toCanonicalFormat().toPixelEnclosing(level, par);
    if (setup->pixels.isNull()) {
        return failWith(tr("The format is empty.").toStdString());
    }

    EffectInstancePtr input = getInput(0);
    if (!input) {
        return failWith(tr("USDRender needs a scene on its Scene input.").toStdString());
    }
    ScenePayload scene;
    if (input->evaluateScene(EvaluateSceneArgs(time, view), &scene, error) != eRenderRoIRetCodeOk) {
        if (error->empty()) {
            *error = tr("The input scene could not be evaluated.").toStdString();
        }

        return false;
    }
    if (scene.isEmpty()) {
        return failWith(tr("The input scene is empty: there is no camera to render through.").toStdString());
    }
    // The camera is read from a pooled stage, given back before the render: Storm renders the
    // node's own stage on the render thread.
    SceneQueryPool::Lease lease = SceneQueryPool::instance().borrow(scene, error);
    const PXR_NS::UsdStageRefPtr stage = lease.getStage();
    if (!stage) {
        if (error->empty()) {
            *error = tr("USD could not compose the input scene.").toStdString();
        }

        return false;
    }
    AppInstancePtr app = getApp();
    const double fps = (app && app->getProject()) ? app->getProject()->getProjectFrameRate() : 24.;
    const PXR_NS::UsdTimeCode timeCode(SceneOps::frameToTimeCode(scene, time, fps));

    KnobChoicePtr cameraKnob = _camera.lock();
    std::string cameraPath = cameraKnob ? cameraKnob->getActiveEntry().id : std::string();
    if (cameraPath.empty()) {
        const std::vector<PXR_NS::SdfPath> cameras = HydraRender::cameraPaths(stage);
        if (cameras.empty()) {
            return failWith(tr("The input scene has no camera to render through.").toStdString());
        }
        cameraPath = cameras.front().GetString();
    }
    if (!PXR_NS::SdfPath::IsValidPathString(cameraPath)) {
        return failWith(tr("No camera at %1 on the input scene.").arg(QString::fromStdString(cameraPath)).toStdString());
    }
    KnobChoicePtr fitKnob = _fitMode.lock();
    const int fitValue = fitKnob ? fitKnob->getValue() : 0;
    const HydraRender::FitModeEnum fit = (fitValue == (int)HydraRender::eFitModeHorizontal) ? HydraRender::eFitModeHorizontal : (fitValue == (int)HydraRender::eFitModeVertical) ? HydraRender::eFitModeVertical
                                                                                                                                                                                 : HydraRender::eFitModeFill;
    setup->request.owner = this;
    setup->request.scene = scene;
    setup->request.time = timeCode;
    setup->request.width = setup->pixels.width();
    setup->request.height = setup->pixels.height();
    setup->request.renderer = rendererId();
    const double aspect = (format.width() * par) / format.height();
    if (!HydraRender::cameraFrustum(stage, PXR_NS::SdfPath(cameraPath), timeCode, aspect, fit, &setup->request.frustum, error)) {
        return false;
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
    // The render stands; what the delegate could not render, or logged as an error naming no prim
    // of the scene, stays on the node until a render without it.
    if (!result.errors.empty()) {
        setPersistentMessage(eMessageTypeError, result.warning.empty() ? result.errors : result.errors + "\n" + result.warning);
    } else if (!result.warning.empty()) {
        setPersistentMessage(eMessageTypeWarning, result.warning);
    } else {
        clearPersistentMessage(false);
    }

    return eStatusOK;
} // USDRender::render

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
    // Only Storm renders into GL; the other delegates' host pixels are read back by render().
    if (!RenderDelegates::isStorm(rendererId())) {
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
    if (!HydraHandoff::instance().record(setup.request, window, args.context, args.output, args.outputRect, &error)) {
        return eStatusFailed;
    }

    return eStatusOK;
#else
    Q_UNUSED(args);

    return eStatusFailed;
#endif
}

NATRON_NAMESPACE_EXIT
