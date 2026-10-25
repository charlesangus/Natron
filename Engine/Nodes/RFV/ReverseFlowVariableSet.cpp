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

#include "ReverseFlowVariableSet.h"

#include "Engine/KnobTypes.h"

NATRON_NAMESPACE_ENTER

void
ReverseFlowVariableSet::initializeKnobs()
{
    // Both are user knobs: a page or group that is not refuses user-knob children, and only user
    // pages are saved with their children's layout.
    KnobPagePtr page = createKnob<KnobPage>(tr(kReverseFlowVariableSetParamPageLabel));
    page->setName(kReverseFlowVariableSetParamPage);
    page->setAsUserKnob(true);

    KnobGroupPtr variables = createKnob<KnobGroup>(tr(kReverseFlowVariableSetParamVariablesLabel));
    variables->setName(kReverseFlowVariableSetParamVariables);
    variables->setAsUserKnob(true);
    variables->setDefaultValue(true);
    variables->setAsRFVGroup(true);
    page->addKnob(variables);
}

Route
ReverseFlowVariableSet::route(double time,
                              ViewIdx /*view*/)
{
    return Route::input(0, time);
}

NATRON_NAMESPACE_EXIT
