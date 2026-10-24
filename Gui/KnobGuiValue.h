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

#ifndef KNOBGUIVALUE_H
#define KNOBGUIVALUE_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <vector> // KnobGuiInt
#include <list>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QObject>
#include <QStyledItemDelegate>
#include <QTextEdit>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Global/GlobalDefines.h"

#include "Engine/EngineFwd.h"
#include "Engine/Knob.h"
#include "Engine/Singleton.h"

#include "Gui/CurveSelection.h"
#include "Gui/KnobGui.h"
#include "Gui/AnimatedCheckBox.h"
#include "Gui/Label.h"
#include "Gui/GuiFwd.h"

NATRON_NAMESPACE_ENTER

struct KnobGuiValuePrivate;
class KnobGuiValue
    : public KnobGui
{
GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
GCC_DIAG_SUGGEST_OVERRIDE_ON

public:

    KnobGuiValue(KnobIPtr knob,
                 KnobGuiContainerI *container);

    virtual ~KnobGuiValue() OVERRIDE;

    virtual void removeSpecificGui() OVERRIDE FINAL;
    virtual KnobIPtr getKnob() const OVERRIDE FINAL;
    virtual bool getAllDimensionsVisible() const OVERRIDE FINAL;

    /**
     * @brief While set, a refresh never unfolds the dimensions, whatever their values.
     **/
    void setFoldStateHeld(bool held);

    /**
     * @brief Folds or expands the dimensions the way the dimension-switch button asks, without
     * pushing an undo command or copying any value, and records the result on the knob so a
     * GUI rebuilt for it starts the same way. Nothing else ever folds an unfolded knob.
     **/
    void setDimensionsFolded(bool folded);

    /**
     * @brief Gives every other dimension dimension 0's value at the current time, as folding
     * with the dimension-switch button does.
     **/
    void copyDimensionZeroValueToOthers();

    int getDimensionForSpinBox(const SpinBox* spinbox) const;

    /**
     * @brief The holder for the below-row widget when it is moved out to a row of its own
     * (see setBelowRowWidgetFullWidth()). Null for a knob with no below-row widget.
     **/
    virtual QWidget* getFullWidthRowWidget() const OVERRIDE FINAL;

public Q_SLOTS:

    void onSpinBoxValueChanged();
    void onSliderValueChanged(double);
    void onSliderEditingFinished(bool hasMovedOnce);
    void onMinMaxChanged(double mini, double maxi, int index = 0);
    void onDisplayMinMaxChanged(double mini, double maxi, int index = 0);
    void onIncrementChanged(double incr, int index = 0);
    void onDecimalsChanged(int deci, int index = 0);

    void onDimensionSwitchClicked(bool);

    void onDimensionsFoldStateRestored();

    void onRectangleFormatButtonClicked();

    void onResetToDefaultRequested();

protected:

    void getSpinBox(int dim, SpinBox** spinbox, Label** label = 0) const;

    virtual bool isSliderDisabled() const = 0;
    virtual bool isAutoFoldDimensionsEnabled() const = 0;
    virtual bool isRectangleType() const = 0;
    virtual bool isSpatialType() const = 0;
    virtual ValueIsNormalizedEnum getNormalizationPolicy(int /*dimension*/) const
    {
        return eValueIsNormalizedNone;
    }

    virtual double denormalize(int /*dimension*/,
                               double /*time*/,
                               double value) const
    {
        return value;
    }

    virtual double normalize(int /*dimension*/,
                             double /*time*/,
                             double value) const
    {
        return value;
    }

    virtual void connectKnobSignalSlots() {}

    virtual void disableSlider() = 0;
    virtual void getIncrements(std::vector<double>* increments) const = 0;
    virtual void getDecimals(std::vector<int>* /*decimals*/) const {}

    virtual void addExtraWidgets(QHBoxLayout* /*containerLayout*/) {}

    /**
     * @brief Reserves the row's dimension-label slot (before spinbox 0) at a fixed width
     * instead of hiding it when folded, so folding or unfolding the knob does not slide
     * spinbox 0 and the slider sideways.
     **/
    virtual bool wantsFixedDimensionLabelSlot() const
    {
        return false;
    }

    /**
     * @brief When true, and the row has a below-row column and a dimension-switch button,
     * unfolding the knob shows one line per extra dimension below the row, each with its
     * label, spinbox and its own slider, and the row's slider then drives dimension 0 only.
     **/
    virtual bool wantsPerDimensionSliders() const
    {
        return false;
    }

    /**
     * @brief The slider driving one dimension on its own: the row's slider for dimension 0 and
     * the line's slider for the others. Null when the knob has no per-dimension lines.
     **/
    ScaleSliderQWidget* getDimensionSlider(int dimension) const;

    /**
     * @brief The widget holding the per-dimension lines, or null when there are none.
     **/
    QWidget* getDimensionLinesWidget() const;

    /**
     * @brief Lends the channel lines out as one widget for a subclass to place elsewhere:
     * spinbox 0's slot with the row's slider, then the per-dimension lines, every slider at
     * least trackMinimum wide and with no right inset. The row keeps its buttons, packed to the
     * right. Null when the row has no slider and spinbox 0 slot to lend. The widget stays
     * owned by whoever it is placed in; reclaimChannelLines() takes its contents back.
     **/
    QWidget* lendChannelLines(int trackMinimum);

    /**
     * @brief Puts the lent lines back where they were, with their insets and minimum widths,
     * and deletes the lending widget. The caller holds updates off and flushes the layouts.
     **/
    void reclaimChannelLines();
    bool areChannelLinesLent() const;

    /**
     * @brief The width the channel lines need with trackMinimum-wide sliders and no inset.
     **/
    int getChannelLinesMinimumWidth(int trackMinimum) const;

    /**
     * @brief The below-row column's own layout (the row, the per-dimension lines when there
     * are any, and the subclass's below-row widget, top to bottom), or null for a knob with no
     * below-row widget and so no column of its own. Lets a subclass place extra widgets at a
     * specific position in that column from outside createBelowRowWidget().
     **/
    QVBoxLayout* getBelowRowColumnLayout() const;

    /**
     * @brief Moves the below-row widget into the full-width row's holder, starting in the label
     * column, or back to the end of the below-row column. The holder is shown only while it
     * holds the widget, the widget itself is not hidden and the knob is shown. The caller holds
     * updates off and flushes the layouts around the move.
     **/
    void setBelowRowWidgetFullWidth(bool fullWidth);
    bool isBelowRowWidgetFullWidth() const;

    /**
     * @brief Lets a subclass supply a widget placed below the knob's row, spanning the
     * field area. It starts hidden; the subclass that created it is responsible for
     * showing and hiding it. Returning non-null also relaxes the row's fixed height so it
     * can grow to fit the widget, and callers should pair this with wantsTopAlignedLabel()
     * so the row label does not drift when the widget appears.
     **/
    virtual QWidget* createBelowRowWidget(QWidget* /*parent*/)
    {
        return 0;
    }

    virtual void _hide() OVERRIDE;
    virtual void _show() OVERRIDE;
    virtual void setEnabledExtraGui(bool /*enabled*/) {}

    virtual void onDimensionsFolded() {}

    virtual void onDimensionsExpanded() {}

    virtual void updateExtraGui(const std::vector<double>& /*values*/)
    {
    }

private:

    /**
     * @brief Normalized parameters handling. It converts from project format
     * to normailzed coords or from project format to normalized coords.
     * @param normalize True if we want to normalize, false otherwise
     * @param dimension Must be either 0 and 1
     * @note If the dimension of the knob is not 1 or 2 this function does nothing.
     **/
    double valueAccordingToType(bool normalize, int dimension, double value) WARN_UNUSED_RETURN;


    void expandAllDimensions();
    void foldAllDimensions();
    void setDimensionLabelVisible(std::size_t index, bool visible);
    bool areAllDimensionsEqual() const;
    bool areAllDimensionCurvesEqual() const;

    void sliderEditingEnd(double d);

    void createDimensionLines(QHBoxLayout* containerLayout,
                              const std::vector<QWidget*>& boxContainers,
                              int labelSlotWidth,
                              const std::vector<double>& displayMins,
                              const std::vector<double>& displayMaxs,
                              const std::vector<double>& mins,
                              const std::vector<double>& maxs);
    void setDimensionLinesRightInset(QHBoxLayout* containerLayout);
    void seekDimensionSlidersToSpinBoxes();
    void dimensionSliderEditingEnd(int dimension, double d);
    void onDimensionSliderValueChanged(int dimension, double d);
    void onDimensionSliderEditingFinished(int dimension, bool hasMovedOnce);
    void onDimensionSliderResetRequested(int dimension);

    virtual void createWidget(QHBoxLayout* layout) OVERRIDE FINAL;

    void setMaximum(int);
    void setMinimum(int);

    virtual bool shouldAddStretch() const OVERRIDE FINAL;
    virtual void setEnabled() OVERRIDE FINAL;
    virtual void setReadOnly(bool readOnly, int dimension) OVERRIDE FINAL;
    virtual void updateGUI(int dimension) OVERRIDE FINAL;
    virtual void setDirty(bool dirty) OVERRIDE FINAL;
    virtual void reflectAnimationLevel(int dimension, AnimationLevelEnum level) OVERRIDE FINAL;
    virtual void reflectExpressionState(int dimension, bool hasExpr) OVERRIDE FINAL;
    virtual void updateToolTip() OVERRIDE FINAL;
    virtual void reflectModificationsState() OVERRIDE FINAL;
    virtual void refreshDimensionName(int dim) OVERRIDE FINAL;

private:

    std::unique_ptr<KnobGuiValuePrivate> _imp;
};

