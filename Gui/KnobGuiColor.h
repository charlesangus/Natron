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

#ifndef Gui_KnobGuiColor_h
#define Gui_KnobGuiColor_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <list>
#include <memory>
#include <vector> // KnobGuiInt

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QObject>
#include <QPointF>
#include <QPointer>
#include <QStyledItemDelegate>
#include <QTextEdit>
#include <QToolButton>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Global/GlobalDefines.h"
#include "Global/QtCompat.h"

#include "Engine/EngineFwd.h"
#include "Engine/Knob.h"
#include "Engine/Singleton.h"

#include "Gui/AnimatedCheckBox.h"
#include "Gui/ColorAdjustPanel.h"
#include "Gui/ColorSelectorWidget.h"
#include "Gui/ColorTrackballTarget.h"
#include "Gui/CurveSelection.h"
#include "Gui/GuiFwd.h"
#include "Gui/KnobGuiValue.h"
#include "Gui/Label.h"

class QFocusEvent;

NATRON_NAMESPACE_ENTER

//================================

class KnobGuiColor;
class ColorKnobEditSession;
class ColorPickerLabel
    : public Label
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    ColorPickerLabel(KnobGuiColor* knob,
                     QWidget* parent = NULL);

    virtual ~ColorPickerLabel() OVERRIDE
    {
    }

    bool isPickingEnabled() const
    {
        return _pickingEnabled;
    }

    void setColor(const QColor & color);

    const QColor& getCurrentColor() const
    {
        return _currentColor;
    }

    void setPickingEnabled(bool enabled);

    void setEnabledMode(bool enabled);

Q_SIGNALS:

    void pickingEnabled(bool);

private:

    virtual void enterEvent(QEnterEvent*) OVERRIDE FINAL;
    virtual void leaveEvent(QEvent*) OVERRIDE FINAL;
    virtual void mousePressEvent(QMouseEvent*) OVERRIDE FINAL;
    virtual void mouseMoveEvent(QMouseEvent*) OVERRIDE FINAL;
    virtual void mouseReleaseEvent(QMouseEvent*) OVERRIDE FINAL;
    virtual void keyPressEvent(QKeyEvent*) OVERRIDE FINAL;
    virtual void keyReleaseEvent(QKeyEvent*) OVERRIDE FINAL;
    virtual void focusOutEvent(QFocusEvent*) OVERRIDE FINAL;
    virtual bool event(QEvent*) OVERRIDE FINAL;

    bool isLockKey(int key) const;
    bool wantsShortcutOverride(const QKeyEvent* e) const;
    ColorAxisEnum activeLock() const;
    QString axisLabel(ColorAxisEnum axis) const;

    /**
     * @brief Starts the swatch's trackball gesture once a drag has cleared the click
     * threshold. False when the knob cannot be edited right now (an expression, a slaved
     * dimension, or disabled), in which case the press stays a plain click no matter how far
     * it then moves.
     **/
    bool beginTrackballDrag();
    void stepTrackballDrag(const QPointF& pos, Qt::KeyboardModifiers modifiers);
    void endTrackballDrag();

private:

    bool _pickingEnabled;
    QColor _currentColor;
    KnobGuiColor* _knob;

    // Click-vs-drag state: a press only decides what it was once it either moves past
    // QApplication::startDragDistance() (a trackball drag) or releases first (a click).
    bool _pressed;
    bool _dragging;
    QPointF _pressPos;
    QPointF _lastDragPos;
    KnobColorValue _segmentBase;
    KnobColorValue _dragValue;
    ColorEdit _dragTotal;
    bool _hasDragTotal;
    std::vector<int> _heldLocks;
    bool _hovered;
    bool _keyboardGrabbed;
};


