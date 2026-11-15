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

#include "Retouch.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <functional>
#include <span>
#include <sstream>

#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
#include "Engine/ChoiceOption.h"
#include "Engine/ContentHasher.h"
#include "Engine/Curve.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/Filter/EdgeAware/EdgeAwareCommon.h"
#include "Engine/Nodes/Filter/EdgeAware/Eigf.h"
#include "Engine/Nodes/Filter/EdgeAware/PoissonHeal.h"
#include "Engine/Nodes/Filter/EdgeAware/Wavelets.h"
#include "Engine/Nodes/Filter/RetouchAlgorithms.h"
#include "Engine/Nodes/Image/PassExecutor.h"
#include "Engine/Nodes/Image/PixelKernel.h"
#include "Engine/Nodes/Image/StagePlan.h"
#include "Engine/Nodes/Image/WholeFrame.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/OverlaySupport.h"
#include "Engine/PluginActionShortcut.h"
#include "Engine/PoolParallelFor.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/RotoDrawableItem.h"
#include "Engine/RotoLayer.h"
#include "Engine/RotoStrokeItem.h"
#include "Engine/UndoCommand.h"
#include "Global/GLIncludes.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRecord.h"
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/GpuContext.h"
#include "Engine/Nodes/Filter/EdgeAware/EdgeAwareGpu.h"
#endif

NATRON_NAMESPACE_ENTER

namespace RC = RetouchCore;

namespace {
// The source mark of a shape tool, which has no brush footprint to draw.
const double kSourceMarkRadiusPixels = 3.5;

template <typename T>
std::shared_ptr<T>
itemKnob(const RotoDrawableItemPtr& item,
         const char* name)
{
    return item ? std::dynamic_pointer_cast<T>(item->getKnobByName(name)) : std::shared_ptr<T>();
}

// Whether item is a shape this node made its knobs for.
bool
isRetouchShape(const RotoItemPtr& item)
{
    const RotoDrawableItemPtr drawable = std::dynamic_pointer_cast<RotoDrawableItem>(item);

    return (std::dynamic_pointer_cast<Bezier>(drawable) || std::dynamic_pointer_cast<RotoStrokeItem>(drawable))
        && bool(itemKnob<KnobInt>(drawable, kRetouchShapeParamScale));
}

// Whether a shape is complete: a Bezier is closed or ended, a brush stroke has had its pen up.
bool
isShapeFinished(const RotoDrawableItemPtr& item,
                const RotoStrokeItemPtr& strokeBeingPainted)
{
    BezierPtr bezier = std::dynamic_pointer_cast<Bezier>(item);
    if (bezier) {
        return bezier->isCurveFinished();
    }

    return std::dynamic_pointer_cast<RotoStrokeItem>(item) && (item != strokeBeingPainted);
}

// The canonical centre line of a brush shape at time, and its radius.
bool
brushCentreLine(const RotoDrawableItemPtr& item,
                double time,
                std::vector<QPointF>* line,
                double* radius)
{
    RotoStrokeItemPtr stroke = std::dynamic_pointer_cast<RotoStrokeItem>(item);
    KnobDoublePtr size = itemKnob<KnobDouble>(item, kRetouchShapeParamBrushSize);
    if (!stroke || !size) {
        return false;
    }
    *radius = size->getValueAtTime(time) / 2.;
    std::list<std::list<std::pair<Point, double>>> strokes;
    stroke->evaluateStroke(0, time, &strokes);
    line->clear();
    for (std::list<std::list<std::pair<Point, double>>>::const_iterator it = strokes.begin(); it != strokes.end(); ++it) {
        for (std::list<std::pair<Point, double>>::const_iterator p = it->begin(); p != it->end(); ++p) {
            line->push_back(QPointF(p->first.x, p->first.y));
        }
    }

    return !line->empty();
}

// Whether pos is on the source of a brush shape whose source is (dx, dy) from it.
bool
brushSourceContains(const RotoDrawableItemPtr& item,
                    double time,
                    double dx,
                    double dy,
                    const QPointF& pos)
{
    std::vector<QPointF> line;
    double radius = 0.;
    if (!brushCentreLine(item, time, &line, &radius)) {
        return false;
    }
    const double px = pos.x() - dx;
    const double py = pos.y() - dy;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const QPointF& a = line[i];
        const QPointF& b = (i + 1 < line.size()) ? line[i + 1] : line[i];
        const double ex = b.x() - a.x();
        const double ey = b.y() - a.y();
        const double len2 = ex * ex + ey * ey;
        const double t = (len2 > 0.) ? std::max(0., std::min(1., ((px - a.x()) * ex + (py - a.y()) * ey) / len2)) : 0.;
        const double qx = a.x() + t * ex - px;
        const double qy = a.y() + t * ey - py;
        if (qx * qx + qy * qy <= radius * radius) {
            return true;
        }
    }

    return false;
}

// The tools that draw a new shape, the ones darktable's source placement applies to.
bool
isDrawingTool(RotoToolEnum tool)
{
    return (tool == eRotoToolDrawBezier) || (tool == eRotoToolDrawEllipse) || (tool == eRotoToolDrawRectangle)
        || (tool == eRotoToolSolidBrush);
}

/**
 * @brief Sets dimension of an item's knob. While the item is selected its knob is slaved to the
 * node's knob of the same name, which the knob then reads, so that one is set too.
 *
 * The item's own state is first made the node knob's: a key set on the item makes the curve context
 * clone the item's own values and curves into the node knob, which would otherwise bring back the
 * item's stale values. The node knob is set before the item so that its key also reaches the curve
 * its panel reads on the main thread; the clone that follows the item's key then changes nothing.
 **/
template <typename T>
void
setItemKnobValue(const KnobIPtr& knob,
                 int dimension,
                 const T& value,
                 double time,
                 bool keyframe)
{
    std::list<Knob<T>*> targets;
    Knob<T>* own = dynamic_cast<Knob<T>*>(knob.get());
    if (!own) {
        return;
    }
    if (knob->isSlave(dimension)) {
        KnobIPtr masterKnob = knob->getMaster(dimension).second;
        Knob<T>* master = dynamic_cast<Knob<T>*>(masterKnob.get());
        if (master) {
            own->clone(master);
            targets.push_back(master);
        }
    }
    targets.push_back(own);
    for (typename std::list<Knob<T>*>::const_iterator it = targets.begin(); it != targets.end(); ++it) {
        if (keyframe) {
            // setValueAtTime writes the new key through this pointer unchecked.
            KeyFrame newKey;
            (*it)->setValueAtTime(time, value, ViewSpec::all(), dimension, eValueChangedReasonNatronInternalEdited, &newKey);
        } else {
            (*it)->setValue(value, ViewSpec::all(), dimension, true);
        }
    }
}

/**
 * @brief Writes a shape's source offset at time and asks for one render. The node's knob, written
 * too while the shape is selected, would otherwise ask for one per dimension, and the curve
 * context's evaluation one more.
 **/
void
writeSourceOffset(const RotoCurveContextPtr& context,
                  const KnobIPtr& offset,
                  const double* values,
                  double time,
                  const bool* keyframe)
{
    NodePtr node = context ? context->getNode() : NodePtr();
    EffectInstancePtr effect = node ? node->getEffectInstance() : EffectInstancePtr();
    if (effect) {
        effect->beginChanges();
    }
    for (int d = 0; d < 2; ++d) {
        setItemKnobValue<double>(offset, d, values[d], time, keyframe[d]);
    }
    if (context) {
        context->incrementAge();
    }
    if (effect) {
        effect->endChanges(true);
        effect->incrHashAndEvaluate(true, false);
    }
}

// The cycles of a draft heal, from the previous solve: enough to follow a drag smoothly.
const int kDraftHealCycles = 2;

const char* const kAlgorithmLabels[] = { "Clone", "Heal", "Blur", "Fill" };

bool
pointInPolygon(const std::list<ParametricPoint>& points,
               double dx,
               double dy,
               const QPointF& pos)
{
    bool inside = false;
    if (points.size() < 3) {
        return false;
    }
    std::list<ParametricPoint>::const_iterator prev = --points.end();
    for (std::list<ParametricPoint>::const_iterator it = points.begin(); it != points.end(); prev = it, ++it) {
        const double xi = it->x + dx, yi = it->y + dy;
        const double xj = prev->x + dx, yj = prev->y + dy;
        if (((yi > pos.y()) != (yj > pos.y())) && (pos.x() < (xj - xi) * (pos.y() - yi) / (yj - yi) + xi)) {
            inside = !inside;
        }
    }

    return inside;
}
} // anonymous namespace

/**
 * @brief Copies shapes to a scale, or moves them there, as one undoable step.
 **/
class RetouchScaleCommand
    : public UndoCommand {
    struct Entry {
        RotoDrawableItemPtr item;
        RotoDrawableItemPtr copy;
        RotoLayerPtr layer;
        int oldScale = 0;
    };

    RotoCurveContextPtr _context;
    std::list<Entry> _entries;
    int _scale;
    bool _move;

public:
    RetouchScaleCommand(const RotoCurveContextPtr& context,
                        const std::list<RotoDrawableItemPtr>& items,
                        int scale,
                        bool move)
        : UndoCommand()
        , _context(context)
        , _entries()
        , _scale(scale)
        , _move(move)
    {
        for (std::list<RotoDrawableItemPtr>::const_iterator it = items.begin(); it != items.end(); ++it) {
            Entry e;
            e.item = *it;
            KnobIntPtr scaleKnob = itemKnob<KnobInt>(*it, kRetouchShapeParamScale);
            e.oldScale = scaleKnob ? scaleKnob->getValue() : 0;
            e.layer = (*it)->getParentLayer();
            if (!move) {
                if (!e.layer) {
                    continue;
                }
                e.copy = context->makeDrawableCopy(*it, copyName(*it));
                if (!e.copy) {
                    continue;
                }
                // A selected shape's knobs read the node's knobs, which its own values may lag.
                const std::list<KnobIPtr>& knobs = (*it)->getKnobs();
                for (std::list<KnobIPtr>::const_iterator k = knobs.begin(); k != knobs.end(); ++k) {
                    KnobIPtr copyKnob = e.copy->getKnobByName((*k)->getName());
                    if (!copyKnob) {
                        continue;
                    }
                    for (int d = 0; d < (*k)->getDimension(); ++d) {
                        if ((*k)->isSlave(d)) {
                            std::pair<int, KnobIPtr> master = (*k)->getMaster(d);
                            if (master.second) {
                                copyKnob->clone(master.second.get(), d, master.first);
                            }
                        }
                    }
                }
                KnobIntPtr copyScale = itemKnob<KnobInt>(e.copy, kRetouchShapeParamScale);
                if (copyScale) {
                    copyScale->setValue(scale, ViewSpec::all(), 0, true);
                }
            }
            _entries.push_back(e);
        }
        std::stringstream ss;
        ss << (move ? "Move to Scale " : "Copy to Scale ") << scale;
        setText(ss.str());
    }

    virtual ~RetouchScaleCommand()
    {
    }

    virtual void redo() OVERRIDE FINAL
    {
        for (std::list<Entry>::const_iterator it = _entries.begin(); it != _entries.end(); ++it) {
            if (_move) {
                setItemKnobValue<int>(it->item->getKnobByName(kRetouchShapeParamScale), 0, _scale, 0., false);
                _context->emitItemPanelColumnsChanged(it->item);
            } else {
                const int index = std::max(0, it->layer->getChildIndex(it->item));
                _context->addItem(it->layer, index, it->copy, RotoItem::eSelectionReasonOther);
                _context->emitItemPanelColumnsChanged(it->copy);
            }
        }
        _context->evaluateChange();
    }

    virtual void undo() OVERRIDE FINAL
    {
        for (std::list<Entry>::const_iterator it = _entries.begin(); it != _entries.end(); ++it) {
            if (_move) {
                setItemKnobValue<int>(it->item->getKnobByName(kRetouchShapeParamScale), 0, it->oldScale, 0., false);
                _context->emitItemPanelColumnsChanged(it->item);
            } else {
                _context->removeItem(it->copy, RotoItem::eSelectionReasonOther);
            }
        }
        _context->evaluateChange();
    }

private:
    std::string copyName(const RotoDrawableItemPtr& item) const
    {
        const std::string base = item->getScriptName() + "_copy";
        std::string name = base;
        for (int i = 1; _context->getItemByName(name); ++i) {
            std::stringstream ss;
            ss << base << i;
            name = ss.str();
        }

        return name;
    }
};

/**
 * @brief A source drag, undone as one step. The drag has already written the new offset.
 **/
class RetouchSourceDragCommand
    : public UndoCommand {
    RotoCurveContextPtr _context;
    RotoDrawableItemPtr _item;
    double _time;
    double _oldValues[2];
    double _newValues[2];
    bool _keyframe[2];

public:
    RetouchSourceDragCommand(const RotoCurveContextPtr& context,
                             const RotoDrawableItemPtr& item,
                             double time,
                             const double* oldValues,
                             const double* newValues,
                             const bool* keyframe)
        : UndoCommand()
        , _context(context)
        , _item(item)
        , _time(time)
    {
        for (int d = 0; d < 2; ++d) {
            _oldValues[d] = oldValues[d];
            _newValues[d] = newValues[d];
            _keyframe[d] = keyframe[d];
        }
        setText("Move Retouch Source");
    }

    virtual ~RetouchSourceDragCommand()
    {
    }

    virtual void redo() OVERRIDE FINAL
    {
        apply(_newValues);
    }

    virtual void undo() OVERRIDE FINAL
    {
        apply(_oldValues);
    }

private:
    void apply(const double* values)
    {
        KnobIPtr offset = _item->getKnobByName(kRetouchShapeParamSourceOffset);
        if (!offset) {
            return;
        }
        writeSourceOffset(_context, offset, values, _time, _keyframe);
    }
};