class KnobGuiDouble
    : public KnobGuiValue
{
    KnobDoubleWPtr _knob;

public:

    static KnobGui * BuildKnobGui(KnobIPtr knob,
                                  KnobGuiContainerI *container)
    {
        return new KnobGuiDouble(knob, container);
    }

    KnobGuiDouble(KnobIPtr knob,
                  KnobGuiContainerI *container);

    virtual ~KnobGuiDouble() {}

private:


    virtual bool isSliderDisabled() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool isAutoFoldDimensionsEnabled() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool isRectangleType() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool isSpatialType() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual ValueIsNormalizedEnum getNormalizationPolicy(int dimension) const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual double denormalize(int dimension, double time, double value) const OVERRIDE FINAL;
    virtual double normalize(int dimension, double time, double value) const OVERRIDE FINAL;
    virtual void connectKnobSignalSlots() OVERRIDE FINAL;
    virtual void disableSlider() OVERRIDE FINAL;
    virtual void getIncrements(std::vector<double>* increments) const OVERRIDE FINAL;
    virtual void getDecimals(std::vector<int>* decimals) const OVERRIDE FINAL;
};

class KnobGuiInt
    : public KnobGuiValue
{
    KnobIntWPtr _knob;

public:

    static KnobGui * BuildKnobGui(KnobIPtr knob,
                                  KnobGuiContainerI *container)
    {
        return new KnobGuiInt(knob, container);
    }

    KnobGuiInt(KnobIPtr knob,
               KnobGuiContainerI *container);

    virtual ~KnobGuiInt() {}

private:

    virtual bool isSpatialType() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return false;
    }

    virtual bool isSliderDisabled() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool isAutoFoldDimensionsEnabled() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual bool isRectangleType() const OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void connectKnobSignalSlots() OVERRIDE FINAL;
    virtual void disableSlider() OVERRIDE FINAL;
    virtual void getIncrements(std::vector<double>* increments) const OVERRIDE FINAL;
};

NATRON_NAMESPACE_EXIT


#endif // KNOBGUIVALUE_H
