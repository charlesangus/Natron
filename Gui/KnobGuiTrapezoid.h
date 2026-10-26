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

#ifndef Gui_KnobGuiTrapezoid_h
#define Gui_KnobGuiTrapezoid_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>

#include <QPointer>
#include <QWidget>

#include "Engine/KnobTypes.h"
#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/UndoGesture.h"

#include "Gui/GuiFwd.h"
#include "Gui/KnobGuiValue.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The drawn range of a 4-point trapezoid knob (outer low, inner low, inner high, outer
 * high): the trapezoid over a gradient of its component, with a handle per point and a tick at
 * the source colour's component. It has no Q_OBJECT, so it needs no moc: edits are reported
 * through a callback.
 *
 * Hue points are in turns and are drawn unwrapped going up the circle from the outer low point,
 * over one turn centred on the plateau, so a range across red is never split. Saturation and
 * luma are drawn over min(0, outer low) to max(1, outer high), or to 0.4 for OkLCh chroma.
 * The domain is held during a drag, so the handle under the cursor never slides away.
 *
 * Dragging a handle moves its point, clamped between its neighbours (for hue, the outer points
 * also stay within a turn of each other). Dragging the plateau moves all four. Shift-dragging an
 * inner handle moves its outer partner by the same amount. Where handles coincide the first
 * move picks one: leftwards the lowest, rightwards the highest.
 **/
class TrapezoidStrip
    : public QWidget {
public:
    enum GesturePhaseEnum {
        eGesturePhaseStart = 0,
        eGesturePhaseMove,
        eGesturePhaseEnd
    };

    // Called with the four points after each edit of a gesture.
    typedef std::function<void(const double* values, GesturePhaseEnum phase)> EditCallback;
    // Converts count working-space RGB triples in place to display-referred values in 0 to 1.
    typedef std::function<void(float* rgb, int count)> DisplayConverter;

    // The handle or the plateau under a press.
    enum PartEnum {
        ePartNone = -1,
        ePartOuterLow = 0,
        ePartInnerLow,
        ePartInnerHigh,
        ePartOuterHigh,
        ePartPlateau
    };

    explicit TrapezoidStrip(KnobDouble::RangeWidgetRoleEnum role,
                            QWidget* parent = 0);

    virtual ~TrapezoidStrip() OVERRIDE;

    KnobDouble::RangeWidgetRoleEnum role() const
    {
        return _role;
    }

    void setValues(const double* values);
    void getValues(double* values) const;

    /**
     * @brief The model the gradient is drawn in, and the source colour's components in it, the
     * hue counting only when hueWeight is positive.
     **/
    void setContext(ColorMath::KeyModelEnum model,
                    const ColorMath::OkLabBasis& basis,
                    const float* sourceComponents,
                    float hueWeight);

    void setDisplayConverter(const DisplayConverter& converter);
    void setEditCallback(const EditCallback& callback);

    // The drawn domain, low and high, in the component's units.
    void getDomain(double* low,
                   double* high) const;

    // The positions of the four points in the drawn domain: hue points unwrapped.
    void getPositions(double* positions) const;

    double valueToX(double value) const;
    double xToValue(double x) const;

    // The part a press at x would grab, before a coincident handle is resolved by the drag.
    PartEnum partAt(double x) const;

    bool isDragging() const
    {
        return _dragPart != ePartNone;
    }

    virtual QSize sizeHint() const OVERRIDE FINAL;
    virtual QSize minimumSizeHint() const OVERRIDE FINAL;

protected:
    virtual void paintEvent(QPaintEvent* e) OVERRIDE FINAL;
    virtual void mousePressEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseMoveEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseReleaseEvent(QMouseEvent* e) OVERRIDE FINAL;

private:
    bool isHue() const
    {
        return _role == KnobDouble::eRangeWidgetRoleHue;
    }

    void computeDomain(double* low,
                       double* high) const;
    void positionsOf(const double* values,
                     double* positions) const;
    void applyDrag(double x);
    void report(GesturePhaseEnum phase);
    void componentsAt(double value,
                      float* comps) const;

    KnobDouble::RangeWidgetRoleEnum _role;
    double _values[4];
    ColorMath::KeyModelEnum _model;
    ColorMath::OkLabBasis _basis;
    float _source[3];
    float _hueWeight;
    DisplayConverter _toDisplay;
    EditCallback _onEdit;

    // The drag in progress: the part, held domain, positions at the press and press x.
    PartEnum _dragPart;
    bool _dragResolved;
    bool _dragStarted;
    bool _dragShift;
    double _dragLow;
    double _dragHigh;
    double _dragStart[4];
    double _pressX;
};

/**
 * @brief The GUI of a KnobDouble with a range-widget hint: its spinboxes below a TrapezoidStrip.
 * Strip edits go through KnobUndoCommand, one undo step per drag, and the strip repaints when the
 * source colour or the model changes.
 **/
class KnobGuiTrapezoid
    : public KnobGuiDouble {
public:
    static KnobGui* BuildKnobGui(KnobIPtr knob,
                                 KnobGuiContainerI* container)
    {
        return new KnobGuiTrapezoid(knob, container);
    }

    KnobGuiTrapezoid(KnobIPtr knob,
                     KnobGuiContainerI* container);

    virtual ~KnobGuiTrapezoid() OVERRIDE;

    TrapezoidStrip* getStrip() const;

    // The label sits level with the strip at the top of the column.
    virtual bool wantsTopAlignedLabel() const OVERRIDE FINAL;

private:
    virtual QWidget* createBelowRowWidget(QWidget* parent) OVERRIDE FINAL;
    virtual void addExtraWidgets(QHBoxLayout* containerLayout) OVERRIDE FINAL;
    virtual void updateExtraGui(const std::vector<double>& values) OVERRIDE FINAL;
    virtual void setEnabledExtraGui(bool enabled) OVERRIDE FINAL;
    virtual void _hide() OVERRIDE FINAL;
    virtual void _show() OVERRIDE FINAL;

    void refreshContext();
    void onStripEdited(const double* values,
                       TrapezoidStrip::GesturePhaseEnum phase);

    KnobDoubleWPtr _rangeKnob;
    QPointer<TrapezoidStrip> _strip;
    UndoGesture _gesture;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_KnobGuiTrapezoid_h
