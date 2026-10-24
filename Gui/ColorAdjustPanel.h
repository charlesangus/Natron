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

#include "Gui/ColorWheelWidget.h"
#include "Gui/GuiFwd.h"

class QResizeEvent;

NATRON_NAMESPACE_ENTER

class ColorEditSlider;
struct ColorAdjustPanelPrivate;

/**
 * @brief The colour knob's adjustment panel: the trackball wheel beside one line each for Hue,
 * Sat, Value, Temp and Tint, every line a caption, a SpinBox and a ColorEditSlider. It
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
 * mired, tint or the Offset axes), except an off-marker drag, which nudges by increments in
 * that unit; a spinbox commit is a one-shot absolute edit.
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
     * @brief The widget holding the wheel and every line.
     **/
    QWidget* getEditArea() const;

    /**
     * @brief Enables the wheel and the panel's lines. The owner turns it off while the knob
     * cannot take the panel's edits.
     **/
    void setEditable(bool editable);
    bool isEditable() const;

    /**
     * @brief The share of the panel's width the wheel takes, flush left; the lines take the
     * rest. The wheel is never shorter than the column of lines beside it. Defaults to 1/3.
     **/
    void setWheelFraction(double fraction);
    double getWheelFraction() const;

    ColorWheelWidget* getWheel() const;
    ColorEditSlider* getCustomSlider(ColorSliderKindEnum kind) const;
    SpinBox* getSpinBox(ColorSliderKindEnum kind) const;

Q_SIGNALS:
    void valuesEdited(KnobColorValue value, ColorAdjustPanel::GesturePhaseEnum phase, QString undoLabel);

    /**
     * @brief The width at which the wheel is still as tall as the lines and the lines still have
     * their minimum width, whatever width the panel has now.
     **/
    virtual QSize minimumSizeHint() const OVERRIDE;

protected:
    virtual void resizeEvent(QResizeEvent* e) OVERRIDE;

private Q_SLOTS:
    void onWheelGestureStarted();
    void onWheelGestureMoved(ColorEdit edit, QString axisLabel);
    void onWheelGestureFinished(bool moved);
    void onSliderResetRequested();
    void onCustomSliderGestureStarted();
    void onCustomSliderGestureMoved(double value, QString axisLabel);
    void onCustomSliderGestureFinished(bool moved);
    void onSpinBoxValueChanged(double value);

private:
    // It wires the edit area to the slots above.
    friend struct ColorAdjustPanelPrivate;

    std::unique_ptr<ColorAdjustPanelPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_COLORADJUSTPANEL_H
