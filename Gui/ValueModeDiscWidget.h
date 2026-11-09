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

#ifndef Gui_ValueModeDiscWidget_h
#define Gui_ValueModeDiscWidget_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QPointF>
#include <QWidget>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/GlobalModeKind.h"

class QPaintEvent;
class QPainter;

NATRON_NAMESPACE_ENTER

struct ColorTrackballRevealLabels;

/**
 * @brief The reveal's pill texts for a value kind: +X/−X/+Y/−Y for positions, CCW/CW for
 * rotation, Scale up/down, Larger/Smaller for size, ±Skew X/Y for skew and +/− otherwise.
 **/
ColorTrackballRevealLabels valueModeRevealDirectionLabels(GlobalModeKindEnum kind);

/**
 * @brief Draws kind's glyph inside a ring of the colour wheel's proportions centred on centre.
 * compact is the small widget-cursor variant: thicker strokes and no fine detail.
 **/
void drawValueModeGlyph(QPainter& p, GlobalModeKindEnum kind, const QPointF& centre, double outerRadius, double innerRadius, double discRadius, bool compact);

/**
 * @brief The reveal's disc for a numeric knob: the colour wheel's ring in neutral greys, so
 * value and colour modes read as one tool, the kind's glyph within it and a puck that follows
 * the stroke's travel.
 **/
class ValueModeDiscWidget
    : public QWidget {
public:
    explicit ValueModeDiscWidget(QWidget* parent = nullptr);
    virtual ~ValueModeDiscWidget() OVERRIDE;

    void setKind(GlobalModeKindEnum kind);
    GlobalModeKindEnum getKind() const
    {
        return _kind;
    }

    /**
     * @brief The stroke's travel in screen pixels (y down); zero hides the puck.
     **/
    void setTravel(double dx, double dy);

    void setCompact(bool compact);

    /**
     * @brief Where the puck sits relative to the centre, in widget pixels.
     **/
    QPointF getPuckOffset() const;

protected:
    virtual void paintEvent(QPaintEvent* e) OVERRIDE FINAL;

private:
    GlobalModeKindEnum _kind;
    double _dx;
    double _dy;
    bool _compact;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_ValueModeDiscWidget_h
