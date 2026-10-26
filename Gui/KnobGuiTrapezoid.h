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
 * @brief The drawn range of a trapezoid knob holding a centre and four offsets
 * (KnobDouble::RangeWidgetDimensionEnum): the trapezoid over a gradient of its component, with
 * no line along its base, a handle per point, a mark at the centre and a tick at the source
 * colour's component. It has no Q_OBJECT, so it needs no moc: edits are reported through a
 * callback.
 *
 * Points are drawn as the offsets key (KnobDouble::sanitizeRangeOffsets()). Hue is in turns and
 * its points lie around the centre, so a range across red is never split. Until it is zoomed or
 * panned the strip shows its default domain: one turn centred on the centre for hue, 0 to 1 for
 * saturation (0 to 0.4 for OkLCh chroma) and 0 to max(1, centre + outer high) for luma. The
 * mouse wheel zooms around the cursor, a middle or Alt drag pans, and resetZoom() goes back to
 * the default domain. The domain is held during a drag, so the handle under the cursor never
 * slides away.
 *
 * Dragging a handle moves its point, clamped between its neighbours, an inner point never
 * crossing the centre (for hue, an outer point also stays within a turn of the centre). Dragging
 * the plateau moves the centre. Shift-dragging an inner handle moves its outer partner by the
 * same amount. Where handles coincide the first move picks one: leftwards the lowest, rightwards
 * the highest.
 **/
class TrapezoidStrip
    : public QWidget {
public:
    enum GesturePhaseEnum {
        eGesturePhaseStart = 0,
        eGesturePhaseMove,
        eGesturePhaseEnd
    };

    // Called with the knob's values, centre first, after each edit of a gesture.
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

    // KnobDouble::eRangeWidgetDimensionCount values, centre first.
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

    // The domain resetZoom() goes back to.
    void getDefaultDomain(double* low,
                          double* high) const;

    /**
     * @brief The positions of the outer low, inner low, inner high and outer high points in the
     * drawn domain, and the centre's when centre is not null. Hue positions are shifted by the
     * whole turns that bring the centre nearest the middle of the domain.
     **/
    void getPositions(double* positions,
                      double* centre = 0) const;

    double valueToX(double value) const;
    double xToValue(double x) const;

    // The part a press at x would grab, before a coincident handle is resolved by the drag.
    PartEnum partAt(double x) const;

    bool isDragging() const
    {
        return _dragPart != ePartNone;
    }

    // Scales the domain by factor around the value under x: below 1 zooms in.
    void zoomAround(double x,
                    double factor);

    // Moves the domain so the value under one x lands dx pixels to the right.
    void panByPixels(double dx);

    bool isZoomed() const
    {
        return _zoomed;
    }

    void resetZoom();

    virtual QSize sizeHint() const OVERRIDE FINAL;
    virtual QSize minimumSizeHint() const OVERRIDE FINAL;

protected:
    virtual void paintEvent(QPaintEvent* e) OVERRIDE FINAL;
    virtual void mousePressEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseMoveEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void mouseReleaseEvent(QMouseEvent* e) OVERRIDE FINAL;
    virtual void wheelEvent(QWheelEvent* e) OVERRIDE FINAL;

private:
    bool isHue() const
    {
        return _role == KnobDouble::eRangeWidgetRoleHue;
    }

    void positionsOf(const double* values,
                     double* positions,
                     double* centre) const;
    void applyDrag(double x);
    void report(GesturePhaseEnum phase);
    void componentsAt(double value,
                      float* comps) const;

    KnobDouble::RangeWidgetRoleEnum _role;
    double _values[KnobDouble::eRangeWidgetDimensionCount];
    ColorMath::KeyModelEnum _model;
    ColorMath::OkLabBasis _basis;
    float _source[3];
    float _hueWeight;
    DisplayConverter _toDisplay;
    EditCallback _onEdit;

    // The domain set by zooming or panning, used while _zoomed.
    bool _zoomed;
    double _viewLow;
    double _viewHigh;

    // The pan in progress: the domain and x at the press.
    bool _panning;
    double _panLow;
    double _panHigh;
    double _panX;

    // The drag in progress: the part, held domain, values, positions and centre at the press,
    // and press x.
    PartEnum _dragPart;
    bool _dragResolved;
    bool _dragStarted;
    bool _dragShift;
    double _dragLow;
    double _dragHigh;
    double _dragValues[KnobDouble::eRangeWidgetDimensionCount];
    double _dragStart[4];
    double _dragCentre;
    double _pressX;
};

/**
 * @brief The GUI of a KnobDouble with a range-widget hint: a TrapezoidStrip with Recentre and
 * Reset Zoom buttons, above the spinboxes, each labelled above its field. Strip edits go through
 * KnobUndoCommand, one undo step per drag; Recentre sets the centre to the source colour's
 * component in one undo step, keeping the offsets. The strip repaints when the source colour or
 * the model changes, and the row greys out while the hint's enable knob is off.
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

    // Sets the centre to the source colour's component; a hue range stays for a source with no hue.
    void recentre();

private:
    virtual QWidget* createBelowRowWidget(QWidget* parent) OVERRIDE FINAL;
    virtual void addExtraWidgets(QHBoxLayout* containerLayout) OVERRIDE FINAL;
    virtual void updateExtraGui(const std::vector<double>& values) OVERRIDE FINAL;
    virtual void setEnabledExtraGui(bool enabled) OVERRIDE FINAL;
    virtual void _hide() OVERRIDE FINAL;
    virtual void _show() OVERRIDE FINAL;

    // The source colour's components in the current model, false without a range knob.
    bool sourceComponents(ColorMath::KeyModelEnum* model,
                          ColorMath::OkLabBasis* basis,
                          float* comps,
                          float* hueWeight) const;
    void refreshContext();
    void refreshEnabledLook();
    void onStripEdited(const double* values,
                       TrapezoidStrip::GesturePhaseEnum phase);

    KnobDoubleWPtr _rangeKnob;
    QPointer<QWidget> _stripRow;
    QPointer<TrapezoidStrip> _strip;
    UndoGesture _gesture;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_KnobGuiTrapezoid_h
