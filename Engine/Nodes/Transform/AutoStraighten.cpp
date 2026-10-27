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

/*
 * Knobs, defaults, the output size and the warp follow darktable's rotate and perspective module,
 * DARKTABLE_REF release-5.6.2 (commit 20891f6c6fa7), src/iop/ashift.c, Copyright (C) 2016-2026
 * darktable developers, distributed under the GNU GPL version 3 or later.
 */

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "AutoStraighten.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QPointF>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Bezier.h"
#include "Engine/BezierCP.h"
#include "Engine/ChoiceOption.h"
#include "Engine/Format.h"
#include "Engine/Image.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/NodeMetadata.h"
#include "Engine/Nodes/KnobReader.h"
#include "Engine/PluginActionShortcut.h"
#include "Engine/Project.h"
#include "Engine/RenderScale.h"
#include "Engine/RotoCurveContext.h"
#include "Engine/RotoCurveInteract.h"
#include "Engine/RotoDrawableItem.h"
#include "Engine/RotoLayer.h"
#include "Engine/TimeLine.h"
#include "Engine/Transform.h"
#include "Engine/UndoCommand.h"

NATRON_NAMESPACE_ENTER

namespace {
constexpr int kMinimumOutputSize = 4;

const char* const kNotEnoughStructure = "not enough guide lines for automatic correction\nminimum 2 lines in each relevant direction";
const char* const kFitFailed = "automatic correction failed, please correct manually";

bool
rectIsEmpty(const RectD& r)
{
    return (r.x2 <= r.x1) || (r.y2 <= r.y1);
}

RectD
rectIntersection(const RectD& a,
                 const RectD& b)
{
    if (rectIsEmpty(a) || rectIsEmpty(b)) {
        return RectD(0., 0., 0., 0.);
    }
    if ((a.x1 > b.x2) || (b.x1 > a.x2) || (a.y1 > b.y2) || (b.y1 > a.y2)) {
        return RectD(0., 0., 0., 0.);
    }
    RectD ret;
    ret.x1 = (std::max)(a.x1, b.x1);
    ret.x2 = (std::max)(ret.x1, (std::min)(a.x2, b.x2));
    ret.y1 = (std::max)(a.y1, b.y1);
    ret.y2 = (std::max)(ret.y1, (std::min)(a.y2, b.y2));

    return ret;
}

RectD
toCanonical(const RectI& r,
            double par)
{
    if ((r.x2 <= r.x1) || (r.y2 <= r.y1)) {
        return RectD(0., 0., 0., 0.);
    }

    return RectD(r.x1 * par, r.y1, r.x2 * par, r.y2);
}

void
setSecretAndDisabled(const KnobIPtr& knob,
                     bool secret)
{
    if (!knob) {
        return;
    }
    knob->setSecret(secret);
    knob->setAllDimensionsEnabled(!secret);
}

// darktable's commit_params(): the generic model ignores the focal length, the lens dependence
// and the aspect.
Ashift::Model
makeModel(double rotation,
          double lensShiftV,
          double lensShiftH,
          double shear,
          bool specific,
          double focalLength,
          double cropFactor,
          double lensDependence,
          double aspectAdjust)
{
    Ashift::Model m;

    m.rotation = -rotation;
    m.lensShiftV = lensShiftV;
    m.lensShiftH = lensShiftH;
    m.shear = shear;
    if (specific) {
        m.focalLength = focalLength * cropFactor;
        m.orthocorr = lensDependence;
        m.aspect = aspectAdjust;
    } else {
        m.focalLength = Ashift::kDefaultFocalLength;
        m.orthocorr = 0.;
        m.aspect = 1.;
    }

    return m;
}

bool
sameModel(const Ashift::Model& a,
          const Ashift::Model& b)
{
    return (a.rotation == b.rotation) && (a.lensShiftV == b.lensShiftV) && (a.lensShiftH == b.lensShiftH) && (a.shear == b.shear)
        && (a.focalLength == b.focalLength) && (a.orthocorr == b.orthocorr) && (a.aspect == b.aspect);
}

KnobChoicePtr
lineOrientationKnob(const RotoItemPtr& item)
{
    const RotoDrawableItemPtr drawable = std::dynamic_pointer_cast<RotoDrawableItem>(item);

    return drawable ? std::dynamic_pointer_cast<KnobChoice>(drawable->getKnobByName(kAutoStraightenParamLineOrientation)) : KnobChoicePtr();
}

std::vector<ChoiceOption>
lineOrientationOptions()
{
    std::vector<ChoiceOption> options;

    options.push_back(ChoiceOption("vertical", "Vertical", "The line should be vertical."));
    options.push_back(ChoiceOption("horizontal", "Horizontal", "The line should be horizontal."));

    return options;
}

/**
 * @brief The line orientation knob: a detached one for an item (host NULL) or the node's.
 **/
KnobChoicePtr
makeLineOrientationKnob(AutoStraighten* host)
{
    const std::string label = kAutoStraightenParamLineOrientationLabel;
    KnobChoicePtr k;

    if (host) {
        k = AppManager::createKnob<KnobChoice>(host, label, 1);
    } else {
        k = std::make_shared<KnobChoice>((KnobHolder*)NULL, label, 1, true);
        k->populate();
    }
    k->setName(kAutoStraightenParamLineOrientation);
    k->setHintToolTip(std::string("Whether the selected guide line should be vertical or horizontal once corrected."));
    k->populateChoices(lineOrientationOptions());
    k->setDefaultValue((int)AutoStraighten::eLineOrientationVertical);
    k->setAnimationEnabled(false);

    return k;
}

/**
 * @brief An item's own orientation. While items are selected their knobs are slaved to the node's
 * knob, whose single value would stand for every selected line, so the master is bypassed: the
 * node keeps the items' own values current (AutoStraighten::knobChanged).
 **/
int
ownOrientation(const KnobChoicePtr& knob)
{
    return knob->getValueAtTime(knob->getCurrentTime(), 0, ViewSpec::current(), true, true /*byPassMaster*/);
}

/**
 * @brief Draws a guide line, as MakeRectangleUndoCommand draws a rectangle: the press and the
 * drag after it merge into one step, which creates the line and leaves its end where the drag
 * stopped.
 **/
class DrawLineUndoCommand
    : public UndoCommand,
      public UndoGestureMember {
    RotoCurveInteractWPtr _roto;
    RotoLayerPtr _parentLayer;
    int _indexInLayer;
    BezierPtr _curve;
    bool _create;
    bool _firstRedoCalled;
    double _fromx, _fromy, _tox, _toy;
    double _time;

public:
    DrawLineUndoCommand(const RotoCurveInteractPtr& roto,
                        bool create,
                        double fromx,
                        double fromy,
                        double tox,
                        double toy,
                        double time)
        : UndoCommand()
        , _roto(roto)
        , _parentLayer()
        , _indexInLayer(-1)
        , _curve()
        , _create(create)
        , _firstRedoCalled(false)
        , _fromx(fromx)
        , _fromy(fromy)
        , _tox(tox)
        , _toy(toy)
        , _time(time)
    {
        setGestureKey(roto->undoGesture.key());
        if (!_create) {
            _curve = roto->getBezierBeingBuild();
        }
        setText("Draw Guide Line");
    }

    virtual ~DrawLineUndoCommand()
    {
    }

    virtual void undo() OVERRIDE FINAL
    {
        RotoCurveInteractPtr roto = _roto.lock();
        if (!roto || !_curve) {
            return;
        }
        roto->removeCurve(_curve);
        roto->evaluate(true);
        roto->setSelection(BezierPtr(), std::make_pair(BezierCPPtr(), BezierCPPtr()));
    }

    virtual void redo() OVERRIDE FINAL
    {
        RotoCurveInteractPtr roto = _roto.lock();
        if (!roto) {
            return;
        }
        RotoCurveContextPtr context = roto->getCurveContext();
        if (!context) {
            return;
        }
        if (_firstRedoCalled) {
            if (_create) {
                context->addItem(_parentLayer, _indexInLayer, _curve, RotoItem::eSelectionReasonOverlayInteract);
                roto->evaluate(true);
            }
        } else {
            if (_create) {
                _curve = context->makeBezier(_fromx, _fromy, kAutoStraightenLineBaseName, _time, true /*isOpenBezier*/);
                if (!_curve) {
                    return;
                }
                _curve->addControlPoint(_tox, _toy, _time);
                _curve->setCurveFinished(true);
                RotoLayerPtr layer = _curve->getParentLayer();
                if (layer) {
                    _parentLayer = std::dynamic_pointer_cast<RotoLayer>(context->getItemByName(layer->getScriptName()));
                    if (_parentLayer) {
                        _indexInLayer = _parentLayer->getChildIndex(_curve);
                    }
                }
            } else if (_curve && (_curve->getControlPointsCount() >= 2)) {
                _curve->setPointByIndex(1, _time, _tox, _toy);
            }
        }
        if (!_curve) {
            return;
        }
        roto->setBuiltBezier(_curve);
        _firstRedoCalled = true;
        roto->setSelection(_curve, std::make_pair(BezierCPPtr(), BezierCPPtr()));
    }

    virtual bool mergeWith(const UndoCommandPtr& other) OVERRIDE FINAL
    {
        const DrawLineUndoCommand* cmd = dynamic_cast<const DrawLineUndoCommand*>(other.get());
        if (!cmd || !inSameGestureAs(*cmd) || cmd->_create || (cmd->_curve != _curve)) {
            return false;
        }
        _tox = cmd->_tox;
        _toy = cmd->_toy;

        return true;
    }
};

/**
 * @brief Sets the orientation of guide lines from the curve panel's menu, undone as one step.
 **/
class LineOrientationCommand
    : public UndoCommand {
    struct Entry {
        RotoDrawableItemPtr item;
        int oldValue;
    };

    NodeWPtr _node;
    std::list<Entry> _entries;
    int _value;

public:
    LineOrientationCommand(const NodePtr& node,
                           const std::list<RotoDrawableItemPtr>& items,
                           int value)
        : UndoCommand()
        , _node(node)
        , _value(value)
    {
        for (std::list<RotoDrawableItemPtr>::const_iterator it = items.begin(); it != items.end(); ++it) {
            KnobChoicePtr k = lineOrientationKnob(*it);
            if (k) {
                Entry e;
                e.item = *it;
                e.oldValue = ownOrientation(k);
                _entries.push_back(e);
            }
        }
        setText((value == (int)AutoStraighten::eLineOrientationVertical) ? "Set Guide Lines Vertical" : "Set Guide Lines Horizontal");
    }

    virtual ~LineOrientationCommand()
    {
    }

    virtual void redo() OVERRIDE FINAL
    {
        for (std::list<Entry>::const_iterator it = _entries.begin(); it != _entries.end(); ++it) {
            apply(it->item, _value);
        }
    }

    virtual void undo() OVERRIDE FINAL
    {
        for (std::list<Entry>::const_iterator it = _entries.begin(); it != _entries.end(); ++it) {
            apply(it->item, it->oldValue);
        }
    }

private:
    void apply(const RotoDrawableItemPtr& item,
               int value)
    {
        NodePtr node = _node.lock();
        AutoStraighten* effect = node ? dynamic_cast<AutoStraighten*>(node->getEffectInstance().get()) : NULL;
        if (effect) {
            effect->setLineOrientation(item, (value == (int)AutoStraighten::eLineOrientationHorizontal) ? AutoStraighten::eLineOrientationHorizontal : AutoStraighten::eLineOrientationVertical);
        }
    }
};

/**
 * @brief The curve interact with a Guide Lines tool group, whose Draw Line tool makes a two-point
 * open curve by a click and drag, then tags it vertical or horizontal by its direction.
 **/
class AutoStraightenInteract
    : public RotoCurveInteract {
    AutoStraighten* _node;
    double _lineTime;
    bool _drawing;

    struct MakeSharedEnabler;

protected:
    explicit AutoStraightenInteract(AutoStraighten* node)
        : RotoCurveInteract(node)
        , _node(node)
        , _lineTime(0.)
        , _drawing(false)
    {
    }

public:
    KnobGroupWPtr guideLinesToolGroup;
    KnobButtonWPtr drawLineAction;

    static RotoCurveInteractPtr create(AutoStraighten* node);

    virtual ~AutoStraightenInteract()
    {
    }

    virtual bool getRoleForGroup(const KnobGroupPtr& group,
                                 RotoRoleEnum* role) const OVERRIDE FINAL
    {
        if (group && (group == guideLinesToolGroup.lock())) {
            *role = eRotoRoleBezierEdition;

            return true;
        }

        return RotoCurveInteract::getRoleForGroup(group, role);
    }

    virtual bool getToolForAction(const KnobButtonPtr& action,
                                  RotoToolEnum* tool) const OVERRIDE FINAL
    {
        if (action && (action == drawLineAction.lock())) {
            *tool = eRotoToolDrawLine;

            return true;
        }

        return RotoCurveInteract::getToolForAction(action, tool);
    }

protected:
    virtual bool onPaintToolPenDown(double time,
                                    const QPointF& pos,
                                    double /*pressure*/,
                                    double /*timestamp*/) OVERRIDE FINAL
    {
        if (selectedTool != eRotoToolDrawLine) {
            return false;
        }
        click = pos;
        _lineTime = time;
        _drawing = false;
        state = eEventStateBuildingLine;

        return true;
    }

    virtual bool setPaintToolCursor(bool* /*redraw*/) OVERRIDE FINAL
    {
        if (selectedTool != eRotoToolDrawLine) {
            return false;
        }
        getHost()->setCurrentCursor(eCursorCross);

        return true;
    }

    virtual bool onPaintStatePenMotion(const QPointF& pos,
                                       double /*pressure*/,
                                       double /*timestamp*/,
                                       double /*dx*/,
                                       double /*dy*/) OVERRIDE FINAL
    {
        if (state != eEventStateBuildingLine) {
            return false;
        }
        if (!_drawing && (pos == click)) {
            return true;
        }
        pushUndoCommand(new DrawLineUndoCommand(shared_from_this(), !_drawing, click.x(), click.y(), pos.x(), pos.y(), _lineTime));
        _drawing = true;
        evaluateOnPenUp = true;

        return true;
    }

    virtual bool onPaintStatePenUp() OVERRIDE FINAL
    {
        if (state != eEventStateBuildingLine) {
            return false;
        }
        if (_drawing) {
            BezierPtr line = getBezierBeingBuild();
            if (line) {
                _node->setLineOrientationFromDirection(line, _lineTime);
            }
        }
        _drawing = false;

        return true;
    }
};

struct AutoStraightenInteract::MakeSharedEnabler
    : public AutoStraightenInteract {
    explicit MakeSharedEnabler(AutoStraighten* node)
        : AutoStraightenInteract(node)
    {
    }
};

RotoCurveInteractPtr
AutoStraightenInteract::create(AutoStraighten* node)
{
    return std::make_shared<AutoStraightenInteract::MakeSharedEnabler>(node);
}
} // anonymous namespace

