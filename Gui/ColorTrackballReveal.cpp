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
#include <QCoreApplication>
#include <QGuiApplication>
#include <QRegion>
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

// Gaps before DPI scaling: pills from the outer ring, and the block from the bottom pill.
const double kPillGap = 6.0;
const double kBlockGap = 8.0;

QString
revealText(const char* text)
{
    return QCoreApplication::translate("ColorTrackballReveal", text);
}

double
halfRange(const SliderRange& range)
{
    return 0.5 * (range.rangeMax - range.rangeMin);
}

// travel's share of the range's half, the distance from its middle to either end.
double
travelAcross(const SliderRange& range,
             double travel)
{
    double half = halfRange(range);

    return (half > 0.0) ? travel / half : 0.0;
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
                             const QPoint& discOffset,
                             const QSize& size,
                             const QRect& screen)
{
    QPoint p = centre - discOffset;

    if (screen.isEmpty()) {
        return p;
    }

    const int right = screen.left() + screen.width() - size.width();
    const int bottom = screen.top() + screen.height() - size.height();

    p.setX(std::max(screen.left(), std::min(p.x(), right)));
    p.setY(std::max(screen.top(), std::min(p.y(), bottom)));

    return p;
}

ColorTrackballRevealLabels
colorTrackballRevealDirectionLabels(ColorGlobalModeEnum mode)
{
    // U+2212 MINUS SIGN, not a hyphen.
    const QString minus = QString(QChar(0x2212));
    ColorTrackballRevealLabels labels;

    switch (mode) {
    case eColorGlobalModeS:
        labels.right = QString::fromUtf8("+") + revealText("Hue");
        labels.left = minus + revealText("Hue");
        labels.top = QString::fromUtf8("+") + revealText("Sat");
        labels.bottom = minus + revealText("Sat");
        break;
    case eColorGlobalModeT:
        // Tint is positive toward magenta, and a drag up raises it.
        labels.right = revealText("Warmer");
        labels.left = revealText("Cooler");
        labels.top = revealText("Less green");
        labels.bottom = revealText("Greener");
        break;
    case eColorGlobalModeV:
        labels.right = labels.top = QString::fromUtf8("+") + revealText("Val");
        labels.left = labels.bottom = minus + revealText("Val");
        break;
    case eColorGlobalModeC:
        break;
    }

    return labels;
}

QPointF
colorTrackballRevealDiscPuck(ColorGlobalModeEnum mode,
                             ColorKnobRoleEnum role,
                             double strokeDx,
                             double strokeDy,
                             const TrackballRates& rates)
{
    switch (mode) {
    case eColorGlobalModeS: {
        SliderRange hue = sliderMapping(eColorSliderKindHue, role, rates);
        SliderRange sat = sliderMapping(eColorSliderKindSat, role, rates);

        return QPointF(travelAcross(hue, strokeDx * rates.hueDegreesPerPixel), travelAcross(sat, -strokeDy * rates.satPlanePerPixel));
    }
    case eColorGlobalModeT: {
        SliderRange temp = sliderMapping(eColorSliderKindTemp, role, rates);
        SliderRange tint = sliderMapping(eColorSliderKindTint, role, rates);

        return QPointF(travelAcross(temp, strokeDx * tempRatePerPixel(rates, role)), travelAcross(tint, -strokeDy * tintRatePerPixel(rates, role)));
    }
    case eColorGlobalModeV: {
        SliderRange value = sliderMapping(eColorSliderKindValue, role, rates);
        double s = travelAcross(value, (strokeDx - strokeDy) * valueRatePerPixel(rates, role));

        return QPointF(s * kInvSqrt2, s * kInvSqrt2);
    }
    case eColorGlobalModeC:
        break;
    }

    return QPointF(0.0, 0.0);
}

ColorTrackballRevealWheelState
colorTrackballRevealWheelState(ColorKnobRoleEnum role,
                               const LumaBasis& basis,
                               const KnobColorValue& start,
                               const KnobColorValue& live,
                               double strokeDx,
                               double strokeDy,
                               const TrackballRates& rates)
{
    ColorTrackballRevealWheelState state;

    state.anchor = start;
    state.offset = offsetBetween(toPlane(role, basis, start), toPlane(role, basis, live));
    if (!state.offset.valid) {
        state.offset.x = strokeDx * rates.globalTrackballPlanePerPixel;
        state.offset.y = -strokeDy * rates.globalTrackballPlanePerPixel;
        state.offset.valid = true;
    }

    return state;
}

