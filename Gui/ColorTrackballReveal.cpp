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

#include "ColorTrackballReveal.h"

#include <algorithm>
#include <cmath>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QGuiApplication>
#include <QScreen>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/AppManager.h"

#include "Gui/ColorModeDiscWidget.h"
#include "Gui/ColorTrackballHud.h"
#include "Gui/ColorWheelWidget.h"

NATRON_NAMESPACE_ENTER

namespace {
// Before DPI scaling.
const double kRevealDiameter = 240.0;

const double kInvSqrt2 = 0.70710678118654752440;

double
halfRange(const SliderRange& range)
{
    return 0.5 * (range.rangeMax - range.rangeMin);
}

// value's place across range, -1 at its minimum and 1 at its maximum.
double
acrossRange(const SliderRange& range,
            double value)
{
    double half = halfRange(range);

    if (!(half > 0.0) || !std::isfinite(value)) {
        return 0.0;
    }

    return (value - 0.5 * (range.rangeMax + range.rangeMin)) / half;
}

// travel's share of the range's half, the distance from its middle to either end.
double
travelAcross(const SliderRange& range,
             double travel)
{
    double half = halfRange(range);

    return (half > 0.0) ? travel / half : 0.0;
}

// The Value axis's own reading, as the panel's Value line shows it: stops from the role's pivot
// for a ratio role (Power's counted on its 18% grey effect vector), luma for Offset.
double
valueAxisReading(ColorKnobRoleEnum role,
                 const LumaBasis& basis,
                 const KnobColorValue& v,
                 const SliderRange& range)
{
    double Y = 0.0;

    if (role == eColorKnobRolePower) {
        double effect[3];

        for (int i = 0; i < 3; ++i) {
            PowerEffectResult e = powerToEffect(v.rgb[i]);

            if (!e.valid) {
                return range.rangeMin;
            }
            effect[i] = e.effect;
        }
        Y = luma(basis, effect);
    } else {
        Y = luma(basis, v.rgb);
    }
    if (!isRatioRole(role)) {
        return Y;
    }

    return (Y > 0.0) ? std::log2(Y / valueStopPivot(role)) : range.rangeMin;
}

KnobColorValue
neutralFor(ColorKnobRoleEnum role,
           const KnobColorValue& like)
{
    KnobColorValue v = neutralGainValue(like.hasAlpha);

    v.a = like.a;
    if (!isRatioRole(role)) {
        v.rgb[0] = v.rgb[1] = v.rgb[2] = 0.0;
    }

    return v;
}

PlanePoint
offsetBetween(const PlanePoint& from,
              const PlanePoint& to)
{
    PlanePoint d;

    d.x = to.x - from.x;
    d.y = to.y - from.y;
    d.valid = from.valid && to.valid;
    d.linear = to.linear;

    return d;
}
} // anonymous namespace

QPoint
colorTrackballRevealPosition(const QPoint& centre,
                             const QSize& size,
                             const QRect& screen)
{
    QPoint p(centre.x() - size.width() / 2, centre.y() - size.height() / 2);

    if (screen.isEmpty()) {
        return p;
    }

    const int right = screen.left() + screen.width() - size.width();
    const int bottom = screen.top() + screen.height() - size.height();

    p.setX(std::max(screen.left(), std::min(p.x(), right)));
    p.setY(std::max(screen.top(), std::min(p.y(), bottom)));

    return p;
}

QPointF
colorTrackballRevealDiscPuck(ColorGlobalModeEnum mode,
                             ColorPanelLayout::Puck puck,
                             ColorKnobRoleEnum role,
                             const LumaBasis& basis,
                             const KnobColorValue& live,
                             double strokeDx,
                             double strokeDy,
                             const TrackballRates& rates)
{
    const bool absolute = (puck == ColorPanelLayout::Puck::Absolute);

    switch (mode) {
    case eColorGlobalModeS: {
        SliderRange hue = sliderMapping(eColorSliderKindHue, role, rates);
        SliderRange sat = sliderMapping(eColorSliderKindSat, role, rates);
        double x = travelAcross(hue, strokeDx * rates.hueDegreesPerPixel);
        double y = travelAcross(sat, -strokeDy * rates.satPlanePerPixel);

        if (absolute) {
            PlanePoint p = toPlane(role, basis, live);

            y = acrossRange(sat, p.valid ? planeSaturation(p) : 0.0);
        }

        return QPointF(x, y);
    }
    case eColorGlobalModeT: {
        SliderRange temp = sliderMapping(eColorSliderKindTemp, role, rates);
        SliderRange tint = sliderMapping(eColorSliderKindTint, role, rates);
        double t = travelAcross(temp, strokeDx * tempRatePerPixel(rates, role));
        double n = travelAcross(tint, -strokeDy * tintRatePerPixel(rates, role));

        if (absolute) {
            TempTintResult tt = toTempTint(role, basis, live);

            if (tt.valid) {
                t = acrossRange(temp, tt.temp);
                n = acrossRange(tint, tt.tint);
            }
        }

        return QPointF(t, n);
    }
    case eColorGlobalModeV: {
        SliderRange value = sliderMapping(eColorSliderKindValue, role, rates);
        double s = travelAcross(value, (strokeDx - strokeDy) * valueRatePerPixel(rates, role));

        if (absolute) {
            s = acrossRange(value, valueAxisReading(role, basis, live, value));
        }

        return QPointF(s * kInvSqrt2, s * kInvSqrt2);
    }
    case eColorGlobalModeC:
        break;
    }

    return QPointF(0.0, 0.0);
}

