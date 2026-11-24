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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "KnobGuiLayerCustomData.h"

#include "Engine/KnobLayerCustomData.h"

NATRON_NAMESPACE_ENTER

KnobGuiLayerCustomData::KnobGuiLayerCustomData(KnobIPtr knob,
                                               KnobGuiContainerI* container)
    : KnobGuiTable(knob, container)
{
    _knob = std::dynamic_pointer_cast<KnobLayerCustomData>(knob);
}

KnobGuiLayerCustomData::~KnobGuiLayerCustomData()
{
}

KnobIPtr
KnobGuiLayerCustomData::getKnob() const
{
    return _knob.lock();
}

bool
KnobGuiLayerCustomData::addNewUserEntry(QStringList& row)
{
    int index = rowCount() + 1;
    QString name;
    bool taken = true;
    while (taken) {
        name = QString::fromUtf8("key%1").arg(index++);
        taken = false;
        for (int i = 0; i < rowCount(); ++i) {
            if (rowValues(i).value(0) == name) {
                taken = true;
                break;
            }
        }
    }
    row.push_back(name);
    row.push_back(QString::fromUtf8(KnobLayerCustomData::typeNames().front().c_str()));
    row.push_back(QString());

    return true;
}

NATRON_NAMESPACE_EXIT
