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

#include "Global/Macros.h"

#include <list>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Serialization/Merge/MergeHandler.h"

NATRON_NAMESPACE_USING

namespace {

// A track whose centre x is animated with the given keys.
Serialization::TrackSerialization
track(const std::string& name,
      const std::map<double, double>& centre,
      const std::list<int>& userKeys = std::list<int>())
{
    Serialization::TrackSerialization result;
    result.scriptName = name;
    result.label = name;
    result.userKeys = userKeys;
    Serialization::KnobSerialization knob;
    knob.name = "centerPoint";
    knob.typeName = "Double";
    knob.dimension = 2;
    knob.dimensions.resize(2);
    for (Serialization::KnobDimensionSerialization& dimension : knob.dimensions) {
        dimension.defaultValue = Serialization::KnobValueSerialization(0.);
        dimension.value = Serialization::KnobValueSerialization(0.);
    }
    Serialization::CurveSerialization curve;
    for (const std::pair<const double, double>& entry : centre) {
        Serialization::KeyFrameSerialization key;
        key.time = entry.first;
        key.value = entry.second;
        key.interpolation = 1;
        curve.keyframes.push_back(key);
    }
    knob.dimensions[0].curve = curve;
    result.knobs.push_back(knob);

    return result;
}

Serialization::TrackerContextSerialization
tracker(const std::vector<Serialization::TrackSerialization>& tracks)
{
    Serialization::TrackerContextSerialization result;
    result.tracks.assign(tracks.begin(), tracks.end());

    return result;
}

std::vector<std::string>
namesOf(const Serialization::TrackerContextSerialization& context)
{
    std::vector<std::string> result;
    for (const Serialization::TrackSerialization& entry : context.tracks) {
        result.push_back(entry.scriptName);
    }

    return result;
}

const Serialization::TrackSerialization&
trackNamed(const Serialization::TrackerContextSerialization& context,
           const std::string& name)
{
    for (const Serialization::TrackSerialization& entry : context.tracks) {
        if (entry.scriptName == name) {
            return entry;
        }
    }
    static const Serialization::TrackSerialization none;
    ADD_FAILURE() << "no track " << name;

    return none;
}

std::map<double, double>
centreOf(const Serialization::TrackSerialization& entry)
{
    std::map<double, double> result;
    for (const Serialization::KnobSerialization& knob : entry.knobs) {
        if ((knob.name == "centerPoint") && knob.dimensions[0].curve) {
            for (const Serialization::KeyFrameSerialization& key : knob.dimensions[0].curve->keyframes) {
                result[key.time] = key.value;
            }
        }
    }

    return result;
}

Serialization::TrackerContextSerialization
merge(const Serialization::TrackerContextSerialization& base,
      const Serialization::TrackerContextSerialization& mine,
      const Serialization::TrackerContextSerialization& theirs,
      std::vector<Serialization::MergeConflict>* conflicts)
{
    return Serialization::mergeTrackerContexts(&base, mine, theirs, Serialization::MergeHandlerRegistry::defaults(), conflicts);
}

} // namespace

TEST(TrackerMerge, TrackCurvesMergeAsWholeKnobsAndConflictsNameTheTrack)
{
    const Serialization::TrackerContextSerialization base = tracker({ track("track1", { { 1., 10. }, { 2., 11. } }) });
    const Serialization::TrackerContextSerialization mine = tracker({ track("track1", { { 1., 10. }, { 2., 12. }, { 3., 13. } }) });
    const Serialization::TrackerContextSerialization theirs = tracker({ track("track1", { { 1., 9. }, { 2., 11. } }) });
    std::vector<Serialization::MergeConflict> conflicts;
    const Serialization::TrackerContextSerialization merged = merge(base, mine, theirs, &conflicts);

    EXPECT_EQ(centreOf(trackNamed(merged, "track1")), (std::map<double, double> { { 1., 10. }, { 2., 12. }, { 3., 13. } }));
    ASSERT_EQ(conflicts.size(), 1u);
    EXPECT_EQ(conflicts[0].kind, Serialization::MergeConflictKind::eValue);
    EXPECT_EQ(conflicts[0].item, "track1");
    EXPECT_EQ(conflicts[0].knob, "centerPoint");
    EXPECT_EQ(conflicts[0].dimension, -1);

    const Serialization::TrackerContextSerialization theirsOnly = merge(base, base, theirs, &conflicts);
    EXPECT_EQ(centreOf(trackNamed(theirsOnly, "track1")), (std::map<double, double> { { 1., 9. }, { 2., 11. } }));
    EXPECT_EQ(conflicts.size(), 1u);
}