Retouch::Retouch(NodePtr node)
    : StagedImageEffect(node)
    // Built here, as RotoPaint does: Node::load() creates the curve context before initializeKnobs()
    // and its endChanges() already calls knobChanged().
    , _ui(RetouchInteract::create(this))
{
}

Retouch::~Retouch()
{
}

const NodeSpec*
Retouch::nodeSpec() const
{
    return staticNodeSpec<kRetouchNodeSpec>();
}

const ImagePipelineSpec&
Retouch::imagePipelineSpec() const
{
    return staticImagePipelineSpec<kRetouchPipeline, kRetouchNodeSpec>();
}

bool
Retouch::setInteractiveDraft(bool draft)
{
    // A render whose hash snapshot predates the hash mark refuses to cache what it rendered.
    KnobContentWrite write;
    const bool was = _draft.exchange(draft);
    if (was == draft) {
        return was;
    }
    if (!draft) {
        std::lock_guard<std::mutex> lock(_warmStartsMutex);
        _warmStarts.clear();
    }
    getNode()->markHashDirty();

    return was;
}

void
Retouch::requestStrokeRender()
{
    ++_renderRequests;
    RotoCurveContextPtr context = getNode()->getCurveContext();
    if (context) {
        context->evaluateChange_noIncrement();
    }
}

void
Retouch::evaluate(bool isSignificant,
                  bool refreshMetadatas)
{
    if (isSignificant) {
        ++_renderRequests;
    }
    StagedImageEffect::evaluate(isSignificant, refreshMetadatas);
}

void
Retouch::appendNodeContentHash(ContentHasher& h) const
{
    StagedImageEffect::appendNodeContentHash(h);
    h.appendBool(_draft.load());
}

RotoCurvePanelHook*
Retouch::getCurvePanelHook() const
{
    return const_cast<Retouch*>(this);
}

void
Retouch::initializeKnobs()
{
    _ui->connectToCurveContext();

    KnobPagePtr page = createControlsPage();
    _ui->createViewerButtonKnobs(page);
    _ui->createBrushToolbarKnobs(_ui->createToolbarKnobs(eRotoCurveToolSetAll));
    _ui->createRightClickMenuKnobs(page);
    KnobButtonPtr resetCloneOffset = _ui->createResetCloneOffsetKnob(page);
    resetCloneOffset->setSecretByDefault(false);
    addKnobToViewerUI(resetCloneOffset);

    const std::vector<ChoiceOption> algorithms = algorithmOptions();

    KnobChoicePtr algorithm = createKnob<KnobChoice>(tr("Algorithm"));
    algorithm->setName(kRetouchParamAlgorithm);
    algorithm->setHintToolTip(tr("The algorithm new shapes get."));
    algorithm->populateChoices(algorithms);
    algorithm->setDefaultValue((int)RC::eAlgorithmHeal);
    algorithm->setAnimationEnabled(false);
    page->addKnob(algorithm);
    _algorithm = algorithm;

    KnobIntPtr scales = createKnob<KnobInt>(tr("Scales"));
    scales->setName(kRetouchParamScales);
    scales->setHintToolTip(tr("The number of wavelet detail scales the image is split into; each is twice as coarse as the one "
                              "before. 0 retouches the image itself. Small images and proxy renders get fewer."));
    scales->setDefaultValue(0);
    scales->setMinimum(0);
    scales->setMaximum(RC::kMaxScales);
    scales->setDisplayMinimum(0);
    scales->setDisplayMaximum(RC::kMaxScales);
    scales->setAnimationEnabled(false);
    page->addKnob(scales);
    _scales = scales;

    KnobIntPtr currentScale = createKnob<KnobInt>(tr("Current Scale"));
    currentScale->setName(kRetouchParamCurrentScale);
    currentScale->setHintToolTip(tr("The scale new shapes go on, and the one Output shows when set to Current Scale: 0 for the "
                                    "image, 1 to Scales for a detail scale, Scales + 1 for the residual."));
    currentScale->setDefaultValue(0);
    currentScale->setMinimum(0);
    currentScale->setMaximum(RC::kMaxScales + 1);
    currentScale->setDisplayMinimum(0);
    currentScale->setDisplayMaximum(RC::kMaxScales + 1);
    currentScale->setAnimationEnabled(false);
    page->addKnob(currentScale);
    _currentScale = currentScale;

    KnobIntPtr mergeFrom = createKnob<KnobInt>(tr("Merge From"));
    mergeFrom->setName(kRetouchParamMergeFrom);
    mergeFrom->setHintToolTip(tr("When above 0, the detail scales from this one on are merged into one, retouched by the shapes "
                                 "on this scale."));
    mergeFrom->setDefaultValue(0);
    mergeFrom->setMinimum(0);
    mergeFrom->setMaximum(RC::kMaxScales);
    mergeFrom->setDisplayMinimum(0);
    mergeFrom->setDisplayMaximum(RC::kMaxScales);
    mergeFrom->setAnimationEnabled(false);
    page->addKnob(mergeFrom);
    _mergeFrom = mergeFrom;

    KnobChoicePtr output = createKnob<KnobChoice>(tr("Output"));
    output->setName(kRetouchParamOutput);
    output->setHintToolTip(tr("What the node outputs."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kRetouchOutputResult, tr("Result").toStdString(),
                                       tr("The retouched image.").toStdString()));
        options.push_back(ChoiceOption(kRetouchOutputCurrentScale, tr("Current Scale").toStdString(),
                                       tr("The retouched Current Scale alone, a detail scale shown through Preview Levels.").toStdString()));
        options.push_back(ChoiceOption(kRetouchOutputResidual, tr("Residual").toStdString(),
                                       tr("The retouched residual, the image without its detail scales.").toStdString()));
        output->populateChoices(options);
    }
    output->setDefaultValue((int)RC::eOutputResult);
    output->setAnimationEnabled(false);
    page->addKnob(output);
    _output = output;

    KnobDoublePtr previewLevels = createKnob<KnobDouble>(tr("Preview Levels"), 3);
    previewLevels->setName(kRetouchParamPreviewLevels);
    previewLevels->setHintToolTip(tr("The black, grey and white points (lightness / 100) a detail scale is shown with when Output is "
                                     "Current Scale."));
    previewLevels->setDefaultValue(RC::kPreviewLevelMin, 0);
    previewLevels->setDefaultValue(0., 1);
    previewLevels->setDefaultValue(RC::kPreviewLevelMax, 2);
    for (int d = 0; d < 3; ++d) {
        previewLevels->setMinimum(RC::kPreviewLevelMin, d);
        previewLevels->setMaximum(RC::kPreviewLevelMax, d);
        previewLevels->setDisplayMinimum(RC::kPreviewLevelMin, d);
        previewLevels->setDisplayMaximum(RC::kPreviewLevelMax, d);
    }
    page->addKnob(previewLevels);
    _previewLevels = previewLevels;

    KnobChoicePtr blurType = createKnob<KnobChoice>(tr("Blur Type"));
    blurType->setName(kRetouchParamBlurType);
    blurType->setHintToolTip(tr("The blur new Blur shapes get."));
    blurType->populateChoices(blurTypeOptions());
    blurType->setDefaultValue((int)RC::eBlurTypeGaussian);
    blurType->setAnimationEnabled(false);
    page->addKnob(blurType);
    _blurType = blurType;

    KnobDoublePtr blurRadius = createKnob<KnobDouble>(tr("Blur Radius"));
    blurRadius->setName(kRetouchParamBlurRadius);
    blurRadius->setHintToolTip(tr("The blur radius new Blur shapes get, in pixels."));
    blurRadius->setDefaultValue(10.);
    blurRadius->setMinimum(0.1);
    blurRadius->setMaximum(200.);
    blurRadius->setDisplayMinimum(0.1);
    blurRadius->setDisplayMaximum(200.);
    page->addKnob(blurRadius);
    _blurRadius = blurRadius;

    KnobChoicePtr fillMode = createKnob<KnobChoice>(tr("Fill Mode"));
    fillMode->setName(kRetouchParamFillMode);
    fillMode->setHintToolTip(tr("The fill new Fill shapes get."));
    fillMode->populateChoices(fillModeOptions());
    fillMode->setDefaultValue((int)RC::eFillModeErase);
    fillMode->setAnimationEnabled(false);
    page->addKnob(fillMode);
    _fillMode = fillMode;

    KnobColorPtr fillColor = createKnob<KnobColor>(tr("Fill Color"), 3);
    fillColor->setName(kRetouchParamFillColor);
    fillColor->setHintToolTip(tr("The colour new Fill shapes get, for the Color fill mode."));
    for (int d = 0; d < 3; ++d) {
        fillColor->setDefaultValue(0., d);
    }
    page->addKnob(fillColor);
    _fillColor = fillColor;

    KnobDoublePtr fillBrightness = createKnob<KnobDouble>(tr("Fill Brightness"));
    fillBrightness->setName(kRetouchParamFillBrightness);
    fillBrightness->setHintToolTip(tr("The brightness new Fill shapes get, added to the fill colour."));
    fillBrightness->setDefaultValue(0.);
    fillBrightness->setMinimum(-1.);
    fillBrightness->setMaximum(1.);
    fillBrightness->setDisplayMinimum(-1.);
    fillBrightness->setDisplayMaximum(1.);
    page->addKnob(fillBrightness);
    _fillBrightness = fillBrightness;

    KnobIntPtr maxHealIterations = createKnob<KnobInt>(tr("Max Heal Iterations"));
    maxHealIterations->setName(kRetouchParamMaxHealIterations);
    maxHealIterations->setHintToolTip(tr("The most iterations a Heal shape's solver runs before it stops. It usually converges within 20; the GPU solver stops at 64."));
    maxHealIterations->setDefaultValue(EdgeAware::kPoissonHealDefaultIterations);
    maxHealIterations->setMinimum(1);
    maxHealIterations->setMaximum(100000);
    maxHealIterations->setDisplayMinimum(1);
    maxHealIterations->setDisplayMaximum(5000);
    maxHealIterations->setAnimationEnabled(false);
    page->addKnob(maxHealIterations);
    _maxHealIterations = maxHealIterations;

    KnobDoublePtr brushSize = createKnob<KnobDouble>(tr("Brush Size"));
    brushSize->setName(kRetouchParamBrushSize);
    brushSize->setHintToolTip(tr("The diameter new brush shapes get, in pixels: where the brush reaches 0."));
    brushSize->setDefaultValue(100.);
    brushSize->setMinimum(1.);
    brushSize->setMaximum(4000.);
    brushSize->setDisplayMinimum(1.);
    brushSize->setDisplayMaximum(500.);
    page->addKnob(brushSize);
    _brushSize = brushSize;
    _ui->brushSizeKnob = brushSize;

    KnobDoublePtr brushHardness = createKnob<KnobDouble>(tr("Brush Hardness"));
    brushHardness->setName(kRetouchParamBrushHardness);
    brushHardness->setHintToolTip(tr("The hardness new brush shapes get: the fraction of the radius at full density, the rest "
                                     "falling off to 0."));
    brushHardness->setDefaultValue(0.66);
    brushHardness->setMinimum(RC::kBrushHardnessMin);
    brushHardness->setMaximum(1.);
    brushHardness->setDisplayMinimum(0.);
    brushHardness->setDisplayMaximum(1.);
    page->addKnob(brushHardness);
    _brushHardness = brushHardness;

    KnobDoublePtr brushDensity = createKnob<KnobDouble>(tr("Brush Density"));
    brushDensity->setName(kRetouchParamBrushDensity);
    brushDensity->setHintToolTip(tr("The density new brush shapes get: the mask's value at the centre of the stroke."));
    brushDensity->setDefaultValue(1.);
    brushDensity->setMinimum(0.);
    brushDensity->setMaximum(1.);
    brushDensity->setDisplayMinimum(0.);
    brushDensity->setDisplayMaximum(1.);
    page->addKnob(brushDensity);
    _brushDensity = brushDensity;

    KnobChoicePtr brushPressure = createKnob<KnobChoice>(tr("Brush Pressure"));
    brushPressure->setName(kRetouchParamBrushPressure);
    brushPressure->setHintToolTip(tr("What pen pressure changes on new brush shapes."));
    brushPressure->populateChoices(brushPressureOptions());
    brushPressure->setDefaultValue((int)RC::eBrushPressureOff);
    brushPressure->setAnimationEnabled(false);
    page->addKnob(brushPressure);
    _brushPressure = brushPressure;

    // The curve context slaves these to the selected shapes. With none selected they edit the
    // next-shape knobs above, which keep those settings while a shape is selected and so are hidden.
    KnobGroupPtr shapeSettings = createKnob<KnobGroup>(tr("Shape Settings"));
    shapeSettings->setName(kRetouchParamShapeSettings);
    shapeSettings->setHintToolTip(tr("The selected shapes' settings. With no shape selected, the settings the next shape gets."));
    shapeSettings->setAsTab();
    page->addKnob(shapeSettings);
    RotoCurveContextPtr context = getNode()->getCurveContext();
    const std::vector<KnobIPtr> shapeKnobs = createShapeKnobs(this, true);
    for (std::vector<KnobIPtr>::const_iterator it = shapeKnobs.begin(); it != shapeKnobs.end(); ++it) {
        const std::string& name = (*it)->getName();
        if (name == kRetouchShapeParamScale) {
            _shapeScaleKnob = *it;
            (*it)->setDefaultAllDimensionsEnabled(false);
        } else if (name == kRetouchShapeParamSourceOffset) {
            _shapeSourceOffsetKnob = *it;
            (*it)->setDefaultAllDimensionsEnabled(false);
        }
        (*it)->setIsPersistent(false);
        shapeSettings->addKnob(*it);
        if (context) {
            context->addHostItemKnob(*it);
        }
    }
    const std::pair<const char*, KnobIPtr> nextShapeKnobs[] = {
        { kRetouchShapeParamAlgorithm, algorithm },
        { kRetouchShapeParamBlurType, blurType },
        { kRetouchShapeParamBlurRadius, blurRadius },
        { kRetouchShapeParamFillMode, fillMode },
        { kRetouchShapeParamFillColor, fillColor },
        { kRetouchShapeParamFillBrightness, fillBrightness },
        { kRetouchShapeParamBrushSize, brushSize },
        { kRetouchShapeParamBrushHardness, brushHardness },
        { kRetouchShapeParamBrushDensity, brushDensity },
        { kRetouchShapeParamBrushPressure, brushPressure },
    };
    for (const std::pair<const char*, KnobIPtr>& next : nextShapeKnobs) {
        for (std::vector<KnobIPtr>::const_iterator it = shapeKnobs.begin(); it != shapeKnobs.end(); ++it) {
            if ((*it)->getName() == next.first) {
                next.second->setSecretByDefault(true);
                _shapeKnobDefaults.push_back(std::make_pair(KnobIWPtr(*it), KnobIWPtr(next.second)));
                break;
            }
        }
    }
    if (context) {
        // The General tab's Color is a Roto shape's paint colour; a retouch mask has none.
        KnobColorPtr color = context->getColorKnob();
        if (color) {
            color->setSecretByDefault(true);
        }
        QObject::connect(context.get(), &RotoCurveContext::selectionChanged, this, [this](int) {
            onShapeSelectionChanged();
        });
    }

    addStageKnobs(page);
} // Retouch::initializeKnobs
std::vector<ChoiceOption>
Retouch::algorithmOptions()
{
    std::vector<ChoiceOption> algorithms;
    algorithms.push_back(ChoiceOption(kRetouchAlgorithmClone, tr("Clone").toStdString(),
                                      tr("Copies the source onto the shape.").toStdString()));
    algorithms.push_back(ChoiceOption(kRetouchAlgorithmHeal, tr("Heal").toStdString(),
                                      tr("Copies the source's texture onto the shape and blends it into the colours around the shape.").toStdString()));
    algorithms.push_back(ChoiceOption(kRetouchAlgorithmBlur, tr("Blur").toStdString(),
                                      tr("Blurs the image under the shape.").toStdString()));
    algorithms.push_back(ChoiceOption(kRetouchAlgorithmFill, tr("Fill").toStdString(),
                                      tr("Fills the shape with a colour.").toStdString()));
    return algorithms;
}

