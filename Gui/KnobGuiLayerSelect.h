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

#ifndef NATRON_GUI_KNOBGUILAYERSELECT_H
#define NATRON_GUI_KNOBGUILAYERSELECT_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <memory>
#include <string>

#include "Engine/EngineFwd.h"

#include "Gui/GuiFwd.h"
#include "Gui/KnobGuiLayerChannelBase.h"

NATRON_NAMESPACE_ENTER

struct KnobGuiLayerSelectPrivate;

/**
 * @brief Runs NewLayerDialog for knob's node and, unless the user cancels or the
 * registry refuses the layer, calls push with the new layer's id and label. Usable for
 * any knob on the node, not just a KnobLayerSelect.
 **/
void runNewLayerDialog(const KnobIPtr& knob,
                       QWidget* parent,
                       const std::function<void(const std::string& layerID, const std::string& layerLabel)>& push);

/**
 * @brief The GUI of a KnobLayerSelect: one LayerChannelRow in layer-select mode, with
 * channel buttons when the knob was created with them. A target knob's combo also
 * offers the "New layer..." entry.
 **/
class KnobGuiLayerSelect
    : public KnobGuiLayerChannelBase {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    static KnobGui* BuildKnobGui(KnobIPtr knob,
                                 KnobGuiContainerI* container)
    {
        return new KnobGuiLayerSelect(knob, container);
    }

    KnobGuiLayerSelect(KnobIPtr knob,
                       KnobGuiContainerI* container);

    virtual ~KnobGuiLayerSelect() OVERRIDE;

    virtual void removeSpecificGui() OVERRIDE FINAL;

    LayerChannelRow* getRow() const;

protected:
    virtual void createWidget(QHBoxLayout* layout) OVERRIDE FINAL;
    virtual void refreshWidgets() OVERRIDE FINAL;

private:
    void onLayerChosen(const QString& layerID);
    void onChannelToggled();
    void onNewLayerRequested();

    /**
     * @brief Runs NewLayerDialog, registers the layer in the project and selects it as
     * one undo step. Cancel and a registry refusal leave the knob untouched.
     **/
    void openNewLayerDialog();

    std::unique_ptr<KnobGuiLayerSelectPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_KNOBGUILAYERSELECT_H
