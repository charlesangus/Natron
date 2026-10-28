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

#include "ProjectTexture.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
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
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/SceneRaster.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/PoolParallelFor.h"
#include "Engine/Project.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_ENTER

namespace {
int
maxThreads()
{
    return appPTR ? appPTR->getNCPUsAvailableForEffect() : 1;
}

// Grows rgba by one texel per pass into the texels filled does not mark, each taking the mean of
// its filled neighbours; a texel with none stays as it is.
void
dilate(int width,
       int height,
       int passes,
       std::vector<std::uint8_t>* filled,
       std::vector<float>* rgba)
{
    std::vector<std::uint8_t> nextFilled;
    std::vector<float> next;

    for (int pass = 0; pass < passes; ++pass) {
        nextFilled = *filled;
        next = *rgba;
        bool grew = false;
        std::vector<std::uint8_t> rowGrew((std::size_t)height, 0);
        parallelForOnGlobalPool(height, maxThreads(), [&](int y) {
            for (int x = 0; x < width; ++x) {
                const std::size_t i = (std::size_t)x + (std::size_t)y * (std::size_t)width;
                if ((*filled)[i]) {
                    continue;
                }
                float sum[4] = { 0.f, 0.f, 0.f, 0.f };
                int count = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx;
                        const int ny = y + dy;
                        if (((dx == 0) && (dy == 0)) || (nx < 0) || (ny < 0) || (nx >= width) || (ny >= height)) {
                            continue;
                        }
                        const std::size_t n = (std::size_t)nx + (std::size_t)ny * (std::size_t)width;
                        if (!(*filled)[n]) {
                            continue;
                        }
                        for (int c = 0; c < 4; ++c) {
                            sum[c] += (*rgba)[n * 4 + (std::size_t)c];
                        }
                        ++count;
                    }
                }
                if (count > 0) {
                    for (int c = 0; c < 4; ++c) {
                        next[i * 4 + (std::size_t)c] = sum[c] / (float)count;
                    }
                    nextFilled[i] = 1;
                    rowGrew[(std::size_t)y] = 1;
                }
            }
        });
        for (std::uint8_t g : rowGrew) {
            grew = grew || g;
        }
        filled->swap(nextFilled);
        rgba->swap(next);
        if (!grew) {
            break;
        }
    }
}
} // anon namespace

ProjectTexture::ProjectTexture(NodePtr node)
    : NativeEffectBase(node)
    , _cameraMenuRefreshPending(false)
{
}

ProjectTexture::~ProjectTexture()
{
}

const NodeSpec*
ProjectTexture::nodeSpec() const
{
    return &kNodeSpec;
}

