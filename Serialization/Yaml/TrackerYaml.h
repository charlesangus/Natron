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

#ifndef Serialization_Yaml_TrackerYaml_h
#define Serialization_Yaml_TrackerYaml_h

#include "Global/Macros.h"

#include <string>
#include <string_view>

#include "Serialization/TrackerSerialization.h"
#include "Serialization/Yaml/BlobSink.h"
#include "Serialization/Yaml/YamlScalars.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief The `tracker` member of a node: its tracks in order, each with its names, flags, user
 * keyframes and knobs.
 *
 * The knob curves of a track with at least kInlineTableRowThreshold keyframes and no free or
 * broken keyframe go into one `curves` table per track, a blob member
 * `blobs/<node>/<track>.track.bin`: a row per keyframe time of any of them, a `time` column, and
 * for each curve a `<knob>.<dimension>` value column, a u8 `<knob>.<dimension>.present` column
 * unless the curve has a key on every row, and a u8 `<knob>.<dimension>.interp` column unless all
 * its keys are linear. The knob then says `curve: table` for that dimension. Other curves stay
 * inline.
 *
 * nodeFullName is the fully qualified name of the node holding the context, used in blob paths.
 **/
std::string encodeTrackerYaml(const TrackerContextSerialization& tracker, const std::string& nodeFullName, BlobSink& sink);
TrackerContextSerialization decodeTrackerYaml(std::string_view text, const BlobSource& source, YamlWarnings* warnings = nullptr);

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Yaml_TrackerYaml_h
