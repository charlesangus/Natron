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

#ifndef NATRON_GUI_KNOBGUICHANNELSET_H
#define NATRON_GUI_KNOBGUICHANNELSET_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <string>
#include <vector>

#include "Engine/EngineFwd.h"

#include "Gui/GuiFwd.h"
#include "Gui/KnobGuiLayerChannelBase.h"

NATRON_NAMESPACE_ENTER

struct ChannelSetRow;
struct KnobGuiChannelSetPrivate;

/**
 * @brief The GUI of a KnobChannelSet: one LayerChannelRow per knob row plus an "Add layer"
 * button. Every user action on a row becomes one setValue of the whole encoded table,
 * pushed as its own undo step.
 **/
class KnobGuiChannelSet
    : public KnobGuiLayerChannelBase {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    static KnobGui* BuildKnobGui(KnobIPtr knob,
                                 KnobGuiContainerI* container)
    {
        return new KnobGuiChannelSet(knob, container);
    }

    KnobGuiChannelSet(KnobIPtr knob,
                      KnobGuiContainerI* container);

    virtual ~KnobGuiChannelSet() OVERRIDE;

    virtual void removeSpecificGui() OVERRIDE FINAL;

    int getRowCount() const;
    LayerChannelRow* getRow(int index) const;
    Button* getAddLayerButton() const;

public Q_SLOTS:

    void onAddLayerClicked();

protected:
    virtual void createWidget(QHBoxLayout* layout) OVERRIDE FINAL;
    virtual void refreshWidgets() OVERRIDE FINAL;

private:
    void onRowModeChosen(LayerChannelRow* row, int setRowMode);
    void onRowLayerChosen(LayerChannelRow* row, const QString& layerID);
    void onRowChannelToggled(LayerChannelRow* row, const QString& channel, bool on);
    void onRowPatternCommitted(LayerChannelRow* row, const QString& pattern);
    void onRowRemoveRequested(LayerChannelRow* row);

    void pushRows(const std::vector<ChannelSetRow>& newRows);

    std::unique_ptr<KnobGuiChannelSetPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_KNOBGUICHANNELSET_H
