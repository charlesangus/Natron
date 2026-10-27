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
#include <cmath>
#include <cstddef>
#include <exception>
#include <utility>
#include <vector>

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

#include "Engine/Ai/InferenceSession.h"
#include "Engine/Ai/ModelDownloader.h"
#include "Engine/Ai/ModelRegistry.h"
#include "Engine/Ai/OrtLoader.h"
#include "Engine/Ai/Tensor.h"
#include "Engine/Ai/TiledInference.h"
#include "Engine/AppManager.h"
#include "Engine/Hash64.h"
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
    , _preserveWideGamut()
    , _shadowBoost()
    , _detailRecovery()
    , _detailBands()
    , _tileSize()
    , _frameMutex()
    , _frame()
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

namespace {
// darktable's Rec.709 luminance, applied to whatever space the values are in, as darktable does.
inline float
luma709(const float* rgb)
{
    return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
}

// darktable's dwt_denoise(): the a-trous B3 wavelet over levels bands, keeping of each band only
// what exceeds its threshold, plus the residue. The filter is symmetric, so rows may run either way.
void
waveletDenoise(std::vector<float>& img,
               int width,
               int height,
               const std::vector<float>& noise)
{
    const std::size_t count = (std::size_t)width * height;
    std::vector<float> accum(count, 0.f);
    std::vector<float> interm(count);
    const int bands = (int)noise.size();

    for (int lev = 0; lev < bands; ++lev) {
        const int vscale = std::min(1 << lev, height - 1);
        for (int row = 0; row < height; ++row) {
            const int below = (row + vscale < height) ? (row + vscale) : 2 * (height - 1) - (row + vscale);
            const float* center = &img[(std::size_t)row * width];
            const float* above = &img[(std::size_t)std::abs(row - vscale) * width];
            const float* under = &img[(std::size_t)below * width];
            float* out = &interm[(std::size_t)row * width];
            for (int col = 0; col < width; ++col) {
                out[col] = 2.f * center[col] + above[col] + under[col];
            }
        }
        const int hscale = std::min(1 << lev, width - 1);
        const float threshold = noise[lev];
        const bool last = (lev + 1) == bands;
        for (int row = 0; row < height; ++row) {
            const std::size_t at = (std::size_t)row * width;
            float* details = &img[at];
            const float* coarse = &interm[at];
            float* accumRow = &accum[at];
            for (int col = 0; col < width; ++col) {
                const int right = (col + hscale < width) ? (col + hscale) : 2 * (width - 1) - (col + hscale);
                const float hat = (2.f * coarse[col] + coarse[std::abs(col - hscale)] + coarse[right]) / 16.f;
                const float diff = details[col] - hat;
                details[col] = hat;
                accumRow[col] += std::max(diff - threshold, 0.f) + std::min(diff + threshold, 0.f);
            }
            if (last) {
                for (int col = 0; col < width; ++col) {
                    details[col] += accumRow[col];
                }
            }
        }
    }
}

inline float
signedSqrt(float v)
{
    return (v >= 0.f) ? std::sqrt(v) : -std::sqrt(-v);
}

inline float
signedSquare(float v)
{
    return v * std::fabs(v);
}

// The Source's region of definition in pixels at scale.
bool
sourcePixelRoD(EffectInstance& effect,
               double time,
               const RenderScale& scale,
               ViewIdx view,
               RectI* rod)
{
    EffectInstancePtr input = effect.getInput(kNeuralDenoiseInputSource);

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
} // anonymous namespace

bool
NeuralDenoise::hasDeepShadows(const float* rgb,
                              std::size_t count)
{
    std::size_t dark = 0;

    for (std::size_t i = 0; i < count; ++i) {
        if (luma709(rgb + i * 3) < 0.005f) {
            ++dark;
        }
    }

    return (count > 0) && ((float)dark / (float)count >= 0.10f);
}

std::vector<float>
NeuralDenoise::filteredLuminanceResidual(const float* original,
                                         const float* denoised,
                                         int width,
                                         int height,
                                         const std::array<double, kDetailBands>& bandMultipliers)
{
    const std::size_t count = (std::size_t)std::max(width, 0) * std::max(height, 0);
    std::vector<float> residual(count);

    if (count == 0) {
        return residual;
    }
    double sum = 0.;
    double sum2 = 0.;
    for (std::size_t i = 0; i < count; ++i) {
        float r = luma709(original + i * 3) - luma709(denoised + i * 3);
        if (!std::isfinite(r)) {
            r = 0.f;
        }
        residual[i] = r;
        sum += (double)r;
        sum2 += (double)r * (double)r;
    }
    const double mean = sum / (double)count;
    const float sigma = (float)std::sqrt(std::max(sum2 / (double)count - mean * mean, 0.));
    std::vector<float> noise(kDetailBands);
    for (int b = 0; b < kDetailBands; ++b) {
        noise[b] = sigma * (float)bandMultipliers[b];
    }
    waveletDenoise(residual, width, height, noise);

    return residual;
}

void
NeuralDenoise::passThroughOutOfGamut(const float* original,
                                     const std::vector<unsigned char>& inGamut,
                                     float* denoised,
                                     int width,
                                     int height)
{
    for (std::size_t p = 0; p < inGamut.size(); ++p) {
        if (!inGamut[p]) {
            std::copy(original + p * 3, original + p * 3 + 3, denoised + p * 3);
        }
    }
    // Reads only in-gamut pixels and writes only the others, so the order does not matter.
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t p = (std::size_t)y * width + x;
            if (inGamut[p]) {
                continue;
            }
            float sum = 0.f;
            int count = 0;
            for (int yy = std::max(y - kGamutRadius, 0); yy <= std::min(y + kGamutRadius, height - 1); ++yy) {
                for (int xx = std::max(x - kGamutRadius, 0); xx <= std::min(x + kGamutRadius, width - 1); ++xx) {
                    const std::size_t q = (std::size_t)yy * width + xx;
                    if (inGamut[q]) {
                        sum += luma709(denoised + q * 3);
                        ++count;
                    }
                }
            }
            if (count > 0) {
                const float dY = sum / (float)count - luma709(original + p * 3);
                for (int c = 0; c < 3; ++c) {
                    denoised[p * 3 + c] = original[p * 3 + c] + dY;
                }
            }
        }
    }
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
    strength->setHintToolTip(tr("How strongly the noise is removed, as darktable's. For the sRGB models, below 1 the "
                                "luminance the model removed is put back through a wavelet filter that keeps its noise "
                                "out (see Detail Recovery), which needs the whole frame denoised. For RawNIND linear the "
                                "denoised image is mixed with the Source in the working space, none of it at 0."));
    strength->setMinimum(0.);
    strength->setMaximum(1.);
    strength->setDisplayMinimum(0.);
    strength->setDisplayMaximum(1.);
    strength->setDefaultValue(1.);
    page->addKnob(strength);
    _strength = strength;

    KnobBoolPtr preserve = createKnob<KnobBool>(tr("Preserve Wide Gamut"));
    preserve->setName(kNeuralDenoiseParamPreserveWideGamut);
    preserve->setHintToolTip(tr("sRGB models only. Pixels whose colour lies outside the sRGB gamut (Linear Rec.709 "
                                "beyond 0 to 1 by more than 0.01) keep their Source colour, their luminance moved to "
                                "the mean of the denoised in-gamut pixels around them. Off denoises every pixel, "
                                "wide-gamut colours as the model returns them."));
    preserve->setDefaultValue(true);
    page->addKnob(preserve);
    _preserveWideGamut = preserve;

    KnobChoicePtr shadowBoost = createKnob<KnobChoice>(tr("Shadow Boost"));
    shadowBoost->setName(kNeuralDenoiseParamShadowBoost);
    shadowBoost->setHintToolTip(tr("sRGB models only. Lifts the shadows with a square root in Linear Rec.709 before the "
                                   "model and squares them after, so the model sees dark noise in the range it was "
                                   "trained on."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kNeuralDenoiseParamShadowBoostAuto, tr("Auto").toStdString(),
                                       tr("As darktable: on for a model whose config declares shadow_boost (NAFNet-SIDD) "
                                          "when at least 10% of the frame has a luminance under 0.005.")
                                           .toStdString()));
        options.push_back(ChoiceOption(kNeuralDenoiseParamShadowBoostOn, tr("On").toStdString(), tr("Always boost.").toStdString()));
        options.push_back(ChoiceOption(kNeuralDenoiseParamShadowBoostOff, tr("Off").toStdString(), tr("Never boost.").toStdString()));
        shadowBoost->populateChoices(options);
    }
    shadowBoost->setDefaultValue((int)eShadowBoostAuto);
    page->addKnob(shadowBoost);
    _shadowBoost = shadowBoost;

    KnobGroupPtr detailRecovery = createKnob<KnobGroup>(tr("Detail Recovery"));
    detailRecovery->setName(kNeuralDenoiseParamDetailRecovery);
    detailRecovery->setHintToolTip(tr("sRGB models only. How Strength below 1 brings texture back: the luminance the "
                                      "model removed is split into five wavelet bands, finest first, and of each band "
                                      "only what exceeds its threshold, a multiple of the removed luminance's standard "
                                      "deviation, is kept. Higher thresholds keep less noise and less detail."));
    detailRecovery->setDefaultValue(false);
    page->addKnob(detailRecovery);
    _detailRecovery = detailRecovery;
    for (int b = 0; b < kDetailBands; ++b) {
        KnobDoublePtr band = createKnob<KnobDouble>(tr("Detail Band %1").arg(b + 1));
        band->setName(kNeuralDenoiseParamDetailBand + std::to_string(b + 1));
        band->setHintToolTip(tr("The threshold of wavelet band %1 (1 the finest, 5 the coarsest), in standard deviations "
                                "of the removed luminance.")
                                 .arg(b + 1));
        band->setMinimum(0.);
        band->setMaximum(10.);
        band->setDisplayMinimum(0.);
        band->setDisplayMaximum(1.);
        band->setDefaultValue(kDetailBandDefaults[b]);
        detailRecovery->addKnob(band);
        _detailBands[b] = band;
    }

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
    refreshKnobsForModel();
}