void
ProjectTexture::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));

    KnobChoicePtr camera = createKnob<KnobChoice>(tr("Camera"));
    camera->setName(kProjectTextureParamCamera);
    camera->setHintToolTip(tr("The camera to project through: one of the input scene's cameras, by path. Until one is "
                              "chosen the scene's first camera is used. A chosen camera the scene no longer has is an "
                              "error naming its path."));
    camera->setAnimationEnabled(false);
    page->addKnob(camera);
    _camera = camera;

    KnobChoicePtr fitMode = createKnob<KnobChoice>(tr("Fit Mode"));
    fitMode->setName(kProjectTextureParamFitMode);
    fitMode->setHintToolTip(tr("How the camera's aperture is conformed to the Image input's format when their shapes "
                               "differ; set it as on the USDRender the image was painted on."));
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

    _primPattern.create(this, page, kProjectTextureParamPrimPattern, tr("Prim Pattern"),
                        tr("The mesh to texture: the first mesh among the prims the pattern selects."));

    KnobStringPtr uvPrimvar = createKnob<KnobString>(tr("UV Primvar"));
    uvPrimvar->setName(kProjectTextureParamUVPrimvar);
    uvPrimvar->setHintToolTip(tr("The mesh's texture coordinate primvar, with or without its primvars: namespace."));
    uvPrimvar->setDefaultValue("st");
    uvPrimvar->setAnimationEnabled(false);
    page->addKnob(uvPrimvar);
    _uvPrimvar = uvPrimvar;

    KnobIntPtr udimTile = createKnob<KnobInt>(tr("UDIM Tile"));
    udimTile->setName(kProjectTextureParamUdimTile);
    udimTile->setHintToolTip(tr("The UDIM tile the texture covers: tile 1001 + u + 10 v spans texture coordinates "
                                "u to u + 1 and v to v + 1. 1001 is the 0 to 1 square."));
    udimTile->setMinimum(1001);
    udimTile->setDisplayMinimum(1001);
    udimTile->setDisplayMaximum(1100);
    udimTile->setDefaultValue(1001);
    udimTile->setAnimationEnabled(false);
    page->addKnob(udimTile);
    _udimTile = udimTile;

    KnobIntPtr resolution = createKnob<KnobInt>(tr("Resolution"), 2);
    resolution->setName(kProjectTextureParamResolution);
    resolution->setHintToolTip(tr("The width and height of the texture in texels; it is the output's format."));
    for (int d = 0; d < 2; ++d) {
        resolution->setMinimum(1, d);
        resolution->setDisplayMinimum(1, d);
        resolution->setDisplayMaximum(8192, d);
        resolution->setDefaultValue(1024, d);
    }
    resolution->setAnimationEnabled(false);
    resolution->setIsMetadataSlave(true);
    page->addKnob(resolution);
    _resolution = resolution;

    KnobBoolPtr occlusion = createKnob<KnobBool>(tr("Occlusion"));
    occlusion->setName(kProjectTextureParamOcclusion);
    occlusion->setHintToolTip(tr("When checked, texels hidden from the camera behind any mesh of the scene, the textured "
                                 "one included, are left transparent; otherwise the image goes through to them."));
    occlusion->setDefaultValue(true);
    occlusion->setAnimationEnabled(false);
    page->addKnob(occlusion);
    _occlusion = occlusion;

    KnobDoublePtr depthBias = createKnob<KnobDouble>(tr("Depth Bias"));
    depthBias->setName(kProjectTextureParamDepthBias);
    depthBias->setHintToolTip(tr("How far behind the nearest surface the camera sees a texel may lie and still be seen, as "
                                 "a fraction of its distance from the camera. Raise it when a surface shadows itself in "
                                 "speckles, at grazing angles; lower it when the image goes through onto a surface just "
                                 "behind another."));
    depthBias->setMinimum(0.);
    depthBias->setMaximum(1.);
    depthBias->setDisplayMinimum(0.);
    depthBias->setDisplayMaximum(0.1);
    depthBias->setDefaultValue(0.01);
    depthBias->setAnimationEnabled(false);
    page->addKnob(depthBias);
    _depthBias = depthBias;

    KnobIntPtr edgeDilation = createKnob<KnobInt>(tr("Edge Dilation"));
    edgeDilation->setName(kProjectTextureParamEdgeDilation);
    edgeDilation->setHintToolTip(tr("How many texels beyond the edges of the UV layout take the colour of the texels "
                                    "inside, so that texture filtering does not bleed black across UV seams."));
    edgeDilation->setMinimum(0);
    edgeDilation->setDisplayMinimum(0);
    edgeDilation->setDisplayMaximum(32);
    edgeDilation->setDefaultValue(4);
    edgeDilation->setAnimationEnabled(false);
    page->addKnob(edgeDilation);
    _edgeDilation = edgeDilation;
} // ProjectTexture::initializeKnobs

void
ProjectTexture::onKnobsLoaded()
{
    NativeEffectBase::onKnobsLoaded();
    scheduleCameraMenuRefresh();
}

void
ProjectTexture::onInputChanged(int inputNo)
{
    NativeEffectBase::onInputChanged(inputNo);
    scheduleCameraMenuRefresh();
}

void
ProjectTexture::onNodeHashChanged(U64 hash)
{
    NativeEffectBase::onNodeHashChanged(hash);
    scheduleCameraMenuRefresh();
}

