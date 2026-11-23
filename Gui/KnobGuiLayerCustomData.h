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

#ifndef Gui_KnobGuiLayerCustomData_h
#define Gui_KnobGuiLayerCustomData_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include "Engine/EngineFwd.h"
#include "Gui/GuiFwd.h"
#include "Gui/KnobGuiTable.h"

NATRON_NAMESPACE_ENTER

// The custom data rows of a KnobLayerCustomData, every cell edited in place.
class KnobGuiLayerCustomData
    : public KnobGuiTable {
public:
    static KnobGui* BuildKnobGui(KnobIPtr knob,
                                 KnobGuiContainerI* container)
    {
        return new KnobGuiLayerCustomData(knob, container);
    }

    KnobGuiLayerCustomData(KnobIPtr knob,
                           KnobGuiContainerI* container);

    virtual ~KnobGuiLayerCustomData() OVERRIDE;

    virtual KnobIPtr getKnob() const OVERRIDE FINAL;

private:
    virtual bool addNewUserEntry(QStringList& row) OVERRIDE FINAL;

    KnobLayerCustomDataWPtr _knob;
};

NATRON_NAMESPACE_EXIT

#endif // Gui_KnobGuiLayerCustomData_h
