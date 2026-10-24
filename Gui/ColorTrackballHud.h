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
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QSize>
#include <QString>
#include <QWidget>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/ColorWheelMath.h"

class QEvent;
class QLabel;
class QPaintEvent;

NATRON_NAMESPACE_ENTER

/**
 * @brief The key that selects mode: "C", "S", "T" or "V".
 **/
QString colorGlobalModeKeyName(ColorGlobalModeEnum mode);

/**
 * @brief Everything one moment of the global trackball mode HUD needs to show, gathered by the
 * controller so the widget itself needs no reference to the knob, the node, or the basis the
 * readouts were computed from.
 **/
struct ColorTrackballHudState {
    QString nodeLabel;
    QString knobLabel;
    int knobIndex = 0;
    int knobCount = 0;
    ColorGlobalModeEnum mode = eColorGlobalModeC;
    /// Offset's temp and tint are plane-axis offsets, not mired and Duv, and read in their units.
    ColorKnobRoleEnum role = eColorKnobRoleGain;
    KnobColorValue value {};
    double luma = 0.0;
    double gear = 1.0;
    /// False when the mode's own chroma readout (hue/sat for C, temp/tint for T) could not be
    /// computed, e.g. a frozen ratio role; V has no chroma readout and is always true.
    bool chromaValid = true;
    double hueDegrees = 0.0;
    double saturation = 0.0;
    double temp = 0.0;
    double tint = 0.0;
    /// "First colour knob." / "Last colour knob.", or empty.
    QString endOfListMessage;
};

/**
 * @brief Where a HUD of hudSize following the pointer at cursor goes: offset below and to the
 * right of it, or on the other side of it on any axis where that would leave screen, and in
 * any case kept on screen. An empty screen leaves just the offset applied.
 **/
QPoint colorTrackballHudFollowPosition(const QPoint& cursor, const QSize& hudSize, const QRect& screen, const QPoint& offset);

/**
 * @brief The global trackball mode's own readout, inserted into the target knob's own column
 * in the properties bin, under its channel lines, so it scrolls with the knob: the node and
 * knob (n/N), the mode
 * and what its x and y axes do, the live values (hue/saturation, temp/tint or just the value,
 * depending on the mode), Y, A, the gear, the key legend (C, S, T, V, Esc/Enter, Ctrl+up/down)
 * with the active mode key lit, and an end-of-list message once a knob switch hits either end
 * of the panel's colour knobs.
 *
 * The widget knows nothing about the controller, the knob, or the Gui it lives in: setState()
 * is the only way data reaches it, which is what lets it be exercised in isolation.
 **/
class ColorTrackballHud
    : public QWidget {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    ColorTrackballHud();

    virtual ~ColorTrackballHud() OVERRIDE;

    void setState(const ColorTrackballHudState& state);

    /**
     * @brief The flags the HUD's window has from construction: a frameless tool
     * window on top of everything that never takes focus or input.
     **/
    static Qt::WindowFlags followCursorWindowFlags();

    /**
     * @brief Places the following HUD by the pointer at globalCursor, by
     * colorTrackballHudFollowPosition() on the screen holding it.
     **/
    void followCursorTo(const QPoint& globalCursor);

protected:
    virtual void paintEvent(QPaintEvent* e) OVERRIDE;

private:
    void placeByCursor();

    static QString modeAxesDescription(ColorGlobalModeEnum mode);
    static void setLegendLit(QLabel* label, bool lit);

    QLabel* _targetLabel;
    QLabel* _modeLabel;
    QLabel* _valuesLabel;
    QLabel* _legendC;
    QLabel* _legendS;
    QLabel* _legendT;
    QLabel* _legendV;
    QLabel* _endOfListLabel;
    QPoint _cursor;
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
