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

#ifndef NATRON_GUI_COLORTRACKBALLREVEAL_H
#define NATRON_GUI_COLORTRACKBALLREVEAL_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QPoint>
#include <QPointF>
#include <QRect>
#include <QSize>
#include <QWidget>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/ColorWheelMath.h"

#include "Gui/ColorPanelLayoutDev.h"

NATRON_NAMESPACE_ENTER

class ColorModeDiscWidget;
class ColorWheelWidget;

/**
 * @brief Where a reveal window of size goes to be centred on centre, moved as little as it takes
 * to keep it inside screen. An empty screen leaves it centred.
 **/
QPoint colorTrackballRevealPosition(const QPoint& centre, const QSize& size, const QRect& screen);

/**
 * @brief The puck on the S, T or V disc, in units of the disc's radius (x right, y up), not yet
 * clamped to the rim. strokeDx and strokeDy are the stroke's gear-scaled pointer travel so far.
 *
 * Relative: the travel alone, scaled per axis so a stroke at gear 1 reaches the rim where it has
 * moved the axis by half the panel slider's range (sliderMapping). Absolute: the axes with an
 * absolute meaning are read off live against the same ranges (S: saturation, bottom = none;
 * T: temp and tint; V: value, along the diagonal); S's hue, which has none, still follows the
 * travel, and T falls back to the travel where live has no temp/tint. The T disc runs warm on
 * the left, so its x is mirrored to stay on the colour the knob is taking.
 **/
QPointF colorTrackballRevealDiscPuck(ColorGlobalModeEnum mode, ColorPanelLayout::Puck puck, ColorKnobRoleEnum role, const LumaBasis& basis, const KnobColorValue& live, double strokeDx, double strokeDy, const TrackballRates& rates);

/**
 * @brief What the C reveal's ColorWheelWidget is given: the value it is anchored on and its puck
 * offset from there (wheel-relative plane units). Relative anchors on the stroke's start, so the
 * puck starts in the centre; Absolute anchors on the role's neutral, so the puck sits at live's
 * own place about white. Where a plane point is missing, the offset is the C stroke's own
 * translate of the travel instead.
 **/
struct ColorTrackballRevealWheelState {
    KnobColorValue anchor;
    PlanePoint offset;
};

ColorTrackballRevealWheelState colorTrackballRevealWheelState(ColorPanelLayout::Puck puck, ColorKnobRoleEnum role, const LumaBasis& basis, const KnobColorValue& start, const KnobColorValue& live, double strokeDx, double strokeDy, const TrackballRates& rates);

/**
 * @brief The global trackball mode's mode graphic: a ColorWheelWidget in C and a
 * ColorModeDiscWidget in S, T and V, in a transparent, frameless top-level window that never
 * takes input or focus, so events keep going to whatever is under it. The controller places it,
 * on the press point or following the pointer, and feeds it the stroke.
 **/
class ColorTrackballReveal
    : public QWidget {
public:
    typedef std::function<void(float* rgb, int count)> DisplayConverter;

    ColorTrackballReveal();
    virtual ~ColorTrackballReveal() OVERRIDE;

    /**
     * @brief The window's side, DPI-scaled.
     **/
    static int diameter();

    /**
     * @brief The target knob's role, basis and display conversion, and the C wheel's rim. Both
     * graphics re-render on their next paint.
     **/
    void setKnob(ColorKnobRoleEnum role, const LumaBasis& basis, const DisplayConverter& converter, double rim);

    /**
     * @brief Shows the wheel for C and the disc for the others, in place.
     **/
    void setMode(ColorGlobalModeEnum mode);

    ColorGlobalModeEnum getMode() const
    {
        return _mode;
    }

    /**
     * @brief Places the current graphic's puck for a stroke from start that has reached live
     * after strokeDx, strokeDy of gear-scaled travel; outside a stroke start and live are both
     * the knob's value and the travel is zero.
     **/
    void setStroke(ColorPanelLayout::Puck puck, const KnobColorValue& start, const KnobColorValue& live, double strokeDx, double strokeDy, const TrackballRates& rates);

    /**
     * @brief Moves the window so it is centred on globalPos, kept inside the screen holding it.
     **/
    void centreOn(const QPoint& globalPos);

    ColorWheelWidget* getWheel() const
    {
        return _wheel;
    }

    ColorModeDiscWidget* getDisc() const
    {
        return _disc;
    }

    /**
     * @brief Whichever of the wheel and the disc the current mode shows.
     **/
    QWidget* getModeWidget() const;

private:
    ColorWheelWidget* _wheel;
    ColorModeDiscWidget* _disc;
    ColorGlobalModeEnum _mode;
    ColorKnobRoleEnum _role;
    LumaBasis _basis;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_COLORTRACKBALLREVEAL_H
