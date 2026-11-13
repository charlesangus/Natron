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

#include "TexturePaintView.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <QString>
#include <QStringList>

#include "Engine/AppInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Scene/MaterialBake.h"
#include "Engine/Nodes/Scene/PrimSelector.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/SceneRaster.h"
#include "Engine/Nodes/Scene/TexturePaintCache.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/Project.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_ENTER

namespace {
// The base files Base Files names, by UDIM tile.
std::map<int, std::string>
parseBaseFiles(const std::string& text)
{
    std::map<int, std::string> files;

    for (const QString& line : QString::fromStdString(text).split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList fields = line.split(QLatin1Char('\t'));
        bool ok = false;
        const int tile = fields.isEmpty() ? 0 : fields.front().toInt(&ok);
        if (ok && (fields.size() >= 3)) {
            files[tile] = fields.mid(2).join(QLatin1Char('\t')).toStdString();
        }
    }

    return files;
}

// The bilinear texel of rgba, size * size RGBA texels bottom-up, at (s, t) within the tile.
void
sampleTile(const std::vector<float>& rgba,
           int size,
           float s,
           float t,
           float* out)
{
    const float x = s * size - 0.5f;
    const float y = t * size - 0.5f;
    const int x0 = (int)std::floor(x);
    const int y0 = (int)std::floor(y);
    const float fx = x - x0;
    const float fy = y - y0;
    const auto at = [&](int xi, int yi, int c) -> float {
        xi = std::min(size - 1, std::max(0, xi));
        yi = std::min(size - 1, std::max(0, yi));

        return rgba[((std::size_t)yi * (std::size_t)size + (std::size_t)xi) * 4 + (std::size_t)c];
    };

    for (int c = 0; c < 4; ++c) {
        const float bottom = at(x0, y0, c) * (1.f - fx) + at(x0 + 1, y0, c) * fx;
        const float top = at(x0, y0 + 1, c) * (1.f - fx) + at(x0 + 1, y0 + 1, c) * fx;
        out[c] = bottom * (1.f - fy) + top * fy;
    }
}

template <typename K, typename V>
void
setIfDifferent(const std::shared_ptr<K>& knob,
               const V& value,
               int dimension = 0)
{
    if (knob && (knob->getValue(dimension) != value)) {
        knob->setValue(value, ViewSpec::all(), dimension);
    }
}
} // anon namespace

TexturePaintView::TexturePaintView(NodePtr node)
    : NativeEffectBase(node)
{
}

TexturePaintView::~TexturePaintView()
{
}

const NodeSpec*
TexturePaintView::nodeSpec() const
{
    return staticNodeSpec<kNodeSpec>();
}