std::vector<ChoiceOption>
Retouch::blurTypeOptions()
{
    std::vector<ChoiceOption> blurTypes;
    blurTypes.push_back(ChoiceOption(kRetouchBlurTypeGaussian, tr("Gaussian").toStdString(),
                                     tr("A gaussian blur.").toStdString()));
    blurTypes.push_back(ChoiceOption(kRetouchBlurTypeBilateral, tr("Bilateral").toStdString(),
                                     tr("An edge-preserving blur of the lightness, through darktable's bilateral grid.").toStdString()));
    return blurTypes;
}

std::vector<ChoiceOption>
Retouch::fillModeOptions()
{
    std::vector<ChoiceOption> fillModes;
    fillModes.push_back(ChoiceOption(kRetouchFillModeErase, tr("Erase").toStdString(),
                                     tr("Fills with grey at the Fill Brightness: on a detail scale, 0 erases the detail.").toStdString()));
    fillModes.push_back(ChoiceOption(kRetouchFillModeColor, tr("Color").toStdString(),
                                     tr("Fills with Fill Color plus Fill Brightness.").toStdString()));
    return fillModes;
}

std::vector<ChoiceOption>
Retouch::brushPressureOptions()
{
    std::vector<ChoiceOption> options;
    options.push_back(ChoiceOption("off", tr("Off").toStdString(), tr("Pressure changes nothing.").toStdString()));
    options.push_back(ChoiceOption("hardnessRelative", tr("Hardness (Relative)").toStdString(),
                                   tr("The hardness is scaled by the pressure.").toStdString()));
    options.push_back(ChoiceOption("hardnessAbsolute", tr("Hardness (Absolute)").toStdString(),
                                   tr("The hardness is the pressure.").toStdString()));
    options.push_back(ChoiceOption("opacityRelative", tr("Opacity (Relative)").toStdString(),
                                   tr("The density is scaled by the pressure.").toStdString()));
    options.push_back(ChoiceOption("opacityAbsolute", tr("Opacity (Absolute)").toStdString(),
                                   tr("The density is the pressure.").toStdString()));
    options.push_back(ChoiceOption("sizeRelative", tr("Brush Size (Relative)").toStdString(),
                                   tr("The size is scaled by the pressure.").toStdString()));
    return options;
}

std::vector<KnobIPtr>
Retouch::createShapeKnobs(Retouch* host,
                          bool withBrush)
{
    std::vector<KnobIPtr> knobs;
    const auto make = [host](auto typeTag, const QString& label, int dimension) {
        typedef typename decltype(typeTag)::element_type K;
        std::shared_ptr<K> k;
        if (host) {
            k = AppManager::createKnob<K>(host, label, dimension);
        } else {
            k = std::make_shared<K>((KnobHolder*)NULL, label.toStdString(), dimension, true);
            k->populate();
        }

        return k;
    };

    KnobChoicePtr algorithm = make(KnobChoicePtr(), tr("Shape Algorithm"), 1);
    algorithm->setName(kRetouchShapeParamAlgorithm);
    algorithm->setHintToolTip(tr("What the shape does."));
    algorithm->populateChoices(algorithmOptions());
    algorithm->setDefaultValue((int)RC::eAlgorithmHeal);
    algorithm->setAnimationEnabled(false);
    knobs.push_back(algorithm);

    KnobIntPtr scale = make(KnobIntPtr(), tr("Shape Scale"), 1);
    scale->setName(kRetouchShapeParamScale);
    scale->setHintToolTip(tr("The scale the shape works on: 0 for the image, 1 to Scales for a detail scale, Scales + 1 for "
                             "the residual. A shape on a higher scale does nothing."));
    scale->setDefaultValue(0);
    scale->setMinimum(0);
    scale->setMaximum(RC::kMaxScales + 1);
    scale->setDisplayMinimum(0);
    scale->setDisplayMaximum(RC::kMaxScales + 1);
    scale->setAnimationEnabled(false);
    knobs.push_back(scale);

    KnobDoublePtr sourceOffset = make(KnobDoublePtr(), tr("Source Offset"), 2);
    sourceOffset->setName(kRetouchShapeParamSourceOffset);
    sourceOffset->setHintToolTip(tr("Where a Clone or Heal shape takes its source: the source's offset from the shape, in "
                                    "pixels. Drag the dashed outline in the viewer to set it."));
    sourceOffset->setDefaultValue(0., 0);
    sourceOffset->setDefaultValue(0., 1);
    knobs.push_back(sourceOffset);

    KnobChoicePtr blurType = make(KnobChoicePtr(), tr("Shape Blur Type"), 1);
    blurType->setName(kRetouchShapeParamBlurType);
    blurType->setHintToolTip(tr("The blur of a Blur shape."));
    blurType->populateChoices(blurTypeOptions());
    blurType->setDefaultValue((int)RC::eBlurTypeGaussian);
    blurType->setAnimationEnabled(false);
    knobs.push_back(blurType);

    KnobDoublePtr blurRadius = make(KnobDoublePtr(), tr("Shape Blur Radius"), 1);
    blurRadius->setName(kRetouchShapeParamBlurRadius);
    blurRadius->setHintToolTip(tr("The blur radius of a Blur shape, in pixels."));
    blurRadius->setDefaultValue(10.);
    blurRadius->setMinimum(0.1);
    blurRadius->setMaximum(200.);
    blurRadius->setDisplayMinimum(0.1);
    blurRadius->setDisplayMaximum(200.);
    knobs.push_back(blurRadius);

    KnobChoicePtr fillMode = make(KnobChoicePtr(), tr("Shape Fill Mode"), 1);
    fillMode->setName(kRetouchShapeParamFillMode);
    fillMode->setHintToolTip(tr("The fill of a Fill shape."));
    fillMode->populateChoices(fillModeOptions());
    fillMode->setDefaultValue((int)RC::eFillModeErase);
    fillMode->setAnimationEnabled(false);
    knobs.push_back(fillMode);

    KnobColorPtr fillColor = make(KnobColorPtr(), tr("Shape Fill Color"), 3);
    fillColor->setName(kRetouchShapeParamFillColor);
    fillColor->setHintToolTip(tr("The colour of a Fill shape in the Color fill mode."));
    for (int d = 0; d < 3; ++d) {
        fillColor->setDefaultValue(0., d);
    }
    knobs.push_back(fillColor);

    KnobDoublePtr fillBrightness = make(KnobDoublePtr(), tr("Shape Fill Brightness"), 1);
    fillBrightness->setName(kRetouchShapeParamFillBrightness);
    fillBrightness->setHintToolTip(tr("Added to a Fill shape's colour."));
    fillBrightness->setDefaultValue(0.);
    fillBrightness->setMinimum(-1.);
    fillBrightness->setMaximum(1.);
    fillBrightness->setDisplayMinimum(-1.);
    fillBrightness->setDisplayMaximum(1.);
    knobs.push_back(fillBrightness);

    if (!withBrush) {
        return knobs;
    }

    KnobDoublePtr brushSize = make(KnobDoublePtr(), tr("Shape Brush Size"), 1);
    brushSize->setName(kRetouchShapeParamBrushSize);
    brushSize->setHintToolTip(tr("The diameter of a brush shape, in pixels: where the brush reaches 0."));
    brushSize->setDefaultValue(100.);
    brushSize->setMinimum(1.);
    brushSize->setMaximum(4000.);
    brushSize->setDisplayMinimum(1.);
    brushSize->setDisplayMaximum(500.);
    knobs.push_back(brushSize);

    KnobDoublePtr brushHardness = make(KnobDoublePtr(), tr("Shape Brush Hardness"), 1);
    brushHardness->setName(kRetouchShapeParamBrushHardness);
    brushHardness->setHintToolTip(tr("The fraction of a brush shape's radius at full density, the rest falling off to 0."));
    brushHardness->setDefaultValue(0.66);
    brushHardness->setMinimum(RC::kBrushHardnessMin);
    brushHardness->setMaximum(1.);
    brushHardness->setDisplayMinimum(0.);
    brushHardness->setDisplayMaximum(1.);
    knobs.push_back(brushHardness);

    KnobDoublePtr brushDensity = make(KnobDoublePtr(), tr("Shape Brush Density"), 1);
    brushDensity->setName(kRetouchShapeParamBrushDensity);
    brushDensity->setHintToolTip(tr("The mask's value at the centre of a brush shape's stroke."));
    brushDensity->setDefaultValue(1.);
    brushDensity->setMinimum(0.);
    brushDensity->setMaximum(1.);
    brushDensity->setDisplayMinimum(0.);
    brushDensity->setDisplayMaximum(1.);
    knobs.push_back(brushDensity);

    KnobChoicePtr brushPressure = make(KnobChoicePtr(), tr("Shape Brush Pressure"), 1);
    brushPressure->setName(kRetouchShapeParamBrushPressure);
    brushPressure->setHintToolTip(tr("What the pen pressure recorded with a brush shape changes."));
    brushPressure->populateChoices(brushPressureOptions());
    brushPressure->setDefaultValue((int)RC::eBrushPressureOff);
    brushPressure->setAnimationEnabled(false);
    knobs.push_back(brushPressure);

    return knobs;
} // Retouch::createShapeKnobs

