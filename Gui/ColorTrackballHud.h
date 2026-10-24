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

#ifndef Gui_ColorTrackballHud_h
#define Gui_ColorTrackballHud_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QColor>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QRegion>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QWidget>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/ColorWheelMath.h"

class QEvent;
class QPaintEvent;

NATRON_NAMESPACE_ENTER

/**
 * @brief The key that selects mode: "C", "S", "T" or "V".
 **/
QString colorGlobalModeKeyName(ColorGlobalModeEnum mode);

/**
 * @brief The flags of the trackball's windows that follow or sit by the pointer: a frameless
 * tool window on top of everything that never takes focus or input.
 **/
Qt::WindowFlags colorTrackballFollowCursorWindowFlags();

/**
 * @brief What the global trackball mode's hint block shows, gathered by the controller so the
 * widget needs no reference to the knob or the node.
 **/
struct ColorTrackballHudState {
    QString knobLabel;
    ColorGlobalModeEnum mode = eColorGlobalModeC;
    double gear = 1.0;
    /// "First colour knob." / "Last colour knob.", or empty.
    QString endOfListMessage;
};

/**
 * @brief Which of the block's gear hints gear() gives for what is held: 0 for Ctrl, 1 for
 * Shift, 2 for Ctrl+Shift, -1 for none.
 **/
int colorTrackballHudLitGear(double gear);

/**
 * @brief Where a window of hudSize following the pointer at cursor goes: offset below and to
 * the right of it, or on the other side of it on any axis where that would leave screen, and in
 * any case kept on screen. An empty screen leaves just the offset applied.
 **/
QPoint colorTrackballHudFollowPosition(const QPoint& cursor, const QSize& hudSize, const QRect& screen, const QPoint& offset);

/**
 * @brief Fill and edge of the trackball's pills and hint block. Opaque, since nothing paints
 * behind a frameless window without a compositor.
 **/
QColor colorTrackballHudFill();
QColor colorTrackballHudEdge();

/**
 * @brief A window mask covering a rounded rect, and the hint block's corner radius.
 **/
QRegion colorTrackballRoundedRegion(const QRect& rect, double radius);
double colorTrackballHudBlockRadius();

/**
 * @brief One direction label outside the trackball reveal's ring, such as "+Hue": a small
 * dark rounded pill sized to its text.
 **/
class ColorTrackballPill
    : public QWidget {
public:
    explicit ColorTrackballPill(QWidget* parent = nullptr);
    virtual ~ColorTrackballPill() OVERRIDE;

    void setText(const QString& text);
    QString text() const { return _text; }

    virtual QSize sizeHint() const OVERRIDE FINAL;

protected:
    virtual void paintEvent(QPaintEvent* e) OVERRIDE FINAL;

private:
    QString _text;
};

/**
 * @brief The global trackball mode's hint block: the knob's name, the C, S, T and V keys with
 * the active one filled in the selection colour, the gear hints with the held one lit, the
 * accept and knob-switch keys, a line on how C works in C, and the end-of-list message when
 * set. It sits under the reveal's disc, or, in a window of its own made with
 * colorTrackballFollowCursorWindowFlags(), by the pointer while no disc is shown.
 *
 * setState() is the only way data reaches it, which is what lets it be exercised in isolation.
 **/
class ColorTrackballHudBlock
    : public QWidget {
public:
    explicit ColorTrackballHudBlock(QWidget* parent = nullptr, Qt::WindowFlags flags = Qt::WindowFlags());
    virtual ~ColorTrackballHudBlock() OVERRIDE;

    void setState(const ColorTrackballHudState& state);
    const ColorTrackballHudState& getState() const { return _state; }

    /**
     * @brief The block's lines of text top to bottom, the key caps as "C S T V".
     **/
    QStringList lines() const;

    /**
     * @brief The lit mode key and gear hint (see colorTrackballHudLitGear).
     **/
    ColorGlobalModeEnum litKey() const { return _state.mode; }
    int litGear() const;

    /**
     * @brief As a window of its own, places it by the pointer at globalCursor, by
     * colorTrackballHudFollowPosition() on the screen holding it, and again by the same pointer
     * whenever a new state resizes it.
     **/
    void followCursorTo(const QPoint& globalCursor);

    virtual QSize sizeHint() const OVERRIDE FINAL;

protected:
    virtual void paintEvent(QPaintEvent* e) OVERRIDE FINAL;

private:
    ColorTrackballHudState _state;
    QPoint _cursor;
    bool _followsCursor;
};

/**
 * @brief A WA_TransparentForMouseEvents overlay kept the same size as, and on top of, a window
 * (the application main window, or a FloatingWidget holding the properties bin or a viewer)
 * for as long as the global trackball mode is engaged, painting a 3px border in the user's
 * selection colour (Settings::getSelectionColor) so the whole window reads as "the trackball
 * mode is live" no matter where the pointer happens to be, since a stroke can start anywhere.
 *
 * An event filter installed on the window re-syncs the overlay's geometry and raises it on
 * resize, move, show and whenever the window gains a new child, since any of those can
 * otherwise leave it the wrong size or buried under a widget added after it.
 **/
class WindowHighlightFrame
    : public QWidget {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    explicit WindowHighlightFrame(QWidget* window);

    virtual ~WindowHighlightFrame() OVERRIDE;

protected:
    virtual void paintEvent(QPaintEvent* e) OVERRIDE FINAL;
    virtual bool eventFilter(QObject* watched, QEvent* event) OVERRIDE FINAL;

private:
    void syncToWindow();

    QPointer<QWidget> _window;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_ColorTrackballHud_h
