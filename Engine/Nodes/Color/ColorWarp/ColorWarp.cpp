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

#include "ColorWarp.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <utility>

#include <QtCore/QCoreApplication>
#include <QtCore/QMetaObject>

#include "Engine/AbortableRenderInfo.h"
#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Image.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobColorWarpPins.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeColorSampler.h"
#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/OverlayDrawList.h"
#include "Engine/OverlaySupport.h"
#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#endif

NATRON_NAMESPACE_ENTER

namespace {

ColorWarpSpaceEnum
spaceFromIndex(int index)
{
    return ((index >= 0) && (index < kColorWarpSpaceCount)) ? (ColorWarpSpaceEnum)index : eColorWarpSpaceOklabLog;
}

ColorWarpKernelEnum
kernelFromIndex(int index)
{
    return ((index >= 0) && (index < kColorWarpKernelCount)) ? (ColorWarpKernelEnum)index : eColorWarpKernelGaussian;
}

#ifdef NATRON_HAVE_GPU
// The layout of the parameter words colorwarp.slang reads: a header, then kGpuPinWords per pin.
constexpr int kGpuPinCount = 0;
constexpr int kGpuKernel = 1;
constexpr int kGpuTransfer = 2;
constexpr int kGpuLightnessStretch = 3;
constexpr int kGpuLightnessAxis = 4;
constexpr int kGpuOrigin = 7;
constexpr int kGpuInvScale = 10;
constexpr int kGpuCutoff = 11;
constexpr int kGpuPre = 12;
constexpr int kGpuPost = 21;
constexpr int kGpuInvPost = 30;
constexpr int kGpuInvPre = 39;
constexpr int kGpuAffine = 48;
constexpr int kGpuWeightSum = 60;
constexpr int kGpuWeightMoment = 63;
constexpr int kGpuFadeRadius = 72;
constexpr int kGpuFadeInvWidth = 73;
constexpr int kGpuHeaderWords = 74;
constexpr int kGpuPinWords = 7;

constexpr std::uint32_t kGpuTransferNone = 0;
constexpr std::uint32_t kGpuTransferLog = 1;
constexpr std::uint32_t kGpuTransferCbrt = 2;

void
putFloat(std::vector<std::uint32_t>& words,
         int index,
         double value)
{
    const float f = (float)value;

    std::memcpy(&words[index], &f, sizeof(float));
}

void
putMatrix(std::vector<std::uint32_t>& words,
          int index,
          const double m[9])
{
    for (int i = 0; i < 9; ++i) {
        putFloat(words, index + i, m[i]);
    }
}

void
multiply3x3(const double a[9],
            const double b[9],
            double out[9])
{
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            out[r * 3 + c] = a[r * 3 + 0] * b[0 * 3 + c] + a[r * 3 + 1] * b[1 * 3 + c] + a[r * 3 + 2] * b[2 * 3 + c];
        }
    }
}

const double kIdentity3[9] = { 1., 0., 0., 0., 1., 0., 0., 0., 1. };
#endif // NATRON_HAVE_GPU

} // anonymous namespace

