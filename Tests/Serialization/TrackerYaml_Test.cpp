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

#include <cmath>
#include <list>
#include <string>

#include <gtest/gtest.h>

#include "Serialization/TrackerSerialization.h"
#include "Serialization/Yaml/BlobSink.h"
#include "Serialization/Yaml/TrackerYaml.h"
#include "YamlTestHelpers.h"

NATRON_NAMESPACE_USING
using Serialization::CurveSerialization;
using Serialization::KeyFrameSerialization;
using Serialization::KnobSerialization;
using Serialization::KnobValueSerialization;
using Serialization::TrackerContextSerialization;
using Serialization::TrackSerialization;
using Serialization::YamlWarnings;

namespace {

// A track's curves as the tracker writes them: one key per tracked frame, values that are not
// short decimals.
CurveSerialization
trackCurve(int firstFrame,
           int lastFrame,
           int step,
           double seed,
           int interpolation)
{
    CurveSerialization curve;
    for (int frame = firstFrame; frame <= lastFrame; frame += step) {
        KeyFrameSerialization key;
        key.time = frame;
        key.value = std::sin(frame * 0.017 + seed) * 512. + seed / 3.;
        key.interpolation = interpolation;
        curve.keyframes.push_back(key);
    }

    return curve;
}

KnobSerialization
curveKnob(const std::string& name,
          std::initializer_list<CurveSerialization> curves)
{
    KnobSerialization knob;
    knob.name = name;
    knob.typeName = "Double";
    knob.dimension = (int)curves.size();
    for (const CurveSerialization& curve : curves) {
        Serialization::KnobDimensionSerialization dimension;
        dimension.curve = curve;
        dimension.value = KnobValueSerialization(std::in_place_type<double>, curve.keyframes.empty() ? 0. : curve.keyframes.back().value);
        knob.dimensions.push_back(dimension);
    }

    return knob;
}

// Four tracks of 1000 frames: a dense center, sparse offsets and error, a short inline curve, a
// smooth curve that needs an interp column, and on the second track one broken tangent, whose
// curve stays inline, and keys at -0 and 0 that must stay apart in the shared time column.
TrackerContextSerialization
makeTracker()
{
    TrackerContextSerialization tracker;
    for (int t = 0; t < 4; ++t) {
        TrackSerialization track;
        track.scriptName = "track" + std::to_string(t + 1);
        track.label = (t == 2) ? "Left eye" : track.scriptName;
        track.isPatternMatching = (t == 3);
        track.userKeys = { 1, 250 + t, 1000 };
        const double seed = t * 1.7;
        track.knobs.push_back(curveKnob("center", { trackCurve(1, 1000, 1, seed, 1), trackCurve(1, 1000, 1, seed + 0.5, 1) }));
        track.knobs.push_back(curveKnob("offset", { trackCurve(1, 1000, 3, seed + 1., 1), trackCurve(2, 1000, 5, seed + 2., 1) }));
        track.knobs.push_back(curveKnob("error", { trackCurve(7, 993, 2 + t, seed + 3., 1) }));
        track.knobs.push_back(curveKnob("searchBoxBtmLeft", { trackCurve(1, 30, 3, seed, 1), trackCurve(1, 30, 3, seed, 0) }));
        track.knobs.push_back(curveKnob("patternTopLeft", { trackCurve(1, 1000, 10, seed, 2), trackCurve(1, 1000, 1, seed, 1) }));
        KnobSerialization enabled;
        enabled.name = "enabled";
        enabled.typeName = "Bool";
        enabled.dimension = 1;
        enabled.dimensions.resize(1);
        enabled.dimensions[0].value = KnobValueSerialization(std::in_place_type<bool>, t != 1);
        track.knobs.push_back(enabled);
        if (t == 1) {
            KeyFrameSerialization& broken = track.knobs.front().dimensions[1].curve->keyframes[500];
            broken.interpolation = 7;
            broken.leftDerivative = -2.5;
            broken.rightDerivative = 0.125;
            track.knobs.front().dimensions[0].curve->keyframes[0].time = -0.;
            std::next(track.knobs.begin())->dimensions[0].curve->keyframes[0].time = 0.;
        }
        tracker.tracks.push_back(track);
    }

    return tracker;
}

void
expectTrackersEqual(const TrackerContextSerialization& e,
                    TrackerContextSerialization a)
{
    ASSERT_EQ(e.tracks.size(), a.tracks.size());
    auto track = a.tracks.begin();
    for (const TrackSerialization& expected : e.tracks) {
        SCOPED_TRACE("track " + expected.scriptName);
        EXPECT_EQ(expected.enabled, track->enabled);
        EXPECT_EQ(expected.isPatternMatching, track->isPatternMatching);
        EXPECT_EQ(expected.label, track->label);
        EXPECT_EQ(expected.scriptName, track->scriptName);
        EXPECT_EQ(expected.userKeys, track->userKeys);
        expectExpectedKnobs(expected.knobs, track->knobs);
        ++track;
    }
}

} // anonymous namespace

