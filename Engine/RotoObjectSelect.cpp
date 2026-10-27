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

#include "RotoObjectSelect.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <stdexcept>

#include <QCoreApplication>
#include <QMetaObject>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeColorSampler.h"
#include "Engine/NodeMetadata.h"
#include "Engine/OverlaySupport.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"
#include "Engine/RotoCurveInteract.h"
#include "Engine/RotoUndoCommand.h"
#include "Engine/ViewIdx.h"
#include "Global/GLIncludes.h"

NATRON_NAMESPACE_ENTER

namespace {

// A press that moves further than this, in screen pixels, draws a box instead of clicking.
const double kDragThreshold = 4.;
const int kCurveSamplesPerSegment = 12;

float
srgbEncode(float v)
{
    v = std::min(1.f, std::max(0.f, v));

    return (v <= 0.0031308f) ? 12.92f * v : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
}

template <typename PIX>
void
readRows(const Image& image,
         const RectI& rect,
         int nComps,
         float scale,
         std::vector<float>* rgba)
{
    Image::ReadAccess access(&image);
    const int w = rect.width();
    const int h = rect.height();

    for (int row = 0; row < h; ++row) {
        // The encoder wants the top row first; image rows go up.
        const int y = rect.y2 - 1 - row;
        float* dst = &(*rgba)[(std::size_t)row * w * 4];
        for (int i = 0; i < w; ++i, dst += 4) {
            const PIX* pix = reinterpret_cast<const PIX*>(access.pixelAt(rect.x1 + i, y));
            if (!pix) {
                dst[0] = dst[1] = dst[2] = 0.f;
            } else if (nComps >= 3) {
                dst[0] = pix[0] * scale;
                dst[1] = pix[1] * scale;
                dst[2] = pix[2] * scale;
            } else if (nComps == 2) {
                dst[0] = pix[0] * scale;
                dst[1] = pix[1] * scale;
                dst[2] = 0.f;
            } else {
                dst[0] = dst[1] = dst[2] = pix[0] * scale;
            }
            dst[3] = 1.f;
        }
    }
}

MaskTrace::Point
cubicAt(const MaskTrace::CurvePoint& a,
        const MaskTrace::CurvePoint& b,
        double t)
{
    const double m = 1. - t;
    MaskTrace::Point p;

    p.x = m * m * m * a.position.x + 3. * m * m * t * a.right.x + 3. * m * t * t * b.left.x + t * t * t * b.position.x;
    p.y = m * m * m * a.position.y + 3. * m * m * t * a.right.y + 3. * m * t * t * b.left.y + t * t * t * b.position.y;

    return p;
}
} // namespace

// Only its lifetime matters: see _shared.
struct RotoObjectSelect::Shared {
};

struct RotoObjectSelect::Job {
    NodePtr source;
    double time = 0.;
    SegmentationKey key;
    ModelKnob::Resolved model;
    // In: the segmenter to reuse, if it is for key.modelId. Out: the one that encoded.
    std::shared_ptr<Segmenter> segmenter;
    std::string segmenterModelId;
    ProjectColorManagement::DisplayProcessorPtr display;
    std::atomic<bool> abort { false };

    bool ok = false;
    std::string error;
    Frame frame;
};

RotoObjectSelect::RotoObjectSelect(RotoCurveInteract* roto)
    : _roto(roto)
    , _shared(std::make_shared<Shared>())
{
}

RotoObjectSelect::~RotoObjectSelect()
{
    _shared.reset();
    if (_running) {
        _running->abort = true;
    }
    joinWorker();
}

void
RotoObjectSelect::joinWorker()
{
    if (_worker.joinable()) {
        _worker.join();
    }
}

