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

class QTabWidget;

NATRON_NAMESPACE_ENTER

class ColorEditSlider;
class ColorSelectorPaletteButton;
struct ColorAdjustPanelPrivate;

/**
 * @brief The colour knob's adjustment panel: a Wheel tab (the trackball wheel beside the Hue,
 * Sat, Value, Temp, Tint and Alpha sliders) and a Values tab (unclamped RGBA and HSV fields, a
 * display-referred hex field and the shared colour palette). It knows nothing about knobs: the
 * owner feeds it a value, a dimension count, a role and a basis, and turns valuesEdited into
 * knob changes and undo steps.
 *
 * Every gesture is applied to the value it started from, with the steps since then accumulated
 * into one edit, so a long drag never drifts by re-rounding its own output. Each gesture is
 * reported as one Start, any number of Moves and one End; a press that never moves reports
 * nothing. While a gesture runs, setKnobValue is ignored, so the knob echoing the panel's own
 * edits back cannot re-anchor the wheel mid-drag; the wheel re-centres on the new value at End.
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

    enum ValueFieldEnum {
        eValueFieldR,
        eValueFieldG,
        eValueFieldB,
        eValueFieldA,
        eValueFieldH,
        eValueFieldS,
        eValueFieldV,
        eValueFieldHex
    };

    typedef ColorWheelWidget::DisplayConverter DisplayConverter;

    explicit ColorAdjustPanel(QWidget* parent = nullptr);
    virtual ~ColorAdjustPanel() OVERRIDE;

    /**
     * @brief nDims is 1, 3 or 4: a 1-dim knob shows only the Value slider and a single field,
     * and a 3-dim knob hides everything alpha. value must already carry hasAlpha to match.
     **/
    void setKnobValue(const KnobColorValue& value, int nDims, ColorKnobRoleEnum role, const LumaBasis& basis);

    /**
     * @brief The cheap per-refresh form, for when only the value changed: the basis and role
     * invalidate the wheel's cached disc, so they are best left alone between config changes.
     **/
    void setKnobValue(const KnobColorValue& value);
    void setDimensions(int nDims);
    void setRole(ColorKnobRoleEnum role);
    void setBasis(const LumaBasis& basis);

    /**
     * @brief Working to display conversion for the wheel and the slider gradients. An empty
     * converter means identity.
     **/
    void setDisplayConverter(const DisplayConverter& converter);

    /**
     * @brief Used by the hex field (both ways) and the palette swatches; null means identity.
     **/
    void setColorManagement(const ProjectColorManagementPtr& colorManagement);

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

    QTabWidget* getTabWidget() const;
    QWidget* getWheelTab() const;
    QWidget* getValuesTab() const;
    ColorWheelWidget* getWheel() const;
    ColorEditSlider* getSlider(ColorSliderKindEnum kind) const;
    LineEdit* getValueField(ValueFieldEnum field) const;
    int getPaletteButtonCount() const;
    ColorSelectorPaletteButton* getPaletteButton(int index) const;

    /**
     * @brief Unclamped hexcone HSV, with H in degrees in [0, 360) and S = (max - min) / max, so
     * HDR and negative values survive the round trip; max == 0 reads as S = 0.
     **/
    static void rgbToHsv(const double rgb[3], double hsv[3]);
    static void hsvToRgb(const double hsv[3], double rgb[3]);

Q_SIGNALS:
    void valuesEdited(KnobColorValue value, ColorAdjustPanel::GesturePhaseEnum phase, QString undoLabel);

public Q_SLOTS:
    void addCurrentColorToPalette();
    void clearPalette();
    void reloadPalette();

private Q_SLOTS:
    void onWheelGestureStarted();
    void onWheelGestureMoved(ColorEdit edit, QString axisLabel);
    void onWheelGestureFinished(bool moved);
    void onSliderGestureStarted();
    void onSliderGestureMoved(double value, bool absolute, QString axisLabel);
    void onSliderGestureFinished(bool moved);
    void onSliderResetRequested();
    void onValueFieldCommitted();
    void onHexCommitted();
    void onPaletteColorPicked(float r, float g, float b, float a);
    void onTabChanged(int index);

private:
    std::unique_ptr<ColorAdjustPanelPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_COLORADJUSTPANEL_H
