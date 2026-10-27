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

#include "NeuralUpscale.h"

#include <algorithm>
#include <cmath>
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
#include "Engine/MemoryInfo.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Settings.h"

NATRON_NAMESPACE_ENTER

NeuralUpscale::NeuralUpscale(NodePtr node)
    : StagedImageEffect(node)
    , _downloader(nullptr)
    , _model()
    , _colourspace()
    , _scale()
    , _alphaFilter()
    , _tileSize()
    , _errorMutex()
    , _postedError()
    , _warnMutex()
    , _warnedOutput()
{
}

NeuralUpscale::~NeuralUpscale()
{
    _colourspace.unwatchConfig();
}

ModelDownloader*&
NeuralUpscale::downloaderForTesting()
{
    static ModelDownloader* downloader = nullptr;

    return downloader;
}

namespace {
// Each model band's result is held whole, so a large window runs in bands of about this size.
const std::size_t kBandBytes = (std::size_t)256 * 1024 * 1024;

inline int
floorDiv(int a,
         int b)
{
    return (a >= 0) ? (a / b) : -((-a + b - 1) / b);
}

inline int
ceilDiv(int a,
        int b)
{
    return -floorDiv(-a, b);
}

AiModelTask
taskForFactor(int factor)
{
    return (factor == 4) ? AiModelTask::eUpscale4x : AiModelTask::eUpscale2x;
}

// The Source's region of definition in pixels at scale.
bool
sourcePixelRoD(EffectInstance& effect,
               double time,
               const RenderScale& scale,
               ViewIdx view,
               RectI* rod)
{
    EffectInstancePtr input = effect.getInput(kNeuralUpscaleInputSource);

    if (!input) {
        return false;
    }
    RectD canonical;
    bool isProjectFormat = false;
    if (input->getRegionOfDefinition_public(input->getRenderHash(), time, RenderScale::identity, view, &canonical, &isProjectFormat) == eStatusFailed) {
        return false;
    }
    *rod = PassExec::toPixelEnclosing(canonical, scale, effect.getRenderMetadata(time, view)->getPixelAspectRatio(-1));

    return !rod->isNull();
}

// The source pixels output pixel x reads, clamped into [lo, hi), and their weights.
struct Taps {
    static constexpr int kMax = 6;
    int index[kMax];
    float weight[kMax];
    int count = 0;
};

Taps
tapsFor(int x,
        int factor,
        NeuralUpscale::AlphaFilterEnum filter,
        int lo,
        int hi)
{
    // Pixel centres: output pixel x sits at source position (x + 0.5) / factor, which is u in
    // source index space, as Resampler::interpolate() places it.
    const double u = (x + 0.5) / factor - 0.5;
    const int base = (int)std::floor(u);
    const double d = u - base;
    Taps taps;

    if (filter == NeuralUpscale::eAlphaFilterCubic) {
        double w[4];
        Resampler::filterWeights(Resampler::eFilterCubic, d, w);
        taps.count = 4;
        for (int k = 0; k < 4; ++k) {
            taps.index[k] = std::min(std::max(base - 1 + k, lo), hi - 1);
            taps.weight[k] = (float)w[k];
        }
    } else {
        double w[Taps::kMax];
        double sum = 0.;
        for (int k = 0; k < Taps::kMax; ++k) {
            w[k] = NeuralUpscale::lanczos3(u - (base - 2 + k));
            sum += w[k];
        }
        taps.count = Taps::kMax;
        for (int k = 0; k < Taps::kMax; ++k) {
            taps.index[k] = std::min(std::max(base - 2 + k, lo), hi - 1);
            taps.weight[k] = (float)(w[k] / sum);
        }
    }

    return taps;
}

// Enlarges channels of the image whose rows readRow() reads (nComps floats per pixel over
// frame.x1 to frame.x2) into the rows dstRow() gives over window, separably, the edges clamped
// to frame. Returns false when aborted.
template <typename ReadRow, typename DstRow, typename Aborted>
bool
resampleChannels(const ReadRow& readRow,
                 const DstRow& dstRow,
                 const Aborted& aborted,
                 const RectI& frame,
                 int nComps,
                 const RectI& window,
                 int factor,
                 NeuralUpscale::AlphaFilterEnum filter,
                 const std::vector<int>& channels)
{
    const int width = window.width();
    const int nChannels = (int)channels.size();

    if ((width <= 0) || (nChannels == 0)) {
        return true;
    }
    std::vector<Taps> columns(width);
    for (int i = 0; i < width; ++i) {
        columns[i] = tapsFor(window.x1 + i, factor, filter, frame.x1, frame.x2);
    }
    std::vector<Taps> rows(window.height());
    int firstRow = frame.y2;
    int endRow = frame.y1;
    for (int j = 0; j < window.height(); ++j) {
        rows[j] = tapsFor(window.y1 + j, factor, filter, frame.y1, frame.y2);
        for (int k = 0; k < rows[j].count; ++k) {
            firstRow = std::min(firstRow, rows[j].index[k]);
            endRow = std::max(endRow, rows[j].index[k] + 1);
        }
    }
    if (endRow <= firstRow) {
        return true;
    }

    // Horizontal pass over every source row the vertical pass reads.
    std::vector<float> source((std::size_t)frame.width() * nComps);
    std::vector<float> horizontal((std::size_t)(endRow - firstRow) * width * nChannels);
    for (int sy = firstRow; sy < endRow; ++sy) {
        if (aborted()) {
            return false;
        }
        readRow(sy, source.data());
        float* out = &horizontal[(std::size_t)(sy - firstRow) * width * nChannels];
        for (int i = 0; i < width; ++i) {
            const Taps& taps = columns[i];
            for (int k = 0; k < nChannels; ++k) {
                const int c = channels[k];
                float sum = 0.f;
                for (int t = 0; t < taps.count; ++t) {
                    sum += taps.weight[t] * source[(std::size_t)(taps.index[t] - frame.x1) * nComps + c];
                }
                out[(std::size_t)i * nChannels + k] = sum;
            }
        }
    }

    for (int j = 0; j < window.height(); ++j) {
        if (((j % 64) == 0) && aborted()) {
            return false;
        }
        float* dst = dstRow(window.y1 + j);
        if (!dst) {
            continue;
        }
        const Taps& taps = rows[j];
        for (int i = 0; i < width; ++i) {
            for (int k = 0; k < nChannels; ++k) {
                float sum = 0.f;
                for (int t = 0; t < taps.count; ++t) {
                    sum += taps.weight[t] * horizontal[((std::size_t)(taps.index[t] - firstRow) * width + i) * nChannels + k];
                }
                dst[(std::size_t)i * nComps + channels[k]] = sum;
            }
        }
    }

    return true;
}
} // anonymous namespace

