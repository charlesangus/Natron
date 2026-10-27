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

#include <QAbstractButton>
#include <QBoxLayout>
#include <QColor>
#include <QHBoxLayout>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Knob.h"
#include "Engine/Project.h"
#include "Engine/ProjectColorManagement.h"
#include "Engine/Utils.h"

#include "Gui/Button.h"
#include "Gui/DisplayLegendColour.h"
#include "Gui/GuiDefines.h"
#include "Gui/KnobUndoCommand.h"
#include "Gui/Label.h"
#include "Gui/SpinBox.h"
#include "Gui/ThemedIcon.h"

NATRON_NAMESPACE_ENTER

namespace {
// How far from a handle, in pixels, a press still grabs it.
const double kGrabRadius = 6.;
// The handles' radius, which is also the strip's side margin so the end handles stay whole.
const double kHandleRadius = 4.;
// The narrowest and widest domains zooming reaches, in the component's units.
const double kMinSpan = 1e-3;
const double kMaxSpan = 1e3;
// The wheel's angle delta, in eighths of a degree, that halves or doubles the domain: four notches.
const double kWheelDeltaPerDoubling = 480.;
// The most copies of a hue trapezoid drawn a turn apart when zoomed out.
const int kMaxHueCopies = 256;
// The share of the default domain's span added beyond a point that widens it.
const double kFitMargin = 0.05;
// How far past 0 or 1 a point must lie to widen the default domain, so that offsets summing
// to an edge up to rounding leave it at 0 to 1.
const double kFitTolerance = 1e-9;

// The band the gradient fills in a strip of width w and height h.
QRectF
bandOf(int w,
       int h)
{
    return QRectF(kHandleRadius, 1., (std::max)(1., w - 2. * kHandleRadius), (std::max)(1., h - 2.));
}

double
wrapUnit(double x)
{
    const double w = x - std::floor(x);

    return (w >= 1.) ? 0. : w;
}

// The four offsets of values as they key, a hue offset counting up to a turn as
// HSVKeyerOp::bake() reads it.
void
keyedOffsets(const double* values,
             bool hue,
             double* offsets)
{
    KnobDouble::rangeOffsetsOf(values, offsets);
    if (hue) {
        for (int i = 0; i < 4; ++i) {
            offsets[i] = (std::min)(1., offsets[i]);
        }
    }
}

// The outer low, inner low, inner high and outer high points around centre c.
void
pointsAround(double c,
             const double* offsets,
             double* points)
{
    points[1] = c - offsets[1];
    points[0] = points[1] - offsets[0];
    points[2] = c + offsets[2];
    points[3] = points[2] + offsets[3];
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
    , _smooth(false)
    , _toDisplay()
    , _onEdit()
    , _zoomed(false)
    , _viewLow(0.)
    , _viewHigh(1.)
    , _panning(false)
    , _panLow(0.)
    , _panHigh(1.)
    , _panX(0.)
    , _dragPart(ePartNone)
    , _dragResolved(false)
    , _dragStarted(false)
    , _dragShift(false)
    , _dragLow(0.)
    , _dragHigh(1.)
    , _dragCentre(0.)
    , _pressX(0.)
{
    for (int i = 0; i < KnobDouble::eRangeWidgetDimensionCount; ++i) {
        _values[i] = 0.;
        _dragValues[i] = 0.;
    }
    for (int i = 0; i < 4; ++i) {
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
    for (int i = 0; i < KnobDouble::eRangeWidgetDimensionCount; ++i) {
        _values[i] = values[i];
    }
    update();
}

void
TrapezoidStrip::getValues(double* values) const
{
    for (int i = 0; i < KnobDouble::eRangeWidgetDimensionCount; ++i) {
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
TrapezoidStrip::setSmooth(bool smooth)
{
    if (_smooth != smooth) {
        _smooth = smooth;
        update();
    }
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
TrapezoidStrip::getDefaultDomain(double* low,
                                 double* high) const
{
    const double centre = _values[KnobDouble::eRangeWidgetDimensionCentre];

    switch (_role) {
    case KnobDouble::eRangeWidgetRoleHue:
        *low = centre - 0.5;
        *high = centre + 0.5;
        break;
    case KnobDouble::eRangeWidgetRoleSaturation:
    case KnobDouble::eRangeWidgetRoleLuma:
    default: {
        double o[4], p[4];
        keyedOffsets(_values, false, o);
        pointsAround(centre, o, p);
        const bool widenLow = p[0] < -kFitTolerance;
        const bool widenHigh = p[3] > 1. + kFitTolerance;
        *low = widenLow ? p[0] : 0.;
        *high = widenHigh ? p[3] : 1.;
        const double margin = kFitMargin * (*high - *low);
        if (widenLow) {
            *low -= margin;
        }
        if (widenHigh) {
            *high += margin;
        }
        break;
    }
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
    if (_zoomed) {
        *low = _viewLow;
        *high = _viewHigh;

        return;
    }
    getDefaultDomain(low, high);
}

void
TrapezoidStrip::positionsOf(const double* values,
                            double* positions,
                            double* centre) const
{
    double o[4];

    keyedOffsets(values, isHue(), o);
    double c = values[KnobDouble::eRangeWidgetDimensionCentre];
    if (isHue()) {
        // The whole turns that bring the centre nearest the middle of the drawn domain, which a
        // drag holds while the centre it writes is wrapped.
        double low, high;
        getDomain(&low, &high);
        c += std::floor((low + high) * 0.5 - c + 0.5);
    }
    pointsAround(c, o, positions);
    if (centre) {
        *centre = c;
    }
}

void
TrapezoidStrip::getPositions(double* positions,
                             double* centre) const
{
    positionsOf(_values, positions, centre);
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

void
TrapezoidStrip::zoomAround(double x,
                           double factor)
{
    if (!(factor > 0.)) {
        return;
    }
    double low, high;
    getDomain(&low, &high);
    const double value = xToValue(x);
    const double span = (std::min)(kMaxSpan, (std::max)(kMinSpan, (high - low) * factor));
    const double scale = span / (high - low);
    _viewLow = value - (value - low) * scale;
    _viewHigh = _viewLow + span;
    _zoomed = true;
    update();
}

void
TrapezoidStrip::panByPixels(double dx)
{
    double low, high;

    getDomain(&low, &high);
    const double dv = dx / (std::max)(1., width() - 2. * kHandleRadius) * (high - low);
    _viewLow = low - dv;
    _viewHigh = high - dv;
    _zoomed = true;
    update();
}

void
TrapezoidStrip::resetZoom()
{
    _zoomed = false;
    update();
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

QPainterPath
TrapezoidStrip::outlinePath(int turn) const
{
    const QRectF band = bandOf(width(), height());
    const double top = band.top() + kHandleRadius;
    const double bottom = band.bottom() - kHandleRadius;
    double p[4];

    getPositions(p);
    const QPointF corners[4] = {
        QPointF(valueToX(p[0] + turn), bottom), QPointF(valueToX(p[1] + turn), top),
        QPointF(valueToX(p[2] + turn), top), QPointF(valueToX(p[3] + turn), bottom)
    };
    QPainterPath outline(corners[0]);
    for (int i = 1; i < 4; ++i) {
        const QPointF& a = corners[i - 1];
        const QPointF& b = corners[i];
        if (_smooth && (i != 2)) {
            // A cubic with its controls a third and two thirds along x, level with its ends,
            // is x linear and y 3t^2 - 2t^3 in t: the smoothstep exactly.
            const double dx = (b.x() - a.x()) / 3.;
            outline.cubicTo(QPointF(a.x() + dx, a.y()), QPointF(b.x() - dx, b.y()), b);
        } else {
            outline.lineTo(b);
        }
    }

    return outline;
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
    {
        double low, high;
        getDomain(&low, &high);
        setProperty("domainLow", low);
        setProperty("domainHigh", high);
    }
    const QRectF band = bandOf(width(), height());

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

    double p[4];
    double centre;
    getPositions(p, &centre);

    // The trapezoid, repeated a turn apart across the domain for hue so a long ramp shows where it
    // wraps. Its base is left open, so the gradient under it reads unbroken.
    int firstTurn = 0;
    int lastTurn = 0;
    if (isHue()) {
        double low, high;
        getDomain(&low, &high);
        firstTurn = (int)std::floor(low - p[3]);
        lastTurn = (int)std::ceil(high - p[0]);
        firstTurn = (std::max)(firstTurn, lastTurn - kMaxHueCopies);
    }
    for (int turn = firstTurn; turn <= lastTurn; ++turn) {
        const QPainterPath outline = outlinePath(turn);
        QPainterPath shape(outline);
        shape.closeSubpath();
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(255, 255, 255, 50));
        painter.drawPath(shape);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(0, 0, 0, 160), 3.));
        painter.drawPath(outline);
        painter.setPen(QPen(QColor(255, 255, 255, 230), 1.5));
        painter.drawPath(outline);
    }

    // The source colour's component; a source with no chroma has no hue to mark.
    if (!isHue() || (_hueWeight > 0.f)) {
        double s = _source[isHue() ? 0 : ((_role == KnobDouble::eRangeWidgetRoleSaturation) ? 1 : 2)];
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

    // The centre, a notch hanging from the top of the band.
    {
        const double x = valueToX(centre);
        QPolygonF notch;
        notch << QPointF(x - kHandleRadius, band.top()) << QPointF(x + kHandleRadius, band.top()) << QPointF(x, band.top() + kHandleRadius);
        painter.setPen(QPen(QColor(0, 0, 0), 1.));
        painter.setBrush(QColor(255, 255, 255));
        painter.drawPolygon(notch);
    }

    painter.setClipping(false);
    const double top = band.top() + kHandleRadius;
    const double bottom = band.bottom() - kHandleRadius;
    for (int i = 0; i < 4; ++i) {
        const bool inner = (i == 1) || (i == 2);
        const QPointF handle(valueToX(p[i]), inner ? top : bottom);
        const bool active = isDragging() && _dragResolved && ((int)_dragPart == i);
        painter.setPen(QPen(QColor(0, 0, 0), 1.));
        painter.setBrush(active ? QColor(255, 200, 80) : QColor(255, 255, 255));
        painter.drawEllipse(handle, kHandleRadius, kHandleRadius);
    }
} // TrapezoidStrip::paintEvent

void
TrapezoidStrip::mousePressEvent(QMouseEvent* e)
{
    if (!isEnabled() || isDragging() || _panning) {
        QWidget::mousePressEvent(e);

        return;
    }
    const double x = e->position().x();
    const bool altLeft = (e->button() == Qt::LeftButton) && ((e->modifiers() & Qt::AltModifier) != 0);
    if ((e->button() == Qt::MiddleButton) || altLeft) {
        getDomain(&_panLow, &_panHigh);
        _panX = x;
        _panning = true;
        e->accept();

        return;
    }
    if (e->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(e);

        return;
    }
    const PartEnum part = partAt(x);
    if (part == ePartNone) {
        e->ignore();

        return;
    }
    getDomain(&_dragLow, &_dragHigh);
    _dragPart = part;
    _dragStarted = false;
    _dragShift = (e->modifiers() & Qt::ShiftModifier) != 0;
    _pressX = x;
    for (int i = 0; i < KnobDouble::eRangeWidgetDimensionCount; ++i) {
        _dragValues[i] = _values[i];
    }
    positionsOf(_values, _dragStart, &_dragCentre);
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
    const double c = _dragCentre;
    const bool hue = isHue();
    double v[KnobDouble::eRangeWidgetDimensionCount];

    for (int i = 0; i < KnobDouble::eRangeWidgetDimensionCount; ++i) {
        v[i] = _dragValues[i];
    }
    const bool onHandle = (_dragPart >= ePartOuterLow) && (_dragPart <= ePartOuterHigh);
    // The dragged handle's point.
    const double q = onHandle ? (s[(int)_dragPart] + dv) : 0.;
    switch (_dragPart) {
    case ePartPlateau: {
        const double moved = _dragValues[KnobDouble::eRangeWidgetDimensionCentre] + dv;
        v[KnobDouble::eRangeWidgetDimensionCentre] = hue ? wrapUnit(moved) : moved;
        break;
    }
    case ePartOuterLow: {
        const double lo = hue ? (s[1] - 1.) : -HUGE_VAL;
        v[KnobDouble::eRangeWidgetDimensionOuterLow] = s[1] - (std::min)(s[1], (std::max)(lo, q));
        break;
    }
    case ePartOuterHigh: {
        const double hi = hue ? (s[2] + 1.) : HUGE_VAL;
        v[KnobDouble::eRangeWidgetDimensionOuterHigh] = (std::min)(hi, (std::max)(s[2], q)) - s[2];
        break;
    }
    case ePartInnerLow: {
        double lo = hue ? (c - 1.) : -HUGE_VAL;
        double hi = c;
        if (_dragShift) {
            // The outer point stays, within a turn for hue.
            lo = (std::max)(lo, s[0]);
            if (hue) {
                hi = (std::min)(hi, s[0] + 1.);
            }
        }
        const double inner = (std::min)(hi, (std::max)(lo, q));
        v[KnobDouble::eRangeWidgetDimensionInnerLow] = c - inner;
        if (_dragShift) {
            v[KnobDouble::eRangeWidgetDimensionOuterLow] = inner - s[0];
        }
        break;
    }
    case ePartInnerHigh: {
        double lo = c;
        double hi = hue ? (c + 1.) : HUGE_VAL;
        if (_dragShift) {
            hi = (std::min)(hi, s[3]);
            if (hue) {
                lo = (std::max)(lo, s[3] - 1.);
            }
        }
        const double inner = (std::min)(hi, (std::max)(lo, q));
        v[KnobDouble::eRangeWidgetDimensionInnerHigh] = inner - c;
        if (_dragShift) {
            v[KnobDouble::eRangeWidgetDimensionOuterHigh] = s[3] - inner;
        }
        break;
    }
    case ePartNone:
    default:
        break;
    }
    for (int i = 0; i < KnobDouble::eRangeWidgetDimensionCount; ++i) {
        _values[i] = v[i];
    }
}

void
TrapezoidStrip::report(GesturePhaseEnum phase)
{
    if (_onEdit) {
        double values[KnobDouble::eRangeWidgetDimensionCount];
        getValues(values);
        _onEdit(values, phase);
    }
}

void
TrapezoidStrip::mouseMoveEvent(QMouseEvent* e)
{
    const double x = e->position().x();

    if (_panning) {
        const double dv = (x - _panX) / (std::max)(1., width() - 2. * kHandleRadius) * (_panHigh - _panLow);
        _viewLow = _panLow - dv;
        _viewHigh = _panHigh - dv;
        _zoomed = true;
        update();

        return;
    }
    if (!isDragging()) {
        QWidget::mouseMoveEvent(e);

        return;
    }
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
    if (_panning && ((e->button() == Qt::MiddleButton) || (e->button() == Qt::LeftButton))) {
        _panning = false;
        e->accept();

        return;
    }
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

void
TrapezoidStrip::wheelEvent(QWheelEvent* e)
{
    const double delta = e->angleDelta().y();

    if (!isEnabled() || (delta == 0.)) {
        e->ignore();

        return;
    }
    // A zoom mid-drag would move the handle out from under the cursor.
    if (!isDragging() && !_panning) {
        zoomAround(e->position().x(), std::pow(2., -delta / kWheelDeltaPerDoubling));
    }
    e->accept();
}

KnobGuiTrapezoid::KnobGuiTrapezoid(KnobIPtr knob,
                                   KnobGuiContainerI* container)
    : KnobGuiDouble(knob, container)
    , _rangeKnob(std::dynamic_pointer_cast<KnobDouble>(knob))
    , _stripRow()
    , _strip()
    , _smoothButton()
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

    if (!knob || !knob->isRangeWidget() || (knob->getDimension() != (int)KnobDouble::eRangeWidgetDimensionCount)) {
        return 0;
    }
    const QString name = QString::fromUtf8(knob->getName().c_str());
    QWidget* row = new QWidget(parent);
    row->setObjectName(QString::fromUtf8("TrapezoidRow_") + name);
    QHBoxLayout* rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(TO_DPIX(3));

    TrapezoidStrip* strip = new TrapezoidStrip(knob->getRangeWidgetRole(), row);
    strip->setObjectName(QString::fromUtf8("TrapezoidStrip_") + name);
    strip->setToolTip(QString::fromUtf8(knob->getHintToolTip().c_str()));
    strip->setDisplayConverter(makeDisplayConverter(knob));
    strip->setEditCallback([this](const double* values, TrapezoidStrip::GesturePhaseEnum phase) {
        onStripEdited(values, phase);
    });
    rowLayout->addWidget(strip, 1);

    const QSize buttonSize(TO_DPIX(NATRON_MEDIUM_BUTTON_SIZE), TO_DPIY(NATRON_MEDIUM_BUTTON_SIZE));
    const QSize iconSize(TO_DPIX(NATRON_MEDIUM_BUTTON_ICON_SIZE), TO_DPIY(NATRON_MEDIUM_BUTTON_ICON_SIZE));
    enum RowButtonEnum {
        eRowButtonSmooth = 0,
        eRowButtonRecentre,
        eRowButtonResetZoom,
        eRowButtonCount
    };
    struct ButtonSpec {
        const char* prefix;
        const char* icon;
        QString tip;
    };
    const ButtonSpec specs[eRowButtonCount] = {
        { "TrapezoidSmooth_", NATRON_ICONS_PATH "natron/smooth-s.svg", tr("Smooth: ease the falloffs between the outer and inner points with a smoothstep instead of a straight ramp.") },
        { "TrapezoidRecentre_", NATRON_ICONS_PATH "lucide/crosshair.svg", tr("Recentre: set the centre to the source colour's component, keeping the offsets.") },
        { "TrapezoidResetZoom_", NATRON_ICONS_PATH "lucide/scan.svg", tr("Reset Zoom: show the default range, 0 to 1 for saturation and luma, or a little more when a point lies outside it. The mouse wheel zooms the strip and a middle or Alt drag pans it.") },
    };
    const KnobBoolPtr smooth = knob->getRangeWidgetSmooth();
    for (int b = 0; b < eRowButtonCount; ++b) {
        if ((b == eRowButtonSmooth) && !smooth) {
            continue;
        }
        Button* button = new Button(ThemedIcon::icon(QString::fromUtf8(specs[b].icon), iconSize.width()), QString(), row);
        button->setObjectName(QString::fromUtf8(specs[b].prefix) + name);
        button->setFixedSize(buttonSize);
        button->setIconSize(iconSize);
        button->setFocusPolicy(Qt::NoFocus);
        button->setToolTip(NATRON_NAMESPACE::convertFromPlainText(specs[b].tip, NATRON_NAMESPACE::WhiteSpaceNormal));
        switch (b) {
        case eRowButtonSmooth:
            button->setCheckable(true);
            // clicked, not toggled, so setting the check from the knob writes nothing back.
            QObject::connect(button, &QAbstractButton::clicked, this, [this](bool checked) {
                setSmooth(checked);
            });
            _smoothButton = button;
            break;
        case eRowButtonRecentre:
            QObject::connect(button, &QAbstractButton::clicked, this, [this]() {
                recentre();
            });
            break;
        case eRowButtonResetZoom:
        default:
            QObject::connect(button, &QAbstractButton::clicked, this, [this]() {
                if (_strip) {
                    _strip->resetZoom();
                }
            });
            break;
        }
        rowLayout->addWidget(button);
    }
    _strip = strip;
    _stripRow = row;

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
    KnobBoolPtr enable = knob->getRangeWidgetEnable();
    KnobSignalSlotHandlerPtr enableHandler = enable ? enable->getSignalSlotHandler() : KnobSignalSlotHandlerPtr();
    if (enableHandler) {
        QObject::connect(enableHandler.get(), &KnobSignalSlotHandler::valueChanged, this, [this](ViewSpec, int, int) {
            refreshEnabledLook();
        });
    }
    KnobSignalSlotHandlerPtr smoothHandler = smooth ? smooth->getSignalSlotHandler() : KnobSignalSlotHandlerPtr();
    if (smoothHandler) {
        QObject::connect(smoothHandler.get(), &KnobSignalSlotHandler::valueChanged, this, [this](ViewSpec, int, int) {
            refreshSmooth();
        });
    }
    refreshContext();
    refreshSmooth();

    return row;
} // KnobGuiTrapezoid::createBelowRowWidget

void
KnobGuiTrapezoid::addExtraWidgets(QHBoxLayout* /*containerLayout*/)
{
    QVBoxLayout* column = getBelowRowColumnLayout();
    QWidget* row = _stripRow.data();
    TrapezoidStrip* strip = _strip.data();

    if (!column || !row || !strip) {
        return;
    }
    // The strip's row goes above the spinboxes' row.
    column->removeWidget(row);
    column->insertWidget(0, row);

    // Each label sits above its field, so the five fit the panel's width without being cut.
    KnobDoublePtr knob = _rangeKnob.lock();
    const int nDims = knob ? knob->getDimension() : 0;
    for (int i = 0; i < nDims; ++i) {
        SpinBox* box = 0;
        Label* label = 0;
        getSpinBox(i, &box, &label);
        QWidget* boxContainer = label ? label->parentWidget() : 0;
        QBoxLayout* boxLayout = boxContainer ? qobject_cast<QBoxLayout*>(boxContainer->layout()) : 0;
        if (!box || !boxLayout) {
            continue;
        }
        boxLayout->setDirection(QBoxLayout::TopToBottom);
        boxLayout->setSpacing(0);
        boxLayout->setAlignment(label, Qt::AlignHCenter);
        boxLayout->setAlignment(box, Qt::AlignHCenter);
        label->setMinimumWidth(label->fontMetrics().horizontalAdvance(label->text()));
    }

    // The page adds a stretch after a sliderless knob's column; a stretch factor on the column
    // gives it the spare width, so the strip grows with the panel.
    QWidget* columnWidget = column->parentWidget();
    QWidget* field = columnWidget ? columnWidget->parentWidget() : 0;
    QBoxLayout* fieldLayout = field ? qobject_cast<QBoxLayout*>(field->layout()) : 0;
    if (columnWidget) {
        columnWidget->setSizePolicy(QSizePolicy::Expanding, columnWidget->sizePolicy().verticalPolicy());
    }
    if (fieldLayout && columnWidget) {
        fieldLayout->setStretchFactor(columnWidget, 1);
    }

    if (knob) {
        double values[KnobDouble::eRangeWidgetDimensionCount];
        for (int i = 0; i < KnobDouble::eRangeWidgetDimensionCount; ++i) {
            values[i] = knob->getValue(i);
        }
        strip->setValues(values);
    }
    row->show();
    refreshEnabledLook();
} // KnobGuiTrapezoid::addExtraWidgets

bool
KnobGuiTrapezoid::sourceComponents(ColorMath::KeyModelEnum* model,
                                   ColorMath::OkLabBasis* basis,
                                   float* comps,
                                   float* hueWeight) const
{
    KnobDoublePtr knob = _rangeKnob.lock();

    if (!knob) {
        return false;
    }
    KnobColorPtr source = knob->getRangeWidgetSourceColour();
    KnobChoicePtr modelKnob = knob->getRangeWidgetModel();
    int index = modelKnob ? modelKnob->getValue() : 0;
    if ((index < 0) || (index >= (int)ColorMath::eKeyModelCount)) {
        index = 0;
    }
    *model = (ColorMath::KeyModelEnum)index;
    double toXYZ[9];
    ColorMath::workingToXYZ(knob->getHolder(), toXYZ);
    *basis = ColorMath::okLabBasis(toXYZ);
    float rgb[3] = { 0.f, 0.f, 0.f };
    if (source) {
        for (int i = 0; i < 3; ++i) {
            rgb[i] = (float)source->getValue(i);
        }
    }
    ColorMath::keyModelComponents(*model, *basis, rgb, comps, hueWeight);

    return true;
}

void
KnobGuiTrapezoid::refreshContext()
{
    TrapezoidStrip* strip = _strip.data();
    ColorMath::KeyModelEnum model;
    ColorMath::OkLabBasis basis;
    float comps[3];
    float hueWeight;

    if (!strip || !sourceComponents(&model, &basis, comps, &hueWeight)) {
        return;
    }
    strip->setContext(model, basis, comps, hueWeight);
}

void
KnobGuiTrapezoid::refreshEnabledLook()
{
    KnobDoublePtr knob = _rangeKnob.lock();
    KnobBoolPtr enable = knob ? knob->getRangeWidgetEnable() : KnobBoolPtr();
    const bool on = !enable || enable->getValue();
    QWidget* field = getFieldContainer();

    if (!field) {
        QVBoxLayout* column = getBelowRowColumnLayout();
        field = column ? column->parentWidget() : 0;
    }
    // A disabled parent greys every widget of the row, whatever their own enabled state.
    if (field) {
        field->setEnabled(on);
    }
    QWidget* label = getLabelContainer();
    if (label) {
        label->setEnabled(on);
    }
}

void
KnobGuiTrapezoid::refreshSmooth()
{
    KnobDoublePtr knob = _rangeKnob.lock();
    KnobBoolPtr smooth = knob ? knob->getRangeWidgetSmooth() : KnobBoolPtr();
    const bool on = smooth && smooth->getValue();

    if (_smoothButton) {
        _smoothButton->setChecked(on);
    }
    if (_strip) {
        _strip->setSmooth(on);
    }
}

void
KnobGuiTrapezoid::setSmooth(bool smooth)
{
    KnobDoublePtr knob = _rangeKnob.lock();
    KnobBoolPtr smoothKnob = knob ? knob->getRangeWidgetSmooth() : KnobBoolPtr();

    if (!smoothKnob || (smoothKnob->getValue() == smooth)) {
        refreshSmooth();

        return;
    }
    // The undo command needs the knob's own GUI, which a hidden knob may lack.
    KnobGuiPtr smoothGui = KnobGui::getLiveGui(smoothKnob);
    if (smoothGui) {
        KnobUndoCommand<bool>* command = new KnobUndoCommand<bool>(smoothGui, smoothKnob->getValue(), smooth, 0, true);
        command->setMergeable(false);
        pushUndoCommand(command);
    } else {
        ignore_result(smoothKnob->setValue(smooth, ViewSpec::all(), 0, eValueChangedReasonUserEdited));
    }
    refreshSmooth();
}

void
KnobGuiTrapezoid::recentre()
{
    KnobDoublePtr knob = _rangeKnob.lock();
    ColorMath::KeyModelEnum model;
    ColorMath::OkLabBasis basis;
    float comps[3];
    float hueWeight;

    if (!knob || !sourceComponents(&model, &basis, comps, &hueWeight)) {
        return;
    }
    const KnobDouble::RangeWidgetRoleEnum role = knob->getRangeWidgetRole();
    // A source with no chroma has no hue to centre on.
    if ((role == KnobDouble::eRangeWidgetRoleHue) && !(hueWeight > 0.f)) {
        return;
    }
    double centre;
    switch (role) {
    case KnobDouble::eRangeWidgetRoleHue:
        centre = wrapUnit(comps[0]);
        break;
    case KnobDouble::eRangeWidgetRoleSaturation:
        centre = comps[1];
        break;
    case KnobDouble::eRangeWidgetRoleLuma:
    default:
        centre = comps[2];
        break;
    }
    std::list<double> oldValues, newValues;
    for (int i = 0; i < KnobDouble::eRangeWidgetDimensionCount; ++i) {
        const double v = knob->getValue(i);
        oldValues.push_back(v);
        newValues.push_back((i == KnobDouble::eRangeWidgetDimensionCentre) ? centre : v);
    }
    if (oldValues == newValues) {
        return;
    }
    KnobUndoCommand<double>* command = new KnobUndoCommand<double>(shared_from_this(), oldValues, newValues, true);
    command->setMergeable(false);
    pushUndoCommand(command);
} // KnobGuiTrapezoid::recentre

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
    for (int i = 0; i < KnobDouble::eRangeWidgetDimensionCount; ++i) {
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
    double v[KnobDouble::eRangeWidgetDimensionCount];
    KnobDoublePtr knob = _rangeKnob.lock();
    for (int i = 0; i < KnobDouble::eRangeWidgetDimensionCount; ++i) {
        if (values.size() >= (std::size_t)KnobDouble::eRangeWidgetDimensionCount) {
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
    if (_stripRow) {
        _stripRow->setEnabled(enabled);
    }
}

void
KnobGuiTrapezoid::_hide()
{
    KnobGuiValue::_hide();
    if (_stripRow) {
        _stripRow->hide();
    }
}

void
KnobGuiTrapezoid::_show()
{
    KnobGuiValue::_show();
    if (_stripRow) {
        _stripRow->show();
    }
}

NATRON_NAMESPACE_EXIT
