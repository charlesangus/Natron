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

#include "Serialization/Merge/MergeHandler.h"

#include <map>
#include <set>
#include <tuple>

#include "Serialization/Merge/MergeRules.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

using MergeDetail::mergedOrder;
using MergeDetail::Pick;
using MergeDetail::pick;
using MergeDetail::takeChanged;

namespace {

    std::map<std::string, const TrackSerialization*>
    tracksByName(const TrackerContextSerialization* context)
    {
        std::map<std::string, const TrackSerialization*> result;
        if (context) {
            for (const TrackSerialization& track : context->tracks) {
                result.emplace(track.scriptName, &track);
            }
        }

        return result;
    }

    const TrackSerialization*
    trackNamed(const std::map<std::string, const TrackSerialization*>& tracks,
               const std::string& name)
    {
        std::map<std::string, const TrackSerialization*>::const_iterator found = tracks.find(name);

        return (found == tracks.end()) ? nullptr : found->second;
    }

    std::vector<std::string>
    trackNames(const TrackerContextSerialization& context)
    {
        std::vector<std::string> result;
        for (const TrackSerialization& track : context.tracks) {
            result.push_back(track.scriptName);
        }

        return result;
    }

    std::set<int>
    userKeysOf(const TrackSerialization* track)
    {
        return track ? std::set<int>(track->userKeys.begin(), track->userKeys.end()) : std::set<int>();
    }

    bool
    sameTrack(const TrackSerialization& a,
              const TrackSerialization& b)
    {
        return (a.scriptName == b.scriptName) && (a.label == b.label) && (a.enabled == b.enabled) && (a.isPatternMatching == b.isPatternMatching) && (userKeysOf(&a) == userKeysOf(&b)) && sameKnobLists(a.knobs, b.knobs);
    }

    std::optional<TrackerContextSerialization>
    trackAlone(const TrackSerialization* track)
    {
        if (!track) {
            return std::nullopt;
        }
        TrackerContextSerialization context;
        context.tracks.push_back(*track);

        return context;
    }

    TrackSerialization
    mergeTrack(const TrackSerialization* base,
               const TrackSerialization& mine,
               const TrackSerialization& theirs,
               const MergeHandlerRegistry& handlers,
               std::vector<MergeConflict>* conflicts)
    {
        TrackSerialization result = mine;
        if (base) {
            takeChanged(base->label, &result.label, theirs.label);
            takeChanged(base->enabled, &result.enabled, theirs.enabled);
            takeChanged(base->isPatternMatching, &result.isPatternMatching, theirs.isPatternMatching);
        }

        std::vector<MergeConflict> knobConflicts;
        result.knobs = mergeKnobLists(handlers, std::string(), base ? &base->knobs : nullptr, mine.knobs, theirs.knobs, &knobConflicts);
        for (MergeConflict& conflict : knobConflicts) {
            conflict.item = mine.scriptName;
            conflicts->push_back(std::move(conflict));
        }

        const std::set<int> baseKeys = userKeysOf(base);
        const std::set<int> mineKeys = userKeysOf(&mine);
        const std::set<int> theirsKeys = userKeysOf(&theirs);
        std::set<int> frames = mineKeys;
        frames.insert(theirsKeys.begin(), theirsKeys.end());
        frames.insert(baseKeys.begin(), baseKeys.end());
        result.userKeys.clear();
        for (int frame : frames) {
            const bool inMine = mineKeys.count(frame);
            const bool inTheirs = theirsKeys.count(frame);
            if ((pick(bool(baseKeys.count(frame)), inMine, inTheirs) == Pick::eTheirs) ? inTheirs : inMine) {
                result.userKeys.push_back(frame);
            }
        }

        return result;
    }

} // namespace

namespace MergeDetail {

    bool
    sameTrackerContexts(const TrackerContextSerialization& a,
                        const TrackerContextSerialization& b)
    {
        if (a.tracks.size() != b.tracks.size()) {
            return false;
        }
        std::list<TrackSerialization>::const_iterator right = b.tracks.begin();
        for (const TrackSerialization& track : a.tracks) {
            if (!sameTrack(track, *right)) {
                return false;
            }
            ++right;
        }

        return true;
    }

} // namespace MergeDetail

TrackerContextSerialization
mergeTrackerContexts(const TrackerContextSerialization* base,
                     const TrackerContextSerialization& mine,
                     const TrackerContextSerialization& theirs,
                     const MergeHandlerRegistry& handlers,
                     std::vector<MergeConflict>* conflicts)
{
    const std::map<std::string, const TrackSerialization*> baseTracks = tracksByName(base);
    const std::map<std::string, const TrackSerialization*> mineTracks = tracksByName(&mine);
    const std::map<std::string, const TrackSerialization*> theirsTracks = tracksByName(&theirs);
    std::map<std::string, TrackSerialization> kept;
    for (const std::map<std::string, const TrackSerialization*>* side : { &mineTracks, &theirsTracks }) {
        for (const std::pair<const std::string, const TrackSerialization*>& entry : *side) {
            const std::string& name = entry.first;
            if (kept.count(name)) {
                continue;
            }
            const TrackSerialization* b = trackNamed(baseTracks, name);
            const TrackSerialization* m = trackNamed(mineTracks, name);
            const TrackSerialization* t = trackNamed(theirsTracks, name);
            if (m && t) {
                kept.emplace(name, mergeTrack(b, *m, *t, handlers, conflicts));
                continue;
            }
            const TrackSerialization* present = m ? m : t;
            if (!b) {
                kept.emplace(name, *present);
            } else if (!sameTrack(*b, *present)) {
                MergeConflict conflict;
                conflict.kind = MergeConflictKind::eDeleteModify;
                conflict.item = name;
                conflict.side = m ? MergeSide::eTheirs : MergeSide::eMine;
                conflict.base.tracker = trackAlone(b);
                conflict.mine.tracker = trackAlone(m);
                conflict.theirs.tracker = trackAlone(t);
                conflicts->push_back(conflict);
                kept.emplace(name, *present);
            }
        }
    }

    std::set<std::string> keptNames;
    for (const std::pair<const std::string, TrackSerialization>& entry : kept) {
        keptNames.insert(entry.first);
    }
    TrackerContextSerialization result;
    for (const std::string& name : mergedOrder(trackNames(mine), trackNames(theirs), keptNames)) {
        result.tracks.push_back(std::move(kept.at(name)));
    }

    return result;
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