void
ProjectTexture::scheduleCameraMenuRefresh()
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
ProjectTexture::refreshCameraMenu()
{
    KnobChoicePtr camera = _camera.lock();

    if (!camera) {
        return;
    }
    std::vector<ChoiceOption> options;
    EffectInstancePtr input = getInput(kProjectTextureInputScene);
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

RectI
ProjectTexture::textureFormat(double time,
                              ViewIdx view) const
{
    KnobIntPtr resolution = _resolution.lock();
    const int width = resolution ? resolution->getValueAtTime(time, 0, view) : 1024;
    const int height = resolution ? resolution->getValueAtTime(time, 1, view) : 1024;

    return RectI(0, 0, std::max(1, width), std::max(1, height));
}

bool
ProjectTexture::imageFormat(double time,
                            ViewIdx view,
                            RectD* format,
                            double* par) const
{
    EffectInstancePtr input = getInput(kProjectTextureInputImage);

    if (!input) {
        return false;
    }
    const NodeMetadataConstPtr metadata = input->getRenderMetadata(time, view);
    if (!metadata) {
        return false;
    }
    const RectI pixels = metadata->getOutputFormat();
    *par = metadata->getPixelAspectRatio(-1);
    if (pixels.isNull() || !(*par > 0.)) {
        return false;
    }
    *format = Format(pixels, *par).toCanonicalFormat();

    return !format->isNull();
}

StatusEnum
ProjectTexture::getPreferredMetadataAt(double time,
                                       ViewIdx view,
                                       NodeMetadata& metadata)
{
    for (int i : { -1, (int)kProjectTextureInputImage }) {
        metadata.setBitDepth(i, eImageBitDepthFloat);
        metadata.setComponentsType(i, kNatronColorLayerID);
        metadata.setNComps(i, 4);
    }
    metadata.setIsFrameVarying(true);
    metadata.setOutputFormat(textureFormat(time, view));
    metadata.setPixelAspectRatio(-1, 1.);

    return eStatusOK;
}

StatusEnum
ProjectTexture::getRegionOfDefinition(U64 /*hash*/,
                                      double time,
                                      const RenderScale& /*scale*/,
                                      ViewIdx view,
                                      RectD* rod)
{
    *rod = Format(textureFormat(time, view), 1.).toCanonicalFormat();

    return eStatusOK;
}

void
ProjectTexture::getComponentsNeededAndProduced(double time,
                                               ViewIdx view,
                                               EffectInstance::ComponentsNeededMap* comps,
                                               double* passThroughTime,
                                               int* passThroughView,
                                               int* passThroughInputNb)
{
    EffectInstance::getComponentsNeededAndProduced(time, view, comps, passThroughTime, passThroughView, passThroughInputNb);
    // Nothing is fetched from the scene input as an image.
    comps->erase(kProjectTextureInputScene);
    if (getInput(kProjectTextureInputImage)) {
        (*comps)[kProjectTextureInputImage] = std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents());
    }
    (*comps)[-1] = std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents());
}

FramesNeededMap
ProjectTexture::getFramesNeeded(double time,
                                ViewIdx view)
{
    FramesNeededMap ret;

    if (getInput(kProjectTextureInputImage)) {
        RangeD range;
        range.min = range.max = time;
        ret[kProjectTextureInputImage][view] = std::vector<RangeD>(1, range);
    }

    return ret;
}

void
ProjectTexture::getRegionsOfInterest(double time,
                                     const RenderScale& /*scale*/,
                                     const RectD& /*outputRoD*/,
                                     const RectD& /*renderWindow*/,
                                     ViewIdx view,
                                     RoIMap* ret)
{
    // Wherever in the camera's frame the texels land, whatever part of the texture is asked for.
    RectD format;
    double par = 1.;
    if (imageFormat(time, view, &format, &par)) {
        (*ret)[getInput(kProjectTextureInputImage)] = format;
    }
}