void
Retouch::onCurveItemCreated(const RotoDrawableItemPtr& item)
{
    const bool isBrush = bool(std::dynamic_pointer_cast<RotoStrokeItem>(item));
    if (!isBrush && !std::dynamic_pointer_cast<Bezier>(item)) {
        return;
    }
    const std::vector<KnobIPtr> knobs = createShapeKnobs(NULL, isBrush);
    for (std::vector<KnobIPtr>::const_iterator it = knobs.begin(); it != knobs.end(); ++it) {
        item->addHostKnob(*it);
    }

    // A new shape takes the node's settings; a loaded or copied one has its own restored after this.
    const double time = getCurrentTime();
    KnobChoicePtr algorithm = _algorithm.lock();
    KnobIntPtr currentScale = _currentScale.lock();
    KnobChoicePtr blurType = _blurType.lock();
    KnobDoublePtr blurRadius = _blurRadius.lock();
    KnobChoicePtr fillMode = _fillMode.lock();
    KnobColorPtr fillColor = _fillColor.lock();
    KnobDoublePtr fillBrightness = _fillBrightness.lock();
    if (algorithm) {
        itemKnob<KnobChoice>(item, kRetouchShapeParamAlgorithm)->setValue(algorithm->getValue(), ViewSpec::all(), 0, true);
    }
    if (currentScale) {
        itemKnob<KnobInt>(item, kRetouchShapeParamScale)->setValue(currentScale->getValue(), ViewSpec::all(), 0, true);
    }
    if (blurType) {
        itemKnob<KnobChoice>(item, kRetouchShapeParamBlurType)->setValue(blurType->getValue(), ViewSpec::all(), 0, true);
    }
    if (blurRadius) {
        itemKnob<KnobDouble>(item, kRetouchShapeParamBlurRadius)->setValue(blurRadius->getValueAtTime(time), ViewSpec::all(), 0, true);
    }
    if (fillMode) {
        itemKnob<KnobChoice>(item, kRetouchShapeParamFillMode)->setValue(fillMode->getValue(), ViewSpec::all(), 0, true);
    }
    if (fillColor) {
        KnobColorPtr k = itemKnob<KnobColor>(item, kRetouchShapeParamFillColor);
        for (int d = 0; d < 3; ++d) {
            k->setValue(fillColor->getValueAtTime(time, d), ViewSpec::all(), d, true);
        }
    }
    if (fillBrightness) {
        itemKnob<KnobDouble>(item, kRetouchShapeParamFillBrightness)->setValue(fillBrightness->getValueAtTime(time), ViewSpec::all(), 0, true);
    }
    if (isBrush) {
        KnobDoublePtr brushSize = _brushSize.lock();
        KnobDoublePtr brushHardness = _brushHardness.lock();
        KnobDoublePtr brushDensity = _brushDensity.lock();
        KnobChoicePtr brushPressure = _brushPressure.lock();
        if (brushSize) {
            itemKnob<KnobDouble>(item, kRetouchShapeParamBrushSize)->setValue(brushSize->getValueAtTime(time), ViewSpec::all(), 0, true);
        }
        if (brushHardness) {
            itemKnob<KnobDouble>(item, kRetouchShapeParamBrushHardness)->setValue(brushHardness->getValueAtTime(time), ViewSpec::all(), 0, true);
        }
        if (brushDensity) {
            itemKnob<KnobDouble>(item, kRetouchShapeParamBrushDensity)->setValue(brushDensity->getValueAtTime(time), ViewSpec::all(), 0, true);
        }
        if (brushPressure) {
            itemKnob<KnobChoice>(item, kRetouchShapeParamBrushPressure)->setValue(brushPressure->getValue(), ViewSpec::all(), 0, true);
        }
    }

    // Keep the panel's Scale / Algorithm column current.
    RotoCurveContextPtr context = getNode()->getCurveContext();
    RotoDrawableItemWPtr weakItem = item;
    std::weak_ptr<RotoCurveContext> weakContext = context;
    const char* const columnKnobs[] = { kRetouchShapeParamAlgorithm, kRetouchShapeParamScale };
    for (const char* name : columnKnobs) {
        KnobIPtr k = item->getKnobByName(name);
        if (k && k->getSignalSlotHandler() && context) {
            QObject::connect(k->getSignalSlotHandler().get(), &KnobSignalSlotHandler::valueChanged, context.get(),
                             [weakItem, weakContext](ViewSpec, int, int) {
                                 RotoCurveContextPtr c = weakContext.lock();
                                 RotoDrawableItemPtr i = weakItem.lock();
                                 if (c && i) {
                                     c->emitItemPanelColumnsChanged(i);
                                 }
                             });
        }
    }

    _newShapes.push_back(NewShape { item, _ui ? _ui->getSelectedTool() : eRotoToolDrawBezier });
} // Retouch::onCurveItemCreated

RC::ShapeSettings
Retouch::readShapeSettings(const RotoDrawableItemPtr& item,
                           double time)
{
    RC::ShapeSettings s;
    KnobChoicePtr algorithm = itemKnob<KnobChoice>(item, kRetouchShapeParamAlgorithm);
    KnobIntPtr scale = itemKnob<KnobInt>(item, kRetouchShapeParamScale);
    KnobChoicePtr blurType = itemKnob<KnobChoice>(item, kRetouchShapeParamBlurType);
    KnobDoublePtr blurRadius = itemKnob<KnobDouble>(item, kRetouchShapeParamBlurRadius);
    KnobChoicePtr fillMode = itemKnob<KnobChoice>(item, kRetouchShapeParamFillMode);
    KnobColorPtr fillColor = itemKnob<KnobColor>(item, kRetouchShapeParamFillColor);
    KnobDoublePtr fillBrightness = itemKnob<KnobDouble>(item, kRetouchShapeParamFillBrightness);

    if (algorithm) {
        s.algorithm = (RC::AlgorithmEnum)std::max(0, std::min(algorithm->getValueAtTime(time), (int)RC::eAlgorithmFill));
    }
    if (scale) {
        s.scale = scale->getValueAtTime(time);
    }
    if (blurType) {
        s.blurType = blurType->getValueAtTime(time) == (int)RC::eBlurTypeBilateral ? RC::eBlurTypeBilateral : RC::eBlurTypeGaussian;
    }
    if (blurRadius) {
        s.blurRadius = (float)blurRadius->getValueAtTime(time);
    }
    if (fillMode) {
        s.fillMode = fillMode->getValueAtTime(time) == (int)RC::eFillModeColor ? RC::eFillModeColor : RC::eFillModeErase;
    }
    if (fillColor) {
        for (int d = 0; d < 3; ++d) {
            s.fillColor[d] = (float)fillColor->getValueAtTime(time, d);
        }
    }
    if (fillBrightness) {
        s.fillBrightness = (float)fillBrightness->getValueAtTime(time);
    }
    s.opacity = item ? (float)item->getOpacity(time) : 1.f;

    return s;
}

void
Retouch::readShapeSourceOffset(const RotoDrawableItemPtr& item,
                               double time,
                               double* dx,
                               double* dy)
{
    KnobDoublePtr offset = itemKnob<KnobDouble>(item, kRetouchShapeParamSourceOffset);

    *dx = offset ? offset->getValueAtTime(time, 0) : 0.;
    *dy = offset ? offset->getValueAtTime(time, 1) : 0.;
}

void
Retouch::setShapeSourceOffset(const RotoDrawableItemPtr& item,
                              double time,
                              double dx,
                              double dy)
{
    KnobIPtr offset = item ? item->getKnobByName(kRetouchShapeParamSourceOffset) : KnobIPtr();
    if (!offset) {
        return;
    }
    RotoCurveContextPtr context = getNode()->getCurveContext();
    const double values[2] = { dx, dy };
    bool keyframe[2];
    for (int d = 0; d < 2; ++d) {
        keyframe[d] = (context && context->isAutoKeyingEnabled()) || offset->isAnimated(d, ViewSpec::all())
            || (offset->isSlave(d) && offset->getMaster(d).second && offset->getMaster(d).second->isAnimated(d, ViewSpec::all()));
    }
    writeSourceOffset(context, offset, values, time, keyframe);
}

UndoCommandPtr
Retouch::makeSourceDragCommand(const RotoDrawableItemPtr& item,
                               double time,
                               const double* start,
                               const double* end) const
{
    RotoCurveContextPtr context = getNode()->getCurveContext();
    KnobIPtr offset = item ? item->getKnobByName(kRetouchShapeParamSourceOffset) : KnobIPtr();
    if (!context || !offset) {
        return UndoCommandPtr();
    }
    bool keyframe[2];
    for (int d = 0; d < 2; ++d) {
        keyframe[d] = context->isAutoKeyingEnabled() || offset->isAnimated(d, ViewSpec::all())
            || (offset->isSlave(d) && offset->getMaster(d).second && offset->getMaster(d).second->isAnimated(d, ViewSpec::all()));
    }

    return std::make_shared<RetouchSourceDragCommand>(context, item, time, start, end, keyframe);
}

std::string
Retouch::shapeColumnText(int scale,
                         int algorithm,
                         int numScales)
{
    std::stringstream ss;
    if (scale == 0) {
        ss << "Image";
    } else if (scale == numScales + 1) {
        ss << "Residual";
    } else {
        ss << "S" << scale;
        if (!RC::shapeScaleProcessed(scale, numScales)) {
            ss << " (off)";
        }
    }
    ss << " / " << kAlgorithmLabels[std::max(0, std::min(algorithm, (int)RC::eAlgorithmFill))];

    return ss.str();
}

std::vector<RotoCurvePanelHook::Column>
Retouch::getColumns() const
{
    std::vector<Column> columns(1);
    columns[0].header = tr("Scale / Algorithm").toStdString();
    columns[0].toolTip = tr("The scale the shape works on and its algorithm. \"(off)\" marks a scale above Scales + 1, "
                            "which does nothing.")
                             .toStdString();

    return columns;
}

std::string
Retouch::getItemText(const RotoItemPtr& item,
                     int column) const
{
    if ((column != 0) || !isRetouchShape(item)) {
        return std::string();
    }
    const RotoDrawableItemPtr drawable = std::dynamic_pointer_cast<RotoDrawableItem>(item);
    const RC::ShapeSettings s = readShapeSettings(drawable, getCurrentTime());
    KnobIntPtr scales = _scales.lock();

    return shapeColumnText(s.scale, (int)s.algorithm, scales ? scales->getValue() : 0);
}

std::vector<RotoCurvePanelHook::MenuAction>
Retouch::getItemMenuActions(const std::list<RotoItemPtr>& items) const
{
    std::vector<MenuAction> actions;
    bool any = false;
    for (std::list<RotoItemPtr>::const_iterator it = items.begin(); it != items.end(); ++it) {
        any = any || isRetouchShape(*it);
    }
    if (!any) {
        return actions;
    }
    KnobIntPtr currentScale = _currentScale.lock();
    const int scale = currentScale ? currentScale->getValue() : 0;
    actions.resize(2);
    actions[0].id = kRetouchActionCopyToScale;
    actions[0].label = tr("Copy to Scale %1").arg(scale).toStdString();
    actions[1].id = kRetouchActionMoveToScale;
    actions[1].label = tr("Move to Scale %1").arg(scale).toStdString();

    return actions;
}

void
Retouch::onItemMenuActionTriggered(const std::string& actionId,
                                   const std::list<RotoItemPtr>& items)
{
    KnobIntPtr currentScale = _currentScale.lock();
    const int scale = currentScale ? currentScale->getValue() : 0;

    if (actionId == kRetouchActionCopyToScale) {
        copyShapesToScale(items, scale, false);
    } else if (actionId == kRetouchActionMoveToScale) {
        copyShapesToScale(items, scale, true);
    }
}

void
Retouch::copyShapesToScale(const std::list<RotoItemPtr>& items,
                           int scale,
                           bool move)
{
    RotoCurveContextPtr context = getNode()->getCurveContext();
    if (!context) {
        return;
    }
    std::list<RotoDrawableItemPtr> shapes;
    for (std::list<RotoItemPtr>::const_iterator it = items.begin(); it != items.end(); ++it) {
        if (isRetouchShape(*it)) {
            shapes.push_back(std::dynamic_pointer_cast<RotoDrawableItem>(*it));
        }
    }
    if (shapes.empty()) {
        return;
    }
    scale = std::max(0, std::min(scale, RC::kMaxScales + 1));
    pushUndoCommand(new RetouchScaleCommand(context, shapes, scale, move));
}

void
Retouch::getPluginShortcuts(std::list<PluginActionShortcut>* shortcuts)
{
    RotoCurveInteract::getViewerButtonShortcuts(shortcuts);
    RotoCurveInteract::getToolbarShortcuts(eRotoCurveToolSetAll, shortcuts);
    RetouchInteract::getBrushToolbarShortcuts(shortcuts);
    RotoCurveInteract::getRightClickMenuShortcuts(shortcuts);
    shortcuts->push_back(PluginActionShortcut(kRotoUIParamResetCloneOffset, kRotoUIParamResetCloneOffsetLabel));
}

void
Retouch::refreshExtraStateAfterTimeChanged(bool isPlayback,
                                           double time)
{
    StagedImageEffect::refreshExtraStateAfterTimeChanged(isPlayback, time);
    _ui->refreshAfterTimeChanged(time);
}

void
Retouch::onKnobsLoaded()
{
    StagedImageEffect::onKnobsLoaded();
    KnobButtonPtr defaultAction = _ui->selectAllAction.lock();

    _ui->setCurrentTool(defaultAction);
    _ui->onRoleChangedInternal(_ui->selectToolGroup.lock());
    _ui->onToolChangedInternal(defaultAction);
    onShapeSelectionChanged();
}

bool
Retouch::hasSelectedShapes() const
{
    RotoCurveContextPtr context = getNode()->getCurveContext();

    return context && !context->getSelectedCurves().empty();
}

