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

#include "ColorTrackballHud.h"

#include <algorithm>
#include <cmath>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QColor>
#include <QCoreApplication>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPolygon>
#include <QRegion>
#include <QScreen>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/AppManager.h"
#include "Engine/Settings.h"

#include "Gui/GuiApplicationManager.h"

NATRON_NAMESPACE_ENTER

namespace {
// Sizes before DPI scaling.
const double kFollowOffset = 16.;
const double kPillFontPx = 12.;
const double kPillPadX = 6.;
const double kPillPadY = 4.;
const double kTitleFontPx = 13.;
const double kHintFontPx = 11.;
const double kBlockPadX = 10.;
const double kBlockPadTop = 5.;
const double kBlockPadBottom = 8.;
const double kBlockRadius = 7.;
const double kTitleAdvance = 21.;
const double kKeyWidth = 18.;
const double kKeyHeight = 17.;
const double kKeyGap = 6.;
const double kKeyAdvance = 22.;
const double kHintAdvance = 16.;

const QColor kTextColour(232, 232, 232);
const QColor kDimColour(170, 170, 170);
const QColor kKeyFill(40, 40, 40);
const QColor kKeyEdge(110, 110, 110);
const QColor kLitKeyText(20, 20, 20);

const ColorGlobalModeEnum kModes[4] = { eColorGlobalModeC, eColorGlobalModeS, eColorGlobalModeT, eColorGlobalModeV };

QString
hudText(const char* text)
{
    return QCoreApplication::translate("ColorTrackballHud", text);
}

QStringList
gearHints()
{
    return QStringList() << hudText("Ctrl 10x slower") << hudText("Shift 4x faster") << hudText("Ctrl+Shift 100x slower");
}

QString
gearSeparator()
{
    return QString::fromUtf8("   ");
}

QString
keyHints()
{
    return hudText("Esc/Enter: accept   Ctrl+up/down: switch knob");
}

QString
chromaHint()
{
    return hudText("Drag toward a hue to push the colour that way");
}

double
dpiY(double v)
{
    return appPTR ? TO_DPIY(v) : v;
}

QFont
pixelFont(const QFont& base,
          double px,
          bool bold)
{
    QFont f(base);

    f.setPixelSize(std::max(1, (int)std::lround(dpiY(px))));
    f.setBold(bold);

    return f;
}

QColor
selectionColour()
{
    if (appPTR && appPTR->getCurrentSettings()) {
        double r = 0.;
        double g = 0.;
        double b = 0.;

        appPTR->getCurrentSettings()->getSelectionColor(&r, &g, &b);

        QColor c;

        c.setRgbF(r, g, b);

        return c;
    }

    return QColor(247, 147, 30);
}

// One axis of colorTrackballHudFollowPosition: lo and hi bound the screen, hi exclusive.
int
followAxis(int cursor,
           int extent,
           int lo,
           int hi,
           int offset)
{
    int p = cursor + offset;

    if (p + extent > hi) {
        p = cursor - offset - extent;
    }
    if (p + extent > hi) {
        p = hi - extent;
    }
    if (p < lo) {
        p = lo;
    }

    return p;
}

void
paintPanel(QPainter& painter,
           const QRectF& rect,
           double radius)
{
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(colorTrackballHudEdge(), 1.0));
    painter.setBrush(colorTrackballHudFill());
    painter.drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
}
} // namespace

QString
colorGlobalModeKeyName(ColorGlobalModeEnum mode)
{
    switch (mode) {
    case eColorGlobalModeC:
        return QString::fromUtf8("C");
    case eColorGlobalModeS:
        return QString::fromUtf8("S");
    case eColorGlobalModeT:
        return QString::fromUtf8("T");
    case eColorGlobalModeV:
        return QString::fromUtf8("V");
    }

    return QString();
}

Qt::WindowFlags
colorTrackballFollowCursorWindowFlags()
{
    return Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::WindowTransparentForInput | Qt::WindowDoesNotAcceptFocus;
}