void
RotoObjectSelect::createKnobs(const KnobPagePtr& page)
{
    EffectInstance* host = _roto->getHost();

    _model = ModelKnob::create(host, page, AiModelTask::eMaskObject);
    std::vector<KnobIPtr> viewerKnobs;
    if (KnobChoicePtr model = _model->modelKnob()) {
        model->setInViewerContextLabel(QString::fromUtf8("Model"));
        viewerKnobs.push_back(model);
    }
    if (KnobButtonPtr download = _model->downloadKnob()) {
        viewerKnobs.push_back(download);
    }

    KnobDoublePtr threshold = AppManager::createKnob<KnobDouble>(host, std::string(kRotoUIParamObjectThresholdLabel));
    threshold->setName(kRotoUIParamObjectThreshold);
    threshold->setInViewerContextLabel(QString::fromUtf8(kRotoUIParamObjectThresholdLabel));
    threshold->setHintToolTip(std::string(kRotoUIParamObjectThresholdHint));
    threshold->setEvaluateOnChange(false);
    threshold->setAnimationEnabled(false);
    threshold->setMinimum(0.01);
    threshold->setMaximum(0.99);
    threshold->setDefaultValue(0.5);
    threshold->disableSlider();
    page->addKnob(threshold);
    _threshold = threshold;
    viewerKnobs.push_back(threshold);

    KnobDoublePtr simplify = AppManager::createKnob<KnobDouble>(host, std::string(kRotoUIParamObjectSimplifyLabel));
    simplify->setName(kRotoUIParamObjectSimplify);
    simplify->setInViewerContextLabel(QString::fromUtf8(kRotoUIParamObjectSimplifyLabel));
    simplify->setHintToolTip(std::string(kRotoUIParamObjectSimplifyHint));
    simplify->setEvaluateOnChange(false);
    simplify->setAnimationEnabled(false);
    simplify->setMinimum(0.1);
    simplify->setMaximum(20.);
    simplify->setDefaultValue(1.5);
    simplify->disableSlider();
    page->addKnob(simplify);
    _simplify = simplify;
    viewerKnobs.push_back(simplify);

    KnobDoublePtr minArea = AppManager::createKnob<KnobDouble>(host, std::string(kRotoUIParamObjectMinAreaLabel));
    minArea->setName(kRotoUIParamObjectMinArea);
    minArea->setInViewerContextLabel(QString::fromUtf8(kRotoUIParamObjectMinAreaLabel));
    minArea->setHintToolTip(std::string(kRotoUIParamObjectMinAreaHint));
    minArea->setEvaluateOnChange(false);
    minArea->setAnimationEnabled(false);
    minArea->setMinimum(0.);
    minArea->setDefaultValue(64.);
    minArea->disableSlider();
    page->addKnob(minArea);
    _minArea = minArea;
    viewerKnobs.push_back(minArea);

    KnobBoolPtr holes = AppManager::createKnob<KnobBool>(host, std::string(kRotoUIParamObjectHolesLabel));
    holes->setName(kRotoUIParamObjectHoles);
    holes->setInViewerContextLabel(QString::fromUtf8(kRotoUIParamObjectHolesLabel));
    holes->setHintToolTip(std::string(kRotoUIParamObjectHolesHint));
    holes->setEvaluateOnChange(false);
    holes->setAnimationEnabled(false);
    holes->setDefaultValue(true);
    page->addKnob(holes);
    _holes = holes;
    viewerKnobs.push_back(holes);

    KnobBoolPtr smooth = AppManager::createKnob<KnobBool>(host, std::string(kRotoUIParamObjectSmoothLabel));
    smooth->setName(kRotoUIParamObjectSmooth);
    smooth->setInViewerContextLabel(QString::fromUtf8(kRotoUIParamObjectSmoothLabel));
    smooth->setHintToolTip(std::string(kRotoUIParamObjectSmoothHint));
    smooth->setEvaluateOnChange(false);
    smooth->setAnimationEnabled(false);
    smooth->setDefaultValue(true);
    page->addKnob(smooth);
    _smooth = smooth;
    viewerKnobs.push_back(smooth);

    KnobDoublePtr feather = AppManager::createKnob<KnobDouble>(host, std::string(kRotoUIParamObjectFeatherLabel));
    feather->setName(kRotoUIParamObjectFeather);
    feather->setInViewerContextLabel(QString::fromUtf8(kRotoUIParamObjectFeatherLabel));
    feather->setHintToolTip(std::string(kRotoUIParamObjectFeatherHint));
    feather->setEvaluateOnChange(false);
    feather->setAnimationEnabled(false);
    feather->setMinimum(0.);
    feather->setDefaultValue(0.);
    feather->disableSlider();
    page->addKnob(feather);
    _feather = feather;
    viewerKnobs.push_back(feather);

    KnobButtonPtr clear = AppManager::createKnob<KnobButton>(host, std::string(kRotoUIParamObjectClearPromptsLabel));
    clear->setName(kRotoUIParamObjectClearPrompts);
    clear->setHintToolTip(std::string(kRotoUIParamObjectClearPromptsHint));
    clear->setEvaluateOnChange(false);
    clear->setIsPersistent(false);
    clear->setInViewerContextNewLineActivated(true);
    page->addKnob(clear);
    _clearPrompts = clear;
    viewerKnobs.push_back(clear);

    // Viewer items only: the properties panel keeps to the shapes.
    for (const KnobIPtr& knob : viewerKnobs) {
        knob->setSecretByDefault(true);
        knob->setInViewerContextSecret(true);
        host->addKnobToViewerUI(knob);
    }
    refreshSourceState();
} // RotoObjectSelect::createKnobs