double
NeuralUpscale::lanczos3(double t)
{
    t = std::fabs(t);
    if (t >= 3.) {
        return 0.;
    }
    if (t < 1e-8) {
        return 1.;
    }
    const double pt = M_PI * t;

    return 3. * std::sin(pt) * std::sin(pt / 3.) / (pt * pt);
}

std::string
NeuralUpscale::largeOutputWarning(const RectI& outputPixels,
                                  int nComps,
                                  U64 cacheBytes)
{
    const double pixels = (double)std::max(outputPixels.width(), 0) * (double)std::max(outputPixels.height(), 0);
    const double megapixels = pixels / 1e6;
    const double bytes = pixels * (double)std::max(nComps, 1) * sizeof(float);
    const bool large = megapixels >= kLargeOutputMegapixels;
    const bool overCache = (cacheBytes > 0) && (bytes > (double)cacheBytes);

    if (!large && !overCache) {
        return std::string();
    }
    QString message = tr("The output is %1 x %2 pixels (%3 megapixels), so upscaling it will be slow")
                          .arg(outputPixels.width())
                          .arg(outputPixels.height())
                          .arg(megapixels, 0, 'f', 1);
    if (overCache) {
        message += tr(", and one image of it (%1 GB) is larger than the image cache (%2 GB), so it will be rendered again "
                      "whenever it is needed")
                       .arg(bytes / (1024. * 1024. * 1024.), 0, 'f', 1)
                       .arg((double)cacheBytes / (1024. * 1024. * 1024.), 0, 'f', 1);
    }
    message += QLatin1Char('.');

    return message.toStdString();
}