void
ColorWarpOp::createKnobs(KnobHolder* h,
                         const KnobPagePtr& page)
{
    holder = h;

    KnobChoicePtr spaceKnob = AppManager::createKnob<KnobChoice>(h, EffectInstance::tr("Warp Space").toStdString());
    spaceKnob->setName(kColorWarpParamWarpSpace);
    spaceKnob->setHintToolTip(EffectInstance::tr("The colour space the pins pull colours through. Distances, and so each pin's reach, "
                                                 "are measured in it.")
                                  .toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("oklablog", EffectInstance::tr("OKLab Log").toStdString(),
                                       EffectInstance::tr("OKLab with a log2 encoding in place of its cube root: even steps per stop of exposure.").toStdString()));
        options.push_back(ChoiceOption("oklab", EffectInstance::tr("OKLab").toStdString(),
                                       EffectInstance::tr("OKLab with its cube root.").toStdString()));
        options.push_back(ChoiceOption("xyz", EffectInstance::tr("XYZ").toStdString(),
                                       EffectInstance::tr("CIE XYZ, linear.").toStdString()));
        options.push_back(ChoiceOption("logrgb", EffectInstance::tr("Log RGB").toStdString(),
                                       EffectInstance::tr("The working space's RGB with a log2 encoding.").toStdString()));
        options.push_back(ChoiceOption("linearrgb", EffectInstance::tr("Linear RGB").toStdString(),
                                       EffectInstance::tr("The working space's linear RGB.").toStdString()));
        spaceKnob->populateChoices(options);
    }
    spaceKnob->setDefaultValue((int)eColorWarpSpaceOklabLog);
    spaceKnob->setAnimationEnabled(false);
    page->addKnob(spaceKnob);
    warpSpace = spaceKnob;

    KnobChoicePtr kernelKnob = AppManager::createKnob<KnobChoice>(h, EffectInstance::tr("Kernel").toStdString());
    kernelKnob->setName(kColorWarpParamKernel);
    kernelKnob->setHintToolTip(EffectInstance::tr("How each pin's move spreads to the colours around it.").toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("gaussian", EffectInstance::tr("Gaussian").toStdString(),
                                       EffectInstance::tr("Each pin moves the colours within its falloff and lands its source exactly on its target.").toStdString()));
        options.push_back(ChoiceOption("rbflinear", EffectInstance::tr("RBF Linear").toStdString(),
                                       EffectInstance::tr("A smooth interpolation through every pin that fades to no change away from the pins; falloff is ignored.").toStdString()));
        options.push_back(ChoiceOption("rbfcubic", EffectInstance::tr("RBF Cubic").toStdString(),
                                       EffectInstance::tr("A smoother interpolation through every pin that fades to no change away from the pins; falloff is ignored.").toStdString()));
        kernelKnob->populateChoices(options);
    }
    kernelKnob->setDefaultValue((int)eColorWarpKernelGaussian);
    kernelKnob->setAnimationEnabled(false);
    page->addKnob(kernelKnob);
    kernel = kernelKnob;

    KnobDoublePtr lightnessKnob = AppManager::createKnob<KnobDouble>(h, EffectInstance::tr("Lightness Falloff").toStdString());
    lightnessKnob->setName(kColorWarpParamLightnessFalloff);
    lightnessKnob->setHintToolTip(EffectInstance::tr("How far each pin reaches along lightness relative to its reach in hue and chroma. "
                                                     "2 reaches twice as far in lightness; 1 reaches as far in every direction. "
                                                     "Ignored by the RBF kernels.")
                                      .toStdString());
    lightnessKnob->setDefaultValue(kColorWarpDefaultLightnessFalloff);
    lightnessKnob->setMinimum(0.01);
    lightnessKnob->setMaximum(DBL_MAX);
    lightnessKnob->setDisplayMinimum(0.1);
    lightnessKnob->setDisplayMaximum(10.);
    page->addKnob(lightnessKnob);
    lightnessFalloff = lightnessKnob;

    KnobColorWarpPinsPtr pinsKnob = AppManager::createKnob<KnobColorWarpPins>(h, EffectInstance::tr("Pins").toStdString());
    pinsKnob->setName(kColorWarpParamPins);
    pinsKnob->setHintToolTip(EffectInstance::tr("The pins, each mapping a source colour to a target colour. Click the viewer to add one; "
                                                "select one to edit it below the list or with the colour global mode.")
                                 .toStdString());
    page->addKnob(pinsKnob);
    pins = pinsKnob;
}

