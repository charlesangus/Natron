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

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/IO/RawDngOpcodes.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"

NATRON_NAMESPACE_USING

namespace {
typedef std::vector<uint8_t> Bytes;

void
putLong(Bytes* b,
        uint32_t v)
{
    for (int shift = 24; shift >= 0; shift -= 8) {
        b->push_back((uint8_t)(v >> shift));
    }
}

void
putFloat(Bytes* b,
         float v)
{
    uint32_t bits;
    std::memcpy(&bits, &v, 4);
    putLong(b, bits);
}

void
putDouble(Bytes* b,
          double v)
{
    uint64_t bits;
    std::memcpy(&bits, &v, 8);
    putLong(b, (uint32_t)(bits >> 32));
    putLong(b, (uint32_t)bits);
}

DngGainMap
makeMap(uint32_t cell,
        uint32_t pointsV = 3,
        uint32_t pointsH = 4)
{
    DngGainMap m;
    m.top = cell / 2;
    m.left = cell % 2;
    m.bottom = 3000;
    m.right = 4000;
    m.plane = 0;
    m.planes = 1;
    m.rowPitch = 2;
    m.colPitch = 2;
    m.mapPointsV = pointsV;
    m.mapPointsH = pointsH;
    m.mapSpacingV = 1. / (pointsV - 1);
    m.mapSpacingH = 1. / (pointsH - 1);
    m.mapOriginV = 0.;
    m.mapOriginH = 0.25;
    m.mapPlanes = 1;
    for (uint32_t i = 0; i < pointsV * pointsH; ++i) {
        m.gains.push_back(1.f + 0.125f * (float)(cell * 100 + i));
    }

    return m;
}

void
putGainMap(Bytes* b,
           const DngGainMap& m)
{
    putLong(b, 9);
    putLong(b, 0x01030000);
    putLong(b, 1);
    putLong(b, (uint32_t)(76 + 4 * m.gains.size()));
    putLong(b, m.top);
    putLong(b, m.left);
    putLong(b, m.bottom);
    putLong(b, m.right);
    putLong(b, m.plane);
    putLong(b, m.planes);
    putLong(b, m.rowPitch);
    putLong(b, m.colPitch);
    putLong(b, m.mapPointsV);
    putLong(b, m.mapPointsH);
    putDouble(b, m.mapSpacingV);
    putDouble(b, m.mapSpacingH);
    putDouble(b, m.mapOriginV);
    putDouble(b, m.mapOriginH);
    putLong(b, m.mapPlanes);
    for (float g : m.gains) {
        putFloat(b, g);
    }
}

void
putOtherOpcode(Bytes* b)
{
    putLong(b, 1);
    putLong(b, 0x01030000);
    putLong(b, 0);
    putLong(b, 8);
    putLong(b, 0xDEADBEEF);
    putLong(b, 0x01020304);
}

void
expectEqual(const DngGainMap& a,
            const DngGainMap& b)
{
    EXPECT_EQ(a.top, b.top);
    EXPECT_EQ(a.left, b.left);
    EXPECT_EQ(a.bottom, b.bottom);
    EXPECT_EQ(a.right, b.right);
    EXPECT_EQ(a.plane, b.plane);
    EXPECT_EQ(a.planes, b.planes);
    EXPECT_EQ(a.rowPitch, b.rowPitch);
    EXPECT_EQ(a.colPitch, b.colPitch);
    EXPECT_EQ(a.mapPointsV, b.mapPointsV);
    EXPECT_EQ(a.mapPointsH, b.mapPointsH);
    EXPECT_EQ(a.mapSpacingV, b.mapSpacingV);
    EXPECT_EQ(a.mapSpacingH, b.mapSpacingH);
    EXPECT_EQ(a.mapOriginV, b.mapOriginV);
    EXPECT_EQ(a.mapOriginH, b.mapOriginH);
    EXPECT_EQ(a.mapPlanes, b.mapPlanes);
    EXPECT_EQ(a.gains, b.gains);
}

std::optional<std::vector<DngGainMap>>
parse(const Bytes& b,
      std::string* error = nullptr)
{
    return RawDngOpcodes::parseOpcodeList2GainMaps(b.data(), b.size(), error);
}
} // namespace