AutoStraightenOp::Params
AutoStraightenOp::bake(const KnobReader& reader) const
{
    Params p;

    p.time = reader.time();
    p.view = reader.view();
    p.model = makeModel(reader.value(rotation), reader.value(lensShiftV), reader.value(lensShiftH), reader.value(shear),
                        reader.value(lensModel) == (int)AutoStraighten::eLensModelSpecific, reader.value(focalLength),
                        reader.value(cropFactor), reader.value(lensDependence), reader.value(aspectAdjust));
    p.crop = (Ashift::CropModeEnum)reader.value(autoCrop);
    p.filter = (Resampler::FilterEnum)reader.value(filter);
    p.clamp = reader.value(clamp);
    p.blackOutside = reader.value(blackOutside);

    return p;
}

bool
AutoStraightenOp::isIdentity(const Params& params,
                             const PassContext& /*context*/) const
{
    if (!node) {
        return true;
    }
    RectI format;
    double par = 1.;
    node->getSourceFormat(params.time, params.view, &format, &par);

    return node->computeGeometry(format, par, params.model, params.crop).bypass;
}

ResampleSettings
AutoStraightenOp::settings(const Params& params,
                           const PassContext& context) const
{
    ResampleSettings s;

    s.filter = params.filter;
    s.clamp = params.clamp;
    s.blackOutside = params.blackOutside;
    s.invert = false;
    s.transformIsIdentity = isIdentity(params, context);

    return s;
}

