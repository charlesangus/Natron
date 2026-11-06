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

#include "FilterKnobs.h"

#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Nodes/KnobReader.h"

NATRON_NAMESPACE_ENTER

namespace Resampler {

void
FilterKnobs::create(KnobHolder* holder,
                    const KnobPagePtr& page,
                    const KnobChoicePtr& filter,
                    const KnobBoolPtr& clamp)
{
    const FilterSpec defaults;

    _filter = filter;
    _clamp = clamp;

    KnobIntPtr lobes = AppManager::createKnob<KnobInt>(holder, EffectInstance::tr(kResamplerParamLobesLabel));
    lobes->setName(kResamplerParamLobes);
    lobes->setHintToolTip(EffectInstance::tr(kResamplerParamLobesHint));
    lobes->setMinimum(2);
    lobes->setMaximum(8);
    lobes->setDefaultValue(defaults.lobes);
    lobes->disableSlider();
    lobes->setAddNewLine(false);
    lobes->setSpacingBetweenItems(1);
    page->addKnob(lobes);
    _lobes = lobes;

    KnobDoublePtr bicubicB = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr(kResamplerParamBicubicBLabel));
    bicubicB->setName(kResamplerParamBicubicB);
    bicubicB->setHintToolTip(EffectInstance::tr(kResamplerParamBicubicBHint));
    bicubicB->setMinimum(0.);
    bicubicB->setMaximum(1.);
    bicubicB->setIncrement(0.01);
    bicubicB->setDefaultValue(defaults.B);
    bicubicB->disableSlider();
    bicubicB->setAddNewLine(false);
    bicubicB->setSpacingBetweenItems(1);
    page->addKnob(bicubicB);
    _bicubicB = bicubicB;

    KnobDoublePtr bicubicC = AppManager::createKnob<KnobDouble>(holder, EffectInstance::tr(kResamplerParamBicubicCLabel));
    bicubicC->setName(kResamplerParamBicubicC);
    bicubicC->setHintToolTip(EffectInstance::tr(kResamplerParamBicubicCHint));
    bicubicC->setMinimum(0.);
    bicubicC->setMaximum(1.);
    bicubicC->setIncrement(0.01);
    bicubicC->setDefaultValue(defaults.C);
    bicubicC->disableSlider();
    page->addKnob(bicubicC);
    _bicubicC = bicubicC;
}

void
FilterKnobs::updateEnabled(double time,
                           ViewSpec view) const
{
    const KnobChoicePtr filter = _filter.lock();

    if (!filter) {
        return;
    }
    const FilterEnum chosen = (FilterEnum)filter->getValueAtTime(time, 0, view);

    if (const KnobBoolPtr clamp = _clamp.lock()) {
        clamp->setAllDimensionsEnabled(filterHonoursClamp(chosen));
    }
    if (const KnobIntPtr lobes = _lobes.lock()) {
        lobes->setAllDimensionsEnabled(chosen == eFilterSinc);
    }
    if (const KnobDoublePtr bicubicB = _bicubicB.lock()) {
        bicubicB->setAllDimensionsEnabled(chosen == eFilterBicubic);
    }
    if (const KnobDoublePtr bicubicC = _bicubicC.lock()) {
        bicubicC->setAllDimensionsEnabled(chosen == eFilterBicubic);
    }
}

std::array<KnobIPtr, 3>
FilterKnobs::parameterKnobs() const
{
    return { _lobes.lock(), _bicubicB.lock(), _bicubicC.lock() };
}

FilterSpec
FilterKnobs::read(const KnobReader& reader) const
{
    FilterSpec spec;
    const int chosen = reader.valueOr(_filter, (int)spec.filter);

    if ((chosen >= 0) && (chosen < (int)filterOptions().size())) {
        spec.filter = (FilterEnum)chosen;
    }
    spec.lobes = reader.valueOr(_lobes, spec.lobes);
    spec.B = reader.valueOr(_bicubicB, spec.B);
    spec.C = reader.valueOr(_bicubicC, spec.C);

    return spec;
}

FilterSpec
FilterKnobs::read(double time,
                  ViewIdx view) const
{
    const KnobReader reader(time, view);

    return read(reader);
}

} // namespace Resampler

NATRON_NAMESPACE_EXIT