ColorWarpOp::Params
ColorWarpOp::bake(const KnobReader& reader) const
{
    Params p;
    const KnobColorWarpPinsPtr pinsKnob = pins.lock();

    if (!pinsKnob) {
        return p;
    }
    const std::vector<ColorWarpPin> evaluated = pinsKnob->evaluate(reader.time());
    if (evaluated.empty()) {
        return p;
    }
    const ColorWarpSpaceEnum space = spaceFromIndex(reader.value(warpSpace));
    const ColorWarpKernelEnum kernelType = kernelFromIndex(reader.value(kernel));
    const double lightness = reader.value(lightnessFalloff);
    double workingToXYZ[9];
    ColorMath::workingToXYZ(holder, workingToXYZ);

    std::vector<double> key;
    key.reserve(12 + evaluated.size() * 8);
    key.push_back((double)space);
    key.push_back((double)kernelType);
    key.push_back(lightness);
    key.insert(key.end(), workingToXYZ, workingToXYZ + 9);
    for (const ColorWarpPin& pin : evaluated) {
        key.insert(key.end(), pin.source, pin.source + 3);
        key.insert(key.end(), pin.target, pin.target + 3);
        key.push_back(pin.strength);
        key.push_back(pin.falloff);
    }

    std::lock_guard<std::mutex> lock(_preparedMutex);
    if (!_prepared.field || (_prepared.key != key)) {
        std::shared_ptr<ColorWarpField> field = std::make_shared<ColorWarpField>();
        field->prepare(evaluated, space, ColorWarpSpaceBasis(workingToXYZ), kernelType, kColorWarpDefaultRegularisation, lightness);
        _prepared.key = std::move(key);
        _prepared.field = field;
    }
    if (!_prepared.field->isIdentity()) {
        p.field = _prepared.field;
    }

    return p;
}

bool
ColorWarpOp::isIdentity(const Params& params) const
{
    return !params.field || params.field->isIdentity();
}

void
ColorWarpOp::row(const Params& params,
                 const RowIO& io) const
{
    const int nComps = io.nComps;
    bool process[4] = { false, false, false, false };

    for (int c = 0; (c < nComps) && (c < 4); ++c) {
        process[c] = io.channels[pixelKernelChannelBit(nComps, c)];
    }
    const float* src = io.src[0];
    float* dst = io.dst;
    const ColorWarpField* field = params.field.get();
    for (int x = 0; x < io.width; ++x, src += nComps, dst += nComps) {
        for (int c = 0; c < nComps; ++c) {
            dst[c] = src[c];
        }
        if (!field || (nComps < 3)) {
            continue;
        }
        const double rgb[3] = { src[0], src[1], src[2] };
        double out[3];
        field->apply(rgb, out);
        for (int c = 0; c < 3; ++c) {
            if (process[c]) {
                dst[c] = (float)out[c];
            }
        }
    }
}