void
RotoObjectSelect::onEffectCreated(const CreateNodeArgs& args)
{
    if (_model) {
        _model->onEffectCreated(args);
    }
}

void
RotoObjectSelect::setToolButton(const KnobButtonPtr& button)
{
    _toolButton = button;
}

void
RotoObjectSelect::setActive(bool active)
{
    _active = active;
    const KnobIPtr knobs[] = {
        _model ? KnobIPtr(_model->modelKnob()) : KnobIPtr(),
        _model ? KnobIPtr(_model->downloadKnob()) : KnobIPtr(),
        _threshold.lock(), _simplify.lock(), _minArea.lock(), _holes.lock(), _smooth.lock(), _feather.lock(), _clearPrompts.lock()
    };
    for (const KnobIPtr& knob : knobs) {
        if (knob) {
            knob->setInViewerContextSecret(!active);
        }
    }
    if (!active) {
        dropPrompts();
        setMessage(std::string());
    }
}

bool
RotoObjectSelect::onKnobChanged(KnobI* k)
{
    if (_model && _model->knobChanged(k)) {
        if (_model->modelKnob() && (k == _model->modelKnob().get())) {
            _hasFailed = false;
            _mask.clear();
            _contours.clear();
            refreshMask();
        }

        return true;
    }
    if (k == _clearPrompts.lock().get()) {
        clearPrompts();

        return true;
    }
    if (k == _threshold.lock().get()) {
        refreshMask();

        return true;
    }
    if ((k == _simplify.lock().get()) || (k == _minArea.lock().get()) || (k == _holes.lock().get()) || (k == _smooth.lock().get())) {
        retrace();

        return true;
    }

    return k == _feather.lock().get();
}

NodePtr
RotoObjectSelect::sourceNode() const
{
    NodePtr node = _roto->getNode();

    return node ? node->getInput(0) : NodePtr();
}

void
RotoObjectSelect::refreshSourceState()
{
    const bool connected = (bool)sourceNode();

    if (KnobButtonPtr button = _toolButton.lock()) {
        button->setAllDimensionsEnabled(connected);
        button->setHintToolTip(std::string(connected ? kRotoUIParamObjectSelectToolButtonActionHint : kRotoUIParamObjectSelectToolButtonActionNoSourceHint));
    }
    if (!connected && !_prompts.empty()) {
        dropPrompts();
        setMessage(kRotoUIParamObjectSelectToolButtonActionNoSourceHint);
        redraw();
    }
}

void
RotoObjectSelect::onTimeChanged(double time)
{
    if (_prompts.empty() || (time == _promptsTime)) {
        return;
    }
    dropPrompts();
    setMessage("The prompts were cleared: they belong to the frame they were made on.");
    redraw();
}

