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

#ifndef Gui_LoopFeedbackBeginList_h
#define Gui_LoopFeedbackBeginList_h

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

class QListWidget;
class QListWidgetItem;

NATRON_NAMESPACE_ENTER

/**
 * @brief Shows a LoopEnd's Feedback LoopBegins knob as the loop's LoopBegins, each with a tick
 * for whether it feeds back. The knob holds the unticked ones.
 **/
class LoopFeedbackBeginList
    : public KnobGui {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    static KnobGui* BuildKnobGui(KnobIPtr knob,
                                 KnobGuiContainerI* container)
    {
        return new LoopFeedbackBeginList(knob, container);
    }

    /**
     * @brief Whether knob is a LoopEnd's Feedback LoopBegins knob.
     **/
    static bool isListKnob(const KnobIPtr& knob);

    LoopFeedbackBeginList(KnobIPtr knob,
                          KnobGuiContainerI* container);

    virtual ~LoopFeedbackBeginList() OVERRIDE;

    virtual void removeSpecificGui() OVERRIDE FINAL;
    virtual KnobIPtr getKnob() const OVERRIDE FINAL;

public Q_SLOTS:

    /**
     * @brief Rebuilds the list once control returns to the event loop, so a tick never deletes
     * its own item under its own signal.
     **/
    void scheduleRebuild();

    void rebuild();

private Q_SLOTS:

    void onItemChanged(QListWidgetItem* item);

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

    virtual void updateGUI(int dimension) OVERRIDE FINAL;

    void disconnectWatched();

    KnobIWPtr _knob;
    QListWidget* _list;
    std::vector<NodeWPtr> _watched;
    bool _rebuildPending;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_LoopFeedbackBeginList_h