class KnobGuiColor
    : public KnobGuiValue
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    static KnobGui * BuildKnobGui(KnobIPtr knob,
                                  KnobGuiContainerI *container)
    {
        return new KnobGuiColor(knob, container);
    }

    KnobGuiColor(KnobIPtr knob,
                 KnobGuiContainerI *container);

    virtual ~KnobGuiColor() OVERRIDE;

    virtual bool wantsTopAlignedLabel() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    /**
     * @brief The row's adjustment panel, or null for simplified and viewer knobs and for knobs
     * that share their line with other knobs, which keep the colour selector popup.
     **/
    ColorAdjustPanel* getAdjustPanel() const;
    bool isAdjustPanelExpanded() const;
    void setAdjustPanelExpanded(bool expanded);

    ColorKnobEditSession* getEditSession();

    /**
     * @brief Records this knob as the last one touched on its panel, for the global trackball
     * mode's target resolution. Called on every edit session's Start, and by the app-wide
     * ColorKnobTouchTracker filter when a press, wheel or focus-in lands on the row or panel
     * without going through a session (for example a plain spinbox click).
     **/
    void touchColorKnob();

    /**
     * @brief The knob's colour role, luma-chroma basis, current value, and whether it can be
     * edited right now (not disabled, no expression or slave on an RGB dimension). Used by
     * ColorPickerLabel to run the swatch's own trackball gesture through the same edit session
     * the adjustment panel uses.
     **/
    ColorKnobRoleEnum getColorRole() const;
    LumaBasis getEditBasis() const;
    KnobColorValue getCurrentColorValue() const;
    bool isColorEditable() const;

    /**
     * @brief The row's own container widget, which the global trackball mode highlights and a
     * readout of the mode can scroll into view. Null for knobs without an adjustment panel.
     **/
    QWidget* getGlobalModeRowWidget() const;

public Q_SLOTS:

    /**
     * @brief Engages the global trackball mode on this knob. Does nothing on simplified and
     * viewer knobs, which have no adjustment panel.
     **/
    void engageGlobalMode();

    void setGlobalModeHighlighted(bool highlighted);

    void updateColorSelector();

    void setPickingEnabled(bool enabled);

    void onPickingEnabled(bool enabled);

    void onMustShowAllDimension();

    void onColorSelectorChanged(float r, float g, float b, float a);

    void onAdjustPanelToggled(bool expanded);

    void onAdjustPanelValuesEdited(KnobColorValue value, ColorAdjustPanel::GesturePhaseEnum phase, QString undoLabel);

    void onProjectColorConfigChanged();

    void refreshAdjustPanelState();

Q_SIGNALS:

    void dimensionSwitchToggled(bool b);

private:

    virtual bool isSpatialType() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return false;
    }

    virtual bool isSliderDisabled() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return false;
    }

    virtual bool isAutoFoldDimensionsEnabled() const OVERRIDE FINAL WARN_UNUSED_RETURN;

    virtual bool isRectangleType() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return false;
    }

    virtual void disableSlider() OVERRIDE FINAL
    {
    }

    virtual void connectKnobSignalSlots() OVERRIDE FINAL;
    virtual void getIncrements(std::vector<double>* increments) const OVERRIDE FINAL;
    virtual void getDecimals(std::vector<int>* decimals) const OVERRIDE FINAL;
    virtual void addExtraWidgets(QHBoxLayout* containerLayout) OVERRIDE FINAL;
    virtual void updateExtraGui(const std::vector<double>& values) OVERRIDE FINAL;
    virtual QWidget* createBelowRowWidget(QWidget* parent) OVERRIDE FINAL;

    ProjectColorManagementPtr getColorManagement() const;
    void refreshAdjustPanelColorManagement();
    bool isAdjustPanelEditable() const;

    void updateLabel(double r, double g, double b, double a);

    virtual void _show() OVERRIDE FINAL;
    virtual void _hide() OVERRIDE FINAL;
    virtual void setEnabledExtraGui(bool enabled) OVERRIDE FINAL;
    virtual void onDimensionsFolded() OVERRIDE FINAL;
    virtual void onDimensionsExpanded() OVERRIDE FINAL;

private:

    KnobColorWPtr _knob;
    ColorPickerLabel *_colorLabel;
    ColorSelectorWidget *_colorSelector;
    QToolButton *_colorSelectorButton;
    std::vector<double> _lastColor;
    bool _useSimplifiedUI;
    QPointer<ColorAdjustPanel> _adjustPanel;
    QPointer<QWidget> _rowContainer;
    std::unique_ptr<ColorKnobEditSession> _editSession;
    std::weak_ptr<ProjectColorManagement> _configCallbackOwner;
    int _configCallbackId;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_KnobGuiColor_h