void
Retouch::onShapeSelectionChanged()
{
    const bool selected = hasSelectedShapes();
    for (const std::pair<KnobIWPtr, KnobIWPtr>& pair : _shapeKnobDefaults) {
        KnobIPtr shown = pair.first.lock();
        KnobIPtr next = pair.second.lock();
        if (!shown || !next) {
            continue;
        }
        if (!selected) {
            shown->cloneAndUpdateGui(next.get());
        }
        // Deselecting a shape disables every knob the curve context slaves.
        shown->setAllDimensionsEnabled(true);
    }
    const KnobIPtr shapeOnly[] = { _shapeScaleKnob.lock(), _shapeSourceOffsetKnob.lock() };
    for (const KnobIPtr& k : shapeOnly) {
        if (k) {
            k->setAllDimensionsEnabled(selected);
        }
    }
}

KnobIPtr
Retouch::counterpartOfShapeKnob(const KnobI* knob) const
{
    for (const std::pair<KnobIWPtr, KnobIWPtr>& pair : _shapeKnobDefaults) {
        KnobIPtr shown = pair.first.lock();
        KnobIPtr next = pair.second.lock();
        if (knob && (shown.get() == knob)) {
            return next;
        }
        if (knob && (next.get() == knob)) {
            return shown;
        }
    }

    return KnobIPtr();
}

bool
Retouch::knobChanged(KnobI* k,
                     ValueChangedReasonEnum reason,
                     ViewSpec view,
                     double time,
                     bool originatedFromMainThread)
{
    bool ret = false;

    if (k && _ui->onKnobChanged(k, time, &ret)) {
        return ret;
    }
    KnobIPtr counterpart = counterpartOfShapeKnob(k);
    // Changes to either side reach the other: the shown knob while nothing is selected, the
    // next-shape knob from the brush size gesture.
    if (counterpart && !hasSelectedShapes()) {
        counterpart->cloneAndUpdateGui(k);
    }
    if (k && (k == _ui->resetCloneOffsetButton.lock().get())) {
        _ui->cloneOffset = std::make_pair(0., 0.);
        redrawOverlayInteract();

        return true;
    }
    if (k && ((k == _scales.lock().get()) || (k->getName() == kRetouchShapeParamScale) || (k->getName() == kRetouchShapeParamAlgorithm))) {
        RotoCurveContextPtr context = getNode()->getCurveContext();
        if (context) {
            context->emitItemPanelColumnsChanged(RotoItemPtr());
        }

        return true;
    }

    return StagedImageEffect::knobChanged(k, reason, view, time, originatedFromMainThread);
}

void
Retouch::applyNewShapeSourceOffsets()
{
    const double time = getCurrentTime();
    const RotoStrokeItemPtr painting = _ui->getStrokeBeingPainted();
    for (std::list<NewShape>::iterator it = _newShapes.begin(); it != _newShapes.end();) {
        RotoDrawableItemPtr item = it->item.lock();
        if (!isRetouchShape(item)) {
            it = _newShapes.erase(it);
            continue;
        }
        if (!isShapeFinished(item, painting)) {
            ++it;
            continue;
        }
        KnobDoublePtr offset = itemKnob<KnobDouble>(item, kRetouchShapeParamSourceOffset);
        if (offset && !offset->isAnimated(0, ViewSpec::all()) && !offset->isAnimated(1, ViewSpec::all())
            && (offset->getValueAtTime(time, 0) == 0.) && (offset->getValueAtTime(time, 1) == 0.)) {
            setShapeSourceOffset(item, time, _ui->cloneOffset.first, _ui->cloneOffset.second);
        }
        it = _newShapes.erase(it);
    }
}

bool
Retouch::newShapesTakeSource() const
{
    KnobChoicePtr algorithm = _algorithm.lock();
    const int newAlgorithm = algorithm ? algorithm->getValue() : (int)RC::eAlgorithmHeal;

    return (newAlgorithm == (int)RC::eAlgorithmClone) || (newAlgorithm == (int)RC::eAlgorithmHeal);
}

RotoDrawableItemPtr
Retouch::pickSource(double time,
                    const QPointF& pos) const
{
    RotoCurveContextPtr context = getNode()->getCurveContext();
    if (!context) {
        return RotoDrawableItemPtr();
    }
    const std::list<RotoDrawableItemPtr> curves = context->getCurvesByRenderOrder();
    // The topmost shape first.
    const RotoStrokeItemPtr painting = _ui->getStrokeBeingPainted();
    for (std::list<RotoDrawableItemPtr>::const_reverse_iterator it = curves.rbegin(); it != curves.rend(); ++it) {
        if (!isRetouchShape(*it) || (*it)->isLockedRecursive() || !isShapeFinished(*it, painting)) {
            continue;
        }
        const RC::ShapeSettings s = readShapeSettings(*it, time);
        if ((s.algorithm != RC::eAlgorithmClone) && (s.algorithm != RC::eAlgorithmHeal)) {
            continue;
        }
        double dx, dy;
        readShapeSourceOffset(*it, time, &dx, &dy);
        BezierPtr bezier = std::dynamic_pointer_cast<Bezier>(*it);
        if (!bezier) {
            if (brushSourceContains(*it, time, dx, dy, pos)) {
                return *it;
            }
            continue;
        }
        std::list<ParametricPoint> points;
        bezier->evaluateAtTime_DeCasteljau(true, time, 0, 50, &points, NULL);
        if (pointInPolygon(points, dx, dy, pos)) {
            return *it;
        }
    }

    return RotoDrawableItemPtr();
}

RotoDrawableItemPtr
Retouch::pickBrush(double time,
                   const QPointF& pos) const
{
    RotoCurveContextPtr context = getNode()->getCurveContext();
    if (!context) {
        return RotoDrawableItemPtr();
    }
    const std::list<RotoDrawableItemPtr> curves = context->getCurvesByRenderOrder();
    const RotoStrokeItemPtr painting = _ui->getStrokeBeingPainted();
    for (std::list<RotoDrawableItemPtr>::const_reverse_iterator it = curves.rbegin(); it != curves.rend(); ++it) {
        if (!isRetouchShape(*it) || std::dynamic_pointer_cast<Bezier>(*it) || (*it)->isLockedRecursive()
            || !isShapeFinished(*it, painting)) {
            continue;
        }
        if (brushSourceContains(*it, time, 0., 0., pos)) {
            return *it;
        }
    }

    return RotoDrawableItemPtr();
}

void
Retouch::drawOverlay(double time,
                     const RenderScale& /*renderScale*/,
                     ViewIdx /*view*/)
{
    _ui->drawOverlay(time);

    RotoCurveContextPtr context = getNode()->getCurveContext();
    OverlaySupport* viewport = getCurrentViewportForOverlays();
    if (!context || !viewport) {
        return;
    }
    const double screenPixelRatio = viewport->getScreenPixelRatio();
    const std::list<RotoDrawableItemPtr> curves = context->getCurvesByRenderOrder();
    const std::list<RotoDrawableItemPtr> selected = context->getSelectedCurves();
    const RotoDrawableItemPtr dragged = _draggedSource.lock();

    GLProtectAttrib a(GL_HINT_BIT | GL_ENABLE_BIT | GL_LINE_BIT | GL_COLOR_BUFFER_BIT | GL_CURRENT_BIT);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_LINE_SMOOTH);
    glHint(GL_LINE_SMOOTH_HINT, GL_DONT_CARE);
    const RotoStrokeItemPtr painting = _ui->getStrokeBeingPainted();
    for (std::list<RotoDrawableItemPtr>::const_iterator it = curves.begin(); it != curves.end(); ++it) {
        if (!isRetouchShape(*it) || !(*it)->isGloballyActivated() || !isShapeFinished(*it, painting)) {
            continue;
        }
        const RC::ShapeSettings s = readShapeSettings(*it, time);
        if ((s.algorithm != RC::eAlgorithmClone) && (s.algorithm != RC::eAlgorithmHeal)) {
            continue;
        }
        double dx, dy;
        readShapeSourceOffset(*it, time, &dx, &dy);
        // A Bezier's source is its outline moved; a brush's is its centre line moved.
        std::vector<QPointF> outline;
        bool closed = true;
        BezierPtr bezier = std::dynamic_pointer_cast<Bezier>(*it);
        if (bezier) {
            std::list<ParametricPoint> points;
            bezier->evaluateAtTime_DeCasteljau(true, time, 0, 50, &points, NULL);
            for (std::list<ParametricPoint>::const_iterator p = points.begin(); p != points.end(); ++p) {
                outline.push_back(QPointF(p->x, p->y));
            }
        } else {
            double radius = 0.;
            if (brushCentreLine(*it, time, &outline, &radius)) {
                closed = false;
            }
        }
        if (outline.empty()) {
            continue;
        }
        RectD bbox(outline.front().x(), outline.front().y(), outline.front().x(), outline.front().y());
        for (const QPointF& p : outline) {
            bbox.x1 = std::min(bbox.x1, p.x());
            bbox.x2 = std::max(bbox.x2, p.x());
            bbox.y1 = std::min(bbox.y1, p.y());
            bbox.y2 = std::max(bbox.y2, p.y());
        }
        const bool isSelected = (*it == dragged) || (std::find(selected.begin(), selected.end(), *it) != selected.end());
        double color[4];
        (*it)->getOverlayColor(color);
        color[3] = isSelected ? 1. : 0.5;
        glColor4dv(color);
        glLineWidth((isSelected ? 1.5 : 1.) * screenPixelRatio);
        glEnable(GL_LINE_STIPPLE);
        glLineStipple(2, 0xAAAA);
        glBegin(closed ? GL_LINE_LOOP : GL_LINE_STRIP);
        for (const QPointF& p : outline) {
            glVertex2d(p.x() + dx, p.y() + dy);
        }
        glEnd();
        // From the source's centre to the shape's.
        const double cx = (bbox.x1 + bbox.x2) / 2.;
        const double cy = (bbox.y1 + bbox.y2) / 2.;
        glLineStipple(1, 0xF0F0);
        glBegin(GL_LINES);
        glVertex2d(cx + dx, cy + dy);
        glVertex2d(cx, cy);
        glEnd();
        glDisable(GL_LINE_STIPPLE);
    }

    // Where the next shape's source goes, drawn as RotoPaint's clone brush draws it.
    const RotoToolEnum tool = _ui->getSelectedTool();
    if (isDrawingTool(tool) && !isDrawingPassiveOverlay() && newShapesTakeSource()) {
        QPointF centre;
        bool hasCentre = true;
        if (painting || (_ui->state == eEventStateDraggingCloneOffset)) {
            centre = _ui->lastMousePos;
        } else {
            double cx, cy;
            viewport->getCursorPosition(cx, cy);
            centre = QPointF(cx, cy);
            hasCentre = viewport->getViewportRect().contains(cx, cy);
        }
        const std::pair<double, double> offset = _ui->cloneOffset;
        if (hasCentre && ((offset.first != 0.) || (offset.second != 0.))) {
            double radius;
            KnobDoublePtr brushSize = _brushSize.lock();
            if ((tool == eRotoToolSolidBrush) && brushSize) {
                radius = brushSize->getValue() / 2.;
            } else {
                double psx, psy;
                viewport->getPixelScale(psx, psy);
                radius = kSourceMarkRadiusPixels * psx;
            }
            std::pair<double, double> viewportSize;
            viewport->getViewportSize(viewportSize.first, viewportSize.second);
            _ui->drawCloneSource(centre, offset, radius, 1., screenPixelRatio, viewportSize);
        }
    }
} // Retouch::drawOverlay

bool
Retouch::onOverlayPenDown(double time,
                          const RenderScale& /*renderScale*/,
                          ViewIdx /*view*/,
                          const QPointF& /*viewportPos*/,
                          const QPointF& pos,
                          double pressure,
                          double timestamp,
                          PenType pen)
{
    // Shapes made before this press have had their pen up.
    const RotoStrokeItemPtr painting = _ui->getStrokeBeingPainted();
    for (std::list<NewShape>::iterator it = _newShapes.begin(); it != _newShapes.end();) {
        RotoDrawableItemPtr item = it->item.lock();
        if (!item || isShapeFinished(item, painting)) {
            it = _newShapes.erase(it);
        } else {
            ++it;
        }
    }

    const RotoToolEnum tool = _ui->getSelectedTool();
    // RotoPaint's clone gesture: Ctrl+drag keeps the source where it is and moves the pen away.
    if (isDrawingTool(tool) && !_ui->getBezierBeingBuild() && !_ui->getStrokeBeingPainted() && newShapesTakeSource()
        && _ui->isCloneOffsetDragModifier()) {
        _ui->startCloneOffsetDrag(pos);
        redrawOverlayInteract();

        return true;
    }
    if ((tool == eRotoToolSelectAll) || (tool == eRotoToolSelectCurves) || (tool == eRotoToolSelectPoints)) {
        // A brush stroke has no outline for the curve interact to pick, so a click inside its
        // footprint selects it, unless a Bezier outline is close enough to claim the click.
        RotoCurveContextPtr context = getNode()->getCurveContext();
        bool nearBezier = false;
        if (context) {
            const std::pair<double, double> pixelScale = _ui->getPixelScale();
            int cpIndex;
            double t;
            bool isFeather;
            nearBezier = bool(context->isNearbyBezier(pos.x(), pos.y(), kBezierSelectionTolerance * pixelScale.first, &cpIndex, &t, &isFeather));
        }
        const RotoDrawableItemPtr brush = nearBezier ? RotoDrawableItemPtr() : pickBrush(time, pos);
        RotoDrawableItemPtr item = pickSource(time, pos);
        // Grabbing a brush's source selects the brush too: a Heal or Clone brush's source starts on
        // the brush itself, so the source would otherwise take every click on it.
        if (item && !std::dynamic_pointer_cast<Bezier>(item) && !nearBezier) {
            if (!_ui->shiftDown) {
                _ui->clearCPSSelection();
            }
            _ui->handleItemSelection(item);
        }
        if (item) {
            _draggedSource = item;
            _dragPressPos = pos;
            _dragTime = time;
            readShapeSourceOffset(item, time, &_dragStart[0], &_dragStart[1]);
            _dragEnd[0] = _dragStart[0];
            _dragEnd[1] = _dragStart[1];
            redrawOverlayInteract();

            return true;
        }
        if (brush) {
            if (!_ui->shiftDown) {
                _ui->clearCPSSelection();
            }
            _ui->handleItemSelection(brush);
            _ui->lastMousePos = pos;
            redrawOverlayInteract();

            return true;
        }
    }

    return _ui->onOverlayPenDown(time, pos, pressure, timestamp, pen);
}