const NodeSpec*
NeuralUpscale::nodeSpec() const
{
    return staticNodeSpec<kNeuralUpscaleNodeSpec>();
}

const ImagePipelineSpec&
NeuralUpscale::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kNeuralUpscalePipeline, kNeuralUpscaleNodeSpec>();
}

std::string
NeuralUpscale::getInputHint(int inputNb) const
{
    if (inputNb == kNeuralUpscaleInputSource) {
        return tr("The image to upscale.").toStdString();
    }

    return std::string();
}

void
NeuralUpscale::addAcceptedComponents(int /*inputNb*/,
                                     std::list<ImageLayerDesc>* comps)
{
    comps->push_back(ImageLayerDesc::getRGBAComponents());
    comps->push_back(ImageLayerDesc::getRGBComponents());
    comps->push_back(ImageLayerDesc::getAlphaComponents());
}

void
NeuralUpscale::initializeKnobs()
{
    KnobPagePtr page = createControlsPage();

    _downloader = downloaderForTesting() ? downloaderForTesting() : ModelDownloader::process();

    KnobChoicePtr scale = createKnob<KnobChoice>(tr("Scale"));
    scale->setName(kNeuralUpscaleParamScale);
    scale->setHintToolTip(tr("How many times the image is enlarged along each axis, as darktable's scale. Each factor "
                             "has its own models: changing it selects the same model for the other factor, and "
                             "selecting a model sets it."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kNeuralUpscaleParamScale2x, tr("2x").toStdString(), tr("Twice the size.").toStdString()));
        options.push_back(ChoiceOption(kNeuralUpscaleParamScale4x, tr("4x").toStdString(), tr("Four times the size.").toStdString()));
        scale->populateChoices(options);
    }
    scale->setDefaultValue(0);
    scale->setAnimationEnabled(false);
    scale->setIsMetadataSlave(true);
    page->addKnob(scale);
    _scale = scale;

    _model = ModelKnob::create(this, page, std::vector<AiModelTask> { AiModelTask::eUpscale2x, AiModelTask::eUpscale4x }, _downloader);

    _colourspace.create(this, page, [this]() { return selectedModelSpace(); });

    KnobChoicePtr alphaFilter = createKnob<KnobChoice>(tr("Alpha Filter"));
    alphaFilter->setName(kNeuralUpscaleParamAlphaFilter);
    alphaFilter->setHintToolTip(tr("How alpha, and any channel left unchecked, is enlarged; the model only sees RGB."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kNeuralUpscaleParamAlphaFilterCubic, tr("Cubic").toStdString(),
                                       tr("Natron's cubic filter: smooth, never overshoots.").toStdString()));
        options.push_back(ChoiceOption(kNeuralUpscaleParamAlphaFilterLanczos, tr("Lanczos").toStdString(),
                                       tr("Lanczos-3: sharper edges, which may ring a little past 0 and 1.").toStdString()));
        alphaFilter->populateChoices(options);
    }
    alphaFilter->setDefaultValue((int)eAlphaFilterCubic);
    page->addKnob(alphaFilter);
    _alphaFilter = alphaFilter;

    KnobIntPtr tileSize = createKnob<KnobInt>(tr("Tile Size"));
    tileSize->setName(kNeuralUpscaleParamTileSize);
    tileSize->setHintToolTip(tr("The edge of the square tiles the model runs over, in Source pixels. 0 takes the size "
                                "the model was exported at, or 512 for a model that takes any size. A model exported at "
                                "a fixed size never runs larger tiles. Smaller tiles need less memory; neighbouring "
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
NeuralUpscale::onKnobsLoaded()
{
    _colourspace.refreshMenus();
}

void
NeuralUpscale::onEffectCreated(bool /*mayCreateFileDialog*/,
                               const CreateNodeArgs& args)
{
    KnobChoicePtr scale = _scale.lock();
    const int chosen = scale ? scale->getValue() : 0;

    if (_model) {
        // The user's default model may be one for the other factor, which would move Scale.
        _model->onEffectCreated(args);
    }
    if (scale && (scale->getValue() != chosen)) {
        scale->setValue(chosen);
    }
    syncModelToScale();
    _colourspace.modelChanged();
}

bool
NeuralUpscale::knobChanged(KnobI* k,
                           ValueChangedReasonEnum reason,
                           ViewSpec /*view*/,
                           double /*time*/,
                           bool /*originatedFromMainThread*/)
{
    KnobChoicePtr scale = _scale.lock();

    if (scale && (k == scale.get())) {
        syncModelToScale();

        return true;
    }
    if (_model && (k == _model->modelKnob().get())) {
        syncScaleToModel();
        _colourspace.modelChanged();

        return true;
    }
    if (_model && _model->knobChanged(k)) {
        return true;
    }

    return _colourspace.knobChanged(k, reason);
}

int
NeuralUpscale::factor() const
{
    KnobChoicePtr scale = _scale.lock();

    return (scale && (scale->getValue() == 1)) ? 4 : 2;
}

const AiModelEntry*
NeuralUpscale::selectedModel() const
{
    if (!_model || !_downloader) {
        return nullptr;
    }

    return _downloader->registry()->find(_model->modelId());
}

std::optional<AiColourSpace>
NeuralUpscale::selectedModelSpace() const
{
    const AiModelEntry* entry = selectedModel();

    if (entry && ((entry->task == AiModelTask::eUpscale2x) || (entry->task == AiModelTask::eUpscale4x))) {
        return AiColourSpace::eSrgb;
    }

    return std::nullopt;
}

void
NeuralUpscale::syncModelToScale()
{
    KnobChoicePtr model = _model ? _model->modelKnob() : KnobChoicePtr();

    if (!model || !_downloader) {
        return;
    }
    const AiModelTask want = taskForFactor(factor());
    const AiModelEntry* current = selectedModel();
    if (current && (current->task == want)) {
        return;
    }
    const ModelRegistry& registry = *_downloader->registry();
    const AiModelEntry* pick = nullptr;
    if (current) {
        for (const AiModelEntry* sibling : registry.packageModels(*current)) {
            if (sibling->task == want) {
                pick = sibling;
                break;
            }
        }
    }
    if (!pick) {
        pick = ModelKnob::defaultModel(registry, want);
    }
    if (pick && (pick->id != _model->modelId())) {
        model->setValueFromID(pick->id, 0);
    }
}

void
NeuralUpscale::syncScaleToModel()
{
    KnobChoicePtr scale = _scale.lock();
    const AiModelEntry* entry = selectedModel();

    if (!scale || !entry) {
        return;
    }
    int index = -1;
    if (entry->task == AiModelTask::eUpscale2x) {
        index = 0;
    } else if (entry->task == AiModelTask::eUpscale4x) {
        index = 1;
    }
    if ((index >= 0) && (scale->getValue() != index)) {
        scale->setValue(index);
    }
}

StatusEnum
NeuralUpscale::getPreferredMetadataAt(double time,
                                      ViewIdx view,
                                      NodeMetadata& metadata)
{
    const StatusEnum status = StagedImageEffect::getPreferredMetadataAt(time, view, metadata);

    if ((status == eStatusFailed) || !getInput(kNeuralUpscaleInputSource)) {
        return status;
    }
    const RectI format = metadata.getOutputFormat();
    const int f = factor();
    metadata.setOutputFormat(RectI(format.x1 * f, format.y1 * f, format.x2 * f, format.y2 * f));

    return eStatusOK;
}

ImageMetadata
NeuralUpscale::deriveOutputMetadata(double time,
                                    ViewIdx view)
{
    ImageMetadata metadata = StagedImageEffect::deriveOutputMetadata(time, view);

    metadata.setInt(kNeuralUpscaleMetadataFactor, factor());

    return metadata;
}

StatusEnum
NeuralUpscale::getRegionOfDefinition(U64 /*hash*/,
                                     double time,
                                     const RenderScale& scale,
                                     ViewIdx view,
                                     RectD* rod)
{
    EffectInstancePtr source = getInput(kNeuralUpscaleInputSource);

    if (!source) {
        rod->clear();

        return eStatusOK;
    }
    RectD sourceRoD;
    bool isProjectFormat = false;
    if (source->getRegionOfDefinition_public(source->getRenderHash(), time, scale, view, &sourceRoD, &isProjectFormat) == eStatusFailed) {
        return eStatusFailed;
    }
    const double f = factor();
    *rod = RectD(sourceRoD.x1 * f, sourceRoD.y1 * f, sourceRoD.x2 * f, sourceRoD.y2 * f);

    return eStatusOK;
}

void
NeuralUpscale::getRegionsOfInterest(double time,
                                    const RenderScale& scale,
                                    const RectD& outputRoD,
                                    const RectD& renderWindow,
                                    ViewIdx view,
                                    RoIMap* ret)
{
    StagedImageEffect::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    EffectInstancePtr source = getInput(kNeuralUpscaleInputSource);
    if (!source) {
        return;
    }
    // The overlap is in Source pixels at the render scale, as the tiles are; it also covers the
    // alpha filter's reach.
    const double f = factor();
    const double par = getRenderMetadata(time, view)->getPixelAspectRatio(-1);
    const double dx = kOverlap * par / scale.toOfxPointD().x;
    const double dy = kOverlap / scale.toOfxPointD().y;
    (*ret)[source] = RectD(renderWindow.x1 / f - dx, renderWindow.y1 / f - dy, renderWindow.x2 / f + dx, renderWindow.y2 / f + dy);
}

void
NeuralUpscale::filterPassThroughLayers(double /*time*/,
                                       ViewIdx /*view*/,
                                       std::list<ImageLayerDesc>* layers)
{
    layers->clear();
}

bool
NeuralUpscale::isOpIdentity(double /*time*/,
                            const RenderScale& /*scale*/,
                            const RectI& /*roi*/,
                            ViewIdx /*view*/)
{
    return false;
}

void
NeuralUpscale::warnLargeOutput(const RectI& outputPixels,
                               int nComps)
{
    const SettingsPtr settings = appPTR ? appPTR->getCurrentSettings() : SettingsPtr();
    const U64 cacheBytes = settings ? (U64)(settings->getRamMaximumPercent() * (double)getSystemTotalRAM()) : 0;
    const std::string warning = largeOutputWarning(outputPixels, nComps, cacheBytes);

    if (warning.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> locker(_warnMutex);
        if (_warnedOutput == outputPixels) {
            return;
        }
        _warnedOutput = outputPixels;
    }
    const NodePtr node = getNode();
    InferenceSession::logWarning((node ? node->getScriptName_mt_safe() + ": " : std::string()) + warning);
}

void
NeuralUpscale::postError(const std::string& message)
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
NeuralUpscale::clearError()
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
NeuralUpscale::renderOp(const RenderActionArgs& args,
                        const StagePlan& plan)
{
    const int f = factor();
    KnobChoicePtr filterKnob = _alphaFilter.lock();
    const AlphaFilterEnum filter = (filterKnob && (filterKnob->getValueAtTime(args.time, 0, args.view) == (int)eAlphaFilterLanczos)) ? eAlphaFilterLanczos : eAlphaFilterCubic;

    RectI sourceRoD;
    const bool haveRoD = sourcePixelRoD(*this, args.time, args.mappedScale, args.view, &sourceRoD);
    if (haveRoD && !args.outputLayers.empty() && args.outputLayers.front().second) {
        warnLargeOutput(RectI(sourceRoD.x1 * f, sourceRoD.y1 * f, sourceRoD.x2 * f, sourceRoD.y2 * f),
                        (int)args.outputLayers.front().second->getComponentsCount());
    }

    // Opened for the first plane that needs the model.
    std::shared_ptr<InferenceSession> session;
    ModelColourspaceKnobs::Conversion conversion;
    AiTileSpec spec;
    const auto openModel = [&]() -> bool {
        if (session) {
            return true;
        }
        const std::optional<ModelKnob::Resolved> resolved = _model ? _model->resolve() : std::optional<ModelKnob::Resolved>();
        if (!resolved) {
            return false;
        }
        if (resolved->files.empty()) {
            postError(tr("The model %1 lists no ONNX file.").arg(QString::fromStdString(resolved->entry->id)).toStdString());

            return false;
        }
        if (resolved->entry->task != taskForFactor(f)) {
            postError(tr("The model %1 does not upscale %2 times; select a %2x model or change Scale.")
                          .arg(QString::fromStdString(resolved->entry->id))
                          .arg(f)
                          .toStdString());

            return false;
        }
        conversion = _colourspace.conversion(args.time);
        if (!conversion.error.empty()) {
            postError(conversion.error);

            return false;
        }
        try {
            session = InferenceSession::open(resolved->files.front(), AiSessionOptions::fromPreferences(AiGraphOptimization::eAll));
        } catch (const AiError& e) {
            const std::shared_ptr<const OrtLoader> loader = OrtLoader::process();
            postError((loader && !loader->available()) ? loader->unavailableReason() : std::string(e.what()));

            return false;
        }
        KnobIntPtr tileSizeKnob = _tileSize.lock();
        const int tileSize = tileSizeKnob ? tileSizeKnob->getValue() : 0;
        spec.tile = (tileSize > 0) ? tileSize : ((resolved->config.tileSize() > 0) ? resolved->config.tileSize() : kDefaultTile);
        spec.tile = std::max(spec.tile, 8);
        spec.overlap = std::min(kOverlap, spec.tile / 4);
        spec.multipleOf = 1;
        spec.scale = f;
        spec.inChannels = 3;
        spec.outChannels = 3;

        return true;
    };
    const auto isAborted = [this]() { return aborted(); };

    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        const ImagePtr& dstImage = it->second;
        if (!dstImage) {
            continue;
        }
        if (!PassExec::isFloatImage(dstImage)) {
            return eStatusFailed;
        }
        const int nComps = (int)dstImage->getComponentsCount();
        RectI window;
        if (!PassExec::intersectRects(args.roi, dstImage->getBounds(), &window) || window.isNull()) {
            continue;
        }

        ImagePtr src;
        ImageLayerDesc sourceLayer;
        if (resolveInputPlaneForRender(kNeuralUpscaleInputSource, args.time, args.view, &sourceLayer, NULL)) {
            RectI sourceRoI;
            src = getImage(kNeuralUpscaleInputSource, args.time, args.mappedScale, args.view, NULL, &sourceLayer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &sourceRoI);
        }
        if (src && !PassExec::isFloatImage(src)) {
            return eStatusFailed;
        }
        const RectI srcBounds = src ? src->getBounds() : RectI();

        Image::WriteAccess dstAccess(dstImage.get());
        const auto dstRow = [&](int y) -> float* {
            return reinterpret_cast<float*>(dstAccess.pixelAt(window.x1, y));
        };
        for (int y = window.y1; y < window.y2; ++y) {
            if (float* row = dstRow(y)) {
                std::fill(row, row + (std::size_t)window.width() * nComps, 0.f);
            }
        }

        RectI frame;
        if (!src || srcBounds.isNull() || !haveRoD || !PassExec::intersectRects(sourceRoD, srcBounds, &frame) || frame.isNull()) {
            continue;
        }
        const Image::ReadAccess srcAccess(src.get());
        const int srcNComps = (int)src->getComponentsCount();

        // The model enlarges the checked RGB channels; the alpha filter every other channel.
        bool modelChannel[4] = { false, false, false, false };
        bool anyModel = false;
        std::vector<int> filtered;
        for (int c = 0; c < nComps; ++c) {
            modelChannel[c] = (nComps >= 3) && (c < 3) && plan.processChannels[pixelKernelChannelBit(nComps, c)];
            anyModel = anyModel || modelChannel[c];
            if (!modelChannel[c]) {
                filtered.push_back(c);
            }
        }

        const auto readRow = [&](int y, float* row) {
            PassExec::readSourceRow(src.get(), &srcAccess, srcBounds, srcNComps, frame.x1, y, frame.width(), nComps, row);
        };
        if (!resampleChannels(readRow, dstRow, isAborted, frame, nComps, window, f, filter, filtered)) {
            return eStatusFailed;
        }
        if (!anyModel) {
            continue;
        }
        if (!openModel()) {
            return eStatusFailed;
        }

        // The Source pixels under the window, run in bands whose results stay bounded.
        RectI target;
        const RectI under(floorDiv(window.x1, f), floorDiv(window.y1, f), ceilDiv(window.x2, f), ceilDiv(window.y2, f));
        if (!PassExec::intersectRects(under, frame, &target) || target.isNull()) {
            continue;
        }
        const std::size_t rowBytes = (std::size_t)target.width() * f * f * 3 * sizeof(float);
        const int bandRows = (int)std::max<std::size_t>(1, kBandBytes / std::max<std::size_t>(rowBytes, 1));
        for (int by = target.y1; by < target.y2; by += bandRows) {
            const RectI band(target.x1, by, target.x2, std::min(by + bandRows, target.y2));
            const RectI grown(band.x1 - spec.overlap, band.y1 - spec.overlap, band.x2 + spec.overlap, band.y2 + spec.overlap);
            RectI run;
            if (!PassExec::intersectRects(grown, frame, &run) || run.isNull()) {
                continue;
            }
            const AiTilePack pack = [&](const RectI& rect, Tensor& tile) {
                const int width = rect.width();
                const std::size_t planeSize = (std::size_t)width * rect.height();
                std::vector<float> rgb((std::size_t)width * 3);
                for (int r = 0; r < rect.height(); ++r) {
                    PassExec::readSourceRow(src.get(), &srcAccess, srcBounds, srcNComps, rect.x1, rect.y2 - 1 - r, width, 3, &rgb[0]);
                    ModelColourspaceKnobs::apply(conversion.toModel, &rgb[0], (std::size_t)width);
                    float* dst = tile.data() + (std::size_t)r * width;
                    for (int x = 0; x < width; ++x) {
                        dst[x] = rgb[(std::size_t)x * 3];
                        dst[planeSize + x] = rgb[(std::size_t)x * 3 + 1];
                        dst[2 * planeSize + x] = rgb[(std::size_t)x * 3 + 2];
                    }
                }
            };
            const AiTileUnpack unpack = [&](const RectI& outRoI, const Tensor& tensor) {
                const RectI bandOut(band.x1 * f, band.y1 * f, band.x2 * f, band.y2 * f);
                RectI inBand;
                RectI inside;
                if (!PassExec::intersectRects(bandOut, window, &inBand) || !PassExec::intersectRects(inBand, outRoI, &inside) || inside.isNull()) {
                    return;
                }
                const int outWidth = outRoI.width();
                const std::size_t planeSize = (std::size_t)outWidth * outRoI.height();
                const int width = inside.width();
                std::vector<float> rgb((std::size_t)width * 3);
                for (int y = inside.y1; y < inside.y2; ++y) {
                    float* dst = dstRow(y);
                    if (!dst) {
                        continue;
                    }
                    const float* srcRow = tensor.data() + (std::size_t)(outRoI.y2 - 1 - y) * outWidth + (inside.x1 - outRoI.x1);
                    for (int x = 0; x < width; ++x) {
                        rgb[(std::size_t)x * 3] = srcRow[x];
                        rgb[(std::size_t)x * 3 + 1] = srcRow[planeSize + x];
                        rgb[(std::size_t)x * 3 + 2] = srcRow[2 * planeSize + x];
                    }
                    ModelColourspaceKnobs::apply(conversion.fromModel, &rgb[0], (std::size_t)width);
                    float* pixel = dst + (std::size_t)(inside.x1 - window.x1) * nComps;
                    for (int x = 0; x < width; ++x, pixel += nComps) {
                        for (int c = 0; c < 3; ++c) {
                            if (modelChannel[c]) {
                                pixel[c] = rgb[(std::size_t)x * 3 + c];
                            }
                        }
                    }
                }
            };
            try {
                (void)runTiled(session, run, spec, pack, unpack, isAborted);
            } catch (const AiAborted&) {
                return eStatusFailed;
            } catch (const AiError& e) {
                postError(e.what());

                return eStatusFailed;
            } catch (const std::exception& e) {
                postError(tr("Upscaling failed: %1").arg(QString::fromUtf8(e.what())).toStdString());

                return eStatusFailed;
            }
        }
    }
    clearError();

    return eStatusOK;
} // NeuralUpscale::renderOp

NATRON_NAMESPACE_EXIT
