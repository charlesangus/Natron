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

#include "Serialization/Yaml/TrackerYaml.h"

#include <algorithm>
#include <cstring>

#include "Serialization/Yaml/YamlPrivate.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {
namespace YamlDetail {

    namespace {

        std::uint64_t
        bitsOf(double value)
        {
            std::uint64_t bits = 0;
            std::memcpy(&bits, &value, sizeof(bits));

            return bits;
        }

        // Orders -0 and 0 apart so that a time keeps its exact bits through the shared time column.
        bool
        timeLess(double a,
                 double b)
        {
            if (a < b) {
                return true;
            }
            if (b < a) {
                return false;
            }

            return bitsOf(a) < bitsOf(b);
        }

        struct TabledCurve {
            const KnobSerialization* knob;
            std::size_t dimension;
            const CurveSerialization* curve;
        };

        bool
        belongsInTable(const CurveSerialization& curve)
        {
            if (curve.keyframes.size() < kInlineTableRowThreshold) {
                return false;
            }
            for (const KeyFrameSerialization& key : curve.keyframes) {
                if (!isTabulableInterpolation(key.interpolation)) {
                    return false;
                }
            }

            return true;
        }

        std::string
        columnName(const std::string& knobName,
                   std::size_t dimension)
        {
            return knobName + "." + std::to_string(dimension);
        }

        Table
        buildTrackTable(const std::vector<TabledCurve>& curves)
        {
            std::vector<double> times;
            for (const TabledCurve& tabled : curves) {
                for (const KeyFrameSerialization& key : tabled.curve->keyframes) {
                    times.push_back(key.time);
                }
            }
            std::sort(times.begin(), times.end(), timeLess);
            times.erase(std::unique(times.begin(), times.end(), [](double a, double b) {
                            return bitsOf(a) == bitsOf(b);
                        }),
                        times.end());

            Table table;
            table.rows = times.size();
            std::vector<std::pair<std::string, std::vector<double>>> values;
            std::vector<std::pair<std::string, std::vector<std::uint8_t>>> bytes;
            for (const TabledCurve& tabled : curves) {
                const std::string name = columnName(tabled.knob->name, tabled.dimension);
                const std::vector<KeyFrameSerialization>& keys = tabled.curve->keyframes;
                std::vector<double> value(times.size(), 0.);
                std::vector<std::uint8_t> present(times.size(), 0);
                std::vector<std::uint8_t> interp(times.size(), (std::uint8_t)kYamlKeyframeLinear);
                bool allPresent = true;
                bool allLinear = true;
                std::size_t k = 0;
                for (std::size_t row = 0; row < times.size(); ++row) {
                    if ((k < keys.size()) && (bitsOf(keys[k].time) == bitsOf(times[row]))) {
                        value[row] = keys[k].value;
                        present[row] = 1;
                        interp[row] = (std::uint8_t)keys[k].interpolation;
                        allLinear = allLinear && (keys[k].interpolation == kYamlKeyframeLinear);
                        ++k;
                    } else {
                        allPresent = false;
                    }
                }
                values.emplace_back(name, std::move(value));
                if (!allPresent) {
                    bytes.emplace_back(name + ".present", std::move(present));
                }
                if (!allLinear) {
                    bytes.emplace_back(name + ".interp", std::move(interp));
                }
            }
            table.addF64("time", std::move(times));
            for (std::pair<std::string, std::vector<double>>& column : values) {
                table.addF64(column.first, std::move(column.second));
            }
            for (std::pair<std::string, std::vector<std::uint8_t>>& column : bytes) {
                table.addU8(column.first, std::move(column.second));
            }

            return table;
        }

        void
        emitTrack(YAML::Emitter& em,
                  const TrackSerialization& track,
                  const BlobEncodeContext& blobs)
        {
            std::vector<TabledCurve> tabled;
            for (const KnobSerialization& knob : track.knobs) {
                for (std::size_t i = 0; i < knob.dimensions.size(); ++i) {
                    const std::optional<CurveSerialization>& curve = knob.dimensions[i].curve;
                    if (curve && belongsInTable(*curve)) {
                        tabled.push_back(TabledCurve { &knob, i, &(*curve) });
                    }
                }
            }

            em << YAML::BeginMap;
            emitKey(em, "name");
            em << YAML::Value;
            emitString(em, track.scriptName);
            if (track.label != track.scriptName) {
                emitKey(em, "label");
                em << YAML::Value;
                emitString(em, track.label);
            }
            if (track.isPatternMatching) {
                emitKey(em, "pattern");
                em << YAML::Value;
                emitBool(em, true);
            }
            if (!track.enabled) {
                emitKey(em, "enabled");
                em << YAML::Value;
                emitBool(em, false);
            }
            if (!track.userKeys.empty()) {
                emitKey(em, "userKeys");
                em << YAML::Value << YAML::Flow << YAML::BeginSeq;
                for (int key : track.userKeys) {
                    emitInt(em, key);
                }
                em << YAML::EndSeq;
            }
            if (!track.knobs.empty()) {
                KnobEmitOptions options;
                options.curveInTable = [&tabled](const KnobSerialization& knob, std::size_t dimension) {
                    for (const TabledCurve& entry : tabled) {
                        if ((entry.knob == &knob) && (entry.dimension == dimension)) {
                            return true;
                        }
                    }

                    return false;
                };
                emitKey(em, "knobs");
                em << YAML::Value;
                emitKnobMap(em, track.knobs, options);
            }
            if (!tabled.empty()) {
                const std::string path = uniqueBlobPath("blobs/" + sanitizeBlobPathComponent(blobs.nodeFullName) + "/" + sanitizeBlobPathComponent(track.scriptName) + ".track.bin", blobs.usedPaths);
                emitKey(em, "curves");
                em << YAML::Value;
                emitTable(em, buildTrackTable(tabled), path, *blobs.sink);
            }
            em << YAML::EndMap;
        }