bool
ProjectTexture::renderTexture(const RenderActionArgs& args,
                              const RectI& pixels,
                              std::vector<float>* rgba,
                              std::string* error)
{
    auto failWith = [error](const QString& message) {
        *error = message.toStdString();

        return false;
    };

    const double time = args.time;
    const ViewIdx view = args.view;
    const unsigned int level = args.mappedScale.toMipmapLevel();

    RectD imageCanonical;
    double imagePar = 1.;
    if (!getInput(kProjectTextureInputImage)) {
        return failWith(tr("ProjectTexture needs an image to project on its Image input."));
    }
    if (!imageFormat(time, view, &imageCanonical, &imagePar)) {
        return failWith(tr("The Image input's format is empty."));
    }
    const RectI imagePixels = imageCanonical.toPixelEnclosing(level, imagePar);
    if (imagePixels.isNull()) {
        return failWith(tr("The Image input's format is empty."));
    }

    EffectInstancePtr input = getInput(kProjectTextureInputScene);
    if (!input) {
        return failWith(tr("ProjectTexture needs a scene on its Scene input."));
    }
    ScenePayload scene;
    if (input->evaluateScene(EvaluateSceneArgs(time, view), &scene, error) != eRenderRoIRetCodeOk) {
        if (error->empty()) {
            *error = tr("The input scene could not be evaluated.").toStdString();
        }

        return false;
    }
    if (scene.isEmpty()) {
        return failWith(tr("The input scene is empty: there is no camera to project through."));
    }
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

    const std::string pattern = _primPattern.getExpression();
    if (!PrimSelector::validate(pattern, error)) {
        return false;
    }
    std::vector<PXR_NS::SdfPath> selected;
    if (!SceneOps::selectPrims(stage, pattern, &selected, error)) {
        return false;
    }
    PXR_NS::SdfPath meshPath;
    for (const PXR_NS::SdfPath& path : selected) {
        if (PXR_NS::UsdGeomMesh(stage->GetPrimAtPath(path))) {
            meshPath = path;
            break;
        }
    }
    if (meshPath.IsEmpty()) {
        return failWith(PrimSelector::isBlank(pattern) ? tr("Set Prim Pattern to the mesh to texture.")
                                                       : tr("Prim Pattern %1 selects no mesh on the input scene.").arg(QString::fromStdString(pattern)));
    }

    // The camera, framed as USDRender frames it on the image's format.
    KnobChoicePtr cameraKnob = _camera.lock();
    std::string cameraPath = cameraKnob ? cameraKnob->getActiveEntry().id : std::string();
    if (cameraPath.empty()) {
        const std::vector<PXR_NS::SdfPath> cameras = HydraRender::cameraPaths(stage);
        if (cameras.empty()) {
            return failWith(tr("The input scene has no camera to project through."));
        }
        cameraPath = cameras.front().GetString();
    }
    if (!PXR_NS::SdfPath::IsValidPathString(cameraPath)) {
        return failWith(tr("No camera at %1 on the input scene.").arg(QString::fromStdString(cameraPath)));
    }
    KnobChoicePtr fitKnob = _fitMode.lock();
    const int fitValue = fitKnob ? fitKnob->getValue() : 0;
    const HydraRender::FitModeEnum fit = (fitValue == (int)HydraRender::eFitModeHorizontal) ? HydraRender::eFitModeHorizontal
        : (fitValue == (int)HydraRender::eFitModeVertical)                                  ? HydraRender::eFitModeVertical
                                                                                            : HydraRender::eFitModeFill;
    PXR_NS::GfFrustum frustum;
    if (!HydraRender::cameraFrustum(stage, PXR_NS::SdfPath(cameraPath), timeCode, imageCanonical.width() / imageCanonical.height(), fit, &frustum, error)) {
        return false;
    }
    const PXR_NS::GfMatrix4d viewMatrix = frustum.ComputeViewMatrix();
    const PXR_NS::GfMatrix4d viewProjection = viewMatrix * frustum.ComputeProjectionMatrix();
    const bool perspective = frustum.GetProjectionType() == PXR_NS::GfFrustum::Perspective;
    const PXR_NS::GfVec3d eye = frustum.GetPosition();
    const PXR_NS::GfVec3d towardCamera = -frustum.ComputeViewDirection();

    KnobBoolPtr occlusionKnob = _occlusion.lock();
    const bool occlusion = occlusionKnob ? occlusionKnob->getValueAtTime(time, 0, view) : true;
    KnobDoublePtr biasKnob = _depthBias.lock();
    const double bias = biasKnob ? biasKnob->getValueAtTime(time, 0, view) : 0.01;
    SceneRaster::CameraBuffers zbuffer;
    PXR_NS::GfMatrix4d zViewProjection(1.);
    if (occlusion) {
        SceneRaster::CameraArgs cameraArgs;
        cameraArgs.cameraPath = PXR_NS::SdfPath(cameraPath);
        cameraArgs.time = timeCode;
        cameraArgs.width = imagePixels.width();
        cameraArgs.height = imagePixels.height();
        cameraArgs.pixelAspect = imagePar;
        cameraArgs.fit = fit;
        if (!SceneRaster::renderCamera(stage, cameraArgs, &zbuffer, error)) {
            return false;
        }
        zViewProjection = zbuffer.frustum.ComputeViewMatrix() * zbuffer.frustum.ComputeProjectionMatrix();
    }

    KnobStringPtr primvarKnob = _uvPrimvar.lock();
    KnobIntPtr tileKnob = _udimTile.lock();
    SceneRaster::BakeArgs bakeArgs;
    bakeArgs.primPath = meshPath;
    bakeArgs.stName = PXR_NS::TfToken(primvarKnob ? primvarKnob->getValue() : std::string("st"));
    bakeArgs.time = timeCode;
    bakeArgs.width = pixels.width();
    bakeArgs.height = pixels.height();
    bakeArgs.udimTile = tileKnob ? tileKnob->getValueAtTime(time, 0, view) : 1001;
    SceneRaster::BakeBuffers bake;
    if (!SceneRaster::bakeUV(stage, bakeArgs, &bake, error)) {
        return false;
    }
    lease = SceneQueryPool::Lease();

    const ImageLayerDesc rgbaLayer = ImageLayerDesc::getRGBAComponents();
    RectI imageRoI;
    ImagePtr image = getImage(kProjectTextureInputImage, time, args.mappedScale, view, &imageCanonical, &rgbaLayer,
                              true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &imageRoI);
    if (image && (image->getBitDepth() != eImageBitDepthFloat)) {
        return failWith(tr("The Image input is not in floating point."));
    }
    std::shared_ptr<Image::ReadAccess> imageAccess;
    Resampler::SourceImage source;
    const int imageComps = image ? (int)image->getComponentsCount() : 0;
    if (image && !image->getBounds().isNull() && (imageComps >= 1) && (imageComps <= 4)) {
        imageAccess = std::make_shared<Image::ReadAccess>(image.get());
        const RectI& bounds = image->getBounds();
        source = Resampler::SourceImage((const float*)imageAccess->pixelAt(bounds.x1, bounds.y1), bounds, imageComps);
    }

    const int width = pixels.width();
    const int height = pixels.height();
    rgba->assign((std::size_t)width * (std::size_t)height * 4, 0.f);
    if (source.isValid()) {
        parallelForOnGlobalPool(height, maxThreads(), [&](int y) {
            for (int x = 0; x < width; ++x) {
                const std::size_t i = (std::size_t)x + (std::size_t)y * (std::size_t)width;
                if (!bake.coverage[i]) {
                    continue;
                }
                const PXR_NS::GfVec3d world(bake.worldPosition[i]);
                const PXR_NS::GfVec4d p(world[0], world[1], world[2], 1.);
                const double depth = -(p * viewMatrix)[2];
                if (!(depth > 0.)) {
                    continue;
                }
                const PXR_NS::GfVec3d normal(bake.worldNormal[i]);
                if (PXR_NS::GfDot(normal, perspective ? (eye - world) : towardCamera) <= 0.) {
                    continue;
                }
                const PXR_NS::GfVec4d clip = p * viewProjection;
                if (clip[3] <= 0.) {
                    continue;
                }
                const double ndcX = clip[0] / clip[3];
                const double ndcY = clip[1] / clip[3];
                if ((ndcX < -1.) || (ndcX > 1.) || (ndcY < -1.) || (ndcY > 1.)) {
                    continue;
                }
                if (occlusion) {
                    const PXR_NS::GfVec4d zClip = p * zViewProjection;
                    if (zClip[3] <= 0.) {
                        continue;
                    }
                    const int zx = std::clamp((int)std::floor((zClip[0] / zClip[3] * 0.5 + 0.5) * zbuffer.width), 0, zbuffer.width - 1);
                    const int zy = std::clamp((int)std::floor((zClip[1] / zClip[3] * 0.5 + 0.5) * zbuffer.height), 0, zbuffer.height - 1);
                    const float nearest = zbuffer.depth[(std::size_t)zx + (std::size_t)zy * (std::size_t)zbuffer.width];
                    if ((double)nearest < depth * (1. - bias)) {
                        continue;
                    }
                }
                const double fx = imagePixels.x1 + (ndcX * 0.5 + 0.5) * imagePixels.width();
                const double fy = imagePixels.y1 + (ndcY * 0.5 + 0.5) * imagePixels.height();
                float sample[4] = { 0.f, 0.f, 0.f, 0.f };
                Resampler::interpolate(Resampler::eFilterBilinear, false, fx, fy, source, false, sample);
                float* out = &(*rgba)[i * 4];
                switch (imageComps) {
                case 1:
                    out[3] = sample[0];
                    break;
                case 2:
                    out[0] = sample[0];
                    out[1] = sample[1];
                    out[3] = 1.f;
                    break;
                case 3:
                    out[0] = sample[0];
                    out[1] = sample[1];
                    out[2] = sample[2];
                    out[3] = 1.f;
                    break;
                default:
                    std::copy(sample, sample + 4, out);
                    break;
                }
            }
        });
    }

    KnobIntPtr dilationKnob = _edgeDilation.lock();
    const int dilation = dilationKnob ? dilationKnob->getValueAtTime(time, 0, view) : 0;
    if (dilation > 0) {
        // Scaled with the texture, so a proxy render dilates as far across the layout as a full one.
        const int passes = std::max(1, (int)std::lround(dilation * args.mappedScale.toOfxPointD().x));
        dilate(width, height, passes, &bake.coverage, rgba);
    }

    return true;
} // ProjectTexture::renderTexture

