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

#ifndef NATRON_GUI_COLORPANELLAYOUTDEV_H
#define NATRON_GUI_COLORPANELLAYOUTDEV_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QObject>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Gui/GuiFwd.h"

class QWidget;

NATRON_NAMESPACE_ENTER

/**
 * @brief How the colour knob's adjustment panel is laid out. Every axis is independent; the
 * default is the shipping layout. Nothing here is saved anywhere.
 **/
struct ColorPanelLayout {
    enum class SliderStyle {
        Natron,
        Custom,
        Vertical
    };

    enum class Placement {
        InKnob,
        BinTop
    };

    // The panel starts in the label column instead of the field column.
    bool extendLeft = false;

    // The panel's own lines run on under the row's buttons instead of stopping at their inset.
    bool extendRight = false;
    SliderStyle style = SliderStyle::Natron;
    Placement placement = Placement::InKnob;

    bool operator==(const ColorPanelLayout& other) const;
    bool operator!=(const ColorPanelLayout& other) const;
};

/**
 * @brief The process-wide development switch for the colour panel layout. The value can be
 * read and set from anywhere; the per-panel bar that sets it interactively only exists when
 * NATRON_COLOR_PANEL_DEV is set to a non-empty value other than 0.
 **/
class ColorPanelLayoutDev
    : public QObject {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    static ColorPanelLayoutDev* instance();

    /**
     * @brief Reads NATRON_COLOR_PANEL_DEV on the first call and answers from that afterwards.
     **/
    static bool isDevEnabled();

    /**
     * @brief Re-reads the environment variable and puts the default layout back, without
     * emitting layoutChanged. Only for tests, which toggle the variable within one process.
     **/
    static void reloadForTests();

    /**
     * @brief A one-line bar of controls bound to the switch: they change it and follow its
     * changes made anywhere else. Null when isDevEnabled() is false.
     **/
    static QWidget* createDevBar(QWidget* parent);

    const ColorPanelLayout& getLayout() const;

    /**
     * @brief Emits layoutChanged once if any axis differs from the current value.
     **/
    void setLayout(const ColorPanelLayout& layout);
    void setExtendLeft(bool extend);
    void setExtendRight(bool extend);
    void setSliderStyle(ColorPanelLayout::SliderStyle style);
    void setPlacement(ColorPanelLayout::Placement placement);

Q_SIGNALS:
    void layoutChanged();

private:
    ColorPanelLayoutDev();

    ColorPanelLayout _layout;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_COLORPANELLAYOUTDEV_H