void
NeuralDenoise::purgeCaches()
{
    std::lock_guard<std::mutex> lock(_frameMutex);
    _frame.reset();
}

void
NeuralDenoise::refreshKnobsForModel()
{
    const std::optional<AiModelTask> task = selectedModelTask();
    // An unlisted model enables everything rather than hide what the user set.
    const bool rgbModel = !task || (*task == AiModelTask::eDenoise);

    KnobIPtr knobs[] = { _preserveWideGamut.lock(), _shadowBoost.lock(), _detailRecovery.lock() };
    for (const KnobIPtr& knob : knobs) {
        if (knob) {
            knob->setAllDimensionsEnabled(rgbModel);
        }
    }
    for (const KnobDoubleWPtr& weak : _detailBands) {
        if (KnobDoublePtr band = weak.lock()) {
            band->setAllDimensionsEnabled(rgbModel);
        }
    }
}

void
NeuralDenoise::onEffectCreated(bool /*mayCreateFileDialog*/,
                               const CreateNodeArgs& args)
{
    if (_model) {
        _model->onEffectCreated(args);
    }
    _colourspace.modelChanged();
    refreshKnobsForModel();
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
        refreshKnobsForModel();

        return true;
    }
    if (_model && _model->knobChanged(k)) {
        return true;
    }

    return _colourspace.knobChanged(k, reason);
}

