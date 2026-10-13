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

#ifndef Serialization_TrackerSerialization_h
#define Serialization_TrackerSerialization_h

#include "Global/Macros.h"

#include <list>
#include <string>

#include "Serialization/KnobSerialization.h"
#include "Serialization/SerializationFwd.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief The saved state of one track of a tracker: its names, its knobs, in the order the
 * track holds them, and the frames the user keyed by hand.
 **/
struct TrackSerialization {
    // The format has always carried this flag, but a track's enabled state lives in its
    // "enabled" knob: the flag is saved as true and ignored on load.
    bool enabled;

    // Whether the track is a pattern-matching track rather than a regular one.
    bool isPatternMatching;
    std::string label;
    std::string scriptName;
    std::list<KnobSerialization> knobs;
    std::list<int> userKeys;

    TrackSerialization()
        : enabled(true)
        , isPatternMatching(false)
        , label()
        , scriptName()
        , knobs()
        , userKeys()
    {
    }
};

/**
 * @brief The saved state of the tracker context of a tracker node: its tracks, in order.
 **/
struct TrackerContextSerialization {
    std::list<TrackSerialization> tracks;
};

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_TrackerSerialization_h
