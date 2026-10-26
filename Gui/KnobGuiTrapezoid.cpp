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

#include "KnobGuiTrapezoid.h"

#include <algorithm>
#include <cmath>
#include <list>
#include <vector>

#include <QColor>
#include <QHBoxLayout>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>
#include <QVBoxLayout>

#include "Engine/AppInstance.h"
#include "Engine/Knob.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"

#include "Gui/DisplayLegendColour.h"
#include "Gui/KnobUndoCommand.h"

NATRON_NAMESPACE_ENTER

namespace {
// How far from a handle, in pixels, a press still grabs it.
const double kGrabRadius = 6.;
// The handles' radius, which is also the strip's side margin so the end handles stay whole.
const double kHandleRadius = 4.;
// Hue's outer points stay this short of a whole turn apart, so the spans bake() reads back
// going up the circle are the ones drawn.
const double kTurnGap = 1e-6;

double
wrapUnit(double x)
{
    const double w = x - std::floor(x);

    return (w >= 1.) ? 0. : w;
}

double
upTheCircle(double from,
            double to)
{
    const double d = to - from;

    return d - std::floor(d);
}

// The gradients' fixed components: a hue strip shows hues at a lively saturation and a middling
// luma, a saturation strip at that luma.
void
referenceComponents(ColorMath::KeyModelEnum model,
                    float* sat,
                    float* luma)
{
    switch (model) {
    case ColorMath::eKeyModelHSL:
        *sat = 0.8f;
        *luma = 0.5f;
        break;
    case ColorMath::eKeyModelOkLCh:
        *sat = 0.12f;
        *luma = 0.7f;
        break;
    case ColorMath::eKeyModelHSV:
    default:
        *sat = 0.8f;
        *luma = 0.9f;
        break;
    }
}

TrapezoidStrip::DisplayConverter
makeDisplayConverter(const KnobIPtr& knob)
{
    KnobHolder* holder = knob ? knob->getHolder() : 0;
    AppInstancePtr app = holder ? holder->getApp() : AppInstancePtr();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();
    ProjectColorManagementPtr colorManagement = project ? project->getColorManagement() : ProjectColorManagementPtr();

    if (!colorManagement) {
        return TrapezoidStrip::DisplayConverter();
    }
    std::weak_ptr<ProjectColorManagement> weak = colorManagement;

    return [weak](float* rgb, int count) {
        ProjectColorManagementPtr cm = weak.lock();

        if (!cm) {
            return;
        }
        for (int i = 0; i < count; ++i) {
            cm->workingToColorPicking(&rgb[3 * i], &rgb[3 * i + 1], &rgb[3 * i + 2]);
        }
    };
}
} // anonymous namespace