bool
AutoStraightenOp::transform(const Params& params,
                            const PassContext& context,
                            double /*amount*/,
                            bool invert,
                            TransformMath::Mat3* matrix) const
{
    if (!node) {
        return false;
    }
    RectI format;
    double par = 1.;
    node->getSourceFormat(params.time, params.view, &format, &par);
    if (context.pixelAspectRatio > 0.) {
        par = context.pixelAspectRatio;
    }
    const AutoStraighten::Geometry geometry = node->computeGeometry(format, par, params.model, params.crop);

    return AutoStraighten::canonicalMatrix(geometry, params.model, invert, matrix);
}

bool
AutoStraightenOp::canConcatenate() const
{
    return false;
}

std::optional<GpuCostHint>
AutoStraightenOp::costHint(const Params& /*params*/,
                           const PassContext& /*context*/) const
{
    return GpuCostHint();
}

AutoStraighten::AutoStraighten(NodePtr node)
    : ImageNode<AutoStraightenOp>(node)
    // Built here, as RotoPaint does: Node::load() creates the curve context before initializeKnobs()
    // and its endChanges() already calls knobChanged().
    , _ui(AutoStraightenInteract::create(this))
    , _settingLineOrientation(false)
{
    op().node = this;
}

AutoStraighten::~AutoStraighten()
{
}

void
AutoStraighten::addAcceptedComponents(int inputNb,
                                      std::list<ImageLayerDesc>* comps)
{
    ImageNode<AutoStraightenOp>::addAcceptedComponents(inputNb, comps);
    comps->push_back(ImageLayerDesc::getXYComponents());
}

RotoCurvePanelHook*
AutoStraighten::getCurvePanelHook() const
{
    return const_cast<AutoStraighten*>(this);
}

