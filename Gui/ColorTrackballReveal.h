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
#include <QString>
#include <QWidget>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/ColorWheelMath.h"

NATRON_NAMESPACE_ENTER

class ColorModeDiscWidget;
class ColorTrackballHudBlock;
class ColorTrackballPill;
class ColorWheelWidget;
struct ColorTrackballHudState;

/**
 * @brief Where a reveal window of size, whose disc is centred discOffset from its top-left, goes
 * to put the disc's centre on centre, moved as little as it takes to keep the window inside
 * screen. An empty screen leaves the disc on centre.
 **/
QPoint colorTrackballRevealPosition(const QPoint& centre, const QPoint& discOffset, const QSize& size, const QRect& screen);

/**
 * @brief The pills around the S, T and V discs: what a drag toward each side does. C has none,
 * since its ring already labels every direction.
 **/
struct ColorTrackballRevealLabels {
    QString right;
    QString left;
    QString top;
    QString bottom;
};

ColorTrackballRevealLabels colorTrackballRevealDirectionLabels(ColorGlobalModeEnum mode);

/**
 * @brief The puck on the S, T or V disc, in units of the disc's radius (x right, y up), not yet
 * clamped to the rim. strokeDx and strokeDy are the stroke's gear-scaled pointer travel so far,
 * scaled per axis so a stroke at gear 1 reaches the rim where it has moved the axis by half the
 * panel slider's range (sliderMapping); the puck starts in the centre and travels with the stroke.
 **/
QPointF colorTrackballRevealDiscPuck(ColorGlobalModeEnum mode, ColorKnobRoleEnum role, double strokeDx, double strokeDy, const TrackballRates& rates);

/**
 * @brief What the C reveal's ColorWheelWidget is given: the value it is anchored on and its puck
 * offset from there (wheel-relative plane units). It anchors on the stroke's start, so the puck
 * starts in the centre. Where a plane point is missing, the offset is the C stroke's own
 * translate of the travel instead.
 **/
struct ColorTrackballRevealWheelState {
    KnobColorValue anchor;
    PlanePoint offset;
};

ColorTrackballRevealWheelState colorTrackballRevealWheelState(ColorKnobRoleEnum role, const LumaBasis& basis, const KnobColorValue& start, const KnobColorValue& live, double strokeDx, double strokeDy, const TrackballRates& rates);

/**
 * @brief The global trackball mode's mode graphic: a ColorWheelWidget in C and a
 * ColorModeDiscWidget in S, T and V, with a direction pill outside the ring on each side in S, T
 * and V and the hint block (ColorTrackballHudBlock) underneath, in a frameless top-level window
 * masked to those shapes that never takes input or focus, so events keep going to whatever is
 * under it. The controller places it by its disc's centre, on the press point or following the
 * pointer, and feeds it the stroke and the hint block's state.
 **/
class ColorTrackballReveal
    : public QWidget {
public:
    typedef std::function<void(float* rgb, int count)> DisplayConverter;

    ColorTrackballReveal();
    virtual ~ColorTrackballReveal() OVERRIDE;

    enum PillSideEnum {
        ePillRight = 0,
        ePillLeft,
        ePillTop,
        ePillBottom
    };

    /**
     * @brief The disc's side, DPI-scaled. The window is larger, to hold the pills and the block.
     **/
    static int diameter();

    /**
     * @brief The target knob's role, basis and display conversion, and the C wheel's rim. Both
     * graphics re-render on their next paint.
     **/
    void setKnob(ColorKnobRoleEnum role, const LumaBasis& basis, const DisplayConverter& converter, double rim);

    /**
     * @brief Shows the wheel for C and the disc for the others, in place, with that mode's
     * pills. The disc keeps its place on screen.
     **/
    void setMode(ColorGlobalModeEnum mode);

    /**
     * @brief What the hint block under the disc shows. The disc keeps its place on screen.
     **/
    void setHudState(const ColorTrackballHudState& state);

    ColorGlobalModeEnum getMode() const
    {
        return _mode;
    }

    /**
     * @brief Places the current graphic's puck for a stroke from start that has reached live
     * after strokeDx, strokeDy of gear-scaled travel; outside a stroke start and live are both
     * the knob's value and the travel is zero.
     **/
    void setStroke(const KnobColorValue& start, const KnobColorValue& live, double strokeDx, double strokeDy, const TrackballRates& rates);

    /**
     * @brief Moves the window so its disc is centred on globalPos, the window kept inside the
     * screen holding it.
     **/
    void centreOn(const QPoint& globalPos);

    /**
     * @brief The disc's rect and centre in window coordinates.
     **/
    QRect getDiscRect() const
    {
        return _discRect;
    }

    QPoint getDiscCentre() const
    {
        return _discRect.topLeft() + QPoint(_discRect.width() / 2, _discRect.height() / 2);
    }

    /**
     * @brief The pill on side; hidden in C.
     **/
    ColorTrackballPill* getPill(PillSideEnum side) const
    {
        return _pills[side];
    }

    ColorTrackballHudBlock* getHudBlock() const
    {
        return _block;
    }

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
    // Lays the disc, pills and block out for the mode and state, sizes and masks the window,
    // and keeps the disc's centre where it was on screen.
    void relayout();

    ColorWheelWidget* _wheel;
    ColorModeDiscWidget* _disc;
    ColorGlobalModeEnum _mode;
    ColorKnobRoleEnum _role;
    LumaBasis _basis;
    ColorTrackballPill* _pills[4];
    ColorTrackballHudBlock* _block;
    QRect _discRect;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_COLORTRACKBALLREVEAL_H