MaskTrace::Options
RotoObjectSelect::traceOptions() const
{
    MaskTrace::Options options;

    if (KnobDoublePtr k = _threshold.lock()) {
        options.threshold = k->getValue();
    }
    if (KnobDoublePtr k = _simplify.lock()) {
        options.simplify = k->getValue();
    }
    if (KnobDoublePtr k = _minArea.lock()) {
        options.minArea = k->getValue();
    }
    if (KnobBoolPtr k = _holes.lock()) {
        options.keepHoles = k->getValue();
    }
    if (KnobBoolPtr k = _smooth.lock()) {
        options.smooth = k->getValue();
    }

    return options;
}

QPointF
RotoObjectSelect::toEncoded(const QPointF& canonical) const
{
    const double sx = _frame.canonical.width() / std::max(1, _frame.width);
    const double sy = _frame.canonical.height() / std::max(1, _frame.height);

    // Prompts put pixel j's centre at j; the canonical rectangle starts at pixel 0's left edge.
    return QPointF((canonical.x() - _frame.canonical.x1) / sx - 0.5, (_frame.canonical.y2 - canonical.y()) / sy - 0.5);
}

MaskTrace::Point
RotoObjectSelect::toCanonical(const MaskTrace::Point& encoded) const
{
    const double sx = _frame.canonical.width() / std::max(1, _frame.width);
    const double sy = _frame.canonical.height() / std::max(1, _frame.height);
    MaskTrace::Point p;

    p.x = _frame.canonical.x1 + encoded.x * sx;
    p.y = _frame.canonical.y2 - encoded.y * sy;

    return p;
}

bool
RotoObjectSelect::currentKey(double time,
                             SegmentationKey* key,
                             std::optional<ModelKnob::Resolved>* model)
{
    NodePtr source = sourceNode();

    if (!source || !source->getEffectInstance()) {
        setMessage(kRotoUIParamObjectSelectToolButtonActionNoSourceHint);

        return false;
    }
    if (!_model) {
        return false;
    }
    std::string error;
    *model = _model->resolve(&error);
    if (!*model) {
        setMessage(error);

        return false;
    }
    key->imageHash = source->getEffectInstance()->getHash();
    key->frame = time;
    key->modelId = (*model)->entry->id;

    return true;
}

void
RotoObjectSelect::addPrompt(double time,
                            const Prompt& prompt)
{
    SegmentationKey key;
    std::optional<ModelKnob::Resolved> model;

    if (!currentKey(time, &key, &model)) {
        redraw();

        return;
    }
    if (prompt.kind == Prompt::eBox) {
        bool boxes = false;
        try {
            boxes = (Segmenter::archFromConfig(model->config) == Segmenter::Arch::eSam);
        } catch (const std::exception& e) {
            setMessage(e.what());
            redraw();

            return;
        }
        if (!boxes) {
            setMessage("This model takes no box prompts: click on the object, or pick a SAM 2.1 model.");
            redraw();

            return;
        }
    }
    if (!_prompts.empty() && (time != _promptsTime)) {
        dropPrompts();
    }
    _promptsTime = time;
    _prompts.push_back(prompt);
    _hasFailed = false;
    setMessage(std::string());
    refreshMask();
}

