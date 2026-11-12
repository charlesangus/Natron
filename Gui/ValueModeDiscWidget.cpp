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

#include "ValueModeDiscWidget.h"

#include <algorithm>
#include <cmath>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QCoreApplication>
#include <QFont>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPolygonF>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Gui/ColorTrackballHud.h"
#include "Gui/ColorTrackballReveal.h"
#include "Gui/ColorWheelWidget.h"

NATRON_NAMESPACE_ENTER

namespace {

const QColor kGlyphColour(222, 222, 222);
const QColor kDimColour(170, 170, 170);
const QColor kRaisedColour(71, 71, 71);
const QColor kSunkenColour(31, 31, 31);

QString
discText(const char* text)
{
    return QCoreApplication::translate("ValueModeDiscWidget", text);
}

QString
minusSign()
{
    return QString(QChar(0x2212));
}

QColor
withAlpha(const QColor& c,
          int alpha)
{
    QColor result(c);

    result.setAlpha(alpha);

    return result;
}

void
drawArrow(QPainter& p,
          const QPointF& from,
          const QPointF& to,
          double head,
          double width,
          const QColor& colour)
{
    QPointF d = to - from;
    const double length = std::hypot(d.x(), d.y());

    if (length < 1e-6) {
        return;
    }
    d /= length;

    const QPointF n(-d.y(), d.x());

    p.setPen(QPen(colour, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawLine(from, to - d * head * 0.5);

    QPolygonF tip;

    tip << to << (to - d * head + n * head * 0.55) << (to - d * head - n * head * 0.55);
    p.setPen(Qt::NoPen);
    p.setBrush(colour);
    p.drawPolygon(tip);
}

// Angles in degrees, 0 at +x and counter-clockwise on screen; the head sits at toDeg.
void
drawArcArrow(QPainter& p,
             const QPointF& c,
             double r,
             double fromDeg,
             double toDeg,
             double head,
             double width,
             const QColor& colour)
{
    const double sign = (toDeg >= fromDeg) ? 1. : -1.;
    const double endShort = sign * (head * 0.6 / r) * 180. / M_PI;
    const int steps = 40;
    QPainterPath path;

    auto at = [&](double deg) {
        const double a = deg * M_PI / 180.;

        return QPointF(c.x() + r * std::cos(a), c.y() - r * std::sin(a));
    };

    path.moveTo(at(fromDeg));
    for (int i = 1; i <= steps; ++i) {
        path.lineTo(at(fromDeg + (toDeg - endShort - fromDeg) * i / steps));
    }
    p.setPen(QPen(colour, width, Qt::SolidLine, Qt::RoundCap));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);

    const double a = toDeg * M_PI / 180.;
    const QPointF tipPoint = at(toDeg);
    const QPointF d(-std::sin(a) * sign, -std::cos(a) * sign);
    const QPointF n(-d.y(), d.x());
    QPolygonF tip;

    tip << tipPoint + d * head * 0.5 << (tipPoint - d * head * 0.5 + n * head * 0.6) << (tipPoint - d * head * 0.5 - n * head * 0.6);
    p.setPen(Qt::NoPen);
    p.setBrush(colour);
    p.drawPolygon(tip);
}

void
drawSquare(QPainter& p,
           const QPointF& c,
           double side,
           const QColor& line,
           const QColor& fill,
           bool dashed,
           double width)
{
    QPen pen(line, width);

    if (dashed) {
        pen.setStyle(Qt::DashLine);
    }
    p.setPen(pen);
    p.setBrush(fill);
    p.drawRect(QRectF(c.x() - side / 2, c.y() - side / 2, side, side));
}

bool
isAxisKind(GlobalModeKindEnum kind)
{
    return (kind == eGlobalModeKindTranslate) || (kind == eGlobalModeKindSkew) || (kind == eGlobalModeKindGenericSpatial2D);
}

} // namespace

ColorTrackballRevealLabels
valueModeRevealDirectionLabels(GlobalModeKindEnum kind)
{
    ColorTrackballRevealLabels labels;
    const QString minus = minusSign();

    switch (kind) {
    case eGlobalModeKindTranslate:
    case eGlobalModeKindGenericSpatial2D:
        labels.right = QString::fromUtf8("+X");
        labels.left = minus + QString::fromUtf8("X");
        labels.top = QString::fromUtf8("+Y");
        labels.bottom = minus + QString::fromUtf8("Y");
        break;
    case eGlobalModeKindRotate:
        labels.right = discText("CCW");
        labels.left = discText("CW");
        labels.top = discText("CCW");
        labels.bottom = discText("CW");
        break;
    case eGlobalModeKindScale:
        labels.right = discText("Scale up");
        labels.left = discText("Scale down");
        labels.top = discText("Scale up");
        labels.bottom = discText("Scale down");
        break;
    case eGlobalModeKindSize:
        labels.right = discText("Larger");
        labels.left = discText("Smaller");
        labels.top = discText("Larger");
        labels.bottom = discText("Smaller");
        break;
    case eGlobalModeKindSkew:
        labels.right = discText("+Skew X");
        labels.left = minus + discText("Skew X");
        labels.top = discText("+Skew Y");
        labels.bottom = minus + discText("Skew Y");
        break;
    case eGlobalModeKindGeneric:
        labels.right = QString::fromUtf8("+");
        labels.left = minus;
        labels.top = QString::fromUtf8("+");
        labels.bottom = minus;
        break;
    case eGlobalModeKindColor:
    case eGlobalModeKindIneligible:
        break;
    }

    return labels;
}

void
drawValueModeGlyph(QPainter& p,
                   GlobalModeKindEnum kind,
                   const QPointF& c,
                   double outerRadius,
                   double innerRadius,
                   double discRadius,
                   bool compact)
{
    const QColor accent = colorTrackballHudAccent();
    const double R = discRadius;
    const double w = compact ? 1.8 : 2.2;
    const double hd = R * (compact ? 0.28 : 0.17);

    p.setRenderHint(QPainter::Antialiasing, true);
    switch (kind) {
    case eGlobalModeKindTranslate:
    case eGlobalModeKindGenericSpatial2D: {
        const double a = R * 0.28;
        const double b = R * 0.78;

        drawArrow(p, c + QPointF(a, 0), c + QPointF(b, 0), hd, w, accent);
        drawArrow(p, c + QPointF(0, -a), c + QPointF(0, -b), hd, w, accent);
        drawArrow(p, c + QPointF(-a, 0), c + QPointF(-b, 0), hd, w, kGlyphColour);
        drawArrow(p, c + QPointF(0, a), c + QPointF(0, b), hd, w, kGlyphColour);
        // A ring at the centre marks a position rather than an offset.
        if ((kind == eGlobalModeKindGenericSpatial2D) && !compact) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(kDimColour, 1.2));
            p.drawEllipse(c, R * 0.12, R * 0.12);
        }
        break;
    }
    case eGlobalModeKindRotate: {
        const double ringMid = 0.5 * (innerRadius + outerRadius);
        const double ringWidth = outerRadius - innerRadius;
        const double arcWidth = ringWidth * 0.21 + 0.8;

        drawArcArrow(p, c, ringMid, 12, 78, ringWidth * 0.95, arcWidth, accent);
        drawArcArrow(p, c, ringMid, 258, 192, ringWidth * 0.95, arcWidth, kGlyphColour);
        p.setPen(QPen(kDimColour, compact ? 1.8 : 1.6, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(c + QPointF(0, R * 0.55), c + QPointF(0, -R * 0.55));
        p.drawLine(c + QPointF(-R * 0.55, 0), c + QPointF(R * 0.55, 0));
        p.setPen(QPen(kGlyphColour, w, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(c, c + QPointF(R * 0.25, -R * 0.435));
        p.setBrush(kGlyphColour);
        p.drawEllipse(c, R * 0.06, R * 0.06);
        break;
    }
    case eGlobalModeKindScale: {
        const double s0 = R * 0.17;
        const double s1 = R * 0.4;
        const QPointF q(R * 0.42, -R * 0.42);

        drawSquare(p, c + q, s1, kGlyphColour, Qt::transparent, true, 1.4);
        drawSquare(p, c + q + QPointF(-(s1 - s0) / 2, (s1 - s0) / 2), s0, accent, accent, false, 1.6);
        drawArrow(p, c + q + QPointF(s0 * 0.2, -s0 * 0.2), c + q + QPointF(s1 * 0.95, -s1 * 0.95) * 0.9, hd * 0.9, w, accent);

        const QPointF m = -q;

        drawSquare(p, c + m, s1, kGlyphColour, Qt::transparent, true, 1.4);
        drawSquare(p, c + m, s0, kGlyphColour, kGlyphColour, false, 1.6);
        drawArrow(p, c + m + QPointF(-s1 * 0.95, s1 * 0.95) * 0.9, c + m + QPointF(-s0 * 0.2, s0 * 0.2), hd * 0.9, w, kGlyphColour);
        break;
    }
    case eGlobalModeKindSize:
        drawSquare(p, c + QPointF(R * 0.36, -R * 0.36), R * 0.5, accent, withAlpha(accent, 90), false, w);
        drawSquare(p, c + QPointF(-R * 0.4, R * 0.4), R * 0.2, kGlyphColour, QColor(255, 255, 255, 60), false, w);
        break;
    case eGlobalModeKindSkew: {
        const double h = R * 0.46;
        const double sh = R * 0.26;
        QPolygonF sheared;

        drawSquare(p, c, h * 2, kDimColour, Qt::transparent, true, 1.2);
        sheared << c + QPointF(-h + sh, -h) << c + QPointF(h + sh, -h) << c + QPointF(h - sh, h) << c + QPointF(-h - sh, h);
        p.setPen(QPen(kGlyphColour, w, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(QColor(255, 255, 255, 40));
        p.drawPolygon(sheared);
        drawArrow(p, c + QPointF(h * 0.1, -h - R * 0.17), c + QPointF(h * 0.1 + R * 0.34, -h - R * 0.17), hd * 0.85, w, accent);
        drawArrow(p, c + QPointF(h * 0.1, h + R * 0.17), c + QPointF(h * 0.1 - R * 0.34, h + R * 0.17), hd * 0.85, w, kGlyphColour);
        if (!compact) {
            drawArrow(p, c + QPointF(h + sh + R * 0.12, h * 0.4), c + QPointF(h + sh + R * 0.12, -h * 0.4), hd * 0.8, w * 0.8, withAlpha(accent, 190));
            drawArrow(p, c + QPointF(-h - sh - R * 0.12, -h * 0.4), c + QPointF(-h - sh - R * 0.12, h * 0.4), hd * 0.8, w * 0.8, kGlyphColour);
        }
        break;
    }
    case eGlobalModeKindGeneric: {
        p.setPen(QPen(kDimColour, w, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(c + QPointF(-R * 0.42, R * 0.42), c + QPointF(R * 0.42, -R * 0.42));
        for (int i = -2; i <= 2; ++i) {
            const QPointF t = c + QPointF(i * R * 0.22, -i * R * 0.22);
            const QPointF n(R * 0.06, R * 0.06);

            p.drawLine(t - n, t + n);
        }
        if (!compact) {
            QFont f = p.font();

            f.setPixelSize(std::max(1, (int)std::lround(R * 0.34)));
            f.setBold(true);
            p.setFont(f);

            const QPointF plusAt = c + QPointF(R * 0.58, -R * 0.58);
            const QPointF minusAt = c - QPointF(R * 0.58, -R * 0.58);

            p.setPen(accent);
            p.drawText(QRectF(plusAt.x() - R * 0.2, plusAt.y() - R * 0.2, R * 0.4, R * 0.4), Qt::AlignCenter, QString::fromUtf8("+"));
            p.setPen(kGlyphColour);
            p.drawText(QRectF(minusAt.x() - R * 0.2, minusAt.y() - R * 0.2, R * 0.4, R * 0.4), Qt::AlignCenter, minusSign());
        }
        p.setBrush(kGlyphColour);
        p.setPen(Qt::NoPen);
        p.drawEllipse(c, R * 0.07, R * 0.07);
        break;
    }
    case eGlobalModeKindColor:
    case eGlobalModeKindIneligible:
        break;
    }
} // drawValueModeGlyph

ValueModeDiscWidget::ValueModeDiscWidget(QWidget* parent)
    : QWidget(parent)
    , _kind(eGlobalModeKindGeneric)
    , _dx(0.)
    , _dy(0.)
    , _compact(false)
{
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setFocusPolicy(Qt::NoFocus);
}

ValueModeDiscWidget::~ValueModeDiscWidget()
{
}

void
ValueModeDiscWidget::setKind(GlobalModeKindEnum kind)
{
    if (kind != _kind) {
        _kind = kind;
        update();
    }
}

void
ValueModeDiscWidget::setTravel(double dx,
                               double dy)
{
    if ((dx != _dx) || (dy != _dy)) {
        _dx = dx;
        _dy = dy;
        update();
    }
}

void
ValueModeDiscWidget::setCompact(bool compact)
{
    if (compact != _compact) {
        _compact = compact;
        update();
    }
}

QPointF
ValueModeDiscWidget::getPuckOffset() const
{
    const double limit = 0.85 * colorWheelDiscRadius(size());
    QPointF offset;

    if (isAxisKind(_kind)) {
        offset = QPointF(_dx, _dy);
    } else {
        // One-value kinds grow right and up, so the puck rides that diagonal.
        const double either = (_dx - _dy) * M_SQRT1_2;

        offset = QPointF(either * M_SQRT1_2, -either * M_SQRT1_2);
    }

    const double length = std::hypot(offset.x(), offset.y());

    if ((length > limit) && (length > 0.)) {
        offset *= limit / length;
    }

    return offset;
}

void
ValueModeDiscWidget::paintEvent(QPaintEvent* /*e*/)
{
    QPainter p(this);
    const QSize s = size();
    const QPointF c(0.5 * s.width(), 0.5 * s.height());
    const double ro = colorWheelRingOuterRadius(s);
    const double ri = colorWheelRingInnerRadius(s);
    const double rd = colorWheelDiscRadius(s);

    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(Qt::NoPen);
    p.setBrush(kRaisedColour);
    p.drawEllipse(c, ro, ro);
    p.setBrush(kSunkenColour);
    p.drawEllipse(c, ri, ri);
    drawColorWheelRingOutlines(p, c, rd, ri, ro);
    drawValueModeGlyph(p, _kind, c, ro, ri, rd, _compact);

    const QPointF puck = getPuckOffset();

    if ((puck.x() != 0.) || (puck.y() != 0.)) {
        const QPointF at = c + puck;
        const double r = _compact ? 3.5 : 5.;

        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(0, 0, 0, 200), 3.));
        p.drawEllipse(at, r, r);
        p.setPen(QPen(Qt::white, 1.5));
        p.drawEllipse(at, r, r);
    }
}

NATRON_NAMESPACE_EXIT