std::optional<AiModelTask>
NeuralDenoise::selectedModelTask() const
{
    if (!_model || !_downloader) {
        return std::nullopt;
    }
    const AiModelEntry* entry = _downloader->registry()->find(_model->modelId());
    if (!entry) {
        return std::nullopt;
    }

    return entry->task;
}

std::optional<AiColourSpace>
NeuralDenoise::selectedModelSpace() const
{
    const std::optional<AiModelTask> task = selectedModelTask();
    if (!task) {
        return std::nullopt;
    }
    switch (*task) {
    case AiModelTask::eDenoiseLinear:
        return AiColourSpace::eLinearRec2020;
    case AiModelTask::eDenoise:
        return AiColourSpace::eSrgb;
    default:
        break;
    }

    return std::nullopt;
}

bool
NeuralDenoise::declaresShadowBoost(const ModelConfig& config)
{
    const QJsonDocument attributes = QJsonDocument::fromJson(QByteArray::fromStdString(config.attributesJson));

    return attributes.isObject() && attributes.object().value(QStringLiteral("shadow_boost")).toBool(false);
}

bool
NeuralDenoise::needsWholeSource(double time,
                                ViewIdx view)
{
    if (selectedModelTask() != AiModelTask::eDenoise) {
        return false;
    }
    KnobDoublePtr strength = _strength.lock();
    if (strength && (strength->getValueAtTime(time, 0, view) < 1.)) {
        return true;
    }
    KnobChoicePtr shadowBoost = _shadowBoost.lock();
    if (!shadowBoost || (shadowBoost->getValueAtTime(time, 0, view) != (int)eShadowBoostAuto) || !_model) {
        return false;
    }
    const std::optional<ModelKnob::Resolved> resolved = _model->resolve();

    return resolved && declaresShadowBoost(resolved->config);
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
    RectD need;
    bool isProjectFormat = false;
    if (!needsWholeSource(time, view) || (source->getRegionOfDefinition_public(source->getRenderHash(), time, RenderScale::identity, view, &need, &isProjectFormat) == eStatusFailed)) {
        const double par = getRenderMetadata(time, view)->getPixelAspectRatio(-1);
        const double dx = (kOverlap + kGamutRadius) * par / scale.toOfxPointD().x;
        const double dy = (kOverlap + kGamutRadius) / scale.toOfxPointD().y;
        need = RectD(renderWindow.x1 - dx, renderWindow.y1 - dy, renderWindow.x2 + dx, renderWindow.y2 + dy);
    }
    RoIMap::iterator found = ret->find(source);
    if (found == ret->end()) {
        ret->insert(std::make_pair(source, need));
    } else {
        found->second.merge(need);
    }
}