GpuOpRef
ColorWarpOp::gpu(const Params& params) const
{
#ifdef NATRON_HAVE_GPU
    if (!params.field) {
        return GpuOpRef::none();
    }
    const ColorWarpField& field = *params.field;
    const int pinCount = field.pinCount();
    std::vector<std::uint32_t> words(kGpuHeaderWords + pinCount * kGpuPinWords, 0u);

    words[kGpuPinCount] = (std::uint32_t)pinCount;
    words[kGpuKernel] = (std::uint32_t)field.kernel();
    const ColorWarpSpaceBasis& basis = field.basis();
    // Every space is pre-matrix, per-channel transfer, post-matrix, and its inverse in reverse.
    double pre[9], post[9], invPost[9], invPre[9];
    std::uint32_t transfer = kGpuTransferNone;
    std::memcpy(pre, kIdentity3, sizeof(pre));
    std::memcpy(post, kIdentity3, sizeof(post));
    std::memcpy(invPost, kIdentity3, sizeof(invPost));
    std::memcpy(invPre, kIdentity3, sizeof(invPre));
    switch (field.space()) {
    case eColorWarpSpaceOklabLog:
    case eColorWarpSpaceOklab:
        multiply3x3(kColorWarpOklabXyzToLms, basis.rgbToXyz, pre);
        std::memcpy(post, kColorWarpOklabLmsToLab, sizeof(post));
        std::memcpy(invPost, colorWarpOklabLabToLms(), sizeof(invPost));
        multiply3x3(basis.xyzToRgb, colorWarpOklabLmsToXyz(), invPre);
        transfer = (field.space() == eColorWarpSpaceOklabLog) ? kGpuTransferLog : kGpuTransferCbrt;
        break;
    case eColorWarpSpaceXyz:
        std::memcpy(pre, basis.rgbToXyz, sizeof(pre));
        std::memcpy(invPre, basis.xyzToRgb, sizeof(invPre));
        break;
    case eColorWarpSpaceLogRgb:
        transfer = kGpuTransferLog;
        break;
    case eColorWarpSpaceLinearRgb:
        break;
    }
    words[kGpuTransfer] = transfer;
    putFloat(words, kGpuLightnessStretch, field.lightnessStretch());
    for (int c = 0; c < 3; ++c) {
        putFloat(words, kGpuLightnessAxis + c, field.lightnessAxis()[c]);
        putFloat(words, kGpuOrigin + c, field.origin()[c]);
    }
    putFloat(words, kGpuInvScale, field.invScale());
    putFloat(words, kGpuCutoff, 2. * kColorWarpGaussianCutoff * kColorWarpGaussianCutoff);
    putMatrix(words, kGpuPre, pre);
    putMatrix(words, kGpuPost, post);
    putMatrix(words, kGpuInvPost, invPost);
    putMatrix(words, kGpuInvPre, invPre);
    for (int i = 0; i < 12; ++i) {
        putFloat(words, kGpuAffine + i, field.affine()[i]);
    }
    const bool gaussian = (field.kernel() == eColorWarpKernelGaussian);
    if (!gaussian) {
        // The device evaluates the spline relative to |x| so that far from the pins it does not
        // cancel large terms in float; these sums of the weights, and of the weights times the
        // centres, carry the terms that rewrite factors out.
        double weightSum[3] = { 0., 0., 0. };
        double weightMoment[9] = { 0., 0., 0., 0., 0., 0., 0., 0., 0. };
        for (int i = 0; i < pinCount; ++i) {
            for (int c = 0; c < 3; ++c) {
                const double w = field.coefficients()[i * 3 + c];
                weightSum[c] += w;
                for (int k = 0; k < 3; ++k) {
                    weightMoment[c * 3 + k] += w * field.centres()[i * 3 + k];
                }
            }
        }
        for (int c = 0; c < 3; ++c) {
            putFloat(words, kGpuWeightSum + c, weightSum[c]);
        }
        putMatrix(words, kGpuWeightMoment, weightMoment);
        putFloat(words, kGpuFadeRadius, field.fadeRadius());
        putFloat(words, kGpuFadeInvWidth, field.fadeInvWidth());
    }
    for (int i = 0; i < pinCount; ++i) {
        const int base = kGpuHeaderWords + i * kGpuPinWords;
        for (int c = 0; c < 3; ++c) {
            putFloat(words, base + c, field.centres()[i * 3 + c]);
            putFloat(words, base + 3 + c, field.coefficients()[i * 3 + c]);
        }
        putFloat(words, base + 6, gaussian ? field.invTwoSigma2()[i] : 0.);
    }
    GpuOpRef ref = GpuOpRef::packed((int)gpu::GpuKernelId::colorwarp_wrapped, std::move(words));
    ref.acceptedComponents = GpuOpRef::componentsBit(3) | GpuOpRef::componentsBit(4);

    return ref;
#else
    Q_UNUSED(params);

    return GpuOpRef::none();
#endif
}

struct ColorWarp::DragSampler {
    struct Request {
        std::uint64_t serial = 0;
        int pinId = 0;
        double x = 0.;
        double y = 0.;
        double time = 0.;
        ViewIdx view;
    };

    ColorWarp* owner = nullptr;
    std::mutex mutex;
    std::condition_variable wake;
    std::optional<Request> pending;
    AbortableRenderInfoPtr running;
    bool quit = false;
    std::thread worker;

    void request(const Request& r,
                 const std::weak_ptr<DragSampler>& token)
    {
        std::lock_guard<std::mutex> locker(mutex);
        pending = r;
        if (running) {
            running->setAborted();
        }
        if (!worker.joinable()) {
            worker = std::thread([this, token]() {
                run(token);
            });
        }
        wake.notify_one();
    }