void
RotoObjectSelect::refreshMask()
{
    if (_prompts.empty()) {
        _mask.clear();
        _contours.clear();
        redraw();

        return;
    }
    if (_running) {
        // The finished encoding segments the prompts made meanwhile.
        return;
    }

    SegmentationKey key;
    std::optional<ModelKnob::Resolved> model;
    if (!currentKey(_promptsTime, &key, &model)) {
        _mask.clear();
        _contours.clear();
        redraw();

        return;
    }

    std::list<Frame>::iterator frame = std::find_if(_frames.begin(), _frames.end(), [&key](const Frame& f) {
        return f.key == key;
    });
    const bool cached = (frame != _frames.end()) && _segmenter && (_segmenterModelId == key.modelId) && _segmenter->select(key);
    if (!cached) {
        if (_hasFailed && (_failedKey == key)) {
            return;
        }
        requestEncoding(_promptsTime, key, *model);

        return;
    }
    _frame = *frame;
    _frames.splice(_frames.begin(), _frames, frame);

    std::vector<SegmentationPoint> points;
    std::vector<SegmentationBox> boxes;
    for (const Prompt& prompt : _prompts) {
        const QPointF a = toEncoded(prompt.a);
        if (prompt.kind == Prompt::eBox) {
            const QPointF b = toEncoded(prompt.b);
            SegmentationBox box;
            box.x1 = (float)std::min(a.x(), b.x());
            box.y1 = (float)std::min(a.y(), b.y());
            box.x2 = (float)std::max(a.x(), b.x());
            box.y2 = (float)std::max(a.y(), b.y());
            boxes.push_back(box);
        } else {
            SegmentationPoint point;
            point.x = (float)a.x();
            point.y = (float)a.y();
            point.label = (prompt.kind == Prompt::ePositive) ? SegmentationPoint::eLabelPositive : SegmentationPoint::eLabelNegative;
            points.push_back(point);
        }
    }
    try {
        _mask = _segmenter->segment(points, boxes, (float)traceOptions().threshold);
    } catch (const std::exception& e) {
        _mask.clear();
        setMessage(e.what());
    }
    retrace();
} // RotoObjectSelect::refreshMask

void
RotoObjectSelect::retrace()
{
    _contours.clear();
    if (_mask.empty() || (_mask.size() != (std::size_t)_frame.width * _frame.height)) {
        redraw();

        return;
    }
    std::vector<MaskTrace::Contour> traced = MaskTrace::trace(_mask, _frame.width, _frame.height, traceOptions());
    const double sx = _frame.canonical.width() / std::max(1, _frame.width);
    const double sy = _frame.canonical.height() / std::max(1, _frame.height);
    for (MaskTrace::Contour& contour : traced) {
        for (MaskTrace::CurvePoint& p : contour.points) {
            p.position = toCanonical(p.position);
            p.left = toCanonical(p.left);
            p.right = toCanonical(p.right);
        }
        contour.area *= sx * sy;
    }
    _contours.swap(traced);
    if (_contours.empty()) {
        setMessage("The selection is empty: click on the object, or lower Object Min Area.");
    } else if (!_message.empty()) {
        setMessage(std::string());
    }
    redraw();
}

void
RotoObjectSelect::requestEncoding(double time,
                                  const SegmentationKey& key,
                                  const ModelKnob::Resolved& model)
{
    std::shared_ptr<Job> job = std::make_shared<Job>();

    job->source = sourceNode();
    job->time = time;
    job->key = key;
    job->model = model;
    if (_segmenter && (_segmenterModelId == key.modelId)) {
        job->segmenter = _segmenter;
        job->segmenterModelId = _segmenterModelId;
    }
    EffectInstance* host = _roto->getHost();
    if (host && host->getApp()) {
        if (ProjectPtr project = host->getApp()->getProject()) {
            std::string display, view;
            project->getDefaultDisplayView(&display, &view);
            if (ProjectColorManagementPtr colorManagement = project->getColorManagement()) {
                job->display = colorManagement->getDisplayProcessor(project->getWorkingColorSpace(), display, view, std::string());
            }
        }
    }

    joinWorker();
    _running = job;
    std::weak_ptr<Shared> token = _shared;
    RotoObjectSelect* self = this;
    _worker = std::thread([job, token, self]() {
        runJob(job.get());
        if (QCoreApplication::instance()) {
            QMetaObject::invokeMethod(QCoreApplication::instance(), [job, token, self]() {
                if ( token.lock() ) {
                    self->onJobFinished(job);
                } }, Qt::QueuedConnection);
        }
    });

    if (NodePtr node = _roto->getNode()) {
        node->setCurrentCursor(eCursorBusy);
    }
    setMessage("Encoding the Source...");
    redraw();
} // RotoObjectSelect::requestEncoding