bool
Retouch::onOverlayPenMotion(double time,
                            const RenderScale& /*renderScale*/,
                            ViewIdx /*view*/,
                            const QPointF& /*viewportPos*/,
                            const QPointF& pos,
                            double pressure,
                            double timestamp)
{
    if (_ui->state == eEventStateDraggingCloneOffset) {
        _ui->dragCloneOffset(pos.x() - _ui->lastMousePos.x(), pos.y() - _ui->lastMousePos.y());
        _ui->lastMousePos = pos;
        redrawOverlayInteract();

        return true;
    }
    RotoDrawableItemPtr dragged = _draggedSource.lock();
    if (dragged) {
        _dragEnd[0] = _dragStart[0] + (pos.x() - _dragPressPos.x());
        _dragEnd[1] = _dragStart[1] + (pos.y() - _dragPressPos.y());
        setInteractiveDraft(true);
        setShapeSourceOffset(dragged, _dragTime, _dragEnd[0], _dragEnd[1]);
        redrawOverlayInteract();

        return true;
    }

    const bool ret = _ui->onOverlayPenMotion(time, pos, pressure, timestamp);
    // The source follows the cursor.
    if (isDrawingTool(_ui->getSelectedTool())) {
        redrawOverlayInteract();
    }

    return ret;
}

bool
Retouch::onOverlayPenUp(double /*time*/,
                        const RenderScale& /*renderScale*/,
                        ViewIdx /*view*/,
                        const QPointF& /*viewportPos*/,
                        const QPointF& /*pos*/,
                        double /*pressure*/,
                        double /*timestamp*/)
{
    if (_ui->state == eEventStateDraggingCloneOffset) {
        _ui->state = eEventStateNone;
        redrawOverlayInteract();

        return true;
    }
    RotoDrawableItemPtr dragged = _draggedSource.lock();
    if (dragged) {
        _draggedSource.reset();
        const bool drafted = setInteractiveDraft(false);
        UndoCommandPtr command;
        if ((_dragEnd[0] != _dragStart[0]) || (_dragEnd[1] != _dragStart[1])) {
            command = makeSourceDragCommand(dragged, _dragTime, _dragStart, _dragEnd);
        }
        // Pushing the step writes the offset again, which renders the final result.
        if (command) {
            pushUndoCommand(command);
        } else if (drafted) {
            incrHashAndEvaluate(true, false);
        }
        redrawOverlayInteract();

        return true;
    }
    const bool ret = _ui->onOverlayPenUp();
    applyNewShapeSourceOffsets();

    return ret;
}

bool
Retouch::onOverlayPenDoubleClicked(double /*time*/,
                                   const RenderScale& /*renderScale*/,
                                   ViewIdx /*view*/,
                                   const QPointF& /*viewportPos*/,
                                   const QPointF& pos)
{
    return _ui->onOverlayPenDoubleClicked(pos);
}

bool
Retouch::onOverlayKeyDown(double /*time*/,
                          const RenderScale& /*renderScale*/,
                          ViewIdx /*view*/,
                          Key key,
                          KeyboardModifiers /*modifiers*/)
{
    const bool ret = _ui->onOverlayKeyDown(key);
    // Return closes an open Bezier.
    applyNewShapeSourceOffsets();

    return ret;
}

bool
Retouch::onOverlayKeyUp(double /*time*/,
                        const RenderScale& /*renderScale*/,
                        ViewIdx /*view*/,
                        Key key,
                        KeyboardModifiers /*modifiers*/)
{
    return _ui->onOverlayKeyUp(key);
}

bool
Retouch::onOverlayFocusLost(double /*time*/,
                            const RenderScale& /*renderScale*/,
                            ViewIdx /*view*/)
{
    _draggedSource.reset();
    if (setInteractiveDraft(false)) {
        incrHashAndEvaluate(true, false);
    }

    return _ui->onOverlayFocusLost();
}

void
Retouch::onInteractViewportSelectionCleared()
{
    _ui->onViewportSelectionCleared();
}

void
Retouch::onInteractViewportSelectionUpdated(const RectD& rectangle,
                                            bool onRelease)
{
    _ui->onViewportSelectionUpdated(rectangle, onRelease);
}

RC::Params
Retouch::readParams(double time,
                    ViewIdx view,
                    float renderScale) const
{
    const KnobReader reader(time, view);
    RC::Params p;

    p.numScales = std::max(0, std::min((int)reader.value(_scales), RC::kMaxScales));
    p.mergeFromScale = std::max(0, std::min((int)reader.value(_mergeFrom), RC::kMaxScales));
    const int output = reader.value(_output);
    const RC::OutputEnum outputMode = (output == (int)RC::eOutputCurrentScale) ? RC::eOutputCurrentScale
        : (output == (int)RC::eOutputResidual)                                 ? RC::eOutputResidual
                                                                               : RC::eOutputResult;
    p.returnLayer = RC::returnLayerForOutput(outputMode, p.numScales, reader.value(_currentScale));
    for (int d = 0; d < 3; ++d) {
        p.previewLevels[d] = (float)reader.value(_previewLevels, d);
    }
    p.maxHealIterations = std::max(1, (int)reader.value(_maxHealIterations));
    p.renderScale = renderScale;

    return p;
}

bool
Retouch::framePixels(double time,
                     ViewIdx view,
                     const RenderScale& scale,
                     RectI* pixels) const
{
    return wholeFramePixels(*this, getSourceInput(), time, view, scale, pixels) && !pixels->isNull();
}

std::vector<std::vector<RC::BrushPoint>>
Retouch::readBrushStrokes(const RotoDrawableItemPtr& item,
                          double time,
                          double scale,
                          const RectI& full)
{
    std::vector<std::vector<RC::BrushPoint>> result;
    RotoStrokeItemPtr stroke = std::dynamic_pointer_cast<RotoStrokeItem>(item);
    KnobDoublePtr sizeKnob = itemKnob<KnobDouble>(item, kRetouchShapeParamBrushSize);
    if (!stroke || !sizeKnob) {
        return result;
    }
    KnobDoublePtr hardnessKnob = itemKnob<KnobDouble>(item, kRetouchShapeParamBrushHardness);
    KnobDoublePtr densityKnob = itemKnob<KnobDouble>(item, kRetouchShapeParamBrushDensity);
    KnobChoicePtr pressureKnob = itemKnob<KnobChoice>(item, kRetouchShapeParamBrushPressure);
    const float radius = (float)(std::max(0., sizeKnob->getValueAtTime(time)) / 2. * scale);
    const float hardness = (float)std::max((double)RC::kBrushHardnessMin, std::min(1., hardnessKnob ? hardnessKnob->getValueAtTime(time) : 1.));
    const float density = (float)std::max(0., std::min(1., densityKnob ? densityKnob->getValueAtTime(time) : 1.));
    const RC::BrushPressureEnum pressureMode = pressureKnob ? (RC::BrushPressureEnum)pressureKnob->getValue() : RC::eBrushPressureOff;
    const float minRadius = RC::kBrushBorderMin * (float)std::min(full.width(), full.height());

    std::list<std::list<std::pair<Point, double>>> strokes;
    stroke->evaluateStroke(0, time, &strokes);
    for (std::list<std::list<std::pair<Point, double>>>::const_iterator it = strokes.begin(); it != strokes.end(); ++it) {
        std::vector<RC::BrushPoint> points;
        points.reserve(it->size());
        for (std::list<std::pair<Point, double>>::const_iterator p = it->begin(); p != it->end(); ++p) {
            RC::BrushPoint b;
            b.x = (float)(p->first.x * scale - full.x1);
            b.y = (float)(full.y2 - p->first.y * scale);
            b.radius = radius;
            b.hardness = hardness;
            b.density = density;
            RC::applyBrushPressure(pressureMode, (float)std::max(0., std::min(1., p->second)), minRadius, &b);
            points.push_back(b);
        }
        if (!points.empty()) {
            result.push_back(points);
        }
    }

    return result;
} // Retouch::readBrushStrokes

bool
Retouch::rasterizeBrushShape(const RotoDrawableItemPtr& item,
                             double time,
                             double scale,
                             const RectI& full,
                             RC::Region* roi,
                             std::vector<float>* mask,
                             const EdgeAware::ParallelFor& parallel,
                             RC::PatchCache* masks)
{
    const std::vector<std::vector<RC::BrushPoint>> strokes = readBrushStrokes(item, time, scale, full);
    std::vector<RC::BrushPoint> all;
    RC::KeyHash key;
    for (const std::vector<RC::BrushPoint>& points : strokes) {
        all.insert(all.end(), points.begin(), points.end());
        key.add(points.size());
        key.addBytes(points.data(), points.size() * sizeof(RC::BrushPoint));
    }
    *roi = RC::brushBounds(all);
    if ((roi->width <= 0) || (roi->height <= 0)) {
        return false;
    }
    key.addRegion(*roi);
    std::unique_ptr<RC::CacheClaim> claim;
    if (masks) {
        const std::shared_ptr<const RC::PatchCache::Patch> hit = masks->acquire(key.value());
        if (hit) {
            *mask = hit->values;

            return true;
        }
        claim.reset(new RC::CacheClaim(masks, key.value()));
    }
    mask->assign((std::size_t)roi->width * (std::size_t)roi->height, 0.f);
    for (const std::vector<RC::BrushPoint>& points : strokes) {
        RC::accumulateBrush(points, *roi, mask->data(), parallel);
    }
    if (claim) {
        std::shared_ptr<RC::PatchCache::Patch> patch = std::make_shared<RC::PatchCache::Patch>();
        patch->area = *roi;
        patch->values = *mask;
        claim->publish(patch);
    }

    return true;
}

std::vector<RC::Shape>
Retouch::collectShapes(double time,
                       const RenderScale& scale,
                       const RectI& full,
                       bool rasterize,
                       bool draft) const
{
    std::vector<RC::Shape> shapes;
    RotoCurveContextPtr context = getNode()->getCurveContext();
    if (!context) {
        return shapes;
    }
    const double s = scale.toOfxPointD().x;
    const unsigned int mipmapLevel = scale.toMipmapLevel();
    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    // Not cancellable: a mask left half drawn would be kept in the mask cache.
    const EdgeAware::ParallelFor parallel = [nThreads](int count, const std::function<void(int)>& body) {
        parallelForOnGlobalPool(count, nThreads, body);
    };
    const std::list<RotoDrawableItemPtr> curves = context->getCurvesByRenderOrder(true);
    for (std::list<RotoDrawableItemPtr>::const_iterator it = curves.begin(); it != curves.end(); ++it) {
        if (!isRetouchShape(*it) || !(*it)->isActivated(time)) {
            continue;
        }
        RC::Shape shape;
        BezierPtr bezier = std::dynamic_pointer_cast<Bezier>(*it);
        if (bezier) {
            const RectD bbox = bezier->getBoundingBox(time);
            if (bbox.isNull()) {
                continue;
            }
            RectI pixels;
            pixels.x1 = (int)std::floor(bbox.x1 * s);
            pixels.y1 = (int)std::floor(bbox.y1 * s);
            pixels.x2 = (int)std::ceil(bbox.x2 * s);
            pixels.y2 = (int)std::ceil(bbox.y2 * s);
            shape.maskRoi.x = pixels.x1 - full.x1;
            shape.maskRoi.y = full.y2 - pixels.y2;
            shape.maskRoi.width = pixels.width();
            shape.maskRoi.height = pixels.height();
            if (rasterize) {
                RC::KeyHash key;
                key.add(bezier->getContentHash());
                key.addBytes(&time, sizeof(time));
                key.add(mipmapLevel);
                key.addRegion(shape.maskRoi);
                const std::shared_ptr<const RC::PatchCache::Patch> hit = _shapeMasks.acquire(key.value());
                if (hit) {
                    shape.mask = hit->values;
                } else {
                    RC::CacheClaim claim(&_shapeMasks, key.value());
                    std::vector<float> coverage;
                    if (!bezier->rasterizeCoverage(time, mipmapLevel, pixels, &coverage)) {
                        continue;
                    }
                    // Coverage rows go up from pixels.y1; the shape's mask rows go down.
                    shape.mask.resize(coverage.size());
                    const std::size_t w = (std::size_t)shape.maskRoi.width;
                    for (int r = 0; r < shape.maskRoi.height; ++r) {
                        std::copy(&coverage[(std::size_t)(shape.maskRoi.height - 1 - r) * w], &coverage[(std::size_t)(shape.maskRoi.height - 1 - r) * w] + w,
                                  &shape.mask[(std::size_t)r * w]);
                    }
                    std::shared_ptr<RC::PatchCache::Patch> patch = std::make_shared<RC::PatchCache::Patch>();
                    patch->area = shape.maskRoi;
                    patch->values = shape.mask;
                    claim.publish(patch);
                }
            }
        } else if (rasterize) {
            if (!rasterizeBrushShape(*it, time, s, full, &shape.maskRoi, &shape.mask, parallel, &_shapeMasks)) {
                continue;
            }
        } else {
            std::vector<RC::BrushPoint> all;
            for (const std::vector<RC::BrushPoint>& points : readBrushStrokes(*it, time, s, full)) {
                all.insert(all.end(), points.begin(), points.end());
            }
            shape.maskRoi = RC::brushBounds(all);
            if ((shape.maskRoi.width <= 0) || (shape.maskRoi.height <= 0)) {
                continue;
            }
        }
        shape.settings = readShapeSettings(*it, time);
        if (draft && (shape.settings.algorithm == RC::eAlgorithmHeal)) {
            std::lock_guard<std::mutex> lock(_warmStartsMutex);
            std::shared_ptr<RC::HealWarmStart>& warm = _warmStarts[(*it)->getScriptName()];
            if (!warm) {
                warm = std::make_shared<RC::HealWarmStart>();
            }
            shape.warmStart = warm;
        }
        double offsetX, offsetY;
        readShapeSourceOffset(*it, time, &offsetX, &offsetY);
        // From the source to the shape, with y down.
        shape.dx = (float)(-offsetX * s);
        shape.dy = (float)(offsetY * s);
        shapes.push_back(shape);
    }

    return shapes;
} // Retouch::collectShapes