    void run(const std::weak_ptr<DragSampler>& token)
    {
        std::unique_lock<std::mutex> locker(mutex);
        for (;;) {
            wake.wait(locker, [this]() {
                return quit || pending.has_value();
            });
            if (quit) {
                return;
            }
            const Request r = *pending;
            pending.reset();
            running = AbortableRenderInfo::create(true, 0);
            const AbortableRenderInfoPtr abortInfo = running;
            locker.unlock();
            std::optional<std::array<double, 3>> source = owner->sampleSource(r.x, r.y, r.time, r.view, abortInfo);
            if (abortInfo->isAborted()) {
                source.reset();
            }
            locker.lock();
            running.reset();
            if (quit) {
                return;
            }
            if (QCoreApplication::instance()) {
                ColorWarp* self = owner;
                QMetaObject::invokeMethod(QCoreApplication::instance(), [token, self, r, source]() {
                    if (token.lock()) {
                        self->onDragSampled(r.serial, r.pinId, r.x, r.y, r.time, source);
                    } }, Qt::QueuedConnection);
            }
        }
    }

    void shutdown()
    {
        {
            std::lock_guard<std::mutex> locker(mutex);
            quit = true;
            pending.reset();
            if (running) {
                running->setAborted();
            }
            wake.notify_one();
        }
        if (!worker.joinable()) {
            return;
        }
        if (worker.get_id() == std::this_thread::get_id()) {
            worker.detach();
        } else {
            worker.join();
        }
    }
};

ColorWarp::ColorWarp(NodePtr node)
    : ImageNode<ColorWarpOp>(node)
    , _interact()
    , _dragSampler(std::make_shared<DragSampler>())
{
    _dragSampler->owner = this;
}

ColorWarp::~ColorWarp()
{
    _dragSampler->shutdown();
}

void
ColorWarp::initializeKnobs()
{
    ImageNode<ColorWarpOp>::initializeKnobs();
    if (KnobColorWarpPinsPtr pinsKnob = getPinsKnob()) {
        addOverlaySlaveParam(pinsKnob);
    }
}

void
ColorWarp::addAcceptedComponents(int inputNb,
                                 std::list<ImageLayerDesc>* comps)
{
    if (isInputMask(inputNb)) {
        StagedImageEffect::addAcceptedComponents(inputNb, comps);

        return;
    }
    comps->push_back(ImageLayerDesc::getRGBAComponents());
    comps->push_back(ImageLayerDesc::getRGBComponents());
}

ColorWarpOp::Params
ColorWarp::bakeColorWarp(double time,
                         ViewIdx view) const
{
    return bakeParams(op(), time, view);
}

void
ColorWarp::colorWarpRow(const ColorWarpOp::Params& params,
                        const RowIO& io) const
{
    op().row(params, io);
}

KnobColorWarpPinsPtr
ColorWarp::getPinsKnob() const
{
    return op().pins.lock();
}

std::optional<std::array<double, 3>>
ColorWarp::sampleSource(double x,
                        double y,
                        double time,
                        ViewIdx view,
                        const AbortableRenderInfoPtr& abortInfo)
{
    const NodePtr node = getNode();
    const std::optional<std::array<float, 4>> sample = sampleNodeInputAt(node, 0, time, view, x, y, ImageLayerDesc::getRGBAComponents(), abortInfo);

    if (!sample) {
        return std::nullopt;
    }
    std::array<double, 3> rgb { (*sample)[0], (*sample)[1], (*sample)[2] };

    const KnobChannelSelectPtr unPremultBy = node ? node->getUnPremultBySelector() : KnobChannelSelectPtr();
    if (!unPremultBy || unPremultBy->isNone() || !getInput(0)) {
        return rgb;
    }
    std::list<ImageLayerDesc> availableLayers;
    getAvailableLayers(time, view, 0, &availableLayers);
    ImageLayerDesc divisorLayer;
    const int divisorChannel = node->getUnPremultChannel(availableLayers, &divisorLayer);
    const std::vector<std::string>& channels = divisorLayer.getChannels();
    if ((divisorChannel < 0) || (divisorChannel >= (int)channels.size())) {
        return rgb;
    }
    // sampleNodeColor() puts a layer's "A" channel in rgba[3] and the others in order.
    const int slot = (channels[divisorChannel] == "A") ? 3 : divisorChannel;
    if (slot > 3) {
        return rgb;
    }
    float divisor = (*sample)[slot];
    if (!(divisorLayer == ImageLayerDesc::getRGBAComponents())) {
        const std::optional<std::array<float, 4>> other = sampleNodeInputAt(node, 0, time, view, x, y, divisorLayer, abortInfo);
        if (!other) {
            return rgb;
        }
        divisor = (*other)[slot];
    }
    if (divisor > 0.f) {
        for (double& v : rgb) {
            v = Image::unPremultiplyValue((float)v, divisor);
        }
    }

    return rgb;
}

