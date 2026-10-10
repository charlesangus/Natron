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

#include "ColorEditSlider.h"

#include <algorithm>
#include <cmath>

#include <QBrush>
#include <QKeyEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPolygonF>
#include <QResizeEvent>

#include "Engine/Settings.h"

#include "Gui/GuiApplicationManager.h"
#include "Gui/GuiMacros.h"
#include "Gui/LineEdit.h"

NATRON_NAMESPACE_ENTER

namespace {

// This text becomes part of an undo command's label verbatim, so it has to read as a
// finished phrase on its own ("value slider (absolute)"), not just a value's name.
QString
sliderKindName(ColorSliderKindEnum kind)
{
    switch (kind) {
    case eColorSliderKindHue:
        return QString::fromUtf8("hue");
    case eColorSliderKindSat:
        return QString::fromUtf8("saturation");
    case eColorSliderKindValue:
        return QString::fromUtf8("value");
    case eColorSliderKindTemp:
        return QString::fromUtf8("temperature");
    case eColorSliderKindTint:
        return QString::fromUtf8("tint");
    case eColorSliderKindAlpha:
        return QString::fromUtf8("alpha");
    }

    return QString();
}

const double kPad = 4.;
const double kMarkerHitPx = 4.;
const double kMarkerHalfWidth = 3.;
const double kChevronLen = 6.;
const double kEditSpacing = 6.;
const double kEditWidth = 56.;

} // namespace

struct ColorEditSliderPrivate {
    enum DragKindEnum {
        eDragKindNone,
        eDragKindAbsolute,
        eDragKindRelative
    };

    ColorSliderKindEnum kind;
    ColorKnobRoleEnum role;
    TrackballRates rates;
    double relativeRate;
    double value;
    QGradientStops gradientStops;
    LineEdit* edit;

    DragKindEnum dragKind;
    double dragGrabOffsetPx; // absolute drag only: press x minus the marker's own x at press time
    double dragLastX; // relative drag only: re-read every move, so the gear can change mid-drag
    bool gestureStarted; // gestureStarted() is only emitted lazily, on the drag's first real move
    bool hoverMarker;

    ColorEditSliderPrivate(ColorSliderKindEnum kind)
        : kind(kind)
        , role(eColorKnobRoleGain)
        , rates()
        , relativeRate(0.)
        , value(0.)
        , gradientStops()
        , edit(0)
        , dragKind(eDragKindNone)
        , dragGrabOffsetPx(0.)
        , dragLastX(0.)
        , gestureStarted(false)
        , hoverMarker(false)
    {
    }

    SliderRange range() const
    {
        return sliderMapping(kind, role, rates);
    }
};

ColorEditSlider::ColorEditSlider(ColorSliderKindEnum kind,
                                 QWidget* parent)
    : QWidget(parent)
    , _imp(new ColorEditSliderPrivate(kind))
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setCursor(Qt::ArrowCursor);
    setToolTip(sliderKindName(kind));

    _imp->edit = new LineEdit(this);
    _imp->edit->setFixedWidth((int)kEditWidth);
    _imp->edit->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    _imp->edit->installEventFilter(this);
    QObject::connect(_imp->edit, &LineEdit::returnPressed, this, &ColorEditSlider::onEditCommitted);

    _imp->edit->setText(QString::number(_imp->value, 'f', 2));
}

ColorEditSlider::~ColorEditSlider()
{
}

ColorSliderKindEnum
ColorEditSlider::kind() const
{
    return _imp->kind;
}

void
ColorEditSlider::setRole(ColorKnobRoleEnum role)
{
    if (_imp->role == role) {
        return;
    }
    _imp->role = role;
    update();
}

ColorKnobRoleEnum
ColorEditSlider::role() const
{
    return _imp->role;
}

void
ColorEditSlider::setRates(const TrackballRates& rates)
{
    _imp->rates = rates;
    update();
}

const TrackballRates&
ColorEditSlider::rates() const
{
    return _imp->rates;
}

void
ColorEditSlider::setRelativeRatePerPixel(double unitsPerPixel)
{
    _imp->relativeRate = unitsPerPixel;
}

double
ColorEditSlider::relativeRatePerPixel() const
{
    return _imp->relativeRate;
}

SliderRange
ColorEditSlider::range() const
{
    return _imp->range();
}

void
ColorEditSlider::setValue(double value)
{
    if (value == _imp->value) {
        return;
    }
    _imp->value = value;
    if (!_imp->edit->hasFocus()) {
        _imp->edit->setText(QString::number(value, 'f', 2));
    }
    update();
}

double
ColorEditSlider::value() const
{
    return _imp->value;
}

void
ColorEditSlider::setGradientStops(const QGradientStops& stops)
{
    _imp->gradientStops = stops;
    update();
}

LineEdit*
ColorEditSlider::valueLineEdit() const
{
    return _imp->edit;
}

QSize
ColorEditSlider::sizeHint() const
{
    return QSize(200, 20);
}

