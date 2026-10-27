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

#include "NeuralDenoise.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <utility>
#include <vector>

#include <QString>

#include "Engine/Ai/InferenceSession.h"
#include "Engine/Ai/ModelDownloader.h"
#include "Engine/Ai/ModelRegistry.h"
#include "Engine/Ai/OrtLoader.h"
#include "Engine/Ai/Tensor.h"
#include "Engine/Ai/TiledInference.h"
#include "Engine/AppManager.h"
#include "Engine/Image.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/Image/StagePlan.h"

NATRON_NAMESPACE_ENTER

NeuralDenoise::NeuralDenoise(NodePtr node)
    : StagedImageEffect(node)
    , _downloader(nullptr)
    , _model()
    , _colourspace()
    , _strength()
    , _tileSize()
    , _errorMutex()
    , _postedError()
{
}

NeuralDenoise::~NeuralDenoise()
{
    _colourspace.unwatchConfig();
}

ModelDownloader*&
NeuralDenoise::downloaderForTesting()
{
    static ModelDownloader* downloader = nullptr;

    return downloader;
}

const NodeSpec*
NeuralDenoise::nodeSpec() const
{
    return staticNodeSpec<kNeuralDenoiseNodeSpec>();
}

const ImagePipelineSpec&
NeuralDenoise::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kNeuralDenoisePipeline, kNeuralDenoiseNodeSpec>();
}

std::string
NeuralDenoise::getInputHint(int inputNb) const
{
    switch (inputNb) {
    case kNeuralDenoiseInputSource:
        return tr("The image to denoise.").toStdString();
    case kNeuralDenoiseInputMask:
        return tr("The mask limiting where the denoised image replaces the Source.").toStdString();
    default:
        break;
    }

    return std::string();
}

void
NeuralDenoise::addAcceptedComponents(int inputNb,
                                     std::list<ImageLayerDesc>* comps)
{
    if (isInputMask(inputNb)) {
        StagedImageEffect::addAcceptedComponents(inputNb, comps);

        return;
    }
    comps->push_back(ImageLayerDesc::getRGBAComponents());
    comps->push_back(ImageLayerDesc::getRGBComponents());
}

void
NeuralDenoise::initializeKnobs()
{
    KnobPagePtr page = createControlsPage();

    _downloader = downloaderForTesting() ? downloaderForTesting() : ModelDownloader::process();
    // The linear model first: it takes scene-linear values as they are.
    _model = ModelKnob::create(this, page, std::vector<AiModelTask> { AiModelTask::eDenoiseLinear, AiModelTask::eDenoise }, _downloader);

    _colourspace.create(this, page, [this]() { return selectedModelSpace(); });

    KnobDoublePtr strength = createKnob<KnobDouble>(tr("Strength"));
    strength->setName(kNeuralDenoiseParamStrength);
    strength->setHintToolTip(tr("How much of the denoised image replaces the Source: none at 0, all of it at 1. "
                                "The two are mixed in the working space."));
    strength->setMinimum(0.);
    strength->setMaximum(1.);
    strength->setDisplayMinimum(0.);
    strength->setDisplayMaximum(1.);
    strength->setDefaultValue(1.);
    page->addKnob(strength);
    _strength = strength;

    KnobIntPtr tileSize = createKnob<KnobInt>(tr("Tile Size"));
    tileSize->setName(kNeuralDenoiseParamTileSize);
    tileSize->setHintToolTip(tr("The edge of the square tiles the model runs over, in pixels. 0 takes the size the "
                                "model was exported at, or 512 for a model that takes any size. A model exported at a "
                                "fixed size never runs larger tiles. Smaller tiles need less memory; neighbouring "
                                "tiles overlap and are blended, so the size barely changes the result."));
    tileSize->setMinimum(0);
    tileSize->setMaximum(4096);
    tileSize->setDisplayMinimum(0);
    tileSize->setDisplayMaximum(2048);
    tileSize->setDefaultValue(0);
    tileSize->setAnimationEnabled(false);
    page->addKnob(tileSize);
    _tileSize = tileSize;

    addStageKnobs(page);

    _colourspace.watchConfig();
}

void
NeuralDenoise::onKnobsLoaded()
{
    _colourspace.refreshMenus();
}

void
NeuralDenoise::onEffectCreated(bool /*mayCreateFileDialog*/,
                               const CreateNodeArgs& args)
{
    if (_model) {
        _model->onEffectCreated(args);
    }
    _colourspace.modelChanged();
}

bool
NeuralDenoise::knobChanged(KnobI* k,
                           ValueChangedReasonEnum reason,
                           ViewSpec /*view*/,
                           double /*time*/,
                           bool /*originatedFromMainThread*/)
{
    if (_model && (k == _model->modelKnob().get())) {
        _colourspace.modelChanged();

        return true;
    }
    if (_model && _model->knobChanged(k)) {
        return true;
    }

    return _colourspace.knobChanged(k, reason);
}

