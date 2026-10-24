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

#ifndef COLOREDITSLIDER_H
#define COLOREDITSLIDER_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QGradientStops>
#include <QRectF>
#include <QString>
#include <QWidget>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/ColorWheelMath.h"

#include "Gui/GuiFwd.h"

NATRON_NAMESPACE_ENTER

struct ColorEditSliderPrivate;

/**
 * @brief A single horizontal gesture control for one colour-wheel axis: a track painted with
 * a caller-supplied gradient and a marker placed through ColorWheelMath::sliderMapping. Dragging
 * the marker sets the value absolutely; dragging anywhere else nudges it relatively, at the gear
 * held during each move. Past either end of the range the marker becomes a chevron. It knows its
 * ColorKnobRoleEnum (which only changes its range, through sliderMapping) and nothing else about
 * any knob: the owner reads its gesture signals and does every bit of ColorWheelMath::applyEdit
 * itself, and supplies any typed field beside it.
 **/
class ColorEditSlider
    : public QWidget {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    explicit ColorEditSlider(ColorSliderKindEnum kind, QWidget* parent = 0);

    virtual ~ColorEditSlider() OVERRIDE;

    ColorSliderKindEnum kind() const;

    void setRole(ColorKnobRoleEnum role);
    ColorKnobRoleEnum role() const;

    void setRates(const TrackballRates& rates);
    const TrackballRates& rates() const;

    // A relative drag moves this many range units per pixel (before the gear) when it is
    // positive, so a slider can share a gesture rate with the wheel locks and the global mode;
    // zero or less spreads the range over the track width instead, so the rate follows the size
    // down to the minimum track length, below which it stays at that length's rate.
    void setRelativeRatePerPixel(double unitsPerPixel);
    double relativeRatePerPixel() const;

    // sliderMapping(kind(), role(), rates()), exposed so a caller (or a test) never has to
    // re-derive it.
    SliderRange range() const;

    // Never clamped and never geared: this is the slider reporting what it has, not editing
    // it, so a value run off either end still has to paint as a chevron rather than snap back.
    void setValue(double value);
    double value() const;

    // The track's gradient. The caller supplies it because its colours (the Sat track's hue
    // end, the Temp/Tint axes) depend on state this widget never sees.
    void setGradientStops(const QGradientStops& stops);
    const QGradientStops& gradientStops() const;

    // The track's extent in widget pixels, so a caller (or a test) maps values to pixels
    // exactly as the widget does.
    QRectF trackRect() const;

    virtual QSize sizeHint() const OVERRIDE FINAL;
    virtual QSize minimumSizeHint() const OVERRIDE FINAL;

Q_SIGNALS:

    void gestureStarted();

    // value is in range()'s own unit: the marker drag's target when absolute is true, otherwise
    // the increment since the previous gestureMoved of this gesture (never the total, so a
    // caller that re-applies from its own gesture-start value never double-counts it).
    void gestureMoved(double value, bool absolute, QString axisLabel);

    void gestureFinished(bool moved);

    void resetRequested();

private:
    virtual void paintEvent(QPaintEvent* e) OVERRIDE FINAL;
    virtual void mousePressEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseMoveEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseReleaseEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseDoubleClickEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void keyPressEvent(QKeyEvent* e) OVERRIDE FINAL;
    virtual void leaveEvent(QEvent* e) OVERRIDE FINAL;

    std::unique_ptr<ColorEditSliderPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // COLOREDITSLIDER_H