int
colorTrackballHudLitGear(double g)
{
    const double held[3] = { gear(true, false), gear(false, true), gear(true, true) };

    for (int i = 0; i < 3; ++i) {
        if (std::abs(g - held[i]) <= 1e-9 * std::max(1.0, held[i])) {
            return i;
        }
    }

    return -1;
}

QPoint
colorTrackballHudFollowPosition(const QPoint& cursor,
                                const QSize& hudSize,
                                const QRect& screen,
                                const QPoint& offset)
{
    if (screen.isEmpty()) {
        return cursor + offset;
    }

    return QPoint(followAxis(cursor.x(), hudSize.width(), screen.left(), screen.left() + screen.width(), offset.x()),
                  followAxis(cursor.y(), hudSize.height(), screen.top(), screen.top() + screen.height(), offset.y()));
}

QRegion
colorTrackballRoundedRegion(const QRect& rect,
                            double radius)
{
    QPainterPath path;

    path.addRoundedRect(QRectF(rect), radius, radius);

    return QRegion(path.toFillPolygon().toPolygon());
}

double
colorTrackballHudBlockRadius()
{
    return dpiY(kBlockRadius);
}

QColor
colorTrackballHudFill()
{
    return QColor(26, 26, 26);
}

QColor
colorTrackballHudEdge()
{
    return QColor(58, 58, 58);
}

ColorTrackballPill::ColorTrackballPill(QWidget* parent)
    : QWidget(parent)
    , _text()
{
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setFocusPolicy(Qt::NoFocus);
}

ColorTrackballPill::~ColorTrackballPill()
{
}

void
ColorTrackballPill::setText(const QString& text)
{
    _text = text;
    resize(sizeHint());
    update();
}

QSize
ColorTrackballPill::sizeHint() const
{
    const QFontMetrics fm(pixelFont(font(), kPillFontPx, false));

    return QSize(fm.horizontalAdvance(_text) + (int)std::ceil(2.0 * dpiY(kPillPadX)), fm.height() + (int)std::ceil(2.0 * dpiY(kPillPadY)));
}

void
ColorTrackballPill::paintEvent(QPaintEvent* /*e*/)
{
    QPainter painter(this);

    paintPanel(painter, QRectF(rect()), 0.5 * height());
    painter.setFont(pixelFont(font(), kPillFontPx, false));
    painter.setPen(kTextColour);
    painter.drawText(rect(), Qt::AlignCenter, _text);
}

ColorTrackballHudBlock::ColorTrackballHudBlock(QWidget* parent,
                                               Qt::WindowFlags flags)
    : QWidget(parent, flags)
    , _state()
    , _cursor()
    , _followsCursor(false)
{
    setAttribute(Qt::WA_TransparentForMouseEvents, true);
    setAttribute(Qt::WA_ShowWithoutActivating, true);
    setFocusPolicy(Qt::NoFocus);
    setObjectName(QString::fromUtf8("ColorTrackballHudBlock"));
    if (!parent) {
        // Only the rounded panel shows; without a compositor the corners would paint black.
        setAttribute(Qt::WA_TranslucentBackground, true);
    }
    setState(_state);
}

ColorTrackballHudBlock::~ColorTrackballHudBlock()
{
}

void
ColorTrackballHudBlock::setState(const ColorTrackballHudState& state)
{
    _state = state;
    // For scripts, which see this as a plain QWidget.
    setProperty("trackballHudKnob", state.knobLabel);
    setProperty("trackballHudMode", colorGlobalModeKeyName(state.mode));

    const QSize s = sizeHint();

    if (s != size()) {
        resize(s);
        if (_followsCursor) {
            followCursorTo(_cursor);
        }
    }
    if (isWindow()) {
        setMask(colorTrackballRoundedRegion(QRect(QPoint(0, 0), s), dpiY(kBlockRadius)));
    }
    update();
}

