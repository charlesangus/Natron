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

#ifndef Gui_KnobGuiColorWheelGrid_h
#define Gui_KnobGuiColorWheelGrid_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <vector>

#include <QPointer>
#include <QWidget>

#include "Engine/ColorWheelMath.h"
#include "Engine/KnobTypes.h"
#include "Engine/UndoGesture.h"

#include "Gui/GuiFwd.h"
#include "Gui/KnobGui.h"

class QFrame;
class QLabel;

NATRON_NAMESPACE_ENTER

class ColorWheelWidget;
class ScaleSliderQWidget;

/**
 * @brief The GUI of a KnobSeparator with a colour wheel grid (KnobSeparator::setColorWheelGrid()):
 * the separator's line, then a full-width grid of wheels two to a row. Each cell has its title
 * above, a ColorWheelWidget in polar mode (angle = hue in degrees, radius = chroma, clamped to
 * the chroma knob's maximum), the luminance slider beneath and a readout of the three values.
 * A wheel drag writes hue and chroma as one undo step, a slider drag the luminance as one; a
 * knob driven by an expression or a link is left alone. Typed values go through the knobs' own
 * rows.
 **/
class KnobGuiColorWheelGrid
    : public KnobGui {
public:
    static KnobGui* BuildKnobGui(KnobIPtr knob,
                                 KnobGuiContainerI* container)
    {
        return new KnobGuiColorWheelGrid(knob, container);
    }

    KnobGuiColorWheelGrid(KnobIPtr knob,
                          KnobGuiContainerI* container);

    virtual ~KnobGuiColorWheelGrid() OVERRIDE;

    virtual void removeSpecificGui() OVERRIDE FINAL;
    virtual bool isLabelOnSameColumn() const OVERRIDE FINAL;
    virtual bool isLabelBold() const OVERRIDE FINAL;
    virtual KnobIPtr getKnob() const OVERRIDE FINAL;
    virtual QWidget* getFullWidthRowWidget() const OVERRIDE FINAL;

    // The wheels' diameter, unscaled.
    static const int kWheelDiameter = 140;

private:
    struct Cell {
        KnobSeparator::ColorWheelSpec spec;
        QPointer<QWidget> widget;
        QPointer<ColorWheelWidget> wheel;
        QPointer<ScaleSliderQWidget> slider;
        QPointer<QLabel> readout;
    };

    virtual bool shouldAddStretch() const OVERRIDE FINAL { return false; }

    virtual void createWidget(QHBoxLayout* layout) OVERRIDE FINAL;
    virtual void _hide() OVERRIDE FINAL;
    virtual void _show() OVERRIDE FINAL;
    virtual void setEnabled() OVERRIDE FINAL;
    virtual void setReadOnly(bool /*readOnly*/,
                             int /*dimension*/) OVERRIDE FINAL
    {
    }

    virtual void setDirty(bool /*dirty*/) OVERRIDE FINAL
    {
    }

    virtual void updateGUI(int /*dimension*/) OVERRIDE FINAL
    {
    }

    QWidget* createCell(int index,
                        QWidget* parent);
    PolarValue polarValue(int index) const;
    void refreshCell(int index);
    void onGestureStarted(int index);
    void onGestureMoved(int index,
                        const ColorEdit& edit);
    void onGestureFinished();
    void pushWheel();
    void onLuminanceSlid(int index,
                         double value);
    void onLuminanceReset(int index);

    KnobSeparatorWPtr _knob;
    QPointer<QFrame> _line;
    QPointer<QWidget> _grid;
    std::vector<Cell> _cells;
    int _activeCell;
    UndoGesture _gesture;
    PolarValue _value;
    bool _moved;
    QPointer<Gui> _draftGui;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_KnobGuiColorWheelGrid_h