TEST(TrackerMerge, UserKeyframesMergeAsSets)
{
    const Serialization::TrackerContextSerialization base = tracker({ track("track1", { { 1., 0. } }, { 1, 5 }) });
    const Serialization::TrackerContextSerialization mine = tracker({ track("track1", { { 1., 0. } }, { 1, 5, 9 }) });
    const Serialization::TrackerContextSerialization theirs = tracker({ track("track1", { { 1., 0. } }, { 5, 7 }) });
    std::vector<Serialization::MergeConflict> conflicts;
    const Serialization::TrackerContextSerialization merged = merge(base, mine, theirs, &conflicts);

    EXPECT_EQ(trackNamed(merged, "track1").userKeys, (std::list<int> { 5, 7, 9 }));
    EXPECT_TRUE(conflicts.empty());
}

TEST(TrackerMerge, TracksFollowTheirsOrderWithMineAfterTheTrackTheyFollow)
{
    const Serialization::TrackerContextSerialization base = tracker({ track("a", { { 1., 0. } }), track("b", { { 1., 0. } }) });
    const Serialization::TrackerContextSerialization mine = tracker({ track("a", { { 1., 0. } }), track("mineOnly", { { 1., 0. } }), track("b", { { 1., 0. } }) });
    const Serialization::TrackerContextSerialization theirs = tracker({ track("b", { { 1., 0. } }), track("a", { { 1., 0. } }), track("theirsOnly", { { 1., 0. } }) });
    std::vector<Serialization::MergeConflict> conflicts;
    const Serialization::TrackerContextSerialization merged = merge(base, mine, theirs, &conflicts);

    EXPECT_EQ(namesOf(merged), (std::vector<std::string> { "b", "a", "mineOnly", "theirsOnly" }));
    EXPECT_TRUE(conflicts.empty());
}

TEST(TrackerMerge, DeletingATrackTheOtherSideChangedKeepsIt)
{
    const Serialization::TrackerContextSerialization base = tracker({ track("a", { { 1., 0. } }), track("b", { { 1., 0. } }) });
    const Serialization::TrackerContextSerialization mine = tracker({ track("a", { { 1., 0. } }), track("b", { { 1., 4. } }) });
    const Serialization::TrackerContextSerialization theirs = tracker({ track("a", { { 1., 0. } }) });
    std::vector<Serialization::MergeConflict> conflicts;
    const Serialization::TrackerContextSerialization merged = merge(base, mine, theirs, &conflicts);
    EXPECT_EQ(namesOf(merged), (std::vector<std::string> { "a", "b" }));
    ASSERT_EQ(conflicts.size(), 1u);
    EXPECT_EQ(conflicts[0].kind, Serialization::MergeConflictKind::eDeleteModify);
    EXPECT_EQ(conflicts[0].item, "b");
    EXPECT_EQ(conflicts[0].side, Serialization::MergeSide::eTheirs);

    const Serialization::TrackerContextSerialization untouched = tracker({ track("a", { { 1., 0. } }), track("b", { { 1., 0. } }) });
    conflicts.clear();
    EXPECT_EQ(namesOf(merge(base, untouched, theirs, &conflicts)), (std::vector<std::string> { "a" }));
    EXPECT_TRUE(conflicts.empty());
}