bool
NeuralDenoise::isOpIdentity(double time,
                            const RenderScale& /*scale*/,
                            const RectI& /*roi*/,
                            ViewIdx view)
{
    KnobDoublePtr strength = _strength.lock();

    // An sRGB model's strength of 0 still recovers detail through the wavelet filter.
    return strength && (strength->getValueAtTime(time, 0, view) == 0.) && (selectedModelTask() == AiModelTask::eDenoiseLinear);
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

    // darktable's sRGB controls apply to its denoise models, not to its raw ones.
    const bool rgbModel = resolved->entry->task == AiModelTask::eDenoise;
    KnobBoolPtr preserveKnob = _preserveWideGamut.lock();
    const bool preserve = rgbModel && preserveKnob && preserveKnob->getValueAtTime(args.time, 0, args.view);
    KnobChoicePtr shadowBoostKnob = _shadowBoost.lock();
    const int shadowBoost = rgbModel ? (shadowBoostKnob ? shadowBoostKnob->getValueAtTime(args.time, 0, args.view) : (int)eShadowBoostAuto) : (int)eShadowBoostOff;
    const bool mayBoost = (shadowBoost == (int)eShadowBoostOn) || ((shadowBoost == (int)eShadowBoostAuto) && declaresShadowBoost(resolved->config));
    const bool recoverDetail = rgbModel && (strength < 1.f);
    std::array<double, kDetailBands> bands = kDetailBandDefaults;
    for (int b = 0; b < kDetailBands; ++b) {
        if (KnobDoublePtr band = _detailBands[b].lock()) {
            bands[b] = band->getValueAtTime(args.time, 0, args.view);
        }
    }

    const ModelColourspaceKnobs::Conversion conversion = _colourspace.conversion(args.time);
    if (!conversion.error.empty()) {
        postError(conversion.error);

        return eStatusFailed;
    }
    // Working to Linear Rec.709 and back, and Linear Rec.709 to the model space and back: the
    // gamut test and the boost happen in linear sRGB, as darktable's.
    ModelColourspaceKnobs::Conversion linear;
    ModelColourspaceKnobs::Conversion linearToModel;
    if (preserve || mayBoost) {
        const std::string linearSpace = _colourspace.configSpace(AiColourSpace::eLinearRec709);
        if (linearSpace.empty()) {
            postError(tr("The OpenColorIO config has no Linear Rec.709 colourspace, which Preserve Wide Gamut and "
                         "Shadow Boost need. Turn them off or use a config that has one.")
                          .toStdString());

            return eStatusFailed;
        }
        linear = _colourspace.conversionBetween(std::string(), linearSpace, args.time);
        if (linear.error.empty() && mayBoost) {
            linearToModel = _colourspace.conversionBetween(linearSpace, std::string(), args.time);
        }
        const std::string& error = linear.error.empty() ? linearToModel.error : linear.error;
        if (!error.empty()) {
            postError(error);

            return eStatusFailed;
        }
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
    RectI sourceRoD;
    const bool haveRoD = sourcePixelRoD(*this, args.time, args.mappedScale, args.view, &sourceRoD);

    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        if (!it->second) {
            continue;
        }
        const int nComps = (int)it->second->getComponentsCount();

        // The denoised RGB in the working space over window, rows from window.y1; empty for a
        // plane the model does not run on, which keeps the Source. detail is Strength's
        // recovered luminance over the same window, when it applies.
        RectI window;
        std::shared_ptr<const WholeFrame> whole;
        std::vector<float> regionResult;
        const std::vector<float>* denoised = nullptr;
        const std::vector<float>* detail = nullptr;
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
            const RectI srcBounds = src ? src->getBounds() : RectI();
            RectI frame;
            if (src && !srcBounds.isNull() && haveRoD && PassExec::intersectRects(sourceRoD, srcBounds, &frame) && !frame.isNull()) {
                // Locked only while the model runs: runStagesCpu() fetches the Source again.
                const Image::ReadAccess access(src.get());
                const int srcNComps = (int)src->getComponentsCount();
                const auto readRows = [&](const RectI& rect, std::vector<float>* rgb) {
                    rgb->assign((std::size_t)rect.width() * rect.height() * 3, 0.f);
                    for (int y = rect.y1; y < rect.y2; ++y) {
                        PassExec::readSourceRow(src.get(), &access, srcBounds, srcNComps, rect.x1, y, rect.width(), 3, &(*rgb)[(std::size_t)(y - rect.y1) * rect.width() * 3]);
                    }
                };

                // darktable decides once per image, from every 16th pixel from the top left.
                bool boost = (shadowBoost == (int)eShadowBoostOn);
                if (!boost && mayBoost) {
                    std::vector<float> samples;
                    std::vector<float> row((std::size_t)frame.width() * 3);
                    for (int y = frame.y2 - 1; y >= frame.y1; y -= 16) {
                        PassExec::readSourceRow(src.get(), &access, srcBounds, srcNComps, frame.x1, y, frame.width(), 3, &row[0]);
                        for (int x = 0; x < frame.width(); x += 16) {
                            samples.insert(samples.end(), &row[(std::size_t)x * 3], &row[(std::size_t)x * 3] + 3);
                        }
                    }
                    boost = hasDeepShadows(samples.data(), samples.size() / 3);
                }

                // Denoises target, which lies within frame, into result; original gets the Source there.
                const auto denoise = [&](const RectI& target, std::vector<float>* original, std::vector<float>* result) -> StatusEnum {
                    const int targetWidth = target.width();
                    readRows(target, original);
                    result->assign(original->size(), 0.f);
                    std::vector<unsigned char> inGamut;
                    if (preserve) {
                        std::vector<float> linearRgb(*original);
                        ModelColourspaceKnobs::apply(linear.toModel, linearRgb.data(), linearRgb.size() / 3);
                        inGamut.resize(linearRgb.size() / 3);
                        const float m = 0.01f;
                        for (std::size_t p = 0; p < inGamut.size(); ++p) {
                            const float* v = &linearRgb[p * 3];
                            inGamut[p] = (v[0] >= -m) && (v[0] <= 1.f + m) && (v[1] >= -m) && (v[1] <= 1.f + m) && (v[2] >= -m) && (v[2] <= 1.f + m);
                        }
                    }
                    const RectI grown(target.x1 - spec.overlap, target.y1 - spec.overlap, target.x2 + spec.overlap, target.y2 + spec.overlap);
                    RectI run;
                    if (!PassExec::intersectRects(grown, frame, &run) || run.isNull()) {
                        return eStatusOK;
                    }
                    const AiTilePack pack = [&](const RectI& rect, Tensor& tile) {
                        const int width = rect.width();
                        const std::size_t planeSize = (std::size_t)width * rect.height();
                        std::vector<float> rgb((std::size_t)width * 3);
                        for (int r = 0; r < rect.height(); ++r) {
                            const int y = rect.y2 - 1 - r;
                            PassExec::readSourceRow(src.get(), &access, srcBounds, srcNComps, rect.x1, y, width, 3, &rgb[0]);
                            if (boost) {
                                ModelColourspaceKnobs::apply(linear.toModel, &rgb[0], (std::size_t)width);
                                for (float& v : rgb) {
                                    v = signedSqrt(v);
                                }
                                ModelColourspaceKnobs::apply(linearToModel.toModel, &rgb[0], (std::size_t)width);
                            } else {
                                ModelColourspaceKnobs::apply(conversion.toModel, &rgb[0], (std::size_t)width);
                            }
                            float* dst = tile.data() + (std::size_t)r * width;
                            for (int x = 0; x < width; ++x) {
                                dst[x] = rgb[(std::size_t)x * 3];
                                dst[planeSize + x] = rgb[(std::size_t)x * 3 + 1];
                                dst[2 * planeSize + x] = rgb[(std::size_t)x * 3 + 2];
                            }
                        }
                    };
                    const AiTileUnpack unpack = [&](const RectI& outRoI, const Tensor& tensor) {
                        RectI inside;
                        if (!PassExec::intersectRects(target, outRoI, &inside) || inside.isNull()) {
                            return;
                        }
                        const int outWidth = outRoI.width();
                        const std::size_t planeSize = (std::size_t)outWidth * outRoI.height();
                        const int width = inside.width();
                        for (int y = inside.y1; y < inside.y2; ++y) {
                            const float* srcRow = tensor.data() + (std::size_t)(outRoI.y2 - 1 - y) * outWidth + (inside.x1 - outRoI.x1);
                            float* rgb = &(*result)[((std::size_t)(y - target.y1) * targetWidth + (inside.x1 - target.x1)) * 3];
                            for (int x = 0; x < width; ++x) {
                                rgb[(std::size_t)x * 3] = srcRow[x];
                                rgb[(std::size_t)x * 3 + 1] = srcRow[planeSize + x];
                                rgb[(std::size_t)x * 3 + 2] = srcRow[2 * planeSize + x];
                            }
                            if (boost) {
                                ModelColourspaceKnobs::apply(linearToModel.fromModel, rgb, (std::size_t)width);
                                for (std::size_t i = 0; i < (std::size_t)width * 3; ++i) {
                                    rgb[i] = signedSquare(rgb[i]);
                                }
                                ModelColourspaceKnobs::apply(linear.fromModel, rgb, (std::size_t)width);
                            } else {
                                ModelColourspaceKnobs::apply(conversion.fromModel, rgb, (std::size_t)width);
                            }
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
                    if (preserve) {
                        passThroughOutOfGamut(original->data(), inGamut, result->data(), targetWidth, target.height());
                    }

                    return eStatusOK;
                };

                if (recoverDetail) {
                    // Detail recovery measures the whole frame, so the frame is denoised once
                    // and kept for every window and Strength.
                    Hash64 hash;
                    EffectInstancePtr input = getInput(kNeuralDenoiseInputSource);
                    hash.append<U64>(input ? input->getRenderHash() : 0);
                    hash.append<double>(args.time);
                    hash.append<int>((int)args.view);
                    hash.append<unsigned int>(args.mappedScale.toMipmapLevel());
                    hash.append<int>(frame.x1);
                    hash.append<int>(frame.y1);
                    hash.append<int>(frame.x2);
                    hash.append<int>(frame.y2);
                    Hash64_appendQString(&hash, QString::fromStdString(resolved->entry->id));
                    Hash64_appendQString(&hash, QString::fromStdString(it->first.getLayerID()));
                    const OcioProcessor* processors[] = { &conversion.toModel, &linear.toModel, &linearToModel.toModel };
                    for (const OcioProcessor* processor : processors) {
                        Hash64_appendQString(&hash, processor->processor ? QString::fromUtf8(processor->processor->getCacheID()) : QString());
                    }
                    hash.append<bool>(preserve);
                    hash.append<bool>(boost);
                    hash.append<int>(spec.tile);
                    for (double band : bands) {
                        hash.append<double>(band);
                    }
                    hash.computeHash();
                    const U64 key = hash.value();

                    std::lock_guard<std::mutex> lock(_frameMutex);
                    if (!_frame || (_frame->key != key)) {
                        _frame.reset();
                        std::shared_ptr<WholeFrame> computed = std::make_shared<WholeFrame>();
                        std::vector<float> original;
                        const StatusEnum status = denoise(frame, &original, &computed->denoised);
                        if (status != eStatusOK) {
                            return status;
                        }
                        computed->detail = filteredLuminanceResidual(original.data(), computed->denoised.data(), frame.width(), frame.height(), bands);
                        computed->frame = frame;
                        computed->key = key;
                        _frame = computed;
                    }
                    whole = _frame;
                    window = whole->frame;
                    denoised = &whole->denoised;
                    detail = &whole->detail;
                } else {
                    // The gamut neighbourhood reaches past the window's edge.
                    const int margin = preserve ? kGamutRadius : 0;
                    const RectI grown(roi.x1 - margin, roi.y1 - margin, roi.x2 + margin, roi.y2 + margin);
                    if (PassExec::intersectRects(grown, frame, &window) && !window.isNull()) {
                        std::vector<float> original;
                        const StatusEnum status = denoise(window, &original, &regionResult);
                        if (status != eStatusOK) {
                            return status;
                        }
                        denoised = &regionResult;
                    }
                }
            }
        }

        const float recovered = 1.f - strength;
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
                    const int x = io.x0 + i;
                    if (denoised && (c < 3) && (x >= window.x1) && (x < window.x2) && (io.y >= window.y1) && (io.y < window.y2)) {
                        const std::size_t p = (std::size_t)(io.y - window.y1) * window.width() + (x - window.x1);
                        const float model = (*denoised)[p * 3 + c];
                        if (detail) {
                            out = model + recovered * (*detail)[p];
                        } else if (rgbModel) {
                            out = model;
                        } else {
                            out = in + strength * (model - in);
                        }
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