        TrackSerialization
        readTrack(const YAML::Node& node,
                  const BlobSource& source,
                  const std::string& context,
                  YamlWarnings* warnings)
        {
            TrackSerialization track;
            MapReader reader(node, context, warnings);
            track.scriptName = readString(reader.get("name"), context + ".name");
            if (YAML::Node label = reader.get("label")) {
                track.label = readString(label, context + ".label");
            } else {
                track.label = track.scriptName;
            }
            if (YAML::Node pattern = reader.get("pattern")) {
                track.isPatternMatching = readBool(pattern, context + ".pattern");
            }
            if (YAML::Node enabled = reader.get("enabled")) {
                track.enabled = readBool(enabled, context + ".enabled");
            }
            if (YAML::Node userKeys = reader.get("userKeys")) {
                if (!userKeys.IsSequence()) {
                    fail(userKeys, context + ".userKeys", "expected a list of frames");
                }
                for (std::size_t i = 0; i < userKeys.size(); ++i) {
                    track.userKeys.push_back(readInt(userKeys[i], context + ".userKeys"));
                }
            }
            std::vector<TabledCurveRef> tabledCurves;
            if (YAML::Node knobs = reader.get("knobs")) {
                track.knobs = readKnobMap(knobs, context + ".knobs", warnings, &tabledCurves);
            }
            YAML::Node curvesNode = reader.get("curves");
            reader.finish();
            if (tabledCurves.empty()) {
                return track;
            }
            if (!curvesNode) {
                warn(warnings, context + ": knob curves are said to be in the track's table, but it has none; they are left empty");

                return track;
            }
            bool missing = false;
            const Table table = readTable(curvesNode, source, context + ".curves", warnings, &missing);
            if (missing) {
                return track;
            }
            const std::vector<double> times = table.f64("time");
            for (const TabledCurveRef& ref : tabledCurves) {
                const std::string name = columnName(ref.knobName, ref.dimension);
                if (!table.find(name)) {
                    warn(warnings, context + ".curves: there is no column \"" + name + "\"; that curve is left empty");
                    continue;
                }
                const std::vector<double> values = table.f64(name);
                const std::vector<std::uint8_t> present = table.u8(name + ".present", 1);
                const std::vector<std::uint8_t> interp = table.u8(name + ".interp", (std::uint8_t)kYamlKeyframeLinear);
                CurveSerialization curve;
                for (std::size_t row = 0; row < table.rows; ++row) {
                    if (!present[row]) {
                        continue;
                    }
                    KeyFrameSerialization key;
                    key.time = times[row];
                    key.value = values[row];
                    key.interpolation = checkedInterpolation(interp[row], curvesNode, context + ".curves");
                    curve.keyframes.push_back(key);
                }
                for (KnobSerialization& knob : track.knobs) {
                    if ((knob.name == ref.knobName) && (ref.dimension < knob.dimensions.size())) {
                        knob.dimensions[ref.dimension].curve = std::move(curve);
                        break;
                    }
                }
            }

            return track;
        }

    } // namespace

    void
    emitTrackerContext(YAML::Emitter& em,
                       const TrackerContextSerialization& tracker,
                       const BlobEncodeContext& blobs)
    {
        em << YAML::BeginMap;
        emitKey(em, "tracks");
        em << YAML::Value;
        if (tracker.tracks.empty()) {
            em << YAML::Flow << YAML::BeginSeq << YAML::EndSeq;
        } else {
            em << YAML::BeginSeq;
            for (const TrackSerialization& track : tracker.tracks) {
                emitTrack(em, track, blobs);
            }
            em << YAML::EndSeq;
        }
        em << YAML::EndMap;
    }

    TrackerContextSerialization
    readTrackerContext(const YAML::Node& node,
                       const BlobSource& source,
                       const std::string& context,
                       YamlWarnings* warnings)
    {
        TrackerContextSerialization tracker;
        MapReader reader(node, context, warnings);
        if (YAML::Node tracks = reader.get("tracks")) {
            if (!tracks.IsSequence()) {
                fail(tracks, context + ".tracks", "expected a list of tracks");
            }
            for (std::size_t i = 0; i < tracks.size(); ++i) {
                tracker.tracks.push_back(readTrack(tracks[i], source, context + ".tracks[" + std::to_string(i) + "]", warnings));
            }
        }
        reader.finish();

        return tracker;
    }

} // namespace YamlDetail

std::string
encodeTrackerYaml(const TrackerContextSerialization& tracker,
                  const std::string& nodeFullName,
                  BlobSink& sink)
{
    YAML::Emitter em;
    std::set<std::string> usedPaths;
    YamlDetail::BlobEncodeContext blobs { &sink, nodeFullName, &usedPaths };
    YamlDetail::emitTrackerContext(em, tracker, blobs);

    return YamlDetail::documentText(em);
}

TrackerContextSerialization
decodeTrackerYaml(std::string_view text,
                  const BlobSource& source,
                  YamlWarnings* warnings)
{
    YAML::Node node = YamlDetail::loadDocument(text, "the tracker data");

    return YamlDetail::readTrackerContext(node, source, "tracker", warnings);
}

} // namespace Serialization
NATRON_NAMESPACE_EXIT
