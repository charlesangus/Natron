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

#include "NoTimeBlur.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "Engine/AppManager.h"
#include "Engine/ChoiceOption.h"
#include "Engine/KnobTypes.h"

NATRON_NAMESPACE_ENTER

namespace NoTimeBlurRounding {
double
apply(Mode mode,
      double time)
{
    switch (mode) {
    case eRint:
        return std::rint(time);
    case eFloor:
        return std::floor(time);
    case eCeil:
        return std::ceil(time);
    case eNone:
        break;
    }

    return time;
}
} // namespace NoTimeBlurRounding

void
NoTimeBlurOp::createKnobs(KnobHolder* holder,
                          const KnobPagePtr& page)
{
    KnobChoicePtr roundingKnob = AppManager::createKnob<KnobChoice>(holder, EffectInstance::tr("Rounding").toStdString());
    roundingKnob->setName(kNoTimeBlurParamRounding);
    roundingKnob->setHintToolTip(EffectInstance::tr("How a fractional time asked of the input becomes a whole frame.").toStdString());
    {
        std::vector<ChoiceOption> options;
        options.push_back(ChoiceOption(kNoTimeBlurRoundingRint, EffectInstance::tr("Round").toStdString(),
                                       EffectInstance::tr("The nearest frame; a time halfway between two frames reads the even one.").toStdString()));
        options.push_back(ChoiceOption(kNoTimeBlurRoundingFloor, EffectInstance::tr("Floor").toStdString(),
                                       EffectInstance::tr("The frame at or before the time.").toStdString()));
        options.push_back(ChoiceOption(kNoTimeBlurRoundingCeil, EffectInstance::tr("Ceil").toStdString(),
                                       EffectInstance::tr("The frame at or after the time.").toStdString()));
        options.push_back(ChoiceOption(kNoTimeBlurRoundingNone, EffectInstance::tr("None").toStdString(),
                                       EffectInstance::tr("The time as asked, unrounded.").toStdString()));
        roundingKnob->populateChoices(options);
    }
    roundingKnob->setDefaultValue((int)NoTimeBlurRounding::eRint);
    page->addKnob(roundingKnob);
    rounding = roundingKnob;
}

NoTimeBlurOp::Params
NoTimeBlurOp::bake(const KnobReader& reader) const
{
    Params params;
    params.rounding = (NoTimeBlurRounding::Mode)std::clamp((int)reader.value(rounding), (int)NoTimeBlurRounding::eRint, (int)NoTimeBlurRounding::eNone);

    return params;
}

Route
NoTimeBlurOp::route(const Params& params,
                    const RouteContext& context) const
{
    if (!context.isConnected(0)) {
        return Route::black();
    }

    return Route::input(0, NoTimeBlurRounding::apply(params.rounding, context.time()));
}

NoTimeBlur::NoTimeBlur(NodePtr node)
    : ImageNode<NoTimeBlurOp>(node)
{
}

NoTimeBlur::~NoTimeBlur()
{
}

NATRON_NAMESPACE_EXIT