void
AutoStraighten::initializeKnobs()
{
    _ui->connectToCurveContext();

    const KnobPagePtr page = createControlsPage();
    _ui->createViewerButtonKnobs(page);
    {
        // Guide lines are selected and their end points moved with the curve tools, and drawn with
        // the Draw Line tool.
        KnobPagePtr toolbar = _ui->createToolbarKnobs(eRotoCurveToolSetSelection | eRotoCurveToolSetPointsEdition);
        AutoStraightenInteract* ui = static_cast<AutoStraightenInteract*>(_ui.get());

        KnobGroupPtr guideLines = createKnob<KnobGroup>(tr(kAutoStraightenUIParamGuideLinesToolButtonLabel));
        guideLines->setName(kAutoStraightenUIParamGuideLinesToolButton);
        guideLines->setAsToolButton(true);
        guideLines->setEvaluateOnChange(false);
        guideLines->setSecretByDefault(true);
        guideLines->setInViewerContextCanHaveShortcut(true);
        guideLines->setIsPersistent(false);
        toolbar->addKnob(guideLines);
        ui->guideLinesToolGroup = guideLines;

        KnobButtonPtr drawLine = createKnob<KnobButton>(tr(kAutoStraightenUIParamDrawLineToolButtonActionLabel));
        drawLine->setName(kAutoStraightenUIParamDrawLineToolButtonAction);
        drawLine->setHintToolTip(tr(kAutoStraightenUIParamDrawLineToolButtonActionHint));
        drawLine->setCheckable(true);
        drawLine->setDefaultValue(true);
        drawLine->setSecretByDefault(true);
        drawLine->setEvaluateOnChange(false);
        drawLine->setIconLabel(NATRON_ICONS_PATH "lucide/pencil.svg");
        drawLine->setIsPersistent(false);
        guideLines->addKnob(drawLine);
        ui->drawLineAction = drawLine;
    }
    _ui->createRightClickMenuKnobs(page);

    const auto makeDouble = [&](const QString& label, const char* name, const QString& hint, double def, double mini, double maxi,
                                double dispMin, double dispMax) {
        KnobDoublePtr knob = createKnob<KnobDouble>(label);
        knob->setName(name);
        knob->setHintToolTip(hint);
        knob->setMinimum(mini);
        knob->setMaximum(maxi);
        knob->setDisplayMinimum(dispMin);
        knob->setDisplayMaximum(dispMax);
        knob->setDefaultValue(def);
        knob->setIsMetadataSlave(true);
        page->addKnob(knob);

        return knob;
    };

    KnobDoublePtr rotation = makeDouble(tr("Rotation"), kAutoStraightenParamRotation,
                                        tr("Rotate the image, in degrees."),
                                        0., -Ashift::kRotationRange, Ashift::kRotationRange, -10., 10.);
    op().rotation = rotation;

    KnobDoublePtr lensShiftV = makeDouble(tr("Lens Shift V"), kAutoStraightenParamLensShiftV,
                                          tr("Apply lens shift correction in the vertical direction: corrects converging verticals."),
                                          0., -Ashift::kLensShiftRange, Ashift::kLensShiftRange, -1., 1.);
    lensShiftV->setDecimals(3);
    op().lensShiftV = lensShiftV;

    KnobDoublePtr lensShiftH = makeDouble(tr("Lens Shift H"), kAutoStraightenParamLensShiftH,
                                          tr("Apply lens shift correction in the horizontal direction: corrects converging horizontals."),
                                          0., -Ashift::kLensShiftRange, Ashift::kLensShiftRange, -1., 1.);
    lensShiftH->setDecimals(3);
    op().lensShiftH = lensShiftH;

    KnobDoublePtr shear = makeDouble(tr("Shear"), kAutoStraightenParamShear,
                                     tr("Shear the image along one diagonal."),
                                     0., -Ashift::kShearRange, Ashift::kShearRange, -0.2, 0.2);
    op().shear = shear;

    KnobChoicePtr autoCrop = createKnob<KnobChoice>(tr("Auto Crop"));
    autoCrop->setName(kAutoStraightenParamAutoCrop);
    autoCrop->setHintToolTip(tr("Automatically crop to avoid black edges."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("off", tr("Off").toStdString(), tr("Keep the whole corrected image, with its empty borders.").toStdString()));
        options.push_back(ChoiceOption("largest", tr("Largest Area").toStdString(), tr("Crop to the largest rectangle inside the corrected image.").toStdString()));
        options.push_back(ChoiceOption("original", tr("Original Format").toStdString(), tr("Crop to the largest rectangle of the source's aspect ratio inside the corrected image.").toStdString()));
        autoCrop->populateChoices(options);
    }
    autoCrop->setDefaultValue((int)Ashift::eCropLargest);
    autoCrop->setAnimationEnabled(false);
    autoCrop->setIsMetadataSlave(true);
    page->addKnob(autoCrop);
    op().autoCrop = autoCrop;

    KnobChoicePtr lensModel = createKnob<KnobChoice>(tr("Lens Model"));
    lensModel->setName(kAutoStraightenParamLensModel);
    lensModel->setHintToolTip(tr("Lens model of the perspective correction: generic, or according to the focal length."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("generic", tr("Generic").toStdString(), tr("A lens of 28 mm on a full-frame sensor.").toStdString()));
        options.push_back(ChoiceOption("specific", tr("Specific").toStdString(), tr("The focal length, crop factor, lens dependence and aspect adjust below.").toStdString()));
        lensModel->populateChoices(options);
    }
    lensModel->setDefaultValue((int)eLensModelGeneric);
    lensModel->setAnimationEnabled(false);
    lensModel->setIsMetadataSlave(true);
    page->addKnob(lensModel);
    op().lensModel = lensModel;

    KnobDoublePtr focalLength = makeDouble(tr("Focal Length"), kAutoStraightenParamFocalLength,
                                           tr("Focal length of the lens, in millimetres."),
                                           Ashift::kDefaultFocalLength, 1., 2000., 10., 1000.);
    focalLength->setDecimals(0);
    op().focalLength = focalLength;

    KnobDoublePtr cropFactor = makeDouble(tr("Crop Factor"), kAutoStraightenParamCropFactor,
                                          tr("Crop factor of the camera sensor."),
                                          1., 0.5, 10., 1., 2.);
    op().cropFactor = cropFactor;

    KnobDoublePtr lensDependence = makeDouble(tr("Lens Dependence"), kAutoStraightenParamLensDependence,
                                              tr("The level of lens dependent correction, in percent: 100 for full lens dependency, 0 for the generic case."),
                                              100., 0., 100., 0., 100.);
    op().lensDependence = lensDependence;

    KnobDoublePtr aspectAdjust = makeDouble(tr("Aspect Adjust"), kAutoStraightenParamAspectAdjust,
                                            tr("Adjust the aspect ratio of the image by horizontal and vertical scaling."),
                                            1., 0.5, 2., 0.5, 2.);
    op().aspectAdjust = aspectAdjust;

    KnobChoicePtr structure = createKnob<KnobChoice>(tr("Structure"));
    structure->setName(kAutoStraightenParamStructure);
    structure->setHintToolTip(tr("What the fit buttons straighten: the guide lines drawn in the viewer, or the four sides of a rectangle placed in the viewer."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("lines", tr("Drawn Lines").toStdString(), tr("Guide lines drawn in the viewer with the Draw Line tool, each tagged vertical or horizontal.").toStdString()));
        options.push_back(ChoiceOption("rectangle", tr("Rectangle").toStdString(), tr("Manually define the perspective rectangle with the four corners.").toStdString()));
        structure->populateChoices(options);
    }
    structure->setDefaultValue((int)eStructureDrawnLines);
    structure->setAnimationEnabled(false);
    page->addKnob(structure);
    _structure = structure;

    // The selected guide line's orientation, slaved to it by the curve context.
    {
        KnobChoicePtr lineOrientation = makeLineOrientationKnob(this);
        lineOrientation->setDefaultAllDimensionsEnabled(false);
        lineOrientation->setIsPersistent(false);
        page->addKnob(lineOrientation);
        _lineOrientation = lineOrientation;
        RotoCurveContextPtr context = getNode()->getCurveContext();
        if (context) {
            context->addHostItemKnob(lineOrientation);
        }
    }

    KnobChoicePtr fitParameters = createKnob<KnobChoice>(tr("Fit Parameters"));
    fitParameters->setName(kAutoStraightenParamFitParameters);
    fitParameters->setHintToolTip(tr("The parameters the fit buttons solve for. All fits rotation and lens shift, and with Fit Both also shear."));
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption("all", tr("All").toStdString(), ""));
        options.push_back(ChoiceOption("rotation", tr("Rotation Only").toStdString(), ""));
        options.push_back(ChoiceOption("lensShift", tr("Lens Shift Only").toStdString(), ""));
        options.push_back(ChoiceOption("rotationAndLensShift", tr("Rotation And Lens Shift").toStdString(), ""));
        fitParameters->populateChoices(options);
    }
    fitParameters->setDefaultValue((int)eFitParametersAll);
    fitParameters->setAnimationEnabled(false);
    page->addKnob(fitParameters);
    _fitParameters = fitParameters;

    KnobButtonPtr fitVertical = createKnob<KnobButton>(tr("Fit Vertical"));
    fitVertical->setName(kAutoStraightenParamFitVertical);
    fitVertical->setHintToolTip(tr("Automatically correct for vertical perspective distortion at the current frame."));
    fitVertical->setAddNewLine(false);
    page->addKnob(fitVertical);
    _fitVertical = fitVertical;

    KnobButtonPtr fitHorizontal = createKnob<KnobButton>(tr("Fit Horizontal"));
    fitHorizontal->setName(kAutoStraightenParamFitHorizontal);
    fitHorizontal->setHintToolTip(tr("Automatically correct for horizontal perspective distortion at the current frame."));
    fitHorizontal->setAddNewLine(false);
    page->addKnob(fitHorizontal);
    _fitHorizontal = fitHorizontal;

    KnobButtonPtr fitBoth = createKnob<KnobButton>(tr("Fit Both"));
    fitBoth->setName(kAutoStraightenParamFitBoth);
    fitBoth->setHintToolTip(tr("Automatically correct for vertical and horizontal perspective distortions at the current frame, fitting rotation, lens shift in both directions, and shear."));
    page->addKnob(fitBoth);
    _fitBoth = fitBoth;

    // darktable's default rectangle, at 20% and 80% of the image.
    const char* const cornerNames[4] = { kAutoStraightenParamTopLeft, kAutoStraightenParamTopRight, kAutoStraightenParamBottomLeft, kAutoStraightenParamBottomRight };
    const QString cornerLabels[4] = { tr("Top Left"), tr("Top Right"), tr("Bottom Left"), tr("Bottom Right") };
    const double cornerDefaults[4][2] = { { 0.2, 0.8 }, { 0.8, 0.8 }, { 0.2, 0.2 }, { 0.8, 0.2 } };
    for (int i = 0; i < 4; ++i) {
        KnobDoublePtr corner = createKnob<KnobDouble>(cornerLabels[i], 2);
        corner->setName(cornerNames[i]);
        corner->setHintToolTip(tr("A corner of the rectangle whose sides the fit makes vertical and horizontal, in Rectangle mode."));
        corner->setSpatial(true);
        corner->disableSlider();
        corner->setDefaultValuesAreNormalized(true);
        for (int d = 0; d < 2; ++d) {
            corner->setMinimum(-DBL_MAX, d);
            corner->setMaximum(DBL_MAX, d);
            corner->setDisplayMinimum(-10000., d);
            corner->setDisplayMaximum(10000., d);
            corner->setDefaultValue(cornerDefaults[i][d], d);
        }
        corner->setAddNewLine((i % 2) == 1);
        page->addKnob(corner);
        corner->setHasHostOverlayHandle(true);
        _corners[i] = corner;
    }

    KnobChoicePtr filter = createKnob<KnobChoice>(tr(kResamplerParamFilterTypeLabel));
    filter->setName(kResamplerParamFilterType);
    filter->setHintToolTip(tr(kResamplerParamFilterTypeHint));
    {
        const std::vector<Resampler::ChoiceOption>& filters = Resampler::filterOptions();
        std::vector<ChoiceOption> options;
        for (std::size_t i = 0; i < filters.size(); ++i) {
            options.push_back(ChoiceOption(filters[i].id, filters[i].label, filters[i].hint));
        }
        filter->populateChoices(options);
    }
    filter->setDefaultValue((int)Resampler::eFilterCubic);
    filter->setAddNewLine(false);
    filter->setSpacingBetweenItems(1);
    page->addKnob(filter);
    op().filter = filter;

    KnobBoolPtr clamp = createKnob<KnobBool>(tr(kResamplerParamFilterClampLabel));
    clamp->setName(kResamplerParamFilterClamp);
    clamp->setHintToolTip(tr(kResamplerParamFilterClampHint));
    clamp->setDefaultValue(false);
    clamp->setAddNewLine(false);
    clamp->setSpacingBetweenItems(1);
    page->addKnob(clamp);
    op().clamp = clamp;

    KnobBoolPtr blackOutside = createKnob<KnobBool>(tr(kResamplerParamFilterBlackOutsideLabel));
    blackOutside->setName(kResamplerParamFilterBlackOutside);
    blackOutside->setHintToolTip(tr(kResamplerParamFilterBlackOutsideHint));
    blackOutside->setDefaultValue(true);
    page->addKnob(blackOutside);
    op().blackOutside = blackOutside;

    addStageKnobs(page);

    refreshVisibility();
    updateClampEnabled(getApp()->getTimeLine()->currentFrame(), ViewIdx(0));
} // AutoStraighten::initializeKnobs