void
TexturePaintView::initializeKnobs()
{
    KnobPagePtr page = createKnob<KnobPage>(tr("Controls"));
    const SceneRaster::ExplicitCamera defaults;

    KnobDoublePtr cameraToWorld = createKnob<KnobDouble>(tr("Camera To World"), 16);
    cameraToWorld->setName(kTexturePaintViewParamCameraToWorld);
    for (int i = 0; i < 16; ++i) {
        cameraToWorld->setDefaultValue(defaults.cameraToWorld[i], i);
    }
    _cameraToWorld = cameraToWorld;

    KnobDoublePtr viewHeight = createKnob<KnobDouble>(tr("View Height"));
    viewHeight->setName(kTexturePaintViewParamViewHeight);
    viewHeight->setDefaultValue(defaults.viewHeight);
    _viewHeight = viewHeight;

    KnobDoublePtr aspect = createKnob<KnobDouble>(tr("Aspect"));
    aspect->setName(kTexturePaintViewParamAspect);
    aspect->setDefaultValue(defaults.aspect);
    _aspect = aspect;

    KnobBoolPtr orthographic = createKnob<KnobBool>(tr("Orthographic"));
    orthographic->setName(kTexturePaintViewParamOrthographic);
    orthographic->setDefaultValue(defaults.orthographic);
    _orthographic = orthographic;

    KnobDoublePtr nearClip = createKnob<KnobDouble>(tr("Near Clip"));
    nearClip->setName(kTexturePaintViewParamNearClip);
    nearClip->setDefaultValue(defaults.nearClip);
    _nearClip = nearClip;

    KnobDoublePtr farClip = createKnob<KnobDouble>(tr("Far Clip"));
    farClip->setName(kTexturePaintViewParamFarClip);
    farClip->setDefaultValue(defaults.farClip);
    _farClip = farClip;

    KnobStringPtr primPattern = createKnob<KnobString>(tr("Prim Pattern"));
    primPattern->setName(kTexturePaintViewParamPrimPattern);
    _primPattern = primPattern;

    KnobStringPtr uvPrimvar = createKnob<KnobString>(tr("UV Primvar"));
    uvPrimvar->setName(kTexturePaintViewParamUvPrimvar);
    uvPrimvar->setDefaultValue("st");
    _uvPrimvar = uvPrimvar;

    KnobStringPtr baseFiles = createKnob<KnobString>(tr("Base Files"));
    baseFiles->setName(kTexturePaintViewParamBaseFiles);
    _baseFiles = baseFiles;

    KnobIntPtr baseResolution = createKnob<KnobInt>(tr("Base Resolution"));
    baseResolution->setName(kTexturePaintViewParamBaseResolution);
    baseResolution->setDefaultValue(1024);
    _baseResolution = baseResolution;

    for (const KnobIPtr& knob : std::vector<KnobIPtr> { cameraToWorld, viewHeight, aspect, orthographic, nearClip, farClip, primPattern, uvPrimvar, baseFiles, baseResolution }) {
        knob->setAnimationEnabled(false);
        page->addKnob(knob);
    }
} // TexturePaintView::initializeKnobs

RectD
TexturePaintView::paintSpaceBounds(double aspect)
{
    if (!(aspect > 0.) || !std::isfinite(aspect)) {
        aspect = 1.;
    }
    const double size = (double)kPaintSpaceSize;
    const double shorter = std::max(1., std::round((aspect >= 1.) ? (size / aspect) : (size * aspect)));

    return (aspect >= 1.) ? RectD(0., 0., size, shorter) : RectD(0., 0., shorter, size);
}

unsigned int
TexturePaintView::bufferMipmapLevel(int bufferResolution)
{
    unsigned int level = 0;

    while ((level < 3) && ((kPaintSpaceSize >> (level + 1)) >= bufferResolution)) {
        ++level;
    }

    return level;
}

void
TexturePaintView::viewToPaintSpace(double aspect,
                                   double u,
                                   double v,
                                   double* x,
                                   double* y)
{
    const RectD bounds = paintSpaceBounds(aspect);

    *x = u * bounds.width();
    *y = v * bounds.height();
}

void
TexturePaintView::setCamera(const SceneRaster::ExplicitCamera& camera)
{
    KnobDoublePtr cameraToWorld = _cameraToWorld.lock();

    if (cameraToWorld) {
        for (int i = 0; i < 16; ++i) {
            setIfDifferent(cameraToWorld, camera.cameraToWorld[i], i);
        }
    }
    setIfDifferent(_viewHeight.lock(), camera.viewHeight);
    setIfDifferent(_aspect.lock(), camera.aspect);
    setIfDifferent(_orthographic.lock(), camera.orthographic);
    setIfDifferent(_nearClip.lock(), camera.nearClip);
    setIfDifferent(_farClip.lock(), camera.farClip);
}

void
TexturePaintView::setPrimPattern(const std::string& pattern)
{
    setIfDifferent(_primPattern.lock(), pattern);
}

void
TexturePaintView::setUvPrimvar(const std::string& primvar)
{
    setIfDifferent(_uvPrimvar.lock(), primvar);
}