QStringList
ColorTrackballHudBlock::lines() const
{
    QStringList keys;

    for (ColorGlobalModeEnum m : kModes) {
        keys << colorGlobalModeKeyName(m);
    }

    QStringList out;

    out << _state.knobLabel << keys.join(QString::fromUtf8(" ")) << gearHints().join(gearSeparator()) << keyHints();
    if (_state.mode == eColorGlobalModeC) {
        out << chromaHint();
    }
    if (!_state.endOfListMessage.isEmpty()) {
        out << _state.endOfListMessage;
    }

    return out;
}

int
ColorTrackballHudBlock::litGear() const
{
    return colorTrackballHudLitGear(_state.gear);
}

QSize
ColorTrackballHudBlock::sizeHint() const
{
    const QFontMetrics title(pixelFont(font(), kTitleFontPx, true));
    const QFontMetrics hint(pixelFont(font(), kHintFontPx, false));
    const QFontMetrics litHint(pixelFont(font(), kHintFontPx, true));
    const QStringList gears = gearHints();
    const int lit = litGear();
    int gearWidth = 2 * hint.horizontalAdvance(gearSeparator());

    for (int i = 0; i < gears.size(); ++i) {
        gearWidth += (i == lit ? litHint : hint).horizontalAdvance(gears[i]);
    }

    int w = std::max(title.horizontalAdvance(_state.knobLabel), gearWidth);

    w = std::max(w, hint.horizontalAdvance(keyHints()));
    w = std::max(w, (int)std::ceil(dpiY(4 * kKeyWidth + 3 * kKeyGap)));

    double h = kBlockPadTop + kTitleAdvance + kKeyAdvance + 2 * kHintAdvance + kBlockPadBottom;

    if (_state.mode == eColorGlobalModeC) {
        w = std::max(w, hint.horizontalAdvance(chromaHint()));
        h += kHintAdvance;
    }
    if (!_state.endOfListMessage.isEmpty()) {
        w = std::max(w, hint.horizontalAdvance(_state.endOfListMessage));
        h += kHintAdvance;
    }

    return QSize(w + (int)std::ceil(2.0 * dpiY(kBlockPadX)), (int)std::ceil(dpiY(h)));
}

void
ColorTrackballHudBlock::paintEvent(QPaintEvent* /*e*/)
{
    QPainter painter(this);
    const QColor accent = selectionColour();
    const QFont titleFont = pixelFont(font(), kTitleFontPx, true);
    const QFont hintFont = pixelFont(font(), kHintFontPx, false);
    const QFont litFont = pixelFont(font(), kHintFontPx, true);
    const double cx = 0.5 * width();
    double y = dpiY(kBlockPadTop);

    paintPanel(painter, QRectF(rect()), dpiY(kBlockRadius));

    painter.setFont(titleFont);
    painter.setPen(kTextColour);
    painter.drawText(QRectF(0.0, y, width(), dpiY(kTitleAdvance)), Qt::AlignHCenter | Qt::AlignTop, _state.knobLabel);
    y += dpiY(kTitleAdvance);

    const double kw = dpiY(kKeyWidth);
    const double kh = dpiY(kKeyHeight);
    const double gap = dpiY(kKeyGap);
    double kx = cx - 0.5 * (4 * kw + 3 * gap);

    painter.setFont(pixelFont(font(), kHintFontPx, true));
    for (ColorGlobalModeEnum m : kModes) {
        const bool isLit = (m == _state.mode);
        const QRectF key(kx, y, kw, kh);

        painter.setPen(isLit ? QPen(Qt::NoPen) : QPen(kKeyEdge, 1.0));
        painter.setBrush(isLit ? accent : kKeyFill);
        painter.drawRoundedRect(key.adjusted(0.5, 0.5, -0.5, -0.5), dpiY(3.0), dpiY(3.0));
        painter.setPen(isLit ? kLitKeyText : kDimColour);
        painter.drawText(key, Qt::AlignCenter, colorGlobalModeKeyName(m));
        kx += kw + gap;
    }
    y += dpiY(kKeyAdvance);

    const QStringList gears = gearHints();
    const int lit = litGear();
    const QFontMetricsF hint(hintFont);
    const QFontMetricsF litHint(litFont);
    double total = 2 * hint.horizontalAdvance(gearSeparator());

    for (int i = 0; i < gears.size(); ++i) {
        total += (i == lit ? litHint : hint).horizontalAdvance(gears[i]);
    }

    double gx = cx - 0.5 * total;

    for (int i = 0; i < gears.size(); ++i) {
        const bool isLit = (i == lit);

        painter.setFont(isLit ? litFont : hintFont);
        painter.setPen(isLit ? accent : kDimColour);
        painter.drawText(QPointF(gx, y + hint.ascent()), gears[i]);
        gx += (isLit ? litHint : hint).horizontalAdvance(gears[i]) + hint.horizontalAdvance(gearSeparator());
    }
    y += dpiY(kHintAdvance);

    QStringList rest;

    rest << keyHints();
    if (_state.mode == eColorGlobalModeC) {
        rest << chromaHint();
    }
    if (!_state.endOfListMessage.isEmpty()) {
        rest << _state.endOfListMessage;
    }
    painter.setFont(hintFont);
    painter.setPen(kDimColour);
    for (const QString& line : rest) {
        painter.drawText(QRectF(0.0, y, width(), dpiY(kHintAdvance)), Qt::AlignHCenter | Qt::AlignTop, line);
        y += dpiY(kHintAdvance);
    }
}