void
AutoStraighten::refreshVisibility()
{
    KnobChoicePtr lensModel = op().lensModel.lock();
    KnobChoicePtr structure = _structure.lock();
    const bool specific = lensModel && (lensModel->getValue() == (int)eLensModelSpecific);
    const bool rectangle = structure && (structure->getValue() == (int)eStructureRectangle);

    setSecretAndDisabled(op().focalLength.lock(), !specific);
    setSecretAndDisabled(op().cropFactor.lock(), !specific);
    setSecretAndDisabled(op().lensDependence.lock(), !specific);
    setSecretAndDisabled(op().aspectAdjust.lock(), !specific);
    KnobChoicePtr lineOrientation = _lineOrientation.lock();
    if (lineOrientation) {
        // Not disabled here: the curve context enables it while a line is selected.
        lineOrientation->setSecret(rectangle);
    }
    for (int i = 0; i < 4; ++i) {
        setSecretAndDisabled(_corners[i].lock(), !rectangle);
    }
}

void
AutoStraighten::updateClampEnabled(double time,
                                   ViewSpec view)
{
    KnobChoicePtr filter = op().filter.lock();
    KnobBoolPtr clamp = op().clamp.lock();

    if (!filter || !clamp) {
        return;
    }
    clamp->setAllDimensionsEnabled(Resampler::filterHonoursClamp((Resampler::FilterEnum)filter->getValueAtTime(time, 0, view)));
}

void
AutoStraighten::onKnobsLoaded()
{
    KnobButtonPtr defaultAction = _ui->selectAllAction.lock();

    _ui->setCurrentTool(defaultAction);
    _ui->onRoleChangedInternal(_ui->selectToolGroup.lock());
    _ui->onToolChangedInternal(defaultAction);
    refreshVisibility();
    updateClampEnabled(getApp()->getTimeLine()->currentFrame(), ViewIdx(0));
}

bool
AutoStraighten::knobChanged(KnobI* k,
                            ValueChangedReasonEnum /*reason*/,
                            ViewSpec view,
                            double time,
                            bool /*originatedFromMainThread*/)
{
    bool ret = false;

    if (k && _ui->onKnobChanged(k, time, &ret)) {
        return ret;
    }
    if (k && (k->getName() == kAutoStraightenParamLineOrientation)) {
        // The node's knob edits every selected line: write the value to each line's own knob,
        // which the fit reads.
        RotoCurveContextPtr context = getNode()->getCurveContext();
        KnobChoicePtr master = _lineOrientation.lock();
        if (context && master && !_settingLineOrientation) {
            const int value = master->getValue();
            const std::list<RotoDrawableItemPtr> selected = context->getSelectedCurves();
            for (std::list<RotoDrawableItemPtr>::const_iterator it = selected.begin(); it != selected.end(); ++it) {
                KnobChoicePtr own = lineOrientationKnob(*it);
                if (own && (ownOrientation(own) != value)) {
                    own->setValue(value, ViewSpec::all(), 0, true);
                    context->emitItemPanelColumnsChanged(*it);
                }
            }
        }

        return true;
    }

    const ViewIdx viewIdx = (view.isAll() || view.isCurrent()) ? ViewIdx(0) : ViewIdx(view);
    KnobChoicePtr filterKnob = op().filter.lock();

    if (filterKnob && (k == filterKnob.get())) {
        updateClampEnabled(time, viewIdx);

        return true;
    }
    KnobChoicePtr lensModel = op().lensModel.lock();
    KnobChoicePtr structure = _structure.lock();
    if ((lensModel && (k == lensModel.get())) || (structure && (k == structure.get()))) {
        refreshVisibility();

        return true;
    }
    const KnobButtonPtr buttons[3] = { _fitVertical.lock(), _fitHorizontal.lock(), _fitBoth.lock() };
    for (int b = 0; b < 3; ++b) {
        if (buttons[b] && (k == buttons[b].get())) {
            (void)fit((FitButtonEnum)b, time, viewIdx);

            return true;
        }
    }

    return false;
}

Ashift::Model
AutoStraighten::getModel(double time,
                         ViewIdx view) const
{
    const AutoStraightenOp& o = op();
    const auto value = [&](const KnobDoubleWPtr& w, double fallback) {
        KnobDoublePtr knob = w.lock();
        return knob ? knob->getValueAtTime(time, 0, view) : fallback;
    };
    KnobChoicePtr lensModel = o.lensModel.lock();

    return makeModel(value(o.rotation, 0.), value(o.lensShiftV, 0.), value(o.lensShiftH, 0.), value(o.shear, 0.),
                     lensModel && (lensModel->getValueAtTime(time, 0, view) == (int)eLensModelSpecific),
                     value(o.focalLength, Ashift::kDefaultFocalLength), value(o.cropFactor, 1.), value(o.lensDependence, 100.),
                     value(o.aspectAdjust, 1.));
}