void
ColorWarp::requestDragSample(double x,
                             double y,
                             double time,
                             ViewIdx view)
{
    DragSampler::Request r;

    r.serial = ++_dragSerial;
    r.pinId = _interact.dragId();
    r.x = x;
    r.y = y;
    r.time = time;
    r.view = view;
    _dragSampler->request(r, _dragSampler);
}

void
ColorWarp::onDragSampled(std::uint64_t serial,
                         int pinId,
                         double x,
                         double y,
                         double time,
                         const std::optional<std::array<double, 3>>& source)
{
    _dragSettledSerial = std::max(_dragSettledSerial, serial);
    const KnobColorWarpPinsPtr pinsKnob = getPinsKnob();
    const bool current = (serial >= _dragFirstSerial) && (_interact.isDragging() || _moveBefore.has_value());
    if (pinsKnob && source && current) {
        ColorWarpPinSet pins = pinsKnob->getPinSet();
        if (ColorWarpInteract::movePin(&pins, pinId, source->data(), x, y, time)) {
            pinsKnob->setPinSet(pins);
        }
        redrawOverlayInteract();
    }
    commitMoveWhenSettled();
}

void
ColorWarp::commitMoveWhenSettled()
{
    if (_moveBefore && (_dragSettledSerial >= _dragSerial)) {
        commitMoveNow();
    }
}

void
ColorWarp::commitMoveNow()
{
    if (!_moveBefore) {
        return;
    }
    const ColorWarpPinSet before = *_moveBefore;
    _moveBefore.reset();
    // A sample still in flight belongs to the move recorded here; it must not land after it.
    _dragSettledSerial = _dragSerial;
    _dragFirstSerial = _dragSerial + 1;
    if (const KnobColorWarpPinsPtr pinsKnob = getPinsKnob()) {
        const ColorWarpPinSet after = pinsKnob->getPinSet();
        if (after != before) {
            pinsKnob->editPinSet(before, after, tr("Move Pin").toStdString());
        }
    }
}

int
ColorWarp::addPinAt(double x,
                    double y,
                    double time,
                    ViewIdx view)
{
    const KnobColorWarpPinsPtr pinsKnob = getPinsKnob();
    const std::optional<std::array<double, 3>> source = sampleSource(x, y, time, view);

    if (!pinsKnob || !source) {
        return 0;
    }
    const ColorWarpPinSet before = pinsKnob->getPinSet();
    ColorWarpPinSet after = before;
    const int index = after.addPin(source->data(), x, y, time);
    const int id = after.pins[index].id;
    pinsKnob->editPinSet(before, after, tr("Add Pin").toStdString());
    pinsKnob->setSelectedPinId(id);

    return id;
}

bool
ColorWarp::removePin(int id)
{
    const KnobColorWarpPinsPtr pinsKnob = getPinsKnob();

    if (!pinsKnob) {
        return false;
    }
    const ColorWarpPinSet before = pinsKnob->getPinSet();
    const int index = before.indexOf(id);
    if (index < 0) {
        return false;
    }
    ColorWarpPinSet after = before;
    after.pins.erase(after.pins.begin() + index);
    pinsKnob->editPinSet(before, after, tr("Delete Pin").toStdString());

    return true;
}