QSize
ColorEditSlider::minimumSizeHint() const
{
    return QSize(120, 18);
}

namespace {

QRectF
trackRectFor(const QWidget* w,
             const QWidget* edit)
{
    double editW = edit ? edit->width() : 0.;

    return QRectF(kPad, 0., std::max(0., w->width() - editW - kEditSpacing - 2. * kPad), w->height());
}

} // namespace

void
ColorEditSlider::resizeEvent(QResizeEvent* e)
{
    int editH = _imp->edit->sizeHint().height();

    _imp->edit->setGeometry(width() - (int)kEditWidth - (int)kPad, (height() - editH) / 2, (int)kEditWidth, editH);
    QWidget::resizeEvent(e);
}

void
ColorEditSlider::paintEvent(QPaintEvent* /*e*/)
{
    QPainter p(this);

    p.setRenderHint(QPainter::Antialiasing);

    QRectF r = trackRectFor(this, _imp->edit);
    SliderRange rng = _imp->range();

    QLinearGradient g(r.topLeft(), r.topRight());
    if (_imp->gradientStops.isEmpty()) {
        g.setColorAt(0., QColor(90, 90, 90));
        g.setColorAt(1., QColor(90, 90, 90));
    } else {
        g.setStops(_imp->gradientStops);
    }
    p.setPen(QPen(QColor(20, 20, 20), 1));
    p.setBrush(QBrush(g));
    p.drawRoundedRect(r, 3., 3.);

    if ((rng.rangeMin < 0.) && (rng.rangeMax > 0.)) {
        double zx = r.left() + valueToMarker(rng, 0.) * r.width();
        p.setPen(QPen(QColor(0, 0, 0, 160), 1));
        p.drawLine(QPointF(zx, r.top() + 1.), QPointF(zx, r.bottom() - 1.));
    }

    double selectionColorRGB[3];
    appPTR->getCurrentSettings()->getSelectionColor(&selectionColorRGB[0], &selectionColorRGB[1], &selectionColorRGB[2]);
    QColor selectionColor;
    selectionColor.setRgbF(selectionColorRGB[0], selectionColorRGB[1], selectionColorRGB[2]);

    bool dragging = _imp->dragKind != ColorEditSliderPrivate::eDragKindNone;
    double mx = r.left() + valueToMarker(rng, _imp->value) * r.width();
    SliderChevronEnum chevron = sliderChevron(rng, _imp->value);

    if (chevron != eSliderChevronNone) {
        double dir = (chevron == eSliderChevronHigh) ? 1. : -1.;
        QPolygonF poly;
        poly << QPointF(mx - dir, r.top() + 1.) << QPointF(mx + dir * kChevronLen, r.center().y()) << QPointF(mx - dir, r.bottom() - 1.);
        p.setPen(QPen(QColor(0, 0, 0), 3.5));
        p.drawPolyline(poly);
        p.setPen(QPen(dragging ? selectionColor : QColor(235, 235, 235), 1.8));
        p.drawPolyline(poly);
    } else {
        QColor fill = (dragging && (_imp->dragKind == ColorEditSliderPrivate::eDragKindAbsolute)) ? selectionColor : _imp->hoverMarker ? QColor(226, 226, 226)
                                                                                                                                       : QColor(210, 210, 210);
        p.setPen(QPen(Qt::black, 1));
        p.setBrush(fill);
        p.drawRect(QRectF(mx - kMarkerHalfWidth, r.top() + 1., 2. * kMarkerHalfWidth, r.height() - 2.));
        if (dragging && (_imp->dragKind == ColorEditSliderPrivate::eDragKindRelative)) {
            p.setPen(QPen(selectionColor, 2));
            p.setBrush(Qt::NoBrush);
            p.drawRect(QRectF(mx - kMarkerHalfWidth - 1., r.top() + 0.5, 2. * kMarkerHalfWidth + 2., r.height() - 1.));
        }
    }
} // paintEvent

bool
ColorEditSlider::eventFilter(QObject* watched,
                             QEvent* e)
{
    if ((watched == _imp->edit) && (e->type() == QEvent::KeyPress)) {
        QKeyEvent* ke = static_cast<QKeyEvent*>(e);
        if (ke->key() == Qt::Key_Escape) {
            // Typing is its own, separate gesture (committed whole on Enter): Esc here only
            // un-types, it never touches an (unrelated) in-progress track drag.
            _imp->edit->setText(QString::number(_imp->value, 'f', 2));
            _imp->edit->clearFocus();

            return true;
        }
    }

    return QWidget::eventFilter(watched, e);
}

void
ColorEditSlider::onEditCommitted()
{
    bool ok = false;
    double v = _imp->edit->text().toDouble(&ok);

    if (!ok) {
        _imp->edit->setText(QString::number(_imp->value, 'f', 2));

        return;
    }
    _imp->value = v;
    update();

    QString label = QString::fromUtf8("typed ") + sliderKindName(_imp->kind);

    Q_EMIT gestureStarted();
    Q_EMIT gestureMoved(v, true, label);
    Q_EMIT gestureFinished(true);

    _imp->edit->clearFocus();
}