void
ColorTrackballHudBlock::followCursorTo(const QPoint& globalCursor)
{
    _cursor = globalCursor;
    _followsCursor = true;

    QScreen* screen = QGuiApplication::screenAt(globalCursor);

    if (!screen) {
        screen = QGuiApplication::primaryScreen();
    }

    const QRect screenRect = screen ? screen->geometry() : QRect();
    const int offset = (int)std::lround(dpiY(kFollowOffset));

    move(colorTrackballHudFollowPosition(globalCursor, size(), screenRect, QPoint(offset, offset)));
}

WindowHighlightFrame::WindowHighlightFrame(QWidget* window)
    : QWidget(window)
    , _window(window)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAutoFillBackground(false);
    if (window) {
        window->installEventFilter(this);
    }
    syncToWindow();
    show();
    raise();
}

WindowHighlightFrame::~WindowHighlightFrame()
{
    if (_window) {
        _window->removeEventFilter(this);
    }
}

void
WindowHighlightFrame::syncToWindow()
{
    if (!_window) {
        return;
    }
    setGeometry(0, 0, _window->width(), _window->height());
}

bool
WindowHighlightFrame::eventFilter(QObject* watched,
                                  QEvent* event)
{
    if (watched == _window.data()) {
        switch (event->type()) {
        case QEvent::Resize:
        case QEvent::Move:
        case QEvent::Show:
        case QEvent::ChildAdded:
            syncToWindow();
            raise();
            break;
        default:
            break;
        }
    }

    return QWidget::eventFilter(watched, event);
}

void
WindowHighlightFrame::paintEvent(QPaintEvent* /*e*/)
{
    double r = 0.;
    double g = 0.;
    double b = 0.;

    appPTR->getCurrentSettings()->getSelectionColor(&r, &g, &b);

    QColor color;

    color.setRgbF(r, g, b);

    const int penWidth = 3;
    const int halfPen = penWidth / 2;
    QPainter painter(this);
    QPen pen(color);

    pen.setWidth(penWidth);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawRect(rect().adjusted(halfPen, halfPen, -(penWidth - halfPen), -(penWidth - halfPen)));
}

NATRON_NAMESPACE_EXIT

NATRON_NAMESPACE_USING
#include "moc_ColorTrackballHud.cpp"
