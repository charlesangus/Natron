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
#ifndef Serialization_Merge_MergeRules_h
#define Serialization_Merge_MergeRules_h

#include "Global/Macros.h"

#include <algorithm>
#include <cstddef>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "Serialization/KnobSerialization.h"
#include "Serialization/RotoSerialization.h"
#include "Serialization/TrackerSerialization.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {
namespace MergeDetail {

    enum class Pick {
        eMine,
        eTheirs,
        eConflict,
    };

    // Which side a three-way choice takes: the one that changed from the base, mine when both made
    // the same change, a conflict when they made different ones.
    template <typename T>
    Pick
    pick(const T& base,
         const T& mine,
         const T& theirs)
    {
        if (mine == theirs) {
            return Pick::eMine;
        }
        if (mine == base) {
            return Pick::eTheirs;
        }
        if (theirs == base) {
            return Pick::eMine;
        }

        return Pick::eConflict;
    }

    // Takes theirs where mine did not change from the base, keeping mine otherwise.
    template <typename T>
    void
    takeChanged(const T& base,
                T* mine,
                const T& theirs)
    {
        if (*mine == base) {
            *mine = theirs;
        }
    }

    // A knob dimension's state without its default value, or nullopt when it is at its default.
    std::optional<KnobDimensionSerialization> dimensionState(const KnobSerialization* knob,
                                                             std::size_t index);

    std::map<std::string, const KnobSerialization*> knobsByName(const std::list<KnobSerialization>& knobs);

    const KnobSerialization* knobNamed(const std::map<std::string, const KnobSerialization*>& knobs,
                                       const std::string& name);

    // The merged order of named items: theirs' order, with each item only mine keeps placed after
    // the item it follows in mine, so neither side's reordering of the shared items is lost
    // wholesale.
    inline std::vector<std::string>
    mergedOrder(const std::vector<std::string>& mine,
                const std::vector<std::string>& theirs,
                const std::set<std::string>& kept)
    {
        std::vector<std::string> result;
        for (const std::string& name : theirs) {
            if (kept.count(name)) {
                result.push_back(name);
            }
        }
        const std::set<std::string> fromTheirs(result.begin(), result.end());
        std::size_t next = 0;
        for (const std::string& name : mine) {
            if (fromTheirs.count(name)) {
                next = std::size_t(std::find(result.begin(), result.end(), name) - result.begin()) + 1;
            } else if (kept.count(name)) {
                result.insert(result.begin() + next, name);
                ++next;
            }
        }

        return result;
    }

    bool sameRotoContexts(const RotoContextSerialization& a,
                          const RotoContextSerialization& b);

    bool sameTrackerContexts(const TrackerContextSerialization& a,
                             const TrackerContextSerialization& b);

} // namespace MergeDetail
} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Merge_MergeRules_h
