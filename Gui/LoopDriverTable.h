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

#ifndef Gui_LoopDriverTable_h
#define Gui_LoopDriverTable_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <vector>

CLANG_DIAG_OFF(deprecated)
CLANG_DIAG_OFF(uninitialized)
#include <QObject>
CLANG_DIAG_ON(deprecated)
CLANG_DIAG_ON(uninitialized)

#include "Engine/EngineFwd.h"

#include "Gui/GuiFwd.h"
#include "Gui/KnobGui.h"

class QTableWidget;

NATRON_NAMESPACE_ENTER

/**
 * @brief The table on LoopEnd's panel of every driven knob dimension in the loop's body: its
 * node, parameter, variable, start, end and easing, edited in place through the same undoable
 * commands as the knob's own driver row, and its value at the loop's preview iteration.
 **/
class LoopDriverTable
    : public KnobGui {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    static KnobGui* BuildKnobGui(KnobIPtr knob,
                                 KnobGuiContainerI* container)
    {
        return new LoopDriverTable(knob, container);
    }

    /**
     * @brief Whether knob is the placeholder of a LoopEnd that the table shows for.
     **/
    static bool isTableKnob(const KnobIPtr& knob);

    LoopDriverTable(KnobIPtr knob,
                    KnobGuiContainerI* container);

    virtual ~LoopDriverTable() OVERRIDE;

    virtual void removeSpecificGui() OVERRIDE FINAL;
    virtual KnobIPtr getKnob() const OVERRIDE FINAL;

public Q_SLOTS:

    /**
     * @brief Rebuilds the table once control returns to the event loop, so an edit made from one
     * of its cells never deletes that cell under its own signal.
     **/
    void scheduleRebuild();

    void rebuild();

    void refreshValues();

private Q_SLOTS:

    void onVariableChanged(int index);
    void onStartChanged(double value);
    void onEndChanged(double value);
    void onEasingChanged(int index);

private:
    virtual bool shouldAddStretch() const OVERRIDE FINAL { return false; }

    virtual void createWidget(QHBoxLayout* layout) OVERRIDE FINAL;
    virtual void _hide() OVERRIDE FINAL;
    virtual void _show() OVERRIDE FINAL;
    virtual void setEnabled() OVERRIDE FINAL
    {
    }

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

    struct Entry {
        KnobIWPtr knob;
        int dimension;
    };

    // The row of the widget that sent the current signal, -1 if none.
    int senderRow() const;

    void disconnectWatched();

    KnobIWPtr _knob;
    QTableWidget* _table;
    std::vector<Entry> _entries;
    std::vector<KnobIWPtr> _watched;
    bool _rebuildPending;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_LoopDriverTable_h