bool
ColorWarp::pixelScale(double* scaleX,
                      double* scaleY) const
{
    OverlaySupport* overlay = getCurrentViewportForOverlays();

    if (!overlay) {
        return false;
    }
    overlay->getPixelScale(*scaleX, *scaleY);

    return true;
}

void
ColorWarp::buildOverlay(double time,
                        const RenderScale& /*renderScale*/,
                        ViewIdx /*view*/,
                        OverlayDrawList* list)
{
    const KnobColorWarpPinsPtr pinsKnob = getPinsKnob();
    double scaleX, scaleY;

    if (!pinsKnob || !pixelScale(&scaleX, &scaleY)) {
        return;
    }
    ColorWarpInteract::build(pinsKnob->getPinSet(), pinsKnob->getSelectedPinId(), time, scaleX, scaleY, list);
}

bool
ColorWarp::onOverlayPenDown(double time,
                            const RenderScale& /*renderScale*/,
                            ViewIdx view,
                            const QPointF& /*viewportPos*/,
                            const QPointF& pos,
                            double /*pressure*/,
                            double /*timestamp*/,
                            PenType pen)
{
    const KnobColorWarpPinsPtr pinsKnob = getPinsKnob();
    double scaleX, scaleY;

    if (!pinsKnob || (pen == ePenTypeMMB) || (pen == ePenTypeRMB) || !pixelScale(&scaleX, &scaleY)) {
        return false;
    }
    commitMoveNow();
    const ColorWarpPinSet pins = pinsKnob->getPinSet();
    const int hit = ColorWarpInteract::hitTest(pins, pos.x(), pos.y(), scaleX, scaleY);
    if (hit != 0) {
        pinsKnob->setSelectedPinId(hit);
        _dragFirstSerial = _dragSerial + 1;
        _interact.beginDrag(hit, pins);
        redrawOverlayInteract();

        return true;
    }
    if (addPinAt(pos.x(), pos.y(), time, view) == 0) {
        return false;
    }
    redrawOverlayInteract();

    return true;
}

bool
ColorWarp::onOverlayPenMotion(double time,
                              const RenderScale& /*renderScale*/,
                              ViewIdx view,
                              const QPointF& /*viewportPos*/,
                              const QPointF& pos,
                              double /*pressure*/,
                              double /*timestamp*/)
{
    const KnobColorWarpPinsPtr pinsKnob = getPinsKnob();

    if (!pinsKnob || !_interact.isDragging()) {
        return false;
    }
    requestDragSample(pos.x(), pos.y(), time, view);

    return true;
}

bool
ColorWarp::onOverlayPenUp(double /*time*/,
                          const RenderScale& /*renderScale*/,
                          ViewIdx /*view*/,
                          const QPointF& /*viewportPos*/,
                          const QPointF& /*pos*/,
                          double /*pressure*/,
                          double /*timestamp*/)
{
    const KnobColorWarpPinsPtr pinsKnob = getPinsKnob();

    if (!pinsKnob || !_interact.isDragging()) {
        return false;
    }
    _moveBefore = _interact.dragBefore();
    _interact.endDrag();
    commitMoveWhenSettled();
    redrawOverlayInteract();

    return true;
}

bool
ColorWarp::onOverlayKeyDown(double /*time*/,
                            const RenderScale& /*renderScale*/,
                            ViewIdx /*view*/,
                            Key key,
                            KeyboardModifiers /*modifiers*/)
{
    if ((key != Key_Delete) && (key != Key_BackSpace)) {
        return false;
    }
    const KnobColorWarpPinsPtr pinsKnob = getPinsKnob();
    if (!pinsKnob || _interact.isDragging()) {
        return false;
    }
    commitMoveNow();
    const int selected = pinsKnob->getSelectedPinId();
    if ((selected == 0) || !removePin(selected)) {
        return false;
    }
    redrawOverlayInteract();

    return true;
}

NATRON_NAMESPACE_EXIT