void
RotoObjectSelect::runJob(Job* job)
{
    try {
        if (!job->source || !job->source->getEffectInstance()) {
            throw std::runtime_error("The Source input is not connected.");
        }
        EffectInstancePtr effect = job->source->getEffectInstance();
        NodeMetadataConstPtr metadata = effect->getMetadata(job->time, ViewIdx(0));
        if (!metadata) {
            throw std::runtime_error("The Source has no format at this frame.");
        }
        const RectI format = metadata->getOutputFormat();
        double par = metadata->getPixelAspectRatio(-1);
        if (!(par > 0.)) {
            par = 1.;
        }

        // Full resolution whatever the viewer's proxy level: the encoder sees the real image.
        RectI rendered;
        ImagePtr image = renderNodeImage(job->source, job->time, ViewIdx(0), 0, format, ImageLayerDesc::getRGBAComponents(), &rendered);
        if (!image) {
            throw std::runtime_error("The Source could not be rendered at this frame.");
        }
        const RectI rect = rendered.intersect(image->getBounds());
        if (rect.isNull()) {
            throw std::runtime_error("The Source is empty at this frame.");
        }
        if (job->abort) {
            return;
        }

        const int w = rect.width();
        const int h = rect.height();
        const int nComps = (int)image->getComponentsCount();
        std::vector<float> rgba((std::size_t)w * h * 4);
        switch (image->getBitDepth()) {
        case eImageBitDepthByte:
            readRows<unsigned char>(*image, rect, nComps, 1.f / 255.f, &rgba);
            break;
        case eImageBitDepthShort:
            readRows<unsigned short>(*image, rect, nComps, 1.f / 65535.f, &rgba);
            break;
        case eImageBitDepthFloat:
            readRows<float>(*image, rect, nComps, 1.f, &rgba);
            break;
        default:
            throw std::runtime_error("The Source's bit depth is not supported.");
        }
        image.reset();

        bool displayed = false;
        if (job->display) {
            try {
                for (int row = 0; row < h; ++row) {
                    job->display->applyForEightBitOutput(&rgba[(std::size_t)row * w * 4], w);
                }
                displayed = true;
            } catch (const std::exception&) {
                displayed = false;
            }
        }
        if (!displayed) {
            for (std::size_t i = 0; i < rgba.size(); i += 4) {
                rgba[i] = srgbEncode(rgba[i]);
                rgba[i + 1] = srgbEncode(rgba[i + 1]);
                rgba[i + 2] = srgbEncode(rgba[i + 2]);
            }
        }

        // The encoder assumes square pixels: resample the rows to the canonical width.
        const int width = std::max(1, (int)std::lround(w * par));
        std::vector<uint8_t> rgb((std::size_t)width * h * 3);
        for (int row = 0; row < h; ++row) {
            const float* src = &rgba[(std::size_t)row * w * 4];
            uint8_t* dst = &rgb[(std::size_t)row * width * 3];
            for (int x = 0; x < width; ++x) {
                const double fx = std::min((double)w - 1., std::max(0., (x + 0.5) * w / (double)width - 0.5));
                const int x0 = (int)fx;
                const int x1 = std::min(x0 + 1, w - 1);
                const float t = (float)(fx - x0);
                for (int c = 0; c < 3; ++c) {
                    const float v = src[x0 * 4 + c] * (1.f - t) + src[x1 * 4 + c] * t;
                    dst[x * 3 + c] = (uint8_t)std::lround(std::min(1.f, std::max(0.f, v)) * 255.f);
                }
            }
        }

        if (!job->segmenter || (job->segmenterModelId != job->key.modelId)) {
            if (job->model.files.size() < 2) {
                throw std::runtime_error("The model package lacks its encoder or decoder.");
            }
            job->segmenter = std::make_shared<Segmenter>(job->model.files[0], job->model.files[1], job->model.config,
                                                         AiSessionOptions::fromPreferences(AiGraphOptimization::eAll));
            job->segmenterModelId = job->key.modelId;
        }
        Job* abortSource = job;
        job->segmenter->encode(rgb.data(), width, h, job->key, [abortSource]() {
            return abortSource->abort.load();
        });

        job->frame.key = job->key;
        job->frame.canonical.x1 = rect.x1 * par;
        job->frame.canonical.x2 = rect.x2 * par;
        job->frame.canonical.y1 = rect.y1;
        job->frame.canonical.y2 = rect.y2;
        job->frame.width = width;
        job->frame.height = h;
        job->ok = !job->abort;
    } catch (const std::exception& e) {
        job->error = e.what();
    }
} // RotoObjectSelect::runJob