StatusEnum
ProjectTexture::render(const RenderActionArgs& args)
{
    const RectI pixels = Format(textureFormat(args.time, args.view), 1.).toCanonicalFormat().toPixelEnclosing(args.mappedScale.toMipmapLevel(), 1.);
    std::vector<float> rgba;
    std::string error;

    if (pixels.isNull() || !renderTexture(args, pixels, &rgba, &error)) {
        setPersistentMessage(eMessageTypeError, error.empty() ? tr("The texture is empty.").toStdString() : error);

        return eStatusFailed;
    }

    const std::size_t width = (std::size_t)pixels.width();
    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        const ImagePtr& image = it->second;
        if (!image) {
            continue;
        }
        if (image->getBitDepth() != eImageBitDepthFloat) {
            return eStatusFailed;
        }
        const bool color = it->first.isColorLayer();
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
                const bool inside = color && pixels.contains(x, y);
                const std::size_t index = inside ? (std::size_t)(y - pixels.y1) * width + (std::size_t)(x - pixels.x1) : 0;
                for (int c = 0; c < nComps; ++c) {
                    // A one-channel colour plane is alpha.
                    const int channel = (nComps == 1) ? 3 : c;
                    out[c] = (inside && (channel < 4)) ? rgba[index * 4 + (std::size_t)channel] : 0.f;
                }
            }
        }
    }
    clearPersistentMessage(false);

    return eStatusOK;
} // ProjectTexture::render

NATRON_NAMESPACE_EXIT