void
Retouch::getRegionsOfInterest(double time,
                              const RenderScale& scale,
                              const RectD& outputRoD,
                              const RectD& renderWindow,
                              ViewIdx view,
                              RoIMap* ret)
{
    StagedImageEffect::getRegionsOfInterest(time, scale, outputRoD, renderWindow, view, ret);

    const int sourceInput = getSourceInput();
    EffectInstancePtr input = getInput(sourceInput);
    if (!input) {
        return;
    }
    const NodeMetadataConstPtr metadata = getRenderMetadata(time, view);
    const double par = metadata ? metadata->getPixelAspectRatio(sourceInput) : 1.;
    const RectI window = PassExec::toPixelEnclosing(renderWindow, scale, par);
    RectI full, crop;
    RC::Params params;
    std::vector<RC::Shape> shapes;
    if (!processPlan(time, view, scale, window, false, false, &full, &crop, &params, &shapes) || crop.isNull()) {
        return;
    }
    const RectD needed = PassExec::toCanonical(crop, scale, par);
    RoIMap::iterator found = ret->find(input);
    if (found == ret->end()) {
        (*ret)[input] = needed;
    } else {
        found->second.merge(needed);
    }
}

bool
Retouch::processPlan(double time,
                     ViewIdx view,
                     const RenderScale& scale,
                     const RectI& window,
                     bool rasterize,
                     bool draft,
                     RectI* full,
                     RectI* crop,
                     RC::Params* params,
                     std::vector<RC::Shape>* shapes) const
{
    if (!framePixels(time, view, scale, full)) {
        return false;
    }
    *params = readParams(time, view, (float)scale.toOfxPointD().x);
    *shapes = collectShapes(time, scale, *full, rasterize, draft);
    RC::Region w;
    w.x = window.x1 - full->x1;
    w.y = full->y2 - window.y2;
    w.width = window.width();
    w.height = window.height();
    const RC::Region c = RC::processRegion(w, *shapes, *params, full->width(), full->height());
    if ((c.width <= 0) || (c.height <= 0)) {
        *crop = RectI();
    } else {
        crop->x1 = full->x1 + c.x;
        crop->x2 = crop->x1 + c.width;
        crop->y2 = full->y2 - c.y;
        crop->y1 = crop->y2 - c.height;
    }
    params->originX = c.x;
    params->originY = c.y;
    params->fullWidth = full->width();
    params->fullHeight = full->height();
    if (draft) {
        params->draftHealCycles = kDraftHealCycles;
    }

    return true;
} // Retouch::processPlan

bool
Retouch::isOpIdentity(double time,
                      const RenderScale& /*scale*/,
                      const RectI& /*roi*/,
                      ViewIdx view)
{
    const RC::Params p = readParams(time, view, 1.f);
    if (p.returnLayer != 0) {
        return false;
    }
    RotoCurveContextPtr context = getNode()->getCurveContext();
    if (!context) {
        return true;
    }
    const std::list<RotoDrawableItemPtr> curves = context->getCurvesByRenderOrder(true);
    for (std::list<RotoDrawableItemPtr>::const_iterator it = curves.begin(); it != curves.end(); ++it) {
        if (isRetouchShape(*it) && (*it)->isActivated(time) && RC::shapeScaleProcessed(readShapeSettings(*it, time).scale, p.numScales)) {
            return false;
        }
    }

    return true;
}