std::optional<AiColourSpace>
NeuralDenoise::selectedModelSpace() const
{
    if (!_model || !_downloader) {
        return std::nullopt;
    }
    const AiModelEntry* entry = _downloader->registry()->find(_model->modelId());
    if (!entry) {
        return std::nullopt;
    }
    switch (entry->task) {
    case AiModelTask::eDenoiseLinear:
        return AiColourSpace::eLinearRec2020;
    case AiModelTask::eDenoise:
        return AiColourSpace::eSrgb;
    default:
        break;
    }

    return std::nullopt;
}

void
NeuralDenoise::getRegionsOfInterest(double time,
                                    const RenderScale& scale,
                                    const RectD& outputRoD,
                                    const RectD& renderWindow,
                                    ViewIdx view,
                                    RoIMap* ret)
{
    StagedImageEffect::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    EffectInstancePtr source = getInput(kNeuralDenoiseInputSource);
    if (!source) {
        return;
    }
    const double par = getRenderMetadata(time, view)->getPixelAspectRatio(-1);
    const double dx = kOverlap * par / scale.toOfxPointD().x;
    const double dy = kOverlap / scale.toOfxPointD().y;
    const RectD grown(renderWindow.x1 - dx, renderWindow.y1 - dy, renderWindow.x2 + dx, renderWindow.y2 + dy);
    RoIMap::iterator found = ret->find(source);
    if (found == ret->end()) {
        ret->insert(std::make_pair(source, grown));
    } else {
        found->second.merge(grown);
    }
}

bool
NeuralDenoise::isOpIdentity(double time,
                            const RenderScale& /*scale*/,
                            const RectI& /*roi*/,
                            ViewIdx view)
{
    KnobDoublePtr strength = _strength.lock();

    return strength && (strength->getValueAtTime(time, 0, view) == 0.);
}

void
NeuralDenoise::postError(const std::string& message)
{
    const NodePtr node = getNode();

    if (!node) {
        return;
    }
    {
        std::lock_guard<std::mutex> locker(_errorMutex);
        _postedError = message;
    }
    QString current;
    int type = 0;
    node->getPersistentMessage(&current, &type, false);
    if ((type == eMessageTypeError) && (current == QString::fromStdString(message))) {
        return;
    }
    node->setPersistentMessage(eMessageTypeError, message);
}

void
NeuralDenoise::clearError()
{
    std::string posted;
    {
        std::lock_guard<std::mutex> locker(_errorMutex);
        posted.swap(_postedError);
    }
    const NodePtr node = getNode();
    if (posted.empty() || !node) {
        return;
    }
    QString current;
    int type = 0;
    node->getPersistentMessage(&current, &type, false);
    if (current == QString::fromStdString(posted)) {
        node->clearPersistentMessage(false);
    }
}