TEST(TrackerYaml, LongTrackCurvesAreOneTablePerTrack)
{
    const TrackerContextSerialization tracker = makeTracker();

    Serialization::MemoryBlobStore blobs;
    const std::string text = Serialization::encodeTrackerYaml(tracker, "Tracker1", blobs);
    ASSERT_EQ(4u, blobs.members().size());
    int index = 1;
    for (const auto& member : blobs.members()) {
        EXPECT_EQ("blobs/Tracker1/track" + std::to_string(index++) + ".track.bin", member.first);
    }
    EXPECT_NE(std::string::npos, text.find("      center: [{curve: table}, {curve: table}]\n")) << text;
    EXPECT_NE(std::string::npos, text.find("      center: [{curve: table}, {curve: [L, 1, ")) << text;
    EXPECT_NE(std::string::npos, text.find("      searchBoxBtmLeft: [{curve: [L, 1, ")) << text;
    EXPECT_NE(std::string::npos, text.find("columns: [time:f64, center.0:f64, center.1:f64, offset.0:f64, offset.1:f64, error.0:f64, patternTopLeft.0:f64, patternTopLeft.1:f64, offset.0.present:u8, offset.1.present:u8, error.0.present:u8, patternTopLeft.0.present:u8, patternTopLeft.0.interp:u8]")) << text;
    EXPECT_LT(text.size(), 120000u);

    YamlWarnings warnings;
    const TrackerContextSerialization decoded = Serialization::decodeTrackerYaml(text, blobs, &warnings);
    EXPECT_TRUE(warnings.empty());
    expectTrackersEqual(tracker, decoded);

    Serialization::MemoryBlobStore again;
    EXPECT_EQ(text, Serialization::encodeTrackerYaml(decoded, "Tracker1", again));
    ASSERT_EQ(blobs.members().size(), again.members().size());
    for (const auto& member : blobs.members()) {
        EXPECT_TRUE(member.second == again.members().at(member.first)) << member.first;
    }
}

TEST(TrackerYaml, TrackFlagsNamesAndKeysStayInline)
{
    TrackerContextSerialization tracker = makeTracker();
    tracker.tracks.back().enabled = false;
    Serialization::MemoryBlobStore blobs;
    const std::string text = Serialization::encodeTrackerYaml(tracker, "Group1.Tracker1", blobs);
    EXPECT_NE(std::string::npos, text.find("  - name: \"track3\"\n    label: \"Left eye\"\n    userKeys: [1, 252, 1000]\n")) << text;
    EXPECT_NE(std::string::npos, text.find("  - name: \"track4\"\n    pattern: true\n    enabled: false\n")) << text;
    expectTrackersEqual(tracker, Serialization::decodeTrackerYaml(text, blobs));
}

TEST(TrackerYaml, MissingBlobLeavesItsCurvesEmpty)
{
    const TrackerContextSerialization tracker = makeTracker();
    Serialization::MemoryBlobStore blobs;
    const std::string text = Serialization::encodeTrackerYaml(tracker, "Tracker1", blobs);
    blobs.removeMember("blobs/Tracker1/track3.track.bin");

    YamlWarnings warnings;
    const TrackerContextSerialization decoded = Serialization::decodeTrackerYaml(text, blobs, &warnings);
    ASSERT_EQ(1u, warnings.size());
    EXPECT_EQ("tracker.tracks[2].curves: the blob member \"blobs/Tracker1/track3.track.bin\" is missing; its data is left empty", warnings[0]);
    auto track = decoded.tracks.begin();
    std::advance(track, 2);
    const KnobSerialization& center = track->knobs.front();
    ASSERT_EQ(2u, center.dimensions.size());
    ASSERT_TRUE(center.dimensions[0].curve.has_value());
    EXPECT_TRUE(center.dimensions[0].curve->keyframes.empty());
    EXPECT_EQ(30u / 3u, std::next(track->knobs.begin(), 3)->dimensions[0].curve->keyframes.size());
}

TEST(TrackerYaml, NoTracks)
{
    Serialization::MemoryBlobStore blobs;
    const std::string text = Serialization::encodeTrackerYaml(TrackerContextSerialization(), "Tracker1", blobs);
    EXPECT_EQ("tracks: []\n", text);
    EXPECT_TRUE(Serialization::decodeTrackerYaml(text, blobs).tracks.empty());
}