void
AutoStraighten::getSourceFormat(double time,
                                ViewIdx view,
                                RectI* format,
                                double* par) const
{
    NodeMetadataConstPtr metadata = getInput(0) ? getInputMetadataAt(0, time, view) : NodeMetadataConstPtr();

    if (metadata) {
        *format = metadata->getOutputFormat();
        *par = metadata->getPixelAspectRatio(-1);
    } else {
        Format f;
        getApp()->getProject()->getProjectDefaultFormat(&f);
        *format = RectI(f.x1, f.y1, f.x2, f.y2);
        *par = f.getPixelAspectRatio();
    }
    if (*par <= 0.) {
        *par = 1.;
    }
}

AutoStraighten::Geometry
AutoStraighten::computeGeometry(const RectI& source,
                                double par,
                                const Ashift::Model& model,
                                Ashift::CropModeEnum crop) const
{
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_hasCachedGeometry && (_cachedGeometry.source == source) && (_cachedGeometry.par == par) && sameModel(_cachedModel, model)
            && (_cachedCrop == crop)) {
            return _cachedGeometry;
        }
    }

    Geometry g;
    g.source = source;
    g.par = par;
    g.width = (int)std::lround(source.width() * par);
    g.height = source.height();
    g.output = source;
    g.outWidth = g.width;
    g.outHeight = g.height;
    if ((g.width > 0) && (g.height > 0) && !Ashift::isNeutral(model, 0., 1., 0., 1.)) {
        CropOverride cropOverride;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            cropOverride = _cropOverride;
        }
        Ashift::CropBox box;
        if (cropOverride) {
            (void)cropOverride(model, g.width, g.height, crop, &box);
        } else if (!Ashift::computeCrop(model, g.width, g.height, crop, &box)) {
            box = Ashift::CropBox();
        }
        int ow = 0, oh = 0;
        Ashift::outputSize(model, g.width, g.height, box.cl, box.cr, box.ct, box.cb, &ow, &oh);
        // darktable's modify_roi_out() leaves an output under 4 pixels unprocessed.
        if ((ow >= kMinimumOutputSize) && (oh >= kMinimumOutputSize)) {
            g.crop = box;
            g.outWidth = ow;
            g.outHeight = oh;
            const int outPixels = (std::max)(1, (int)std::lround(ow / par));
            g.output = RectI(source.x1, source.y1, source.x1 + outPixels, source.y1 + oh);
            g.bypass = false;
        }
    }

    std::lock_guard<std::mutex> lock(_mutex);
    _cachedGeometry = g;
    _cachedModel = model;
    _cachedCrop = crop;
    _hasCachedGeometry = true;

    return g;
} // AutoStraighten::computeGeometry

AutoStraighten::Geometry
AutoStraighten::getGeometry(double time,
                            ViewIdx view) const
{
    RectI format;
    double par = 1.;
    getSourceFormat(time, view, &format, &par);
    KnobChoicePtr autoCrop = op().autoCrop.lock();
    const Ashift::CropModeEnum crop = autoCrop ? (Ashift::CropModeEnum)autoCrop->getValueAtTime(time, 0, view) : Ashift::eCropLargest;

    return computeGeometry(format, par, getModel(time, view), crop);
}

bool
AutoStraighten::canonicalMatrix(const Geometry& g,
                                const Ashift::Model& model,
                                bool invert,
                                TransformMath::Mat3* matrix)
{
    if (g.bypass) {
        return false;
    }
    using TransformMath::Mat3;
    const double par = g.par;
    // darktable's process(): output pixel (i, j) samples the input at hinv * (i + cx, j + cy), with
    // pixel centres at integers, y down, in square pixels.
    const double cx = (g.crop.cr > g.crop.cl) ? g.outWidth / (g.crop.cr - g.crop.cl) * g.crop.cl : 0.;
    const double cy = (g.crop.cb > g.crop.ct) ? g.outHeight / (g.crop.cb - g.crop.ct) * g.crop.ct : 0.;
    const Mat3 toOutput(1., 0., -(0.5 + g.output.x1) * par + cx,
                        0., -1., (g.output.y2 - 0.5) + cy,
                        0., 0., 1.);
    const Mat3 fromInput(1., 0., (0.5 + g.source.x1) * par,
                         0., -1., g.source.y2 - 0.5,
                         0., 0., 1.);
    const Mat3 m = fromInput * Ashift::homography(model, g.width, g.height, true) * toOutput;

    if (!invert) {
        *matrix = m;

        return true;
    }

    return m.inverse(matrix);
}

void
AutoStraighten::setCropOverride(const CropOverride& cropOverride)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _cropOverride = cropOverride;
    _hasCachedGeometry = false;
}

int
AutoStraighten::fitAxes(FitButtonEnum button,
                        FitParametersEnum parameters)
{
    switch (parameters) {
    case eFitParametersRotation:
        return (button == eFitButtonVertical)  ? Ashift::eFitRotationVerticalLines
            : (button == eFitButtonHorizontal) ? Ashift::eFitRotationHorizontalLines
                                               : Ashift::eFitRotationBothLines;
    case eFitParametersLensShift:
        return (button == eFitButtonVertical)  ? Ashift::eFitVerticallyNoRotation
            : (button == eFitButtonHorizontal) ? Ashift::eFitHorizontallyNoRotation
                                               : Ashift::eFitBothNoRotation;
    case eFitParametersRotationAndLensShift:
        return (button == eFitButtonVertical)  ? Ashift::eFitVertically
            : (button == eFitButtonHorizontal) ? Ashift::eFitHorizontally
                                               : Ashift::eFitBoth;
    case eFitParametersAll:
    default:
        return (button == eFitButtonVertical)  ? Ashift::eFitVertically
            : (button == eFitButtonHorizontal) ? Ashift::eFitHorizontally
                                               : Ashift::eFitBothShear;
    }
}

Ashift::LineSet
AutoStraighten::rectangleLines(double time,
                               ViewIdx view) const
{
    RectI format;
    double par = 1.;
    getSourceFormat(time, view, &format, &par);
    // Canonical to darktable's square-pixel input coordinates.
    const double x0 = (0.5 + format.x1) * par;
    const double y0 = format.y2 - 0.5;
    // darktable's order: top left, bottom left, top right, bottom right.
    const int order[4] = { 0, 2, 1, 3 };
    double pts[8];
    for (int i = 0; i < 4; ++i) {
        KnobDoublePtr corner = _corners[order[i]].lock();
        const double x = corner ? corner->getValueAtTime(time, 0, view) : 0.;
        const double y = corner ? corner->getValueAtTime(time, 1, view) : 0.;
        pts[2 * i] = x - x0;
        pts[2 * i + 1] = y0 - y;
    }

    return Ashift::quadLines(pts, (int)std::lround(format.width() * par), format.height());
}

