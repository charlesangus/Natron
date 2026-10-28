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
#include "Engine/Nodes/KnobReader.h"
#include "Engine/OverlaySupport.h"
#include "Engine/PluginActionShortcut.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/RotoDrawableItem.h"
#include "Engine/RotoLayer.h"
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

    return std::dynamic_pointer_cast<Bezier>(drawable) && bool(itemKnob<KnobInt>(drawable, kRetouchShapeParamScale));
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
        for (int d = 0; d < 2; ++d) {
            setItemKnobValue<double>(offset, d, values[d], _time, _keyframe[d]);
        }
        _context->evaluateChange();
    }
};

Retouch::Retouch(NodePtr node)
    : StagedImageEffect(node)
    // Built here, as RotoPaint does: Node::load() creates the curve context before initializeKnobs()
    // and its endChanges() already calls knobChanged().
    , _ui(RotoCurveInteract::create(this))
    , _result(kRetouchFrameResult)
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
    _ui->createToolbarKnobs(eRotoCurveToolSetAll);
    _ui->createRightClickMenuKnobs(page);

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
    maxHealIterations->setHintToolTip(tr("The most iterations a Heal shape's solver runs before it stops."));
    maxHealIterations->setDefaultValue(EdgeAware::kPoissonHealDefaultIterations);
    maxHealIterations->setMinimum(1);
    maxHealIterations->setMaximum(100000);
    maxHealIterations->setDisplayMinimum(1);
    maxHealIterations->setDisplayMaximum(5000);
    maxHealIterations->setAnimationEnabled(false);
    page->addKnob(maxHealIterations);
    _maxHealIterations = maxHealIterations;

    // The selected shapes' own settings, slaved to them by the curve context.
    KnobGroupPtr selectedShape = createKnob<KnobGroup>(tr("Selected Shape"));
    selectedShape->setName(kRetouchParamSelectedShape);
    selectedShape->setAsTab();
    page->addKnob(selectedShape);
    RotoCurveContextPtr context = getNode()->getCurveContext();
    const std::vector<KnobIPtr> shapeKnobs = createShapeKnobs(this);
    for (std::vector<KnobIPtr>::const_iterator it = shapeKnobs.begin(); it != shapeKnobs.end(); ++it) {
        (*it)->setDefaultAllDimensionsEnabled(false);
        (*it)->setIsPersistent(false);
        selectedShape->addKnob(*it);
        if (context) {
            context->addHostItemKnob(*it);
        }
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

std::vector<KnobIPtr>
Retouch::createShapeKnobs(Retouch* host)
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

    return knobs;
} // Retouch::createShapeKnobs

