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

#include "Switch.h"

#include <algorithm>

#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"

NATRON_NAMESPACE_ENTER

namespace {
// The display range ends at 1 at least, so that the slider moves before a second input is connected.
const int kMinimumDisplayMaximum = 1;
} // anonymous namespace

void
SwitchOp::createKnobs(KnobHolder* holder,
                      const KnobPagePtr& page)
{
    KnobIntPtr whichKnob = AppManager::createKnob<KnobInt>(holder, EffectInstance::tr("Which").toStdString());

    whichKnob->setName(kSwitchParamWhich);
    whichKnob->setHintToolTip(EffectInstance::tr("The input to output. Beyond the highest connected input, that input is output.").toStdString());
    whichKnob->setDefaultValue(0);
    whichKnob->setMinimum(0);
    whichKnob->setMaximum(kSwitchInputCount - 1);
    whichKnob->setDisplayMinimum(0);
    whichKnob->setDisplayMaximum(kMinimumDisplayMaximum);
    page->addKnob(whichKnob);
    which = whichKnob;
}

SwitchOp::Params
SwitchOp::bake(const KnobReader& reader) const
{
    Params params;

    params.which = reader.value(which);

    return params;
}

Route
SwitchOp::route(const Params& params,
                const RouteContext& context) const
{
    int highest = -1;

    for (int k = kSwitchInputCount - 1; k >= 0; --k) {
        if (context.isConnected(k)) {
            highest = k;
            break;
        }
    }
    if (highest < 0) {
        return Route::black();
    }
    const int chosen = std::clamp(params.which, 0, highest);
    if (!context.isConnected(chosen)) {
        return Route::black();
    }

    return Route::input(chosen, context.time());
}

Switch::Switch(NodePtr node)
    : ImageNode<SwitchOp>(node)
{
}

Switch::~Switch()
{
}

void
Switch::onInputChanged(int /*inputNo*/)
{
    updateRange();
}

void
Switch::updateRange()
{
    KnobIntPtr which = op().which.lock();

    if (!which) {
        return;
    }
    int highest = kMinimumDisplayMaximum;
    for (int k = kMinimumDisplayMaximum + 1; k < kSwitchInputCount; ++k) {
        if (getInput(k)) {
            highest = k;
        }
    }
    which->setDisplayMinimum(0);
    which->setDisplayMaximum(highest);
}

NATRON_NAMESPACE_EXIT
