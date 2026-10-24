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

/*
 * The OpcodeList2 GainMap parsing is ported from darktable, DARKTABLE_REF release-5.6.2
 * (commit 20891f6c6fa7): dt_dng_opcode_process_opcode_list_2() of src/common/dng_opcode.c
 * (Copyright (C) 2011-2023 darktable developers), with every read bounds-checked. darktable is
 * distributed under the GNU GPL version 3 or later, so this file is too.
 */

#ifndef Engine_Nodes_IO_RawDngOpcodes_h
#define Engine_Nodes_IO_RawDngOpcodes_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

NATRON_NAMESPACE_ENTER

/**
 * @brief One DNG GainMap opcode (id 9) of an OpcodeList2: a grid of gain values over the
 * rectangle top/left/bottom/right, applied to the planes plane..plane+planes-1 of the pixels
 * lying on the row and column pitch. The geometry is in sensor pixels, top-down, as the file
 * stores it. gains holds mapPointsV * mapPointsH * mapPlanes values, row by row, then column,
 * then map plane.
 **/
struct DngGainMap {
    uint32_t top = 0;
    uint32_t left = 0;
    uint32_t bottom = 0;
    uint32_t right = 0;
    uint32_t plane = 0;
    uint32_t planes = 0;
    uint32_t rowPitch = 0;
    uint32_t colPitch = 0;
    uint32_t mapPointsV = 0;
    uint32_t mapPointsH = 0;
    double mapSpacingV = 0.;
    double mapSpacingH = 0.;
    double mapOriginV = 0.;
    double mapOriginH = 0.;
    uint32_t mapPlanes = 0;
    std::vector<float> gains;
};

namespace RawDngOpcodes {
/**
 * @brief Number of leading values of a serialized map that precede its gains.
 **/
constexpr std::size_t kGainMapHeaderValues = 15;

/**
 * @brief Parses the GainMap opcodes of an OpcodeList2 tag, whose bytes are big-endian. Other
 * opcodes are skipped. Returns the maps in file order, empty when the list has none, and nullopt
 * when the list is truncated, overruns its size, or has a GainMap whose size or geometry is
 * inconsistent; error then says why.
 **/
std::optional<std::vector<DngGainMap>> parseOpcodeList2GainMaps(const uint8_t* data,
                                                                std::size_t size,
                                                                std::string* error);

/**
 * @brief Flat form of maps, as raw/gain_maps stores it: the map count, then per map the values
 * top, left, bottom, right, plane, planes, rowPitch, colPitch, mapPointsV, mapPointsH,
 * mapSpacingV, mapSpacingH, mapOriginV, mapOriginH, mapPlanes, followed by the gains.
 **/
std::vector<double> serializeGainMaps(const std::vector<DngGainMap>& maps);

/**
 * @brief Inverse of serializeGainMaps(); nullopt when values is not a well-formed encoding.
 **/
std::optional<std::vector<DngGainMap>> deserializeGainMaps(const std::vector<double>& values);
} // namespace RawDngOpcodes

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_IO_RawDngOpcodes_h
