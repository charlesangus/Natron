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

#ifndef Serialization_Yaml_RotoYaml_h
#define Serialization_Yaml_RotoYaml_h

#include "Global/Macros.h"

#include <string>
#include <string_view>

#include "Serialization/RotoSerialization.h"
#include "Serialization/Yaml/BlobSink.h"
#include "Serialization/Yaml/YamlScalars.h"

NATRON_NAMESPACE_ENTER
namespace Serialization {

/**
 * @brief The `roto` member of a node: the editing toggles, the selection and the item tree from
 * the base layer down, items in order, each with its knobs.
 *
 * Point data goes into column tables, inline under kInlineTableRowThreshold rows and blob members
 * `blobs/<node>/<item>.<kind>.bin` from there on:
 * - a bezier's `points` (one row per control point: x, y, lx, ly, rx, ry, the same six prefixed
 *   with f for its feather point, and broken flags) hold the static positions;
 * - an animated bezier whose curves all share the same keyframe times, with one interpolation
 *   per point and keyframe that is neither free nor broken, has a `shape` table with a row per
 *   keyframe and control point (time, cp, the twelve coordinates, interp); any other animated
 *   bezier keeps its curves inline under `curves`, losslessly;
 * - a stroke's samples are one `stroke` table (x, y, pressure, time, and interp unless every
 *   sample shares one), with `segments` giving the rows of each pen-down movement; a stroke whose
 *   three curves do not share key times keeps them inline under `curves`.
 *
 * nodeFullName is the fully qualified name of the node holding the context, used in blob paths.
 **/
std::string encodeRotoYaml(const RotoContextSerialization& roto, const std::string& nodeFullName, BlobSink& sink);
RotoContextSerialization decodeRotoYaml(std::string_view text, const BlobSource& source, YamlWarnings* warnings = nullptr);

} // namespace Serialization
NATRON_NAMESPACE_EXIT

#endif // Serialization_Yaml_RotoYaml_h