Ashift::LineSet
AutoStraighten::drawnLines(double time,
                           ViewIdx view) const
{
    RectI format;
    double par = 1.;
    getSourceFormat(time, view, &format, &par);
    // Canonical to darktable's square-pixel input coordinates, as rectangleLines().
    const double x0 = (0.5 + format.x1) * par;
    const double y0 = format.y2 - 0.5;
    std::vector<Ashift::DrawnLine> segments;
    RotoCurveContextPtr context = getNode()->getCurveContext();
    if (context) {
        const std::list<RotoDrawableItemPtr> curves = context->getCurvesByRenderOrder(true);
        for (std::list<RotoDrawableItemPtr>::const_iterator it = curves.begin(); it != curves.end(); ++it) {
            BezierPtr bezier = std::dynamic_pointer_cast<Bezier>(*it);
            LineOrientationEnum orientation;
            if (!bezier || !bezier->isOpenBezier() || !bezier->isActivated(time) || !getLineOrientation(bezier, &orientation)) {
                continue;
            }
            Transform::Matrix3x3 transform;
            bezier->getTransformAtTime(time, &transform);
            const int count = bezier->getControlPointsCount();
            bool havePrevious = false;
            double px = 0., py = 0.;
            for (int i = 0; i < count; ++i) {
                BezierCPPtr cp = bezier->getControlPointAtIndex(i);
                Transform::Point3D p;
                p.z = 1.;
                if (!cp) {
                    continue;
                }
                cp->getPositionAtTime(false /*useGuiCurves*/, time, view, &p.x, &p.y);
                p = Transform::matApply(transform, p);
                const double x = p.x / p.z - x0;
                const double y = y0 - p.y / p.z;
                if (havePrevious) {
                    Ashift::DrawnLine segment;
                    segment.x1 = px;
                    segment.y1 = py;
                    segment.x2 = x;
                    segment.y2 = y;
                    segment.vertical = (orientation == eLineOrientationVertical);
                    segments.push_back(segment);
                }
                px = x;
                py = y;
                havePrevious = true;
            }
        }
    }

    return Ashift::drawnLines(segments, (int)std::lround(format.width() * par), format.height());
} // AutoStraighten::drawnLines

bool
AutoStraighten::getLineOrientation(const RotoItemPtr& item,
                                   LineOrientationEnum* orientation)
{
    KnobChoicePtr k = lineOrientationKnob(item);
    if (!k) {
        return false;
    }
    *orientation = (ownOrientation(k) == (int)eLineOrientationHorizontal) ? eLineOrientationHorizontal : eLineOrientationVertical;

    return true;
}

void
AutoStraighten::setLineOrientation(const RotoDrawableItemPtr& item,
                                   LineOrientationEnum orientation)
{
    KnobChoicePtr k = lineOrientationKnob(item);
    if (!k) {
        return;
    }
    k->setValue((int)orientation, ViewSpec::all(), 0, true);
    // A selected item shows through the node's knob: show the new value there without writing it
    // back to the other selected items.
    KnobChoicePtr master = _lineOrientation.lock();
    if (master && k->isSlave(0)) {
        _settingLineOrientation = true;
        master->setValue((int)orientation, ViewSpec::all(), 0, true);
        _settingLineOrientation = false;
    }
    RotoCurveContextPtr context = getNode()->getCurveContext();
    if (context) {
        context->emitItemPanelColumnsChanged(item);
    }
}

void
AutoStraighten::setLineOrientationFromDirection(const BezierPtr& line,
                                                double time)
{
    if (!line || (line->getControlPointsCount() < 2)) {
        return;
    }
    BezierCPPtr first = line->getControlPointAtIndex(0);
    BezierCPPtr last = line->getControlPointAtIndex(line->getControlPointsCount() - 1);
    double x1 = 0., y1 = 0., x2 = 0., y2 = 0.;
    if (!first || !last) {
        return;
    }
    first->getPositionAtTime(false /*useGuiCurves*/, time, ViewIdx(0), &x1, &y1);
    last->getPositionAtTime(false /*useGuiCurves*/, time, ViewIdx(0), &x2, &y2);
    setLineOrientation(line, (std::fabs(x2 - x1) > std::fabs(y2 - y1)) ? eLineOrientationHorizontal : eLineOrientationVertical);
}

void
AutoStraighten::onCurveItemCreated(const RotoDrawableItemPtr& item)
{
    if (!std::dynamic_pointer_cast<Bezier>(item)) {
        return;
    }
    KnobChoicePtr orientation = makeLineOrientationKnob(NULL);
    item->addHostKnob(orientation);

    // Keep the panel's Orientation column current.
    RotoCurveContextPtr context = getNode()->getCurveContext();
    if (context && orientation->getSignalSlotHandler()) {
        RotoDrawableItemWPtr weakItem = item;
        std::weak_ptr<RotoCurveContext> weakContext = context;
        QObject::connect(orientation->getSignalSlotHandler().get(), &KnobSignalSlotHandler::valueChanged, context.get(),
                         [weakItem, weakContext](ViewSpec, int, int) {
                             RotoCurveContextPtr c = weakContext.lock();
                             RotoDrawableItemPtr i = weakItem.lock();
                             if (c && i) {
                                 c->emitItemPanelColumnsChanged(i);
                             }
                         });
    }
}

std::vector<RotoCurvePanelHook::Column>
AutoStraighten::getColumns() const
{
    std::vector<Column> columns(1);
    columns[0].header = tr("Orientation").toStdString();
    columns[0].toolTip = tr("Whether the guide line should be vertical or horizontal once corrected.").toStdString();

    return columns;
}

std::string
AutoStraighten::getItemText(const RotoItemPtr& item,
                            int column) const
{
    LineOrientationEnum orientation;
    if ((column != 0) || !getLineOrientation(item, &orientation)) {
        return std::string();
    }

    return (orientation == eLineOrientationHorizontal) ? tr("Horizontal").toStdString() : tr("Vertical").toStdString();
}

std::vector<RotoCurvePanelHook::MenuAction>
AutoStraighten::getItemMenuActions(const std::list<RotoItemPtr>& items) const
{
    std::vector<MenuAction> actions;
    bool any = false;
    for (std::list<RotoItemPtr>::const_iterator it = items.begin(); it != items.end(); ++it) {
        any = any || bool(lineOrientationKnob(*it));
    }
    if (!any) {
        return actions;
    }
    actions.resize(2);
    actions[0].id = kAutoStraightenActionSetVertical;
    actions[0].label = tr("Set Vertical").toStdString();
    actions[1].id = kAutoStraightenActionSetHorizontal;
    actions[1].label = tr("Set Horizontal").toStdString();

    return actions;
}

void
AutoStraighten::onItemMenuActionTriggered(const std::string& actionId,
                                          const std::list<RotoItemPtr>& items)
{
    RotoCurveContextPtr context = getNode()->getCurveContext();
    if (!context) {
        return;
    }
    int value;
    if (actionId == kAutoStraightenActionSetVertical) {
        value = (int)eLineOrientationVertical;
    } else if (actionId == kAutoStraightenActionSetHorizontal) {
        value = (int)eLineOrientationHorizontal;
    } else {
        return;
    }
    std::list<RotoDrawableItemPtr> lines;
    for (std::list<RotoItemPtr>::const_iterator it = items.begin(); it != items.end(); ++it) {
        if (lineOrientationKnob(*it)) {
            lines.push_back(std::dynamic_pointer_cast<RotoDrawableItem>(*it));
        }
    }
    if (!lines.empty()) {
        pushUndoCommand(new LineOrientationCommand(getNode(), lines, value));
    }
}

void
AutoStraighten::getPluginShortcuts(std::list<PluginActionShortcut>* shortcuts)
{
    const int toolSet = eRotoCurveToolSetSelection | eRotoCurveToolSetPointsEdition;

    RotoCurveInteract::getViewerButtonShortcuts(shortcuts);
    RotoCurveInteract::getToolbarShortcuts(toolSet, shortcuts);
    shortcuts->push_back(PluginActionShortcut(kAutoStraightenUIParamGuideLinesToolButton, kAutoStraightenUIParamGuideLinesToolButtonLabel, Key_V));
    RotoCurveInteract::getRightClickMenuShortcuts(shortcuts);
}

