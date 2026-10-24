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

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "RawDngOpcodes.h"

#include <cmath>
#include <cstring>

NATRON_NAMESPACE_ENTER

namespace RawDngOpcodes {
namespace {
    const uint32_t kOpcodeIdGainMap = 9;
    const std::size_t kOpcodeHeaderSize = 16;
    const std::size_t kGainMapFixedSize = 76;

    // Reads past size are the caller's to prevent: every use below checks the remaining length first.
    uint32_t
    readLong(const uint8_t* p)
    {
        return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
    }

    float
    readFloat(const uint8_t* p)
    {
        const uint32_t bits = readLong(p);
        float value;
        std::memcpy(&value, &bits, sizeof(value));

        return value;
    }

    double
    readDouble(const uint8_t* p)
    {
        const uint64_t bits = ((uint64_t)readLong(p) << 32) | readLong(p + 4);
        double value;
        std::memcpy(&value, &bits, sizeof(value));

        return value;
    }

    bool
    fail(std::string* error,
         const std::string& message)
    {
        if (error) {
            *error = message;
        }

        return false;
    }

    bool
    parseGainMap(const uint8_t* param,
                 std::size_t paramSize,
                 DngGainMap* map,
                 std::string* error)
    {
        if (paramSize < kGainMapFixedSize) {
            return fail(error, "GainMap opcode is smaller than its fixed fields");
        }
        map->top = readLong(param);
        map->left = readLong(param + 4);
        map->bottom = readLong(param + 8);
        map->right = readLong(param + 12);
        map->plane = readLong(param + 16);
        map->planes = readLong(param + 20);
        map->rowPitch = readLong(param + 24);
        map->colPitch = readLong(param + 28);
        map->mapPointsV = readLong(param + 32);
        map->mapPointsH = readLong(param + 36);
        map->mapSpacingV = readDouble(param + 40);
        map->mapSpacingH = readDouble(param + 48);
        map->mapOriginV = readDouble(param + 56);
        map->mapOriginH = readDouble(param + 64);
        map->mapPlanes = readLong(param + 72);

        if (map->rowPitch == 0 || map->colPitch == 0 || map->mapPointsV == 0 || map->mapPointsH == 0 || map->mapPlanes == 0) {
            return fail(error, "GainMap opcode has a zero pitch or an empty map");
        }
        if (!std::isfinite(map->mapSpacingV) || !std::isfinite(map->mapSpacingH) || !std::isfinite(map->mapOriginV) || !std::isfinite(map->mapOriginH)) {
            return fail(error, "GainMap opcode has a non-finite spacing or origin");
        }

        // Dividing the available count instead of multiplying the dimensions cannot overflow.
        const uint64_t available = (paramSize - kGainMapFixedSize) / 4;
        const uint64_t perRow = (uint64_t)map->mapPointsH * map->mapPlanes;
        if (available % perRow != 0 || available / perRow != map->mapPointsV) {
            return fail(error, "GainMap opcode size does not match its map dimensions");
        }

        map->gains.resize((std::size_t)available);
        for (std::size_t i = 0; i < map->gains.size(); ++i) {
            map->gains[i] = readFloat(param + kGainMapFixedSize + 4 * i);
        }

        return true;
    }
} // namespace

std::optional<std::vector<DngGainMap>>
parseOpcodeList2GainMaps(const uint8_t* data,
                         std::size_t size,
                         std::string* error)
{
    std::vector<DngGainMap> maps;

    if (!data || size < 4) {
        fail(error, "OpcodeList2 is too short to hold an opcode count");

        return std::nullopt;
    }

    uint32_t remaining = readLong(data);
    std::size_t offset = 4;
    while (remaining > 0) {
        if (size - offset < kOpcodeHeaderSize) {
            fail(error, "OpcodeList2 is truncated inside an opcode header");

            return std::nullopt;
        }
        const uint32_t opcodeId = readLong(data + offset);
        const std::size_t paramSize = readLong(data + offset + 12);
        const uint8_t* param = data + offset + kOpcodeHeaderSize;
        if (paramSize > size - offset - kOpcodeHeaderSize) {
            fail(error, "OpcodeList2 has an opcode larger than the list");

            return std::nullopt;
        }
        if (opcodeId == kOpcodeIdGainMap) {
            DngGainMap map;
            if (!parseGainMap(param, paramSize, &map, error)) {
                return std::nullopt;
            }
            maps.push_back(std::move(map));
        }
        offset += kOpcodeHeaderSize + paramSize;
        --remaining;
    }

    return maps;
}

std::vector<double>
serializeGainMaps(const std::vector<DngGainMap>& maps)
{
    std::vector<double> out;
    out.push_back((double)maps.size());
    for (const DngGainMap& m : maps) {
        const double header[kGainMapHeaderValues] = {
            (double)m.top, (double)m.left, (double)m.bottom, (double)m.right,
            (double)m.plane, (double)m.planes, (double)m.rowPitch, (double)m.colPitch,
            (double)m.mapPointsV, (double)m.mapPointsH,
            m.mapSpacingV, m.mapSpacingH, m.mapOriginV, m.mapOriginH,
            (double)m.mapPlanes
        };
        out.insert(out.end(), header, header + kGainMapHeaderValues);
        out.insert(out.end(), m.gains.begin(), m.gains.end());
    }

    return out;
}

std::optional<std::vector<DngGainMap>>
deserializeGainMaps(const std::vector<double>& values)
{
    if (values.empty() || !(values[0] >= 0.) || values[0] > (double)values.size()) {
        return std::nullopt;
    }
    const std::size_t count = (std::size_t)values[0];
    std::vector<DngGainMap> maps;
    std::size_t pos = 1;
    for (std::size_t k = 0; k < count; ++k) {
        if (values.size() - pos < kGainMapHeaderValues) {
            return std::nullopt;
        }
        const double* h = values.data() + pos;
        bool integral = true;
        for (std::size_t i = 0; i < kGainMapHeaderValues; ++i) {
            const bool isCount = (i < 10) || (i == 14);
            if (isCount && !(h[i] >= 0. && h[i] <= 4294967295. && h[i] == std::floor(h[i]))) {
                integral = false;
            }
        }
        if (!integral) {
            return std::nullopt;
        }
        DngGainMap m;
        m.top = (uint32_t)h[0];
        m.left = (uint32_t)h[1];
        m.bottom = (uint32_t)h[2];
        m.right = (uint32_t)h[3];
        m.plane = (uint32_t)h[4];
        m.planes = (uint32_t)h[5];
        m.rowPitch = (uint32_t)h[6];
        m.colPitch = (uint32_t)h[7];
        m.mapPointsV = (uint32_t)h[8];
        m.mapPointsH = (uint32_t)h[9];
        m.mapSpacingV = h[10];
        m.mapSpacingH = h[11];
        m.mapOriginV = h[12];
        m.mapOriginH = h[13];
        m.mapPlanes = (uint32_t)h[14];
        pos += kGainMapHeaderValues;

        const uint64_t perRow = (uint64_t)m.mapPointsH * m.mapPlanes;
        const uint64_t room = values.size() - pos;
        if (perRow == 0 || m.mapPointsV > room / perRow) {
            return std::nullopt;
        }
        const uint64_t gainCount = perRow * m.mapPointsV;
        for (uint64_t i = 0; i < gainCount; ++i) {
            m.gains.push_back((float)values[pos + (std::size_t)i]);
        }
        pos += (std::size_t)gainCount;
        maps.push_back(std::move(m));
    }
    if (pos != values.size()) {
        return std::nullopt;
    }

    return maps;
}
} // namespace RawDngOpcodes

NATRON_NAMESPACE_EXIT