StatusEnum
NeuralDenoise::renderOp(const RenderActionArgs& args,
                        const StagePlan& plan)
{
    KnobDoublePtr strengthKnob = _strength.lock();
    const float strength = strengthKnob ? (float)strengthKnob->getValueAtTime(args.time, 0, args.view) : 1.f;

    const std::optional<ModelKnob::Resolved> resolved = _model ? _model->resolve() : std::optional<ModelKnob::Resolved>();
    if (!resolved) {
        return eStatusFailed;
    }
    if (resolved->files.empty()) {
        postError(tr("The model %1 lists no ONNX file.").arg(QString::fromStdString(resolved->entry->id)).toStdString());

        return eStatusFailed;
    }

    const ModelColourspaceKnobs::Conversion conversion = _colourspace.conversion(args.time);
    if (!conversion.error.empty()) {
        postError(conversion.error);

        return eStatusFailed;
    }

    std::shared_ptr<InferenceSession> session;
    try {
        session = InferenceSession::open(resolved->files.front(), AiSessionOptions::fromPreferences(AiGraphOptimization::eAll));
    } catch (const AiError& e) {
        const std::shared_ptr<const OrtLoader> loader = OrtLoader::process();
        postError((loader && !loader->available()) ? loader->unavailableReason() : std::string(e.what()));

        return eStatusFailed;
    }

    KnobIntPtr tileSizeKnob = _tileSize.lock();
    const int tileSize = tileSizeKnob ? tileSizeKnob->getValue() : 0;
    AiTileSpec spec;
    spec.tile = (tileSize > 0) ? tileSize : ((resolved->config.tileSize() > 0) ? resolved->config.tileSize() : kDefaultTile);
    spec.tile = std::max(spec.tile, 8);
    spec.overlap = std::min(kOverlap, spec.tile / 4);
    // The U-Nets halve the image four times.
    spec.multipleOf = 16;
    spec.scale = 1;
    spec.inChannels = 3;
    spec.outChannels = 3;

    const RectI& roi = args.roi;
    const int roiWidth = roi.width();

    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        if (!it->second) {
            continue;
        }
        const int nComps = (int)it->second->getComponentsCount();

        // The denoised RGB over the window, in the working space; empty for a plane the model
        // does not run on, which keeps the Source.
        std::vector<float> denoised;
        if (nComps >= 3) {
            ImagePtr src;
            ImageLayerDesc sourceLayer;
            if (resolveInputPlaneForRender(kNeuralDenoiseInputSource, args.time, args.view, &sourceLayer, NULL)) {
                RectI sourceRoI;
                src = getImage(kNeuralDenoiseInputSource, args.time, args.mappedScale, args.view, NULL, &sourceLayer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &sourceRoI);
            }
            if (src && !PassExec::isFloatImage(src)) {
                return eStatusFailed;
            }
            denoised.assign((std::size_t)roiWidth * roi.height() * 3, 0.f);
            const RectI srcBounds = src ? src->getBounds() : RectI();
            const RectI grown(roi.x1 - spec.overlap, roi.y1 - spec.overlap, roi.x2 + spec.overlap, roi.y2 + spec.overlap);
            RectI run;
            if (src && !srcBounds.isNull() && PassExec::intersectRects(grown, srcBounds, &run) && !run.isNull()) {
                // Locked only while the model runs: runStagesCpu() fetches the Source again.
                const Image::ReadAccess access(src.get());
                const int srcNComps = (int)src->getComponentsCount();

                const AiTilePack pack = [&](const RectI& rect, Tensor& tile) {
                    const int width = rect.width();
                    const std::size_t planeSize = (std::size_t)width * rect.height();
                    std::vector<float> rgb((std::size_t)width * 3);
                    for (int r = 0; r < rect.height(); ++r) {
                        const int y = rect.y2 - 1 - r;
                        PassExec::readSourceRow(src.get(), &access, srcBounds, srcNComps, rect.x1, y, width, 3, &rgb[0]);
                        ModelColourspaceKnobs::apply(conversion.toModel, &rgb[0], (std::size_t)width);
                        float* dst = tile.data() + (std::size_t)r * width;
                        for (int x = 0; x < width; ++x) {
                            dst[x] = rgb[(std::size_t)x * 3];
                            dst[planeSize + x] = rgb[(std::size_t)x * 3 + 1];
                            dst[2 * planeSize + x] = rgb[(std::size_t)x * 3 + 2];
                        }
                    }
                };
                const AiTileUnpack unpack = [&](const RectI& outRoI, const Tensor& result) {
                    RectI window;
                    if (!PassExec::intersectRects(roi, outRoI, &window) || window.isNull()) {
                        return;
                    }
                    const int outWidth = outRoI.width();
                    const std::size_t planeSize = (std::size_t)outWidth * outRoI.height();
                    const int width = window.width();
                    for (int y = window.y1; y < window.y2; ++y) {
                        const float* srcRow = result.data() + (std::size_t)(outRoI.y2 - 1 - y) * outWidth + (window.x1 - outRoI.x1);
                        float* rgb = &denoised[((std::size_t)(y - roi.y1) * roiWidth + (window.x1 - roi.x1)) * 3];
                        for (int x = 0; x < width; ++x) {
                            rgb[(std::size_t)x * 3] = srcRow[x];
                            rgb[(std::size_t)x * 3 + 1] = srcRow[planeSize + x];
                            rgb[(std::size_t)x * 3 + 2] = srcRow[2 * planeSize + x];
                        }
                        ModelColourspaceKnobs::apply(conversion.fromModel, rgb, (std::size_t)width);
                    }
                };
                try {
                    (void)runTiled(session, run, spec, pack, unpack, [this]() { return aborted(); });
                } catch (const AiAborted&) {
                    return eStatusFailed;
                } catch (const AiError& e) {
                    postError(e.what());

                    return eStatusFailed;
                } catch (const std::exception& e) {
                    postError(tr("Denoising failed: %1").arg(QString::fromUtf8(e.what())).toStdString());

                    return eStatusFailed;
                }
            }
        }

        const auto row = [&](const RowIO& io) {
            const float* src = io.src[0];
            for (int c = 0; c < io.nComps; ++c) {
                if (!io.channels[pixelKernelChannelBit(io.nComps, c)]) {
                    continue;
                }
                for (int i = 0; i < io.width; ++i) {
                    const std::size_t at = (std::size_t)i * io.nComps + c;
                    const float in = src ? src[at] : 0.f;
                    float out = in;
                    if (!denoised.empty() && (c < 3)) {
                        const float model = denoised[((std::size_t)(io.y - roi.y1) * roiWidth + (io.x0 + i - roi.x1)) * 3 + c];
                        out = in + strength * (model - in);
                    }
                    io.dst[at] = out;
                }
            }
        };

        EffectInstance::RenderActionArgs planeArgs = args;
        planeArgs.outputLayers.clear();
        planeArgs.outputLayers.push_back(*it);
        const StatusEnum status = runStagesCpu(*this, plan, planeArgs, StageRowFn(row));
        if (status != eStatusOK) {
            return status;
        }
    }
    clearError();

    return eStatusOK;
} // NeuralDenoise::renderOp

NATRON_NAMESPACE_EXIT