void
TexturePaintView::setBase(const std::string& files,
                          int resolution)
{
    setIfDifferent(_baseFiles.lock(), files);
    setIfDifferent(_baseResolution.lock(), resolution);
}

double
TexturePaintView::getAspect() const
{
    KnobDoublePtr aspect = _aspect.lock();

    return aspect ? aspect->getValue() : 1.;
}

StatusEnum
TexturePaintView::getPreferredMetadataAt(double /*time*/,
                                         ViewIdx /*view*/,
                                         NodeMetadata& metadata)
{
    metadata.setBitDepth(-1, eImageBitDepthFloat);
    metadata.setComponentsType(-1, kNatronColorLayerID);
    metadata.setNComps(-1, 4);
    metadata.setIsFrameVarying(true);
    const RectD bounds = paintSpaceBounds(getAspect());
    metadata.setOutputFormat(RectI((int)bounds.x1, (int)bounds.y1, (int)bounds.x2, (int)bounds.y2));
    metadata.setPixelAspectRatio(-1, 1.);

    return eStatusOK;
}

StatusEnum
TexturePaintView::getRegionOfDefinition(U64 /*hash*/,
                                        double /*time*/,
                                        const RenderScale& /*scale*/,
                                        ViewIdx /*view*/,
                                        RectD* rod)
{
    *rod = paintSpaceBounds(getAspect());

    return eStatusOK;
}

FramesNeededMap
TexturePaintView::getFramesNeeded(double /*time*/,
                                  ViewIdx /*view*/)
{
    return FramesNeededMap();
}

void
TexturePaintView::getRegionsOfInterest(double /*time*/,
                                       const RenderScale& /*scale*/,
                                       const RectD& /*outputRoD*/,
                                       const RectD& /*renderWindow*/,
                                       ViewIdx /*view*/,
                                       RoIMap* /*ret*/)
{
}