ColorTrackballReveal::ColorTrackballReveal()
    : QWidget(nullptr, colorTrackballFollowCursorWindowFlags())
    , _wheel(nullptr)
    , _disc(nullptr)
    , _mode(eColorGlobalModeC)
    , _role(eColorKnobRoleGain)
    , _basis(kRec709LumaWeights)
    , _pills()
    , _block(nullptr)
    , _discRect()
{
    setAttribute(Qt::WA_TranslucentBackground, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setFocusPolicy(Qt::NoFocus);

    const int side = diameter();

    _discRect = QRect(0, 0, side, side);
    _wheel = new ColorWheelWidget(this);
    _disc = new ColorModeDiscWidget(this);
    for (QWidget* w : { static_cast<QWidget*>(_wheel), static_cast<QWidget*>(_disc) }) {
        w->setAttribute(Qt::WA_TransparentForMouseEvents, true);
        w->setFocusPolicy(Qt::NoFocus);
        w->setGeometry(_discRect);
    }
    _wheel->setPreferredDiameter(side);
    _disc->hide();

    const char* pillNames[4] = { "ColorTrackballRevealPillRight", "ColorTrackballRevealPillLeft", "ColorTrackballRevealPillTop", "ColorTrackballRevealPillBottom" };

    for (int i = 0; i < 4; ++i) {
        _pills[i] = new ColorTrackballPill(this);
        _pills[i]->setObjectName(QString::fromUtf8(pillNames[i]));
        _pills[i]->hide();
    }
    _block = new ColorTrackballHudBlock(this);
    relayout();
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
    _disc->forgetHue();
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

    const ColorTrackballRevealLabels labels = colorTrackballRevealDirectionLabels(mode);
    const QString texts[4] = { labels.right, labels.left, labels.top, labels.bottom };
    bool changed = false;

    for (int i = 0; i < 4; ++i) {
        if ((_pills[i]->text() != texts[i]) || (_pills[i]->isHidden() != texts[i].isEmpty())) {
            _pills[i]->setText(texts[i]);
            _pills[i]->setVisible(!texts[i].isEmpty());
            changed = true;
        }
    }
    if (changed) {
        relayout();
    }
}

void
ColorTrackballReveal::setHudState(const ColorTrackballHudState& state)
{
    const QSize before = _block->size();

    _block->setState(state);
    if (_block->size() != before) {
        relayout();
    }
}

void
ColorTrackballReveal::relayout()
{
    const QPoint globalDiscCentre = mapToGlobal(getDiscCentre());
    const int side = diameter();
    const double Ro = colorWheelRingOuterRadius(QSize(side, side));
    const double gap = TO_DPIX(kPillGap);
    // Laid out around a disc centred on the origin, then shifted so the window starts at 0, 0.
    const int reach = (int)std::ceil(Ro + gap);
    QRect disc(-side / 2, -side / 2, side, side);
    QRect pillRects[4];

    for (int i = 0; i < 4; ++i) {
        const QSize s = _pills[i]->sizeHint();

        switch ((PillSideEnum)i) {
        case ePillRight:
            pillRects[i] = QRect(QPoint(reach, -s.height() / 2), s);
            break;
        case ePillLeft:
            pillRects[i] = QRect(QPoint(-reach - s.width(), -s.height() / 2), s);
            break;
        case ePillTop:
            pillRects[i] = QRect(QPoint(-s.width() / 2, -reach - s.height()), s);
            break;
        case ePillBottom:
            pillRects[i] = QRect(QPoint(-s.width() / 2, reach), s);
            break;
        }
    }

    const bool hasPills = !_pills[ePillBottom]->isHidden();
    const int blockTop = (hasPills ? pillRects[ePillBottom].bottom() + 1 : (int)std::ceil(Ro)) + (int)std::ceil(TO_DPIY(kBlockGap));
    const QSize blockSize = _block->size();
    QRect block(QPoint(-blockSize.width() / 2, blockTop), blockSize);
    QRect bounds = disc.united(block);

    if (hasPills) {
        for (int i = 0; i < 4; ++i) {
            bounds = bounds.united(pillRects[i]);
        }
    }

    const QPoint shift = -bounds.topLeft();

    _discRect = disc.translated(shift);
    _wheel->setGeometry(_discRect);
    _disc->setGeometry(_discRect);
    for (int i = 0; i < 4; ++i) {
        _pills[i]->setGeometry(pillRects[i].translated(shift));
    }
    _block->setGeometry(block.translated(shift));
    setFixedSize(bounds.size());

    // The translucent background shows as black without a compositor, so only the shapes are
    // let through.
    QRegion mask(_discRect, QRegion::Ellipse);

    if (hasPills) {
        for (int i = 0; i < 4; ++i) {
            mask = mask.united(colorTrackballRoundedRegion(_pills[i]->geometry(), 0.5 * _pills[i]->height()));
        }
    }
    mask = mask.united(colorTrackballRoundedRegion(_block->geometry(), colorTrackballHudBlockRadius()));
    setMask(mask);

    move(globalDiscCentre - getDiscCentre());
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
ColorTrackballReveal::setStroke(const KnobColorValue& start,
                                const KnobColorValue& live,
                                double strokeDx,
                                double strokeDy,
                                const TrackballRates& rates)
{
    if (_mode == eColorGlobalModeC) {
        ColorTrackballRevealWheelState state = colorTrackballRevealWheelState(_role, _basis, start, live, strokeDx, strokeDy, rates);

        _wheel->setValue(state.anchor);
        _wheel->setDragOffset(state.offset);

        return;
    }
    _disc->setCentreValue(_role, live);
    _disc->setPuck(colorTrackballRevealDiscPuck(_mode, _role, strokeDx, strokeDy, rates));
}

void
ColorTrackballReveal::centreOn(const QPoint& globalPos)
{
    QScreen* screen = QGuiApplication::screenAt(globalPos);

    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }
    move(colorTrackballRevealPosition(globalPos, getDiscCentre(), size(), screen ? screen->geometry() : QRect()));
}

NATRON_NAMESPACE_EXIT
