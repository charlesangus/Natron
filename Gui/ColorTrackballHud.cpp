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

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QColor>
#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPen>
#include <QVBoxLayout>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/Settings.h"

#include "Gui/GuiApplicationManager.h"

NATRON_NAMESPACE_ENTER

ColorTrackballHud::ColorTrackballHud(QWidget* parent)
    : QWidget(parent)
    , _targetLabel(new QLabel(this))
    , _modeLabel(new QLabel(this))
    , _valuesLabel(new QLabel(this))
    , _legendC(new QLabel(QString::fromUtf8("C"), this))
    , _legendS(new QLabel(QString::fromUtf8("S"), this))
    , _legendT(new QLabel(QString::fromUtf8("T"), this))
    , _legendV(new QLabel(QString::fromUtf8("V"), this))
    , _endOfListLabel(new QLabel(this))
{
    setObjectName(QString::fromUtf8("ColorTrackballHud"));
    _targetLabel->setObjectName(QString::fromUtf8("ColorTrackballHudTarget"));
    _modeLabel->setObjectName(QString::fromUtf8("ColorTrackballHudMode"));
    _valuesLabel->setObjectName(QString::fromUtf8("ColorTrackballHudValues"));
    _legendC->setObjectName(QString::fromUtf8("ColorTrackballHudLegendC"));
    _legendS->setObjectName(QString::fromUtf8("ColorTrackballHudLegendS"));
    _legendT->setObjectName(QString::fromUtf8("ColorTrackballHudLegendT"));
    _legendV->setObjectName(QString::fromUtf8("ColorTrackballHudLegendV"));
    _endOfListLabel->setObjectName(QString::fromUtf8("ColorTrackballHudEndOfList"));

    QLabel* acceptLegend = new QLabel(tr("Esc/Enter: accept"), this);
    QLabel* stepLegend = new QLabel(tr("Ctrl+up/down: switch knob"), this);

    acceptLegend->setObjectName(QString::fromUtf8("ColorTrackballHudLegendAccept"));
    stepLegend->setObjectName(QString::fromUtf8("ColorTrackballHudLegendStepKnob"));

    QHBoxLayout* legendLayout = new QHBoxLayout();

    legendLayout->setContentsMargins(0, 0, 0, 0);
    legendLayout->setSpacing(10);
    legendLayout->addWidget(_legendC);
    legendLayout->addWidget(_legendS);
    legendLayout->addWidget(_legendT);
    legendLayout->addWidget(_legendV);
    legendLayout->addWidget(acceptLegend);
    legendLayout->addWidget(stepLegend);
    legendLayout->addStretch();

    QVBoxLayout* mainLayout = new QVBoxLayout(this);

    mainLayout->setContentsMargins(6, 4, 6, 4);
    mainLayout->setSpacing(2);
    mainLayout->addWidget(_targetLabel);
    mainLayout->addWidget(_modeLabel);
    mainLayout->addWidget(_valuesLabel);
    mainLayout->addLayout(legendLayout);
    mainLayout->addWidget(_endOfListLabel);

    _endOfListLabel->setVisible(false);
    setLegendLit(_legendC, false);
    setLegendLit(_legendS, false);
    setLegendLit(_legendT, false);
    setLegendLit(_legendV, false);
}

ColorTrackballHud::~ColorTrackballHud()
{
}

QString
ColorTrackballHud::modeKeyName(ColorGlobalModeEnum mode)
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

QString
ColorTrackballHud::modeAxesDescription(ColorGlobalModeEnum mode)
{
    switch (mode) {
    case eColorGlobalModeC:
        return tr("trackball (drag moves hue and saturation together)");
    case eColorGlobalModeS:
        return tr("x = hue, y = saturation");
    case eColorGlobalModeT:
        return tr("x = temp, y = tint");
    case eColorGlobalModeV:
        return tr("either axis = value");
    }

    return QString();
}

void
ColorTrackballHud::setLegendLit(QLabel* label,
                                bool lit)
{
    if (!label) {
        return;
    }

    QFont f = label->font();

    f.setBold(lit);
    label->setFont(f);
    // A plain bool property, read by the tests and available to a stylesheet rule, rather than
    // baking a colour in here: the HUD has no opinion on what "lit" looks like beyond bold.
    label->setProperty("trackballHudKeyLit", lit);
}

void
ColorTrackballHud::setState(const ColorTrackballHudState& state)
{
    _targetLabel->setText(tr("%1 - %2 (%3/%4)")
                              .arg(state.nodeLabel)
                              .arg(state.knobLabel)
                              .arg(state.knobIndex)
                              .arg(state.knobCount));

    _modeLabel->setText(tr("Mode %1: %2").arg(modeKeyName(state.mode)).arg(modeAxesDescription(state.mode)));

    QString values;

    switch (state.mode) {
    case eColorGlobalModeC:
    case eColorGlobalModeS:
        values = state.chromaValid
            ? tr("Hue %1deg  Sat %2").arg(state.hueDegrees, 0, 'f', 1).arg(state.saturation, 0, 'f', 4)
            : tr("Hue/Sat unavailable");
        break;
    case eColorGlobalModeT:
        values = state.chromaValid
            ? tr("Temp %1 mired  Tint %2").arg(state.temp, 0, 'f', 1).arg(state.tint, 0, 'f', 1)
            : tr("Temp/tint unavailable");
        break;
    case eColorGlobalModeV:
        values = tr("Value");
        break;
    }
    values += tr("  Y %1  A %2  Gear x%3")
                  .arg(state.luma, 0, 'f', 3)
                  .arg(state.value.hasAlpha ? QString::number(state.value.a, 'f', 3) : QString::fromUtf8("-"))
                  .arg(state.gear, 0, 'f', 2);
    _valuesLabel->setText(values);

    setLegendLit(_legendC, state.mode == eColorGlobalModeC);
    setLegendLit(_legendS, state.mode == eColorGlobalModeS);
    setLegendLit(_legendT, state.mode == eColorGlobalModeT);
    setLegendLit(_legendV, state.mode == eColorGlobalModeV);

    _endOfListLabel->setText(state.endOfListMessage);
    _endOfListLabel->setVisible(!state.endOfListMessage.isEmpty());
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
