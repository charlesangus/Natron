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

#ifndef Engine_Nodes_Image_FilterKnobs_h
#define Engine_Nodes_Image_FilterKnobs_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <array>

#include "Engine/EngineFwd.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_ENTER

class KnobReader;

namespace Resampler {

/**
 * @brief The filter parameter knobs every resampling node shares, under the same script names on
 * each, together with the Filter and Clamp knobs they depend on.
 **/
class FilterKnobs {
public:
    /// Adds one row of parameter knobs to page. Call it after the knob that ends the Filter row.
    void create(KnobHolder* holder, const KnobPagePtr& page, const KnobChoicePtr& filter, const KnobBoolPtr& clamp);

    /// Enables Clamp, Lobes, Bicubic B and Bicubic C for what the Filter choice reads at time.
    void updateEnabled(double time, ViewSpec view) const;

    /// Lobes, Bicubic B and Bicubic C, for a node that hides its Filter row by method.
    std::array<KnobIPtr, 3> parameterKnobs() const WARN_UNUSED_RETURN;

    /// A Filter value outside the choice list reads as the default.
    FilterSpec read(const KnobReader& reader) const WARN_UNUSED_RETURN;
    FilterSpec read(double time, ViewIdx view) const WARN_UNUSED_RETURN;

private:
    KnobChoiceWPtr _filter;
    KnobBoolWPtr _clamp;
    KnobIntWPtr _lobes;
    KnobDoubleWPtr _bicubicB;
    KnobDoubleWPtr _bicubicC;
};

} // namespace Resampler

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Image_FilterKnobs_h