void
AutoStraighten::refreshExtraStateAfterTimeChanged(bool isPlayback,
                                                  double time)
{
    ImageNode<AutoStraightenOp>::refreshExtraStateAfterTimeChanged(isPlayback, time);
    _ui->refreshAfterTimeChanged(time);
}

void
AutoStraighten::drawOverlay(double time,
                            const RenderScale& /*renderScale*/,
                            ViewIdx /*view*/)
{
    _ui->drawOverlay(time);
}

bool
AutoStraighten::onOverlayPenDown(double time,
                                 const RenderScale& /*renderScale*/,
                                 ViewIdx /*view*/,
                                 const QPointF& /*viewportPos*/,
                                 const QPointF& pos,
                                 double pressure,
                                 double timestamp,
                                 PenType pen)
{
    return _ui->onOverlayPenDown(time, pos, pressure, timestamp, pen);
}

bool
AutoStraighten::onOverlayPenMotion(double time,
                                   const RenderScale& /*renderScale*/,
                                   ViewIdx /*view*/,
                                   const QPointF& /*viewportPos*/,
                                   const QPointF& pos,
                                   double pressure,
                                   double timestamp)
{
    return _ui->onOverlayPenMotion(time, pos, pressure, timestamp);
}

bool
AutoStraighten::onOverlayPenUp(double /*time*/,
                               const RenderScale& /*renderScale*/,
                               ViewIdx /*view*/,
                               const QPointF& /*viewportPos*/,
                               const QPointF& /*pos*/,
                               double /*pressure*/,
                               double /*timestamp*/)
{
    return _ui->onOverlayPenUp();
}

bool
AutoStraighten::onOverlayPenDoubleClicked(double /*time*/,
                                          const RenderScale& /*renderScale*/,
                                          ViewIdx /*view*/,
                                          const QPointF& /*viewportPos*/,
                                          const QPointF& pos)
{
    return _ui->onOverlayPenDoubleClicked(pos);
}

bool
AutoStraighten::onOverlayKeyDown(double /*time*/,
                                 const RenderScale& /*renderScale*/,
                                 ViewIdx /*view*/,
                                 Key key,
                                 KeyboardModifiers /*modifiers*/)
{
    return _ui->onOverlayKeyDown(key);
}

bool
AutoStraighten::onOverlayKeyUp(double /*time*/,
                               const RenderScale& /*renderScale*/,
                               ViewIdx /*view*/,
                               Key key,
                               KeyboardModifiers /*modifiers*/)
{
    return _ui->onOverlayKeyUp(key);
}

bool
AutoStraighten::onOverlayFocusLost(double /*time*/,
                                   const RenderScale& /*renderScale*/,
                                   ViewIdx /*view*/)
{
    return _ui->onOverlayFocusLost();
}

void
AutoStraighten::onInteractViewportSelectionCleared()
{
    _ui->onViewportSelectionCleared();
}

void
AutoStraighten::onInteractViewportSelectionUpdated(const RectD& rectangle,
                                                   bool onRelease)
{
    _ui->onViewportSelectionUpdated(rectangle, onRelease);
}

bool
AutoStraighten::fit(FitButtonEnum button,
                    double time,
                    ViewIdx view)
{
    clearPersistentMessage(false);

    KnobChoicePtr structure = _structure.lock();
    const Ashift::LineSet lines = (structure && (structure->getValue() == (int)eStructureRectangle)) ? rectangleLines(time, view) : drawnLines(time, view);
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _lastLines = lines;
        _lastLinesTime = time;
        _hasLastLines = true;
    }

    KnobChoicePtr fitParameters = _fitParameters.lock();
    const FitParametersEnum parameters = fitParameters ? (FitParametersEnum)fitParameters->getValue() : eFitParametersAll;
    Ashift::Model model = getModel(time, view);
    switch (Ashift::fit(lines, fitAxes(button, parameters), &model)) {
    case Ashift::eNmsSuccess:
        break;
    case Ashift::eNmsNotEnoughLines:
        setPersistentMessage(eMessageTypeError, kNotEnoughStructure);

        return false;
    case Ashift::eNmsDidNotConverge:
    case Ashift::eNmsInsane:
        setPersistentMessage(eMessageTypeError, kFitFailed);

        return false;
    }

    const AutoStraightenOp& o = op();
    const KnobDoublePtr knobs[4] = { o.rotation.lock(), o.lensShiftV.lock(), o.lensShiftH.lock(), o.shear.lock() };
    const double values[4] = { -model.rotation, model.lensShiftV, model.lensShiftH, model.shear };
    AppInstancePtr app = getApp();
    const bool oneUndoStep = app && !app->isCreatingPythonGroup();
    if (oneUndoStep) {
        setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOnCreateNewCommand);
    }
    beginChanges();
    for (int i = 0; i < 4; ++i) {
        if (!knobs[i]) {
            continue;
        }
        if (knobs[i]->isAnimated(0, ViewSpec::all())) {
            knobs[i]->setValueAtTime(time, values[i], ViewSpec::all(), 0, eValueChangedReasonPluginEdited, NULL);
        } else {
            knobs[i]->setValue(values[i], ViewSpec::all(), 0, eValueChangedReasonPluginEdited, NULL);
        }
    }
    endChanges();
    if (oneUndoStep) {
        setMultipleParamsEditLevel(KnobHolder::eMultipleParamsEditOff);
    }

    return true;
} // AutoStraighten::fit

Ashift::LineSet
AutoStraighten::lines(double time) const
{
    std::lock_guard<std::mutex> lock(_mutex);

    return (_hasLastLines && (_lastLinesTime == time)) ? _lastLines : Ashift::LineSet();
}

StatusEnum
AutoStraighten::getRegionOfDefinition(U64 hash,
                                      double time,
                                      const RenderScale& scale,
                                      ViewIdx view,
                                      RectD* rod)
{
    const StatusEnum status = ImageNode<AutoStraightenOp>::getRegionOfDefinition(hash, time, scale, view, rod);

    if ((status != eStatusOK) || !getInput(0)) {
        return status;
    }
    const Geometry g = getGeometry(time, view);
    if (!g.bypass) {
        *rod = rectIntersection(*rod, toCanonical(g.output, g.par));
    }

    return eStatusOK;
}

StatusEnum
AutoStraighten::getPreferredMetadataAt(double time,
                                       ViewIdx view,
                                       NodeMetadata& metadata)
{
    // The metadata being computed hold the source's format and pixel aspect ratio.
    const RectI source = metadata.getOutputFormat();
    const double par = (metadata.getPixelAspectRatio(0) > 0.) ? metadata.getPixelAspectRatio(0) : 1.;
    KnobChoicePtr autoCrop = op().autoCrop.lock();
    const Ashift::CropModeEnum crop = autoCrop ? (Ashift::CropModeEnum)autoCrop->getValueAtTime(time, 0, view) : Ashift::eCropLargest;
    const Geometry g = computeGeometry(source, par, getModel(time, view), crop);

    metadata.setOutputFormat(g.output);

    return eStatusOK;
}

NATRON_NAMESPACE_EXIT
