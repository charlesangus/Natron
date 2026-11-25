/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2023 The Natron developers
 *
 * Natron is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Natron is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Natron.  If not, see <http://www.gnu.org/licenses/gpl-3.0.html>
 * ***** END LICENSE BLOCK ***** */

#ifndef Gui_KnobGuiColorWarpPins_h
#define Gui_KnobGuiColorWarpPins_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>

#include "Engine/EngineFwd.h"

#include "Gui/ColorGlobalModeHook.h"
#include "Gui/GuiFwd.h"
#include "Gui/KnobGui.h"

class QTreeWidget;

NATRON_NAMESPACE_ENTER

struct KnobGuiColorWarpPinsPrivate;

/**
 * @brief The GUI of a ColorWarp node's pin set: a list of the pins (enabled, name, source and
 * target swatches) with add and delete buttons, and below it the selected pin's target,
 * strength and falloff with their key buttons. Selection is the knob's, so the list and the
 * viewer follow each other. The selected pin's target is the colour the global mode drives.
 **/
class KnobGuiColorWarpPins
    : public KnobGui,
      public ColorGlobalModeHookProvider {
public:
    static KnobGui* BuildKnobGui(KnobIPtr knob,
                                 KnobGuiContainerI* container)
    {
        return new KnobGuiColorWarpPins(knob, container);
    }

    KnobGuiColorWarpPins(KnobIPtr knob,
                         KnobGuiContainerI* container);

    virtual ~KnobGuiColorWarpPins() OVERRIDE;

    virtual void removeSpecificGui() OVERRIDE FINAL;
    virtual KnobIPtr getKnob() const OVERRIDE FINAL;

    virtual bool shouldCreateLabel() const OVERRIDE FINAL
    {
        return false;
    }

    virtual std::shared_ptr<ColorGlobalModeHook> makeColorGlobalModeHook() OVERRIDE FINAL;

    QTreeWidget* getPinList() const;

    void setGlobalModeHighlighted(bool highlighted);

    /**
     * @brief The time the panel shows and edits the pins at.
     **/
    double getEditTime() const;

protected:
    virtual void createWidget(QHBoxLayout* layout) OVERRIDE FINAL;
    virtual void _hide() OVERRIDE FINAL;
    virtual void _show() OVERRIDE FINAL;
    virtual void setEnabled() OVERRIDE FINAL;
    virtual void setReadOnly(bool readOnly, int dimension) OVERRIDE FINAL;
    virtual void setDirty(bool dirty) OVERRIDE FINAL;
    virtual void updateGUI(int dimension) OVERRIDE FINAL;

private:
    void rebuildList();
    void refreshEditors();
    void onSelectionChangedInKnob(int id);
    void onListSelectionChanged();
    void onListItemChanged(int row, int column);
    void onAddClicked();
    void onDeleteClicked();
    void onEditorValueChanged(int editor, double value, quint64 gestureKey);
    void onKeyClicked(int which);

    std::unique_ptr<KnobGuiColorWarpPinsPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_KnobGuiColorWarpPins_h
