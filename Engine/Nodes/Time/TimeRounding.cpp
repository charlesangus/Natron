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

#include "TimeRounding.h"

#include <vector>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"

NATRON_NAMESPACE_ENTER

namespace TimeRounding {
KnobChoicePtr
createKnob(KnobHolder* holder,
           const KnobPagePtr& page,
           const std::string& name)
{
    KnobChoicePtr knob = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr("Rounding Mode").toStdString());
    knob->setName(name);
    knob->setHintToolTip(EffectInstance::tr("How the time read from the input is calculated.").toStdString());
    std::vector<ChoiceOption> options;
    options.push_back(ChoiceOption(kTimeRoundingWholeFrames, EffectInstance::tr("Whole Frames").toStdString(), EffectInstance::tr("Round the time down to a whole frame.").toStdString()));
    options.push_back(ChoiceOption(kTimeRoundingFractional, EffectInstance::tr("Fractional").toStdString(), EffectInstance::tr("Read the exact time, which may fall between frames, for an input that renders between frames.").toStdString()));
    knob->populateChoices(options);
    knob->setDefaultValue((int)eWholeFrames);
    knob->setAnimationEnabled(false);
    page->addKnob(knob);

    return knob;
}
} // namespace TimeRounding

NATRON_NAMESPACE_EXIT