TrapezoidStrip::TrapezoidStrip(KnobDouble::RangeWidgetRoleEnum role,
                               QWidget* parent)
    : QWidget(parent)
    , _role(role)
    , _model(ColorMath::eKeyModelHSV)
    , _basis()
    , _hueWeight(0.f)
    , _toDisplay()
    , _onEdit()
    , _dragPart(ePartNone)
    , _dragResolved(false)
    , _dragStarted(false)
    , _dragShift(false)
    , _dragLow(0.)
    , _dragHigh(1.)
    , _pressX(0.)
{
    for (int i = 0; i < 4; ++i) {
        _values[i] = 0.;
        _dragStart[i] = 0.;
    }
    for (int i = 0; i < 3; ++i) {
        _source[i] = 0.f;
    }
    setMouseTracking(false);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

TrapezoidStrip::~TrapezoidStrip()
{
}

void
TrapezoidStrip::setValues(const double* values)
{
    for (int i = 0; i < 4; ++i) {
        _values[i] = values[i];
    }
    update();
}

void
TrapezoidStrip::getValues(double* values) const
{
    for (int i = 0; i < 4; ++i) {
        values[i] = _values[i];
    }
}

void
TrapezoidStrip::setContext(ColorMath::KeyModelEnum model,
                           const ColorMath::OkLabBasis& basis,
                           const float* sourceComponents,
                           float hueWeight)
{
    _model = model;
    _basis = basis;
    for (int i = 0; i < 3; ++i) {
        _source[i] = sourceComponents[i];
    }
    _hueWeight = hueWeight;
    update();
}

void
TrapezoidStrip::setDisplayConverter(const DisplayConverter& converter)
{
    _toDisplay = converter;
    update();
}

void
TrapezoidStrip::setEditCallback(const EditCallback& callback)
{
    _onEdit = callback;
}

void
TrapezoidStrip::computeDomain(double* low,
                              double* high) const
{
    if (isHue()) {
        double u[4];
        const double rise = upTheCircle(_values[0], _values[1]);
        const double full = upTheCircle(_values[1], _values[2]);
        u[1] = _values[0] + rise;
        u[2] = u[1] + full;
        const double centre = (u[1] + u[2]) * 0.5;
        *low = centre - 0.5;
        *high = centre + 0.5;

        return;
    }
    double ordered[4];
    for (int i = 0; i < 4; ++i) {
        ordered[i] = (i > 0) ? (std::max)(_values[i], ordered[i - 1]) : _values[i];
    }
    const double top = ((_role == KnobDouble::eRangeWidgetRoleSaturation) && (_model == ColorMath::eKeyModelOkLCh)) ? 0.4 : 1.;
    *low = (std::min)(0., ordered[0]);
    *high = (std::max)(top, ordered[3]);
    if (!(*high > *low)) {
        *high = *low + 1.;
    }
}

void
TrapezoidStrip::getDomain(double* low,
                          double* high) const
{
    if (isDragging()) {
        *low = _dragLow;
        *high = _dragHigh;

        return;
    }
    computeDomain(low, high);
}

void
TrapezoidStrip::positionsOf(const double* values,
                            double* positions) const
{
    if (!isHue()) {
        for (int i = 0; i < 4; ++i) {
            positions[i] = values[i];
        }

        return;
    }
    positions[0] = values[0];
    positions[1] = positions[0] + upTheCircle(values[0], values[1]);
    positions[2] = positions[1] + upTheCircle(values[1], values[2]);
    positions[3] = positions[2] + upTheCircle(values[2], values[3]);
    // The whole turns that bring the plateau into the drawn domain, which a drag holds while the
    // points it writes are wrapped.
    double low, high;
    getDomain(&low, &high);
    const double centre = (positions[1] + positions[2]) * 0.5;
    const double turns = std::floor((low + high) * 0.5 - centre + 0.5);
    for (int i = 0; i < 4; ++i) {
        positions[i] += turns;
    }
}

void
TrapezoidStrip::getPositions(double* positions) const
{
    positionsOf(_values, positions);
}

double
TrapezoidStrip::valueToX(double value) const
{
    double low, high;

    getDomain(&low, &high);
    const double span = (std::max)(1., width() - 2. * kHandleRadius);

    return kHandleRadius + (value - low) / (high - low) * span;
}

double
TrapezoidStrip::xToValue(double x) const
{
    double low, high;

    getDomain(&low, &high);
    const double span = (std::max)(1., width() - 2. * kHandleRadius);

    return low + (x - kHandleRadius) / span * (high - low);
}

TrapezoidStrip::PartEnum
TrapezoidStrip::partAt(double x) const
{
    double p[4];

    getPositions(p);
    int best = -1;
    double bestDistance = kGrabRadius;
    for (int i = 0; i < 4; ++i) {
        const double d = std::abs(valueToX(p[i]) - x);
        // Strictly nearer only, so among coincident handles the lowest is reported.
        if (d < bestDistance) {
            best = i;
            bestDistance = d;
        }
    }
    if (best >= 0) {
        return (PartEnum)best;
    }
    if ((x > valueToX(p[1])) && (x < valueToX(p[2]))) {
        return ePartPlateau;
    }

    return ePartNone;
}

QSize
TrapezoidStrip::sizeHint() const
{
    return QSize(fontMetrics().height() * 16, fontMetrics().height() * 2);
}

QSize
TrapezoidStrip::minimumSizeHint() const
{
    return QSize(fontMetrics().height() * 6, fontMetrics().height() * 2);
}

void
TrapezoidStrip::componentsAt(double value,
                             float* comps) const
{
    float refSat, refLuma;

    referenceComponents(_model, &refSat, &refLuma);
    switch (_role) {
    case KnobDouble::eRangeWidgetRoleHue:
        comps[0] = (float)wrapUnit(value);
        comps[1] = refSat;
        comps[2] = refLuma;
        break;
    case KnobDouble::eRangeWidgetRoleSaturation:
        // Grey towards the source's hue, or towards red for a source with no hue.
        comps[0] = (_hueWeight > 0.f) ? _source[0] : 0.f;
        comps[1] = (float)value;
        comps[2] = refLuma;
        break;
    case KnobDouble::eRangeWidgetRoleLuma:
    default:
        comps[0] = 0.f;
        comps[1] = 0.f;
        comps[2] = (float)value;
        break;
    }
}

void
TrapezoidStrip::paintEvent(QPaintEvent* /*e*/)
{
    QPainter painter(this);

    painter.setRenderHint(QPainter::Antialiasing, true);
    if (!isEnabled()) {
        painter.setOpacity(0.5);
    }
    const int w = (std::max)(1, width());
    const QRectF band(kHandleRadius, 1., (std::max)(1., width() - 2. * kHandleRadius), (std::max)(1., height() - 2.));

    // The gradient, one column per pixel.
    std::vector<float> rgb(3 * (std::size_t)w);
    for (int x = 0; x < w; ++x) {
        float comps[3];
        componentsAt(xToValue(x + 0.5), comps);
        ColorMath::keyModelToWorking(_model, _basis, comps, &rgb[3 * (std::size_t)x]);
    }
    if (_toDisplay) {
        _toDisplay(rgb.data(), w);
    } else {
        for (std::size_t i = 0; i < rgb.size(); ++i) {
            rgb[i] = (float)srgbEncode(rgb[i]);
        }
    }
    QImage gradient(w, 1, QImage::Format_RGB32);
    for (int x = 0; x < w; ++x) {
        const float* c = &rgb[3 * (std::size_t)x];
        QColor colour;
        colour.setRgbF((std::min)(1.f, (std::max)(0.f, c[0])), (std::min)(1.f, (std::max)(0.f, c[1])), (std::min)(1.f, (std::max)(0.f, c[2])));
        gradient.setPixel(x, 0, colour.rgb());
    }
    painter.drawImage(QRectF(0., band.top(), w, band.height()), gradient);
    painter.setClipRect(band);

    const double top = band.top() + kHandleRadius;
    const double bottom = band.bottom() - kHandleRadius;
    double p[4];
    getPositions(p);

    // The trapezoid, repeated a turn either side for hue so a long ramp shows where it wraps.
    const int copies = isHue() ? 1 : 0;
    for (int turn = -copies; turn <= copies; ++turn) {
        QPolygonF shape;
        shape << QPointF(valueToX(p[0] + turn), bottom) << QPointF(valueToX(p[1] + turn), top)
              << QPointF(valueToX(p[2] + turn), top) << QPointF(valueToX(p[3] + turn), bottom);
        painter.setPen(QPen(QColor(0, 0, 0, 160), 3.));
        painter.setBrush(QColor(255, 255, 255, 50));
        painter.drawPolygon(shape);
        painter.setPen(QPen(QColor(255, 255, 255, 230), 1.5));
        painter.setBrush(Qt::NoBrush);
        painter.drawPolygon(shape);
    }

    // The source colour's component; a source with no chroma has no hue to mark.
    if (!isHue() || (_hueWeight > 0.f)) {
        double s = _source[(_role == KnobDouble::eRangeWidgetRoleHue) ? 0 : ((_role == KnobDouble::eRangeWidgetRoleSaturation) ? 1 : 2)];
        if (isHue()) {
            double low, high;
            getDomain(&low, &high);
            s += std::floor((low + high) * 0.5 - s + 0.5);
        }
        const double x = valueToX(s);
        QPen pen(QColor(0, 0, 0, 220), 1.);
        pen.setDashPattern(QVector<qreal>() << 2. << 2.);
        painter.setPen(pen);
        painter.drawLine(QPointF(x, band.top()), QPointF(x, band.bottom()));
        pen.setColor(QColor(255, 255, 255, 220));
        pen.setDashOffset(2.);
        painter.setPen(pen);
        painter.drawLine(QPointF(x, band.top()), QPointF(x, band.bottom()));
    }

    painter.setClipping(false);
    for (int i = 0; i < 4; ++i) {
        const bool inner = (i == 1) || (i == 2);
        const QPointF centre(valueToX(p[i]), inner ? top : bottom);
        const bool active = isDragging() && _dragResolved && ((int)_dragPart == i);
        painter.setPen(QPen(QColor(0, 0, 0), 1.));
        painter.setBrush(active ? QColor(255, 200, 80) : QColor(255, 255, 255));
        painter.drawEllipse(centre, kHandleRadius, kHandleRadius);
    }
} // TrapezoidStrip::paintEvent

void
TrapezoidStrip::mousePressEvent(QMouseEvent* e)
{
    if ((e->button() != Qt::LeftButton) || !isEnabled()) {
        QWidget::mousePressEvent(e);

        return;
    }
    const double x = e->position().x();
    const PartEnum part = partAt(x);
    if (part == ePartNone) {
        e->ignore();

        return;
    }
    computeDomain(&_dragLow, &_dragHigh);
    _dragPart = part;
    _dragStarted = false;
    _dragShift = (e->modifiers() & Qt::ShiftModifier) != 0;
    _pressX = x;
    positionsOf(_values, _dragStart);
    // A handle sharing its place with another is chosen by the first move's direction.
    _dragResolved = true;
    if (part != ePartPlateau) {
        const double px = valueToX(_dragStart[part]);
        for (int i = 0; i < 4; ++i) {
            if ((i != (int)part) && (std::abs(valueToX(_dragStart[i]) - px) < 0.5)) {
                _dragResolved = false;
            }
        }
    }
    e->accept();
    update();
}

void
TrapezoidStrip::applyDrag(double x)
{
    const double dv = xToValue(x) - xToValue(_pressX);
    const double* s = _dragStart;
    double p[4] = { s[0], s[1], s[2], s[3] };
    const bool hue = isHue();

    if (_dragPart == ePartPlateau) {
        for (int i = 0; i < 4; ++i) {
            p[i] = s[i] + dv;
        }
    } else if (_dragShift && ((_dragPart == ePartInnerLow) || (_dragPart == ePartInnerHigh))) {
        double d = dv;
        if (_dragPart == ePartInnerLow) {
            d = (std::min)(d, s[2] - s[1]);
            if (hue) {
                d = (std::max)(d, s[3] - 1. + kTurnGap - s[0]);
            }
            p[0] = s[0] + d;
            p[1] = s[1] + d;
        } else {
            d = (std::max)(d, s[1] - s[2]);
            if (hue) {
                d = (std::min)(d, s[0] + 1. - kTurnGap - s[3]);
            }
            p[2] = s[2] + d;
            p[3] = s[3] + d;
        }
    } else {
        const int i = (int)_dragPart;
        double lo = (i > 0) ? s[i - 1] : -HUGE_VAL;
        double hi = (i < 3) ? s[i + 1] : HUGE_VAL;
        if (hue && (i == 0)) {
            lo = s[3] - 1. + kTurnGap;
        }
        if (hue && (i == 3)) {
            hi = s[0] + 1. - kTurnGap;
        }
        p[i] = (std::min)(hi, (std::max)(lo, s[i] + dv));
    }
    for (int i = 0; i < 4; ++i) {
        _values[i] = hue ? wrapUnit(p[i]) : p[i];
    }
}

void
TrapezoidStrip::report(GesturePhaseEnum phase)
{
    if (_onEdit) {
        const double values[4] = { _values[0], _values[1], _values[2], _values[3] };
        _onEdit(values, phase);
    }
}

void
TrapezoidStrip::mouseMoveEvent(QMouseEvent* e)
{
    if (!isDragging()) {
        QWidget::mouseMoveEvent(e);

        return;
    }
    const double x = e->position().x();
    if (!_dragResolved) {
        if (x == _pressX) {
            return;
        }
        const double px = valueToX(_dragStart[_dragPart]);
        int chosen = (int)_dragPart;
        for (int i = 0; i < 4; ++i) {
            if (std::abs(valueToX(_dragStart[i]) - px) < 0.5) {
                if (x < _pressX) {
                    chosen = (std::min)(chosen, i);
                } else {
                    chosen = (std::max)(chosen, i);
                }
            }
        }
        _dragPart = (PartEnum)chosen;
        _dragResolved = true;
    }
    applyDrag(x);
    if (!_dragStarted) {
        _dragStarted = true;
        report(eGesturePhaseStart);
    }
    report(eGesturePhaseMove);
    update();
}

void
TrapezoidStrip::mouseReleaseEvent(QMouseEvent* e)
{
    if (!isDragging() || (e->button() != Qt::LeftButton)) {
        QWidget::mouseReleaseEvent(e);

        return;
    }
    if (_dragStarted) {
        report(eGesturePhaseEnd);
    }
    _dragPart = ePartNone;
    _dragStarted = false;
    _dragResolved = false;
    update();
}

KnobGuiTrapezoid::KnobGuiTrapezoid(KnobIPtr knob,
                                   KnobGuiContainerI* container)
    : KnobGuiDouble(knob, container)
    , _rangeKnob(std::dynamic_pointer_cast<KnobDouble>(knob))
    , _strip()
    , _gesture()
{
}

KnobGuiTrapezoid::~KnobGuiTrapezoid()
{
}

TrapezoidStrip*
KnobGuiTrapezoid::getStrip() const
{
    return _strip.data();
}

bool
KnobGuiTrapezoid::wantsTopAlignedLabel() const
{
    return true;
}

QWidget*
KnobGuiTrapezoid::createBelowRowWidget(QWidget* parent)
{
    KnobDoublePtr knob = _rangeKnob.lock();

    if (!knob || !knob->isRangeWidget() || (knob->getDimension() != 4)) {
        return 0;
    }
    TrapezoidStrip* strip = new TrapezoidStrip(knob->getRangeWidgetRole(), parent);
    strip->setObjectName(QString::fromUtf8("TrapezoidStrip_") + QString::fromUtf8(knob->getName().c_str()));
    strip->setToolTip(QString::fromUtf8(knob->getHintToolTip().c_str()));
    strip->setDisplayConverter(makeDisplayConverter(knob));
    strip->setEditCallback([this](const double* values, TrapezoidStrip::GesturePhaseEnum phase) {
        onStripEdited(values, phase);
    });
    _strip = strip;

    // The strip draws the source colour's components in the current model.
    const KnobIPtr drivers[2] = { knob->getRangeWidgetSourceColour(), knob->getRangeWidgetModel() };
    for (const KnobIPtr& driver : drivers) {
        KnobSignalSlotHandlerPtr handler = driver ? driver->getSignalSlotHandler() : KnobSignalSlotHandlerPtr();
        if (handler) {
            QObject::connect(handler.get(), &KnobSignalSlotHandler::valueChanged, this, [this](ViewSpec, int, int) {
                refreshContext();
            });
        }
    }
    refreshContext();

    return strip;
}

void
KnobGuiTrapezoid::addExtraWidgets(QHBoxLayout* /*containerLayout*/)
{
    QVBoxLayout* column = getBelowRowColumnLayout();
    TrapezoidStrip* strip = _strip.data();

    if (!column || !strip) {
        return;
    }
    // The strip goes above the spinboxes' row.
    column->removeWidget(strip);
    column->insertWidget(0, strip);
    KnobDoublePtr knob = _rangeKnob.lock();
    if (knob) {
        double values[4];
        for (int i = 0; i < 4; ++i) {
            values[i] = knob->getValue(i);
        }
        strip->setValues(values);
    }
    strip->show();
}

void
KnobGuiTrapezoid::refreshContext()
{
    KnobDoublePtr knob = _rangeKnob.lock();
    TrapezoidStrip* strip = _strip.data();

    if (!knob || !strip) {
        return;
    }
    KnobColorPtr source = knob->getRangeWidgetSourceColour();
    KnobChoicePtr modelKnob = knob->getRangeWidgetModel();
    int index = modelKnob ? modelKnob->getValue() : 0;
    if ((index < 0) || (index >= (int)ColorMath::eKeyModelCount)) {
        index = 0;
    }
    const ColorMath::KeyModelEnum model = (ColorMath::KeyModelEnum)index;
    double toXYZ[9];
    ColorMath::workingToXYZ(knob->getHolder(), toXYZ);
    const ColorMath::OkLabBasis basis = ColorMath::okLabBasis(toXYZ);
    float rgb[3] = { 0.f, 0.f, 0.f };
    if (source) {
        for (int i = 0; i < 3; ++i) {
            rgb[i] = (float)source->getValue(i);
        }
    }
    float comps[3];
    float hueWeight;
    ColorMath::keyModelComponents(model, basis, rgb, comps, &hueWeight);
    strip->setContext(model, basis, comps, hueWeight);
}

void
KnobGuiTrapezoid::onStripEdited(const double* values,
                                TrapezoidStrip::GesturePhaseEnum phase)
{
    KnobDoublePtr knob = _rangeKnob.lock();

    if (!knob) {
        return;
    }
    if (phase == TrapezoidStrip::eGesturePhaseStart) {
        _gesture.begin();

        return;
    }
    std::list<double> oldValues, newValues;
    for (int i = 0; i < 4; ++i) {
        oldValues.push_back(knob->getValue(i));
        newValues.push_back(values[i]);
    }
    if (oldValues != newValues) {
        pushUndoCommand(new KnobUndoCommand<double>(shared_from_this(), oldValues, newValues, true, _gesture.key()));
    }
    if (phase == TrapezoidStrip::eGesturePhaseEnd) {
        _gesture.end();
    }
}

void
KnobGuiTrapezoid::updateExtraGui(const std::vector<double>& values)
{
    TrapezoidStrip* strip = _strip.data();

    if (!strip || strip->isDragging()) {
        return;
    }
    double v[4];
    KnobDoublePtr knob = _rangeKnob.lock();
    for (int i = 0; i < 4; ++i) {
        if (values.size() >= 4) {
            v[i] = values[i];
        } else {
            v[i] = knob ? knob->getValue(i) : 0.;
        }
    }
    strip->setValues(v);
}

void
KnobGuiTrapezoid::setEnabledExtraGui(bool enabled)
{
    if (_strip) {
        _strip->setEnabled(enabled);
    }
}

void
KnobGuiTrapezoid::_hide()
{
    KnobGuiValue::_hide();
    if (_strip) {
        _strip->hide();
    }
}

void
KnobGuiTrapezoid::_show()
{
    KnobGuiValue::_show();
    if (_strip) {
        _strip->show();
    }
}

NATRON_NAMESPACE_EXIT
