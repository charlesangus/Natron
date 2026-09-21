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

#ifndef NATRON_GUI_KNOBGUICHANNELSELECT_H
#define NATRON_GUI_KNOBGUICHANNELSELECT_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>

#include "Engine/EngineFwd.h"

#include "Gui/GuiFwd.h"
#include "Gui/KnobGuiLayerChannelBase.h"

NATRON_NAMESPACE_ENTER

struct KnobGuiChannelSelectPrivate;

/**
 * @brief The GUI of a KnobChannelSelect: one LayerChannelRow in channel-select mode,
 * offering None and every "layer.channel" of the layers the knob lists.
 **/
class KnobGuiChannelSelect
    : public KnobGuiLayerChannelBase {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    static KnobGui* BuildKnobGui(KnobIPtr knob,
                                 KnobGuiContainerI* container)
    {
        return new KnobGuiChannelSelect(knob, container);
    }

    KnobGuiChannelSelect(KnobIPtr knob,
                         KnobGuiContainerI* container);

    virtual ~KnobGuiChannelSelect() OVERRIDE;

    virtual void removeSpecificGui() OVERRIDE FINAL;

    LayerChannelRow* getRow() const;

protected:
    virtual void createWidget(QHBoxLayout* layout) OVERRIDE FINAL;
    virtual void refreshWidgets() OVERRIDE FINAL;

private:
    void onChannelSelected(const QString& layerDotChannel);

    std::unique_ptr<KnobGuiChannelSelectPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_KNOBGUICHANNELSELECT_H