void
RotoObjectSelect::onJobFinished(const std::shared_ptr<Job>& job)
{
    if (_running == job) {
        _running.reset();
    }
    joinWorker();
    if (NodePtr node = _roto->getNode()) {
        node->setCurrentCursor(eCursorDefault);
    }
    if (job->segmenter) {
        if (job->segmenter != _segmenter) {
            _frames.clear();
        }
        _segmenter = job->segmenter;
        _segmenterModelId = job->segmenterModelId;
    }
    if (!job->ok) {
        _hasFailed = true;
        _failedKey = job->key;
        setMessage(job->error.empty() ? std::string("Encoding the Source failed.") : job->error);
        redraw();
        // Prompts made for another key meanwhile still get their encoding.
        refreshMask();

        return;
    }
    _frames.remove_if([&job](const Frame& f) {
        return f.key == job->key;
    });
    _frames.push_front(job->frame);
    while (_frames.size() > Segmenter::kCacheCapacity) {
        _frames.pop_back();
    }
    setMessage(std::string());
    refreshMask();
}

void
RotoObjectSelect::dropPrompts()
{
    _prompts.clear();
    _mask.clear();
    _contours.clear();
    _pressed = false;
    _dragging = false;
}

void
RotoObjectSelect::setMessage(const std::string& message)
{
    _message = message;
}

void
RotoObjectSelect::redraw()
{
    _roto->redrawOverlays();
}

bool
RotoObjectSelect::penDown(double time,
                          const QPointF& pos,
                          bool negative)
{
    if (!sourceNode()) {
        setMessage(kRotoUIParamObjectSelectToolButtonActionNoSourceHint);
        redraw();

        return true;
    }
    _pressed = true;
    _dragging = false;
    _pressNegative = negative;
    _pressTime = time;
    _pressPos = pos;
    _dragPos = pos;

    return true;
}

bool
RotoObjectSelect::penMotion(const QPointF& pos)
{
    if (!_pressed) {
        return false;
    }
    const std::pair<double, double> pixelScale = _roto->getPixelScale();
    const double dx = (pos.x() - _pressPos.x()) / std::max(1e-9, pixelScale.first);
    const double dy = (pos.y() - _pressPos.y()) / std::max(1e-9, pixelScale.second);
    if (_dragging || (std::hypot(dx, dy) > kDragThreshold)) {
        _dragging = true;
        _dragPos = pos;
        redraw();
    }

    return true;
}

bool
RotoObjectSelect::penUp()
{
    if (!_pressed) {
        return false;
    }
    _pressed = false;
    Prompt prompt;
    prompt.a = _pressPos;
    if (_dragging) {
        _dragging = false;
        prompt.kind = Prompt::eBox;
        prompt.b = _dragPos;
    } else {
        prompt.kind = _pressNegative ? Prompt::eNegative : Prompt::ePositive;
        prompt.b = _pressPos;
    }
    addPrompt(_pressTime, prompt);

    return true;
}

bool
RotoObjectSelect::removeLastPrompt()
{
    if (!_prompts.empty()) {
        _prompts.pop_back();
        setMessage(std::string());
        refreshMask();
    }

    return true;
}

bool
RotoObjectSelect::clearPrompts()
{
    dropPrompts();
    setMessage(std::string());
    redraw();

    return true;
}

bool
RotoObjectSelect::commit()
{
    if (_running) {
        setMessage("The Source is still being encoded.");
        redraw();

        return true;
    }
    if (_contours.empty()) {
        setMessage(_prompts.empty() ? "Click on an object first." : "The selection is empty: nothing to commit.");
        redraw();

        return true;
    }
    KnobDoublePtr feather = _feather.lock();
    _roto->pushUndoCommand(new CommitObjectMaskUndoCommand(_roto->shared_from_this(), _contours, _promptsTime, feather ? feather->getValue() : 0.));
    dropPrompts();
    setMessage(std::string());
    redraw();

    return true;
}