bool
TexturePaintView::renderView(double time,
                             ViewIdx view,
                             const RectI& pixels,
                             std::vector<float>* rgba,
                             std::string* error)
{
    const int width = pixels.width();
    const int height = pixels.height();

    rgba->assign((std::size_t)width * (std::size_t)height * 4, 0.f);

    // With nothing to look at, strokes still paint over a transparent view.
    EffectInstancePtr input = getInput(0);
    if (!input) {
        return true;
    }
    ScenePayload scene;
    if (input->evaluateScene(EvaluateSceneArgs(time, view), &scene, error) != eRenderRoIRetCodeOk) {
        if (error->empty()) {
            *error = tr("The input scene could not be evaluated.").toStdString();
        }

        return false;
    }
    if (scene.isEmpty()) {
        return true;
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

    KnobStringPtr primPattern = _primPattern.lock();
    const std::string pattern = primPattern ? primPattern->getValue() : std::string();
    if (!PrimSelector::validate(pattern, error)) {
        return false;
    }
    std::vector<PXR_NS::SdfPath> selected;
    if (!SceneOps::selectPrims(stage, pattern, &selected, error)) {
        return false;
    }
    // SceneRaster draws every mesh for an empty prim list; selecting nothing must show nothing.
    if (selected.empty()) {
        return true;
    }

    SceneRaster::ExplicitCamera camera;
    KnobDoublePtr cameraToWorld = _cameraToWorld.lock();
    for (int i = 0; i < 16; ++i) {
        camera.cameraToWorld[i] = cameraToWorld ? cameraToWorld->getValue(i) : camera.cameraToWorld[i];
    }
    {
        KnobDoublePtr knob = _viewHeight.lock();
        camera.viewHeight = knob ? knob->getValue() : camera.viewHeight;
    }
    camera.aspect = getAspect();
    {
        KnobBoolPtr knob = _orthographic.lock();
        camera.orthographic = knob ? knob->getValue() : camera.orthographic;
    }
    {
        KnobDoublePtr knob = _nearClip.lock();
        camera.nearClip = knob ? knob->getValue() : camera.nearClip;
    }
    {
        KnobDoublePtr knob = _farClip.lock();
        camera.farClip = knob ? knob->getValue() : camera.farClip;
    }
    KnobStringPtr uvPrimvar = _uvPrimvar.lock();
    const std::string primvar = uvPrimvar ? uvPrimvar->getValue() : std::string("st");

    SceneRaster::CameraArgs args;
    args.explicitCamera = camera;
    args.stName = PXR_NS::TfToken(primvar.empty() ? std::string("st") : primvar);
    args.time = timeCode;
    args.width = width;
    args.height = height;
    args.prims = selected;
    SceneRaster::CameraBuffers buffers;
    if (!SceneRaster::renderCamera(stage, args, &buffers, error)) {
        return false;
    }
    if ((buffers.width != width) || (buffers.height != height)) {
        *error = tr("The view was drawn at the wrong size.").toStdString();

        return false;
    }

    std::vector<PXR_NS::GfVec3f> colors;
    colors.reserve(buffers.meshPaths.size());
    for (const PXR_NS::SdfPath& path : buffers.meshPaths) {
        colors.push_back(TexturePaintCache::meshBaseColor(stage->GetPrimAtPath(path), timeCode));
    }

    const std::size_t count = (std::size_t)width * (std::size_t)height;
    for (std::size_t i = 0; i < count; ++i) {
        const std::int32_t mesh = buffers.primIndex[i];
        // A pixel with no UV there has no texel for its paint to land in.
        if ((mesh < 0) || ((std::size_t)mesh >= colors.size()) || (buffers.udimTile[i] == 0)) {
            continue;
        }
        const PXR_NS::GfVec3f& color = colors[(std::size_t)mesh];
        float* out = rgba->data() + i * 4;
        out[0] = color[0];
        out[1] = color[1];
        out[2] = color[2];
        out[3] = 1.f;
    }

    KnobStringPtr baseFiles = _baseFiles.lock();
    const std::map<int, std::string> bases = parseBaseFiles(baseFiles ? baseFiles->getValue() : std::string());
    if (bases.empty()) {
        return true;
    }
    KnobIntPtr baseResolution = _baseResolution.lock();
    const int size = std::max(1, baseResolution ? baseResolution->getValue() : 1024);
    // One tile at a time, so that no more than one base is ever held in memory.
    for (const std::pair<const int, std::string>& base : bases) {
        if (std::find(buffers.udimTile.begin(), buffers.udimTile.end(), base.first) == buffers.udimTile.end()) {
            continue;
        }
        std::vector<float> texels;
        if (!MaterialBake::readBaseTile(base.second, size, &texels, 0, error)) {
            return false;
        }
        for (std::size_t i = 0; i < count; ++i) {
            const std::int32_t mesh = buffers.primIndex[i];
            if ((mesh < 0) || (buffers.udimTile[i] != base.first)) {
                continue;
            }
            const PXR_NS::GfVec2f& st = buffers.st[i];
            sampleTile(texels, size, st[0] - std::floor(st[0]), st[1] - std::floor(st[1]), rgba->data() + i * 4);
        }
    }

    return true;
} // TexturePaintView::renderView

StatusEnum
TexturePaintView::render(const RenderActionArgs& args)
{
    const RectI pixels = paintSpaceBounds(getAspect()).toPixelEnclosing(args.mappedScale.toMipmapLevel(), 1.);
    std::vector<float> rgba;
    std::string error;

    if (pixels.isNull() || !renderView(args.time, args.view, pixels, &rgba, &error)) {
        setPersistentMessage(eMessageTypeError, error.empty() ? tr("The view is empty.").toStdString() : error);

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
} // TexturePaintView::render

NATRON_NAMESPACE_EXIT