TEST(RawDngOpcodes, FourBayerCellMapsParseExactly)
{
    Bytes list;
    putLong(&list, 4);
    std::vector<DngGainMap> expected;
    for (uint32_t cell = 0; cell < 4; ++cell) {
        expected.push_back(makeMap(cell));
        putGainMap(&list, expected.back());
    }

    const auto maps = parse(list);
    ASSERT_TRUE(maps.has_value());
    ASSERT_EQ(maps->size(), 4u);
    for (std::size_t i = 0; i < 4; ++i) {
        expectEqual((*maps)[i], expected[i]);
    }
}

TEST(RawDngOpcodes, NonGainMapOpcodeIsSkipped)
{
    Bytes list;
    putLong(&list, 2);
    putOtherOpcode(&list);
    const DngGainMap map = makeMap(3);
    putGainMap(&list, map);

    const auto maps = parse(list);
    ASSERT_TRUE(maps.has_value());
    ASSERT_EQ(maps->size(), 1u);
    expectEqual((*maps)[0], map);
}

TEST(RawDngOpcodes, NoOpcodesGivesNoMapsAndNoKey)
{
    Bytes list;
    putLong(&list, 0);

    const auto maps = parse(list);
    ASSERT_TRUE(maps.has_value());
    EXPECT_TRUE(maps->empty());

    ImageMetadata metadata;
    EXPECT_FALSE(metadata.getDoubleVector(kRawMetadataGainMaps).has_value());
    EXPECT_TRUE(RawMetadata::getGainMaps(metadata).empty());
}

TEST(RawDngOpcodes, TruncatedListsFailCleanly)
{
    Bytes list;
    putLong(&list, 2);
    putGainMap(&list, makeMap(0));
    putGainMap(&list, makeMap(1));

    for (std::size_t cut = 0; cut < list.size(); ++cut) {
        const Bytes truncated(list.begin(), list.begin() + cut);
        std::string error;
        const auto maps = parse(truncated, &error);
        EXPECT_FALSE(maps.has_value()) << "cut at " << cut;
        EXPECT_FALSE(error.empty()) << "cut at " << cut;
    }
    EXPECT_FALSE(RawDngOpcodes::parseOpcodeList2GainMaps(nullptr, 0, nullptr).has_value());
}

TEST(RawDngOpcodes, OversizedCountsFailCleanly)
{
    Bytes list;
    putLong(&list, 0xFFFFFFFFu);
    putGainMap(&list, makeMap(0));
    EXPECT_FALSE(parse(list).has_value());

    // A parameter size that points past the end of the list.
    Bytes big;
    putLong(&big, 1);
    putLong(&big, 9);
    putLong(&big, 0);
    putLong(&big, 0);
    putLong(&big, 0xFFFFFFF0u);
    EXPECT_FALSE(parse(big).has_value());
}

TEST(RawDngOpcodes, InconsistentMapDimensionsFail)
{
    DngGainMap map = makeMap(0);
    map.gains.pop_back();
    Bytes list;
    putLong(&list, 1);
    putGainMap(&list, map);
    EXPECT_FALSE(parse(list).has_value());

    DngGainMap hugeDims = makeMap(0);
    hugeDims.mapPointsV = 0xFFFFFFFFu;
    hugeDims.mapPointsH = 0xFFFFFFFFu;
    hugeDims.mapPlanes = 0xFFFFFFFFu;
    Bytes huge;
    putLong(&huge, 1);
    putGainMap(&huge, hugeDims);
    EXPECT_FALSE(parse(huge).has_value());
}

TEST(RawDngOpcodes, MetadataRoundTrip)
{
    std::vector<DngGainMap> maps;
    for (uint32_t cell = 0; cell < 4; ++cell) {
        maps.push_back(makeMap(cell));
    }
    ImageMetadata metadata;
    RawMetadata::setGainMaps(&metadata, maps);

    const std::optional<std::vector<double>> flat = metadata.getDoubleVector(kRawMetadataGainMaps);
    ASSERT_TRUE(flat.has_value());
    EXPECT_EQ((*flat)[0], 4.);
    EXPECT_EQ(flat->size(), 1u + 4u * (RawDngOpcodes::kGainMapHeaderValues + 12u));
    EXPECT_EQ((*flat)[1], 0.);

    const std::vector<DngGainMap> back = RawMetadata::getGainMaps(metadata);
    ASSERT_EQ(back.size(), 4u);
    for (std::size_t i = 0; i < 4; ++i) {
        expectEqual(back[i], maps[i]);
    }

    ImageMetadata garbled;
    garbled.setDoubleVector(kRawMetadataGainMaps, std::vector<double> { 2., 1., 2. });
    EXPECT_TRUE(RawMetadata::getGainMaps(garbled).empty());
}
