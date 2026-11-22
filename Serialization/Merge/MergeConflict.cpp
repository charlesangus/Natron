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

#include "Serialization/Merge/MergeConflict.h"

#include "Serialization/Merge/MergeRules.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

namespace {

    template <typename T, typename Same>
    bool
    sameOptional(const std::optional<T>& a,
                 const std::optional<T>& b,
                 Same same)
    {
        if (a.has_value() != b.has_value()) {
            return false;
        }

        return !a || same(*a, *b);
    }

} // namespace

bool
operator==(const MergeConflictValue& a,
           const MergeConflictValue& b)
{
    return (a.knob == b.knob) && (a.key == b.key) && (a.otherKeys == b.otherKeys) && (a.connections == b.connections) && (a.parent == b.parent) && (a.knobs == b.knobs)
        && sameOptional(a.roto, b.roto, MergeDetail::sameRotoContexts) && sameOptional(a.tracker, b.tracker, MergeDetail::sameTrackerContexts);
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
