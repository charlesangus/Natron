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

#ifndef NATRON_GUI_COLORADJUSTPANEL_H
#define NATRON_GUI_COLORADJUSTPANEL_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QString>
#include <QWidget>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/ColorWheelMath.h"

#include "Gui/ColorPanelLayoutDev.h"
#include "Gui/ColorWheelWidget.h"
#include "Gui/GuiFwd.h"

class QResizeEvent;

NATRON_NAMESPACE_ENTER

class ColorEditSlider;
struct ColorAdjustPanelPrivate;

/**
 * @brief The colour knob's adjustment panel: the trackball wheel beside one line each for Hue,
 * Sat, Value, Temp and Tint, every line a caption, a SpinBox and a slider. The slider style
 * decides the slider and where the lines go: a horizontal ScaleSliderQWidget per row (Natron),
 * a horizontal ColorEditSlider per row (Custom), or a vertical ScaleSliderQWidget per column to
 * the right of the wheel, caption on top and SpinBox underneath (Vertical). It
 * knows nothing about knobs: the owner feeds it a value, a dimension count, a role and a basis,
 * and turns valuesEdited into knob changes and undo steps.
 *
 * Every gesture is applied to the value it started from, with the steps since then accumulated
 * into one edit, so a long drag never drifts by re-rounding its own output. Each gesture is
 * reported as one Start, any number of Moves and one End; a gesture that never edits (a disc
 * press released without moving) reports nothing. While a gesture runs, setKnobValue is
 * ignored, so the knob echoing the panel's own edits back cannot re-anchor the wheel mid-drag;
 * the wheel re-centres on the new value at End.
 *
 * A slider reports absolute targets in its line's unit (degrees, plane distance, stops or Y,
 * mired, tint or the Offset axes), except a ColorEditSlider's off-marker drag, which nudges by
 * increments in that unit; a spinbox commit is a one-shot absolute edit.
 *
 * The edit area is built for ColorPanelLayoutDev's current layout and rebuilt when it changes;
 * a change that arrives during a gesture waits for the gesture's End, so one gesture never spans
 * two sets of widgets. Pointers from getEditArea, getWheel, getSlider and getSpinBox are only
 * good until the next layoutApplied.
 **/
class ColorAdjustPanel
    : public QWidget {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    enum GesturePhaseEnum {
        eGesturePhaseStart,
        eGesturePhaseMove,
        eGesturePhaseEnd
    };
    Q_ENUM(GesturePhaseEnum)

    typedef ColorWheelWidget::DisplayConverter DisplayConverter;

    explicit ColorAdjustPanel(QWidget* parent = nullptr);
    virtual ~ColorAdjustPanel() OVERRIDE;

    /**
     * @brief nDims is 1, 3 or 4: a 1-dim knob shows only the Value line. value must already
     * carry hasAlpha to match.
     **/
    void setKnobValue(const KnobColorValue& value, int nDims, ColorKnobRoleEnum role, const LumaBasis& basis);

    /**
     * @brief The cheap per-refresh form, for when only the value changed: the basis and role
     * invalidate the wheel's cached disc, so they are best left alone between config changes.
     **/
    void setKnobValue(const KnobColorValue& value);
    void setRole(ColorKnobRoleEnum role);
    void setBasis(const LumaBasis& basis);

    /**
     * @brief Working to display conversion for the wheel and the slider gradients. An empty
     * converter means identity.
     **/
    void setDisplayConverter(const DisplayConverter& converter);

    /**
     * @brief Leaves this many pixels free right of the sliders, so they end at the same x as
     * the knob's own sliders, which stop short of the row's buttons.
     **/
    void setRightInset(int pixels);
    int getRightInset() const;

    /**
     * @brief The layout the edit area is currently built in, which lags the switch's value
     * while a gesture defers a change.
     **/
    const ColorPanelLayout& getLayout() const;

    /**
     * @brief The development layout bar above the edit area; null unless
     * ColorPanelLayoutDev::isDevEnabled().
     **/
    QWidget* getDevBar() const;

    const KnobColorValue& getValue() const;
    int getDimensions() const;
    ColorKnobRoleEnum getRole() const;
    const LumaBasis& getBasis() const;

    /**
     * @brief The hue a saturation edit grows along once the value is neutral: the last hue the
     * value had while it was chromatic, or the last hue set on it while neutral.
     **/
    double getRememberedHue() const;
    bool isEditing() const;

    /**
     * @brief Everything that edits: the wheel and every line. The owner disables it while the
     * knob cannot take the panel's edits.
     **/
    QWidget* getEditArea() const;
    ColorWheelWidget* getWheel() const;

    /**
     * @brief The line's ScaleSliderQWidget in the Natron and Vertical styles, null in Custom;
     * getCustomSlider is the reverse.
     **/
    ScaleSliderQWidget* getSlider(ColorSliderKindEnum kind) const;
    ColorEditSlider* getCustomSlider(ColorSliderKindEnum kind) const;
    SpinBox* getSpinBox(ColorSliderKindEnum kind) const;

Q_SIGNALS:
    void valuesEdited(KnobColorValue value, ColorAdjustPanel::GesturePhaseEnum phase, QString undoLabel);

    /**
     * @brief The edit area has been rebuilt for a new layout, getLayout() returns it, and the
     * value, role, basis, converter, inset and enabled state carry over.
     **/
    void layoutApplied();

protected:
    virtual void resizeEvent(QResizeEvent* e) OVERRIDE;

private Q_SLOTS:
    void onLayoutChanged();
    void onWheelGestureStarted();
    void onWheelGestureMoved(ColorEdit edit, QString axisLabel);
    void onWheelGestureFinished(bool moved);
    void onSliderPositionChanged(double value);
    void onSliderEditingFinished(bool hasMovedOnce);
    void onSliderResetRequested();
    void onCustomSliderGestureStarted();
    void onCustomSliderGestureMoved(double value, bool absolute, QString axisLabel);
    void onCustomSliderGestureFinished(bool moved);
    void onSpinBoxValueChanged(double value);

private:
    // It wires each rebuilt edit area to the slots above.
    friend struct ColorAdjustPanelPrivate;

    std::unique_ptr<ColorAdjustPanelPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_COLORADJUSTPANEL_H