void
Retouch::onCurveItemCreated(const RotoDrawableItemPtr& item)
{
    if (!std::dynamic_pointer_cast<Bezier>(item)) {
        return;
    }
    const std::vector<KnobIPtr> knobs = createShapeKnobs(NULL);
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

    _newShapes.push_back(item);
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
    for (int d = 0; d < 2; ++d) {
        const bool keyframe = (context && context->isAutoKeyingEnabled()) || offset->isAnimated(d, ViewSpec::all())
            || (offset->isSlave(d) && offset->getMaster(d).second && offset->getMaster(d).second->isAnimated(d, ViewSpec::all()));
        setItemKnobValue<double>(offset, d, values[d], time, keyframe);
    }
    if (context) {
        context->evaluateChange();
    }
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
    RotoCurveInteract::getRightClickMenuShortcuts(shortcuts);
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
Retouch::applyDefaultSourceOffsets()
{
    const double time = getCurrentTime();
    for (std::list<RotoDrawableItemWPtr>::iterator it = _newShapes.begin(); it != _newShapes.end();) {
        RotoDrawableItemPtr item = it->lock();
        BezierPtr bezier = std::dynamic_pointer_cast<Bezier>(item);
        if (!bezier) {
            it = _newShapes.erase(it);
            continue;
        }
        if (!bezier->isCurveFinished()) {
            ++it;
            continue;
        }
        KnobDoublePtr offset = itemKnob<KnobDouble>(item, kRetouchShapeParamSourceOffset);
        if (offset && !offset->isAnimated(0, ViewSpec::all()) && !offset->isAnimated(1, ViewSpec::all())
            && (offset->getValueAtTime(time, 0) == 0.) && (offset->getValueAtTime(time, 1) == 0.)) {
            const RectD bbox = bezier->getBoundingBox(time);
            if (!bbox.isNull()) {
                setShapeSourceOffset(item, time, bbox.width() / 2., bbox.height() / 2.);
            }
        }
        it = _newShapes.erase(it);
    }
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
    for (std::list<RotoDrawableItemPtr>::const_reverse_iterator it = curves.rbegin(); it != curves.rend(); ++it) {
        BezierPtr bezier = std::dynamic_pointer_cast<Bezier>(*it);
        if (!bezier || !isRetouchShape(bezier) || bezier->isLockedRecursive() || !bezier->isCurveFinished()) {
            continue;
        }
        const RC::ShapeSettings s = readShapeSettings(*it, time);
        if ((s.algorithm != RC::eAlgorithmClone) && (s.algorithm != RC::eAlgorithmHeal)) {
            continue;
        }
        double dx, dy;
        readShapeSourceOffset(*it, time, &dx, &dy);
        std::list<ParametricPoint> points;
        bezier->evaluateAtTime_DeCasteljau(true, time, 0, 50, &points, NULL);
        if (pointInPolygon(points, dx, dy, pos)) {
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
    for (std::list<RotoDrawableItemPtr>::const_iterator it = curves.begin(); it != curves.end(); ++it) {
        BezierPtr bezier = std::dynamic_pointer_cast<Bezier>(*it);
        if (!bezier || !isRetouchShape(bezier) || !bezier->isGloballyActivated() || !bezier->isCurveFinished()) {
            continue;
        }
        const RC::ShapeSettings s = readShapeSettings(*it, time);
        if ((s.algorithm != RC::eAlgorithmClone) && (s.algorithm != RC::eAlgorithmHeal)) {
            continue;
        }
        double dx, dy;
        readShapeSourceOffset(*it, time, &dx, &dy);
        std::list<ParametricPoint> points;
        RectD bbox;
        bezier->evaluateAtTime_DeCasteljau(true, time, 0, 50, &points, &bbox);
        if (points.empty()) {
            continue;
        }
        const bool isSelected = (*it == dragged) || (std::find(selected.begin(), selected.end(), *it) != selected.end());
        double color[4];
        bezier->getOverlayColor(color);
        color[3] = isSelected ? 1. : 0.5;
        glColor4dv(color);
        glLineWidth((isSelected ? 1.5 : 1.) * screenPixelRatio);
        glEnable(GL_LINE_STIPPLE);
        glLineStipple(2, 0xAAAA);
        glBegin(GL_LINE_LOOP);
        for (std::list<ParametricPoint>::const_iterator p = points.begin(); p != points.end(); ++p) {
            glVertex2d(p->x + dx, p->y + dy);
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
    for (std::list<RotoDrawableItemWPtr>::iterator it = _newShapes.begin(); it != _newShapes.end();) {
        BezierPtr bezier = std::dynamic_pointer_cast<Bezier>(it->lock());
        if (!bezier || bezier->isCurveFinished()) {
            it = _newShapes.erase(it);
        } else {
            ++it;
        }
    }

    const RotoToolEnum tool = _ui->getSelectedTool();
    if ((tool == eRotoToolSelectAll) || (tool == eRotoToolSelectCurves) || (tool == eRotoToolSelectPoints)) {
        RotoDrawableItemPtr item = pickSource(time, pos);
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
    RotoDrawableItemPtr dragged = _draggedSource.lock();
    if (dragged) {
        _dragEnd[0] = _dragStart[0] + (pos.x() - _dragPressPos.x());
        _dragEnd[1] = _dragStart[1] + (pos.y() - _dragPressPos.y());
        setShapeSourceOffset(dragged, _dragTime, _dragEnd[0], _dragEnd[1]);
        redrawOverlayInteract();

        return true;
    }

    return _ui->onOverlayPenMotion(time, pos, pressure, timestamp);
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
    RotoDrawableItemPtr dragged = _draggedSource.lock();
    if (dragged) {
        _draggedSource.reset();
        if ((_dragEnd[0] != _dragStart[0]) || (_dragEnd[1] != _dragStart[1])) {
            UndoCommandPtr command = makeSourceDragCommand(dragged, _dragTime, _dragStart, _dragEnd);
            if (command) {
                pushUndoCommand(command);
            }
        }
        redrawOverlayInteract();

        return true;
    }
    const bool ret = _ui->onOverlayPenUp();
    applyDefaultSourceOffsets();

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
    applyDefaultSourceOffsets();

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

void
Retouch::purgeCaches()
{
    _result.clear();
}

std::vector<RC::Shape>
Retouch::collectShapes(double time,
                       const RenderScale& scale,
                       const RectI& full) const
{
    std::vector<RC::Shape> shapes;
    RotoCurveContextPtr context = getNode()->getCurveContext();
    if (!context) {
        return shapes;
    }
    const double s = scale.toOfxPointD().x;
    const unsigned int mipmapLevel = scale.toMipmapLevel();
    const std::list<RotoDrawableItemPtr> curves = context->getCurvesByRenderOrder(true);
    for (std::list<RotoDrawableItemPtr>::const_iterator it = curves.begin(); it != curves.end(); ++it) {
        BezierPtr bezier = std::dynamic_pointer_cast<Bezier>(*it);
        if (!bezier || !isRetouchShape(bezier) || !bezier->isActivated(time)) {
            continue;
        }
        const RectD bbox = bezier->getBoundingBox(time);
        if (bbox.isNull()) {
            continue;
        }
        RectI pixels;
        pixels.x1 = (int)std::floor(bbox.x1 * s);
        pixels.y1 = (int)std::floor(bbox.y1 * s);
        pixels.x2 = (int)std::ceil(bbox.x2 * s);
        pixels.y2 = (int)std::ceil(bbox.y2 * s);
        std::vector<float> coverage;
        if (!bezier->rasterizeCoverage(time, mipmapLevel, pixels, &coverage)) {
            continue;
        }
        RC::Shape shape;
        shape.settings = readShapeSettings(*it, time);
        shape.maskRoi.x = pixels.x1 - full.x1;
        shape.maskRoi.y = full.y2 - pixels.y2;
        shape.maskRoi.width = pixels.width();
        shape.maskRoi.height = pixels.height();
        // Coverage rows go up from pixels.y1; the shape's mask rows go down.
        shape.mask.resize(coverage.size());
        const std::size_t w = (std::size_t)shape.maskRoi.width;
        for (int r = 0; r < shape.maskRoi.height; ++r) {
            std::copy(&coverage[(std::size_t)(shape.maskRoi.height - 1 - r) * w], &coverage[(std::size_t)(shape.maskRoi.height - 1 - r) * w] + w,
                      &shape.mask[(std::size_t)r * w]);
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
    RectI frame;
    if (!input || !framePixels(time, view, scale, &frame)) {
        return;
    }
    // The whole frame, and the window, outside the frame passed through.
    const NodeMetadataConstPtr metadata = getRenderMetadata(time, view);
    RectD need = PassExec::toCanonical(frame, scale, metadata ? metadata->getPixelAspectRatio(sourceInput) : 1.);
    need.merge(renderWindow);
    (*ret)[input] = need;
}

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
    RectI full;
    if (!framePixels(args.time, args.view, args.mappedScale, &full)) {
        return eStatusFailed;
    }
    RectI overlap;
    // A window outside the frame is the source alone.
    const bool touchesFrame = PassExec::intersectRects(args.roi, full, &overlap) && !PassExec::isEmptyRect(overlap);

    RenderCancellation cancel(this);
    const int nThreads = appPTR->getNCPUsAvailableForEffect();
    const EdgeAware::ParallelFor parallel = [&cancel, nThreads](int count, const std::function<void(int)>& body) {
        parallelForCancellable(count, nThreads, cancel, body);
    };
    const int width = full.width();
    const int height = full.height();

    for (std::list<std::pair<ImageLayerDesc, ImagePtr>>::const_iterator it = args.outputLayers.begin(); it != args.outputLayers.end(); ++it) {
        if (!it->second) {
            continue;
        }
        const int nComps = (int)it->second->getComponentsCount();

        WholeFrameCache::Result result;
        if (touchesFrame) {
            const std::string key = wholeFrameKey(*this, sourceInput, args.time, args.view, args.mappedScale, full, it->first.getLayerID(), nComps);
            result = _result.get(key, [&]() -> WholeFrameCache::Result {
                ImagePtr src;
                ImageLayerDesc sourceLayer;
                if (resolveInputPlaneForRender(sourceInput, args.time, args.view, &sourceLayer, NULL)) {
                    RectI sourceRoI;
                    src = getImage(sourceInput, args.time, args.mappedScale, args.view, NULL, &sourceLayer, true /*mapToClipPrefs*/, false /*dontUpscale*/, eStorageModeRAM, NULL, &sourceRoI);
                }
                if (src && !PassExec::isFloatImage(src)) {
                    return WholeFrameCache::Result();
                }
                std::shared_ptr<Image::ReadAccess> srcAccess;
                RectI srcBounds;
                int srcNComps = 0;
                if (src) {
                    srcAccess = std::make_shared<Image::ReadAccess>(src.get());
                    srcBounds = src->getBounds();
                    srcNComps = (int)src->getComponentsCount();
                }
                const RC::Params params = readParams(args.time, args.view, (float)args.mappedScale.toOfxPointD().x);
                const std::vector<RC::Shape> shapes = collectShapes(args.time, args.mappedScale, full);

                // The whole source, top-down, as darktable's 4 channels: one channel goes to all three colours.
                auto processed = std::make_shared<std::vector<float>>((std::size_t)width * height * 4, 0.f);
                std::vector<float>& image = *processed;
                parallel(height, [&](int r) {
                    const int y = full.y2 - 1 - r;
                    std::vector<float> row((std::size_t)width * nComps);
                    PassExec::readSourceRow(src.get(), srcAccess.get(), srcBounds, srcNComps, full.x1, y, width, nComps, row.data());
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
                    return WholeFrameCache::Result();
                }
                RC::process(image.data(), width, height, params, shapes, parallel);
                if (cancel.check()) {
                    return WholeFrameCache::Result();
                }

                return processed;
            });
            if (!result) {
                return eStatusFailed;
            }
        }
        const float* image = result ? result->data() : nullptr;

        const auto row = [&](const RowIO& io) {
            const float zero[4] = { 0.f, 0.f, 0.f, 0.f };
            const bool insideRow = image && (io.y >= full.y1) && (io.y < full.y2);
            const std::size_t rowStart = insideRow ? (std::size_t)(full.y2 - 1 - io.y) * width : 0;
            for (int i = 0; i < io.width; ++i) {
                const int x = io.x0 + i;
                const float* in = io.src[0] ? &io.src[0][(std::size_t)i * io.nComps] : zero;
                float out[4] = { 0.f, 0.f, 0.f, 0.f };
                if (insideRow && (x >= full.x1) && (x < full.x2)) {
                    const float* p = &image[4 * (rowStart + (x - full.x1))];
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

bool
deviceHasPushDescriptors()
{
    GpuContext* gpu = appPTR ? appPTR->getGpuContext() : nullptr;
    gpu::GpuRuntime* runtime = gpu ? gpu->runtime() : nullptr;

    return runtime && runtime->device().info().pushDescriptor;
}
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
    // The frame is held as four floats a pixel in one buffer.
    if (!wholeFrameFitsGpu(full, 4, query.maxScratchBytes)) {
        return false;
    }
    RotoCurveContextPtr context = getNode()->getCurveContext();
    if (context) {
        const bool healIterationsFit = (params.maxHealIterations <= EdgeAware::kPoissonHealGpuMaxIterationsWithoutPushDescriptors) || deviceHasPushDescriptors();
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
            if ((s.algorithm == RC::eAlgorithmHeal) && !healIterationsFit) {
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
    RectI full;

    return framePixels(time, view, scale, &full) ? full : outputRect;
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
    const std::vector<RC::Shape> shapes = collectShapes(query.time, query.mappedScale, full);

    const int width = full.width();
    const int height = full.height();
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
    if (!ctx.scratch(imageFloats * sizeof(float), image) || !image || !ctx.scratch(filteredFloats * sizeof(float), filteredBuffer) || !filteredBuffer
        || !ctx.scratch(4 * sizeof(float), standIn) || !standIn) {
        return eStatusFailed;
    }
    const gpu::GpuBuffer* srcBuffer = source.buffer ? source.buffer : standIn;

    RetouchGpuParams p = {};
    p.width = (uint32_t)width;
    p.height = (uint32_t)height;
    p.fullX1 = full.x1;
    p.fullY2 = full.y2;
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
            sp.maskX = maskRoi.x;
            sp.maskY = maskRoi.y;
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
                if (!EdgeAware::recordPoissonHeal(ctx, *aux2, *aux, *maskBuffer, maskRoi.width, maskRoi.height, params.maxHealIterations)) {
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

    if (!run(gpu::GpuKernelId::retouch_load, p, image, nullptr, nullptr, p.width, p.height) || !processLayer(image, 0)) {
        return eStatusFailed;
    }
    if (scales > 0) {
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
        if (!EdgeAware::recordDwtDecomposeScales(ctx, *image, width, height, scales, std::span<const gpu::GpuBuffer* const>(bandViews.data(), bandViews.size()), *residual, bands)
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