void
RotoObjectSelect::draw(double /*time*/)
{
    OverlaySupport* overlay = _roto->getHost()->getCurrentViewportForOverlays();

    if (!overlay) {
        return;
    }
    double psx = 1., psy = 1.;
    overlay->getPixelScale(psx, psy);
    const double ratio = overlay->getScreenPixelRatio();

    {
        GLProtectAttrib a(GL_HINT_BIT | GL_ENABLE_BIT | GL_LINE_BIT | GL_COLOR_BUFFER_BIT | GL_POINT_BIT | GL_CURRENT_BIT);

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_LINE_SMOOTH);
        glHint(GL_LINE_SMOOTH_HINT, GL_DONT_CARE);
        glEnable(GL_POINT_SMOOTH);

        // The preview outlines.
        glLineWidth(2. * ratio);
        for (const MaskTrace::Contour& contour : _contours) {
            if (contour.isHole) {
                glColor4f(1.f, 0.35f, 0.9f, 1.f);
            } else {
                glColor4f(1.f, 0.7f, 0.1f, 1.f);
            }
            const std::size_t n = contour.points.size();
            glBegin(GL_LINE_LOOP);
            for (std::size_t k = 0; k < n; ++k) {
                for (int s = 0; s < kCurveSamplesPerSegment; ++s) {
                    const MaskTrace::Point p = cubicAt(contour.points[k], contour.points[(k + 1) % n], (double)s / kCurveSamplesPerSegment);
                    glVertex2d(p.x, p.y);
                }
            }
            glEnd();
        }

        // Boxes, and the one being dragged.
        glLineWidth(1.5 * ratio);
        for (const Prompt& prompt : _prompts) {
            if (prompt.kind != Prompt::eBox) {
                continue;
            }
            glColor4f(0.2f, 1.f, 0.3f, 1.f);
            glBegin(GL_LINE_LOOP);
            glVertex2d(prompt.a.x(), prompt.a.y());
            glVertex2d(prompt.b.x(), prompt.a.y());
            glVertex2d(prompt.b.x(), prompt.b.y());
            glVertex2d(prompt.a.x(), prompt.b.y());
            glEnd();
        }
        if (_pressed && _dragging) {
            glColor4f(1.f, 1.f, 1.f, 0.8f);
            glBegin(GL_LINE_LOOP);
            glVertex2d(_pressPos.x(), _pressPos.y());
            glVertex2d(_dragPos.x(), _pressPos.y());
            glVertex2d(_dragPos.x(), _dragPos.y());
            glVertex2d(_pressPos.x(), _dragPos.y());
            glEnd();
        }

        // Click handles: a dark rim, then green for positive and red for negative.
        for (int pass = 0; pass < 2; ++pass) {
            glPointSize((pass == 0 ? 10. : 7.) * ratio);
            glBegin(GL_POINTS);
            for (const Prompt& prompt : _prompts) {
                if (prompt.kind == Prompt::eBox) {
                    continue;
                }
                if (pass == 0) {
                    glColor4f(0.f, 0.f, 0.f, 0.8f);
                } else if (prompt.kind == Prompt::ePositive) {
                    glColor4f(0.2f, 1.f, 0.3f, 1.f);
                } else {
                    glColor4f(1.f, 0.2f, 0.2f, 1.f);
                }
                glVertex2d(prompt.a.x(), prompt.a.y());
            }
            glEnd();
        }
    }

    if (!_message.empty()) {
        const RectD viewport = overlay->getViewportRect();
        overlay->renderText(viewport.x1 + 12. * psx, viewport.y2 - 40. * psy, _message, 1., 0.8, 0.3);
    }
    glCheckError();
} // RotoObjectSelect::draw

NATRON_NAMESPACE_EXIT