namespace {

bool
onMarker(const QRectF& r,
         const SliderRange& rng,
         double value,
         double xPixel)
{
    if (sliderChevron(rng, value) != eSliderChevronNone) {
        // A chevron is not the marker: pressing it is a relative drag, same as pressing
        // anywhere else on the track.
        return false;
    }
    double mx = r.left() + valueToMarker(rng, value) * r.width();

    return std::abs(xPixel - mx) <= kMarkerHitPx;
}

} // namespace

void
ColorEditSlider::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(e);

        return;
    }

    QRectF r = trackRectFor(this, _imp->edit);
    SliderRange rng = _imp->range();
    double x = e->position().x();
    bool absolute = onMarker(r, rng, _imp->value, x);
    double mx = r.left() + valueToMarker(rng, _imp->value) * r.width();

    _imp->dragKind = absolute ? ColorEditSliderPrivate::eDragKindAbsolute : ColorEditSliderPrivate::eDragKindRelative;
    _imp->dragGrabOffsetPx = absolute ? (x - mx) : 0.;
    _imp->dragLastX = x;
    _imp->gestureStarted = false;
    setFocus();
    e->accept();
}

void
ColorEditSlider::mouseMoveEvent(QMouseEvent* e)
{
    double x = e->position().x();
    QRectF r = trackRectFor(this, _imp->edit);
    SliderRange rng = _imp->range();

    if (_imp->dragKind == ColorEditSliderPrivate::eDragKindNone) {
        bool hovering = onMarker(r, rng, _imp->value, x);
        if (hovering != _imp->hoverMarker) {
            _imp->hoverMarker = hovering;
            setCursor(hovering ? Qt::SizeHorCursor : Qt::ArrowCursor);
            update();
        }

        return;
    }

    double w = r.width();
    if (w <= 0.) {
        return;
    }

    if (!_imp->gestureStarted) {
        _imp->gestureStarted = true;
        Q_EMIT gestureStarted();
    }

    bool absolute = _imp->dragKind == ColorEditSliderPrivate::eDragKindAbsolute;
    QString label = sliderKindName(_imp->kind) + QString::fromUtf8(" slider (") + QString::fromUtf8(absolute ? "absolute" : "relative") + QString::fromUtf8(")");

    if (absolute) {
        double frac = (x - _imp->dragGrabOffsetPx - r.left()) / w;
        _imp->value = markerToValue(rng, frac);
        Q_EMIT gestureMoved(_imp->value, true, label);
    } else {
        double g = gear(modifierHasControl(e), modifierHasShift(e));
        double perPixel = (_imp->relativeRate > 0.) ? _imp->relativeRate : (rng.rangeMax - rng.rangeMin) / w;
        double delta = (x - _imp->dragLastX) * perPixel * g;
        _imp->value += delta;
        Q_EMIT gestureMoved(delta, false, label);
    }
    _imp->dragLastX = x;
    update();
}

void
ColorEditSlider::mouseReleaseEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(e);

        return;
    }
    if (_imp->dragKind != ColorEditSliderPrivate::eDragKindNone) {
        bool moved = _imp->gestureStarted;
        _imp->dragKind = ColorEditSliderPrivate::eDragKindNone;
        _imp->gestureStarted = false;
        if (moved) {
            Q_EMIT gestureFinished(true);
        }
        update();
    }
}

void
ColorEditSlider::mouseDoubleClickEvent(QMouseEvent* e)
{
    if (e->button() == Qt::LeftButton) {
        _imp->dragKind = ColorEditSliderPrivate::eDragKindNone;
        _imp->gestureStarted = false;
        Q_EMIT resetRequested();
        update();
    }
}

void
ColorEditSlider::keyPressEvent(QKeyEvent* e)
{
    // Esc never rolls a gesture back here -- undo is how a mistaken drag gets reverted --
    // so this only ends it in place, exactly like a release.
    if ((e->key() == Qt::Key_Escape) && (_imp->dragKind != ColorEditSliderPrivate::eDragKindNone)) {
        bool moved = _imp->gestureStarted;
        _imp->dragKind = ColorEditSliderPrivate::eDragKindNone;
        _imp->gestureStarted = false;
        if (moved) {
            Q_EMIT gestureFinished(true);
        }
        update();
        e->accept();

        return;
    }
    QWidget::keyPressEvent(e);
}

void
ColorEditSlider::leaveEvent(QEvent* e)
{
    if ((_imp->dragKind == ColorEditSliderPrivate::eDragKindNone) && _imp->hoverMarker) {
        _imp->hoverMarker = false;
        setCursor(Qt::ArrowCursor);
        update();
    }
    QWidget::leaveEvent(e);
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_ColorEditSlider.cpp"