StatusEnum
Retouch::renderOp(const RenderActionArgs& args,
                  const StagePlan& plan)
{
    const int sourceInput = getSourceInput();
    RectI full, crop;
    RC::Params params;
    std::vector<RC::Shape> shapes;
    // Read once, after the render's hash snapshot: setInteractiveDraft() brackets its flip as a
    // content write, so a frame whose flag differs from its hash is never cached under it.
    const bool draft = _draft.load();
    if (!processPlan(args.time, args.view, args.mappedScale, args.roi, true, draft, &full, &crop, &params, &shapes)) {
        return eStatusFailed;
    }

    ImagePtr src;
    ImageLayerDesc sourceLayer;
    if (resolveInputPlaneForRender(sourceInput, args.time, args.view, &sourceLayer, NULL)) {
        RectI sourceRoI;
        src = getImage(sourceInput, args.time, args.mappedScale, args.view, NULL, &sourceLayer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &sourceRoI);
    }
    if (src && !PassExec::isFloatImage(src)) {
        return eStatusFailed;
    }
    std::shared_ptr<Image::ReadAccess> srcAccess;
    RectI srcBounds;
    int srcNComps = 0;
    if (src) {
        srcAccess = std::make_shared<Image::ReadAccess>(src.get());
        srcBounds = src->getBounds();
        srcNComps = (int)src->getComponentsCount();
    }

    RenderCancellation cancel(this);
    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    const EdgeAware::ParallelFor parallel = [&cancel, nThreads](int count, const std::function<void(int)>& body) {
        parallelForCancellable(count, nThreads, cancel, body);
    };
    const int width = crop.width();
    const int height = crop.height();
    params.cache = &_shapeResults;
    params.cancelled = [&cancel]() {
        return cancel.check();
    };

    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        if (!it->second) {
            continue;
        }
        const int nComps = (int)it->second->getComponentsCount();

        // The source image names the crop's pixels only where it covers the crop.
        params.inputKey = 0;
        if (src && srcBounds.contains(crop) && (src->getKey().getTreeVersion() != 0)) {
            RC::KeyHash key;
            key.add(src->getKey().getHash());
            key.add(src->getMipmapLevel());
            key.addBytes(sourceLayer.getLayerID().data(), sourceLayer.getLayerID().size());
            key.add((std::uint64_t)nComps);
            key.add((std::uint64_t)srcNComps);
            key.add((std::uint64_t)(std::uint32_t)full.x1 | ((std::uint64_t)(std::uint32_t)full.y1 << 32));
            key.add((std::uint64_t)(std::uint32_t)full.x2 | ((std::uint64_t)(std::uint32_t)full.y2 << 32));
            params.inputKey = std::max<std::uint64_t>(key.value(), 1);
        }

        // The crop of the source, top-down, as darktable's 4 channels: one channel goes to all three colours.
        std::vector<float> image((std::size_t)width * height * 4, 0.f);
        parallel(height, [&](int r) {
            const int y = crop.y2 - 1 - r;
            std::vector<float> row((std::size_t)width * nComps);
            PassExec::readSourceRow(src.get(), srcAccess.get(), srcBounds, srcNComps, crop.x1, y, width, nComps, row.data());
            float* dst = &image[(std::size_t)r * width * 4];
            for (int x = 0; x < width; ++x) {
                const float* p = &row[(std::size_t)x * nComps];
                if (nComps == 1) {
                    dst[4 * x] = dst[4 * x + 1] = dst[4 * x + 2] = p[0];
                } else {
                    for (int c = 0; c < std::min(nComps, 4); ++c) {
                        dst[4 * x + c] = p[c];
                    }
                }
            }
        });
        if (cancel.check()) {
            return eStatusFailed;
        }
        RC::process(image.data(), width, height, params, shapes, parallel);
        if (cancel.check()) {
            return eStatusFailed;
        }

        const auto row = [&](const RowIO& io) {
            const float zero[4] = { 0.f, 0.f, 0.f, 0.f };
            const bool insideRow = (io.y >= crop.y1) && (io.y < crop.y2);
            const std::size_t rowStart = insideRow ? (std::size_t)(crop.y2 - 1 - io.y) * width : 0;
            for (int i = 0; i < io.width; ++i) {
                const int x = io.x0 + i;
                const float* in = io.src[0] ? &io.src[0][(std::size_t)i * io.nComps] : zero;
                float out[4] = { 0.f, 0.f, 0.f, 0.f };
                if (insideRow && (x >= crop.x1) && (x < crop.x2)) {
                    const float* p = &image[4 * (rowStart + (x - crop.x1))];
                    std::copy(p, p + std::min(io.nComps, 4), out);
                } else {
                    std::copy(in, in + io.nComps, out);
                }
                for (int c = 0; c < io.nComps; ++c) {
                    if (io.channels[pixelKernelChannelBit(io.nComps, c)]) {
                        io.dst[(std::size_t)i * io.nComps + c] = out[c];
                    }
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

    return cancel.check() ? eStatusFailed : eStatusOK;
} // Retouch::renderOp

#ifdef NATRON_HAVE_GPU
namespace {
// Mirrors RtParams in Engine/Gpu/Kernels/retouch.slang.
struct RetouchGpuParams {
    uint32_t width;
    uint32_t height;
    int32_t fullX1;
    int32_t fullY2;
    uint32_t srcOffset;
    uint32_t srcStride;
    uint32_t srcComps;
    int32_t srcX1;
    int32_t srcY1;
    int32_t srcX2;
    int32_t srcY2;
    int32_t srcIndex0;
    int32_t srcIndex1;
    int32_t srcIndex2;
    int32_t srcIndex3;
    uint32_t nComps;
    int32_t outX1;
    int32_t outY1;
    uint32_t outWidth;
    uint32_t outHeight;
    int32_t maskX;
    int32_t maskY;
    uint32_t maskW;
    uint32_t maskH;
    int32_t dx;
    int32_t dy;
    float opacity;
    uint32_t fill;
    float fill0;
    float fill1;
    float fill2;
    uint32_t padding0;
};
static_assert(sizeof(RetouchGpuParams) == 128, "RetouchGpuParams must match the push-constant block");
} // anonymous namespace
#endif

bool
Retouch::gpuPlan(double time,
                 ViewIdx view,
                 const RenderScale& scale,
                 RectI* full,
                 RC::Params* params,
                 int* scales) const
{
    if (!framePixels(time, view, scale, full)) {
        return false;
    }
    *params = readParams(time, view, (float)scale.toOfxPointD().x);
    // The GPU renders the result without merged scales; the other outputs go to the CPU.
    if (params->returnLayer != 0) {
        return false;
    }
    EdgeAware::DwtParams dwt;
    dwt.width = full->width();
    dwt.height = full->height();
    dwt.scales = std::max(0, std::min(params->numScales, RC::kMaxScales));
    dwt.previewScale = params->renderScale > 0.f ? params->renderScale : 1.f;
    *scales = std::min(dwt.scales, EdgeAware::dwtMaxScale(dwt));
    if ((params->mergeFromScale > 0) && (params->mergeFromScale <= *scales)) {
        return false;
    }

    return true;
}

bool
Retouch::canRenderOpOnGpu(const GpuQuery& query) const
{
#ifdef NATRON_HAVE_GPU
    if ((query.bitDepth != eImageBitDepthFloat) || (query.nComps < 1) || (query.nComps > 4)) {
        return false;
    }
    RectI full;
    RC::Params params;
    int scales = 0;
    if (!gpuPlan(query.time, query.view, query.mappedScale, &full, &params, &scales)) {
        return false;
    }
    RotoCurveContextPtr context = getNode()->getCurveContext();
    if (context) {
        const std::list<RotoDrawableItemPtr> curves = context->getCurvesByRenderOrder(true);
        for (std::list<RotoDrawableItemPtr>::const_iterator it = curves.begin(); it != curves.end(); ++it) {
            if (!isRetouchShape(*it) || !(*it)->isActivated(query.time)) {
                continue;
            }
            const RC::ShapeSettings s = readShapeSettings(*it, query.time);
            if (!RC::shapeScaleProcessed(s.scale, params.numScales)) {
                continue;
            }
            // The bilateral grid has no GPU port.
            if ((s.algorithm == RC::eAlgorithmBlur) && (s.blurType == RC::eBlurTypeBilateral) && (std::fabs(s.blurRadius) > 0.1f)) {
                return false;
            }
        }
    }

    return canRunStagesGpu(resolveGpuPlan(query), query);
#else
    Q_UNUSED(query);

    return false;
#endif
}

bool
Retouch::getOpGpuInputLayer(int inputNb,
                            const ImageLayerDesc& plane,
                            double time,
                            ViewIdx view,
                            ImageLayerDesc* layer) const
{
    if (inputNb != getSourceInput()) {
        stageGpuInputLayer(*this, inputNb, plane, time, view, layer);

        return true;
    }

    return const_cast<Retouch*>(this)->resolveInputPlaneForRender(inputNb, time, view, layer, NULL);
}

RectI
Retouch::getOpGpuInputRoI(int inputNb,
                          double time,
                          ViewIdx view,
                          const RectI& outputRect,
                          const RenderScale& scale) const
{
    if (inputNb != getSourceInput()) {
        return outputRect;
    }
    RectI full, crop;
    RC::Params params;
    std::vector<RC::Shape> shapes;
    if (!processPlan(time, view, scale, outputRect, false, false, &full, &crop, &params, &shapes) || crop.isNull()) {
        return outputRect;
    }
    // retouch_store copies the source over the whole output rect, not only over the crop.
    RectI needed = crop;
    needed.merge(outputRect);

    return needed;
}

StatusEnum
Retouch::renderOpGpu(const GpuRenderArgs& args)
{
#ifdef NATRON_HAVE_GPU
    const GpuQuery& query = args.query;
    const RectI& rect = args.outputRect;
    const int nComps = query.nComps;
    if (!canRenderOpOnGpu(query) || rect.isNull() || !PassExec::isUsableGpuView(args.output, nComps) || !args.output.bounds.contains(rect)) {
        return eStatusFailed;
    }
    RectI full;
    RC::Params params;
    int scales = 0;
    if (!gpuPlan(query.time, query.view, query.mappedScale, &full, &params, &scales)) {
        return eStatusFailed;
    }
    const int sourceInput = getSourceInput();
    GpuImageView source;
    if (!stageGpuInputView(*this, args, sourceInput, &source) || (source.buffer && ((source.byteOffset % sizeof(float)) != 0))) {
        return eStatusFailed;
    }
    const StagePlan plan = resolveGpuPlan(query);
    // The GPU heal solves in full, so a draft renders as the final frame does.
    RectI crop;
    std::vector<RC::Shape> shapes;
    if (!processPlan(query.time, query.view, query.mappedScale, rect, true, false, &full, &crop, &params, &shapes)) {
        return eStatusFailed;
    }

    // Only the crop is processed; the rest of the rect, outside the source, stores the source.
    const int width = crop.isNull() ? 0 : crop.width();
    const int height = crop.isNull() ? 0 : crop.height();
    const uint64_t imageFloats = (uint64_t)4 * (uint64_t)width * (uint64_t)height;
    const uint64_t filteredFloats = (uint64_t)rect.area() * (uint64_t)nComps;
    const uint64_t srcEnd = source.buffer ? (source.byteOffset / sizeof(float) + (uint64_t)source.bounds.height() * (uint64_t)source.rowStrideFloats) : 0;
    if ((imageFloats > UINT32_MAX) || (filteredFloats > UINT32_MAX) || (srcEnd > UINT32_MAX)) {
        return eStatusFailed;
    }

    gpu::GpuRecordContext& ctx = args.context;
    gpu::GpuBuffer* image = nullptr;
    gpu::GpuBuffer* filteredBuffer = nullptr;
    gpu::GpuBuffer* standIn = nullptr;
    if (!ctx.scratch(std::max<uint64_t>(imageFloats, 4) * sizeof(float), image) || !image || !ctx.scratch(filteredFloats * sizeof(float), filteredBuffer) || !filteredBuffer
        || !ctx.scratch(4 * sizeof(float), standIn) || !standIn) {
        return eStatusFailed;
    }
    const gpu::GpuBuffer* srcBuffer = source.buffer ? source.buffer : standIn;

    RetouchGpuParams p = {};
    p.width = (uint32_t)width;
    p.height = (uint32_t)height;
    p.fullX1 = crop.x1;
    p.fullY2 = crop.y2;
    if (source.buffer) {
        p.srcOffset = (uint32_t)(source.byteOffset / sizeof(float));
        p.srcStride = (uint32_t)source.rowStrideFloats;
        p.srcComps = (uint32_t)source.nComps;
        p.srcX1 = source.bounds.x1;
        p.srcY1 = source.bounds.y1;
        p.srcX2 = source.bounds.x2;
        p.srcY2 = source.bounds.y2;
    }
    int srcIndex[4] = { -1, -1, -1, -1 };
    for (int c = 0; c < nComps; ++c) {
        srcIndex[c] = source.buffer ? PassExec::channelIndexForBit(source.nComps, pixelKernelChannelBit(nComps, c)) : -1;
    }
    p.srcIndex0 = srcIndex[0];
    p.srcIndex1 = srcIndex[1];
    p.srcIndex2 = srcIndex[2];
    p.srcIndex3 = srcIndex[3];
    p.nComps = (uint32_t)nComps;
    p.outX1 = rect.x1;
    p.outY1 = rect.y1;
    p.outWidth = (uint32_t)rect.width();
    p.outHeight = (uint32_t)rect.height();

    const auto run = [&](gpu::GpuKernelId kernel, const RetouchGpuParams& params, const gpu::GpuBuffer* layer, const gpu::GpuBuffer* aux,
                         const gpu::GpuBuffer* mask, uint32_t x, uint32_t y) {
        if (!ctx.dispatch(kernel, { srcBuffer, layer, aux ? aux : standIn, mask ? mask : standIn, filteredBuffer }, params,
                          { std::max<uint32_t>(x, 1), std::max<uint32_t>(y, 1), 1 })) {
            return false;
        }
        ctx.computeBarrier();

        return true;
    };

    // processLayer() of the shapes on one scale, on a layer of the region's size.
    const auto processLayer = [&](const gpu::GpuBuffer* layer, int scale) {
        RC::Region layerRoi;
        layerRoi.x = params.originX;
        layerRoi.y = params.originY;
        layerRoi.width = width;
        layerRoi.height = height;
        std::vector<float> mask;
        for (const RC::Shape& shape : shapes) {
            const RC::ShapeSettings& s = shape.settings;
            if (s.scale != scale) {
                continue;
            }
            const bool shifts = (s.algorithm == RC::eAlgorithmClone) || (s.algorithm == RC::eAlgorithmHeal);
            const int dx = shifts ? (int)shape.dx : 0;
            const int dy = shifts ? (int)shape.dy : 0;
            RC::Region maskRoi;
            if (!RC::buildRegionMask(shape, layerRoi, dx, dy, &mask, &maskRoi)) {
                continue;
            }
            if (!(dx != 0 || dy != 0 || !shifts) || (maskRoi.width <= 2) || (maskRoi.height <= 2)) {
                continue;
            }
            if ((s.algorithm == RC::eAlgorithmBlur) && (std::fabs(s.blurRadius) <= 0.1f)) {
                continue;
            }
            const uint64_t maskPixels = (uint64_t)maskRoi.width * (uint64_t)maskRoi.height;
            gpu::GpuBuffer* maskBuffer = nullptr;
            if (!ctx.scratch(maskPixels * sizeof(float), maskBuffer) || !maskBuffer) {
                return false;
            }
            const std::span<const std::byte> bytes = std::as_bytes(std::span<const float>(mask.data(), mask.size()));
            const std::size_t chunk = 65536;
            for (std::size_t offset = 0; offset < bytes.size(); offset += chunk) {
                if (!ctx.updateBuffer(*maskBuffer, offset, bytes.subspan(offset, std::min(chunk, bytes.size() - offset)))) {
                    return false;
                }
            }

            RetouchGpuParams sp = p;
            sp.maskX = maskRoi.x - layerRoi.x;
            sp.maskY = maskRoi.y - layerRoi.y;
            sp.maskW = (uint32_t)maskRoi.width;
            sp.maskH = (uint32_t)maskRoi.height;
            sp.opacity = s.opacity;
            gpu::GpuBuffer* aux = nullptr;
            gpu::GpuBuffer* aux2 = nullptr;
            if (s.algorithm != RC::eAlgorithmFill) {
                if (!ctx.scratch(4 * maskPixels * sizeof(float), aux) || !aux || !ctx.scratch(4 * maskPixels * sizeof(float), aux2) || !aux2) {
                    return false;
                }
            }
            switch (s.algorithm) {
            case RC::eAlgorithmClone:
                sp.dx = dx;
                sp.dy = dy;
                if (!run(gpu::GpuKernelId::retouch_crop, sp, layer, aux, maskBuffer, sp.maskW, sp.maskH)) {
                    return false;
                }
                break;
            case RC::eAlgorithmHeal: {
                sp.dx = dx;
                sp.dy = dy;
                if (!run(gpu::GpuKernelId::retouch_crop, sp, layer, aux2, maskBuffer, sp.maskW, sp.maskH)) {
                    return false;
                }
                sp.dx = 0;
                sp.dy = 0;
                if (!run(gpu::GpuKernelId::retouch_crop, sp, layer, aux, maskBuffer, sp.maskW, sp.maskH)) {
                    return false;
                }
                if (!EdgeAware::recordPoissonHeal(ctx, *aux2, *aux, *maskBuffer, mask.data(), maskRoi.width, maskRoi.height, params.maxHealIterations)) {
                    return false;
                }
                break;
            }
            case RC::eAlgorithmBlur: {
                if (!run(gpu::GpuKernelId::retouch_crop, sp, layer, aux2, maskBuffer, sp.maskW, sp.maskH)) {
                    return false;
                }
                const float maxValue[4] = { FLT_MAX, FLT_MAX, FLT_MAX, FLT_MAX };
                const float minValue[4] = { -FLT_MAX, -FLT_MAX, -FLT_MAX, -FLT_MAX };
                const float sigma = EdgeAware::clampGaussianSigma(s.blurRadius * params.renderScale, maskRoi.width, maskRoi.height);
                if (!EdgeAware::recordRecursiveGaussianBlur(ctx, *aux2, *aux, maskRoi.width, maskRoi.height, 4, sigma, minValue, maxValue)) {
                    return false;
                }
                break;
            }
            case RC::eAlgorithmFill: {
                sp.fill = 1;
                float fill[3];
                for (int c = 0; c < 3; ++c) {
                    fill[c] = (s.fillMode == RC::eFillModeErase) ? s.fillBrightness : s.fillColor[c] + s.fillBrightness;
                }
                sp.fill0 = fill[0];
                sp.fill1 = fill[1];
                sp.fill2 = fill[2];
                break;
            }
            }
            if (!run(gpu::GpuKernelId::retouch_blend, sp, layer, aux, maskBuffer, sp.maskW, sp.maskH)) {
                return false;
            }
        }

        return true;
    };

    if ((width > 0) && (!run(gpu::GpuKernelId::retouch_load, p, image, nullptr, nullptr, p.width, p.height) || !processLayer(image, 0))) {
        return eStatusFailed;
    }
    if ((width > 0) && (scales > 0)) {
        std::vector<gpu::GpuBuffer*> details((std::size_t)scales, nullptr);
        for (gpu::GpuBuffer*& band : details) {
            if (!ctx.scratch(imageFloats * sizeof(float), band) || !band) {
                return eStatusFailed;
            }
        }
        gpu::GpuBuffer* residual = nullptr;
        if (!ctx.scratch(imageFloats * sizeof(float), residual) || !residual) {
            return eStatusFailed;
        }
        const std::vector<const gpu::GpuBuffer*> bandViews(details.begin(), details.end());
        int bands = 0;
        if (!EdgeAware::recordDwtDecomposeScales(ctx, *image, width, height, scales, std::span<const gpu::GpuBuffer* const>(bandViews.data(), bandViews.size()), *residual, bands,
                                                 full.width(), full.height())
            || (bands != scales)) {
            return eStatusFailed;
        }
        for (int k = 1; k <= scales; ++k) {
            if (!processLayer(details[(std::size_t)k - 1], k)) {
                return eStatusFailed;
            }
        }
        if (!processLayer(residual, params.numScales + 1)) {
            return eStatusFailed;
        }
        if (!EdgeAware::recordWaveletRecompose(ctx, std::span<const gpu::GpuBuffer* const>(bandViews.data(), bandViews.size()), *residual, width, height, *image)) {
            return eStatusFailed;
        }
    }
    if (!run(gpu::GpuKernelId::retouch_store, p, image, nullptr, nullptr, p.outWidth, p.outHeight)) {
        return eStatusFailed;
    }

    GpuImageView filtered;
    filtered.buffer = filteredBuffer;
    filtered.bounds = rect;
    filtered.nComps = nComps;
    filtered.rowStrideFloats = rect.width() * nComps;
    // The stages blend with the source as the CPU fetches it, cut to its region of definition.
    GpuRenderArgs finishArgs(args);
    if ((sourceInput >= 0) && (sourceInput < (int)finishArgs.inputs.size())) {
        finishArgs.inputs[sourceInput] = source;
    }

    return recordStagesGpuFinish(plan, finishArgs, filtered);
#else
    Q_UNUSED(args);

    return eStatusFailed;
#endif
} // Retouch::renderOpGpu

NATRON_NAMESPACE_EXIT