ColorTrackballRevealWheelState
colorTrackballRevealWheelState(ColorPanelLayout::Puck puck,
                               ColorKnobRoleEnum role,
                               const LumaBasis& basis,
                               const KnobColorValue& start,
                               const KnobColorValue& live,
                               double strokeDx,
                               double strokeDy,
                               const TrackballRates& rates)
{
    ColorTrackballRevealWheelState state;

    state.anchor = (puck == ColorPanelLayout::Puck::Absolute) ? neutralFor(role, live) : start;
    state.offset = offsetBetween(toPlane(role, basis, state.anchor), toPlane(role, basis, live));
    if (!state.offset.valid) {
        if (puck == ColorPanelLayout::Puck::Absolute) {
            state.anchor = start;
        }
        state.offset.x = strokeDx * rates.globalTrackballPlanePerPixel;
        state.offset.y = -strokeDy * rates.globalTrackballPlanePerPixel;
        state.offset.valid = true;
    }

    return state;
}

ColorTrackballReveal::ColorTrackballReveal()
    : QWidget(nullptr, ColorTrackballHud::followCursorWindowFlags())
    , _wheel(nullptr)
    , _disc(nullptr)
    , _mode(eColorGlobalModeC)
    , _role(eColorKnobRoleGain)
    , _basis(kRec709LumaWeights)
{
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setFocusPolicy(Qt::NoFocus);

    const int side = diameter();

    setFixedSize(side, side);

    _wheel = new ColorWheelWidget(this);
    _disc = new ColorModeDiscWidget(this);
    for (QWidget* w : { static_cast<QWidget*>(_wheel), static_cast<QWidget*>(_disc) }) {
        w->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        w->setFocusPolicy(Qt::NoFocus);
        w->setGeometry(0, 0, side, side);
    }
    _wheel->setPreferredDiameter(side);
    _disc->hide();
}

ColorTrackballReveal::~ColorTrackballReveal()
{
}

int
ColorTrackballReveal::diameter()
{
    return (int)TO_DPIX(kRevealDiameter);
}

void
ColorTrackballReveal::setKnob(ColorKnobRoleEnum role,
                              const LumaBasis& basis,
                              const DisplayConverter& converter,
                              double rim)
{
    _role = role;
    _basis = basis;
    _wheel->setBasis(basis);
    _wheel->setRole(role);
    _wheel->setRim(rim);
    _wheel->setDisplayConverter(converter);
    _disc->setBasis(basis);
    _disc->setDisplayConverter(converter);
}

void
ColorTrackballReveal::setMode(ColorGlobalModeEnum mode)
{
    _mode = mode;
    if (mode == eColorGlobalModeC) {
        _disc->hide();
        _wheel->show();
    } else {
        _disc->setMode(mode);
        _wheel->hide();
        _disc->show();
    }
}

QWidget*
ColorTrackballReveal::getModeWidget() const
{
    if (_mode == eColorGlobalModeC) {
        return _wheel;
    }

    return _disc;
}

void
ColorTrackballReveal::setStroke(ColorPanelLayout::Puck puck,
                                const KnobColorValue& start,
                                const KnobColorValue& live,
                                double strokeDx,
                                double strokeDy,
                                const TrackballRates& rates)
{
    if (_mode == eColorGlobalModeC) {
        ColorTrackballRevealWheelState state = colorTrackballRevealWheelState(puck, _role, _basis, start, live, strokeDx, strokeDy, rates);

        _wheel->setValue(state.anchor);
        _wheel->setDragOffset(state.offset);

        return;
    }
    _disc->setPuck(colorTrackballRevealDiscPuck(_mode, puck, _role, _basis, live, strokeDx, strokeDy, rates));
}

void
ColorTrackballReveal::centreOn(const QPoint& globalPos)
{
    QScreen* screen = QGuiApplication::screenAt(globalPos);

    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }
    move(colorTrackballRevealPosition(globalPos, size(), screen ? screen->geometry() : QRect()));
}

NATRON_NAMESPACE_EXIT
