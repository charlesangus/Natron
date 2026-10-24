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

#include <array>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/RectI.h"

NATRON_NAMESPACE_USING

namespace {

const RawCfaColorEnum R = eRawCfaColorRed;
const RawCfaColorEnum G = eRawCfaColorGreen;
const RawCfaColorEnum B = eRawCfaColorBlue;

typedef std::array<std::array<RawCfaColorEnum, 2>, 2> Bayer;

struct BayerCase {
    const char* pattern;
    // [y][x] for the four pixels at the origin.
    Bayer uncropped;
    // The same four pixels once the window starts one pixel further right.
    Bayer croppedOneX;
    // ... and once it starts one pixel further up.
    Bayer croppedOneY;
};

const BayerCase kBayerCases[] = {
    { "RGGB", { { { R, G }, { G, B } } }, { { { G, R }, { B, G } } }, { { { G, B }, { R, G } } } },
    { "BGGR", { { { B, G }, { G, R } } }, { { { G, B }, { R, G } } }, { { { G, R }, { B, G } } } },
    { "GRBG", { { { G, R }, { B, G } } }, { { { R, G }, { G, B } } }, { { { B, G }, { G, R } } } },
    { "GBRG", { { { G, B }, { R, G } } }, { { { B, G }, { G, R } } }, { { { R, G }, { G, B } } } },
};

// Fujifilm's X-Trans layout, row 0 first.
const char* const kXTransRows[6] = {
    "GGRGGB",
    "GGBGGR",
    "BRGRBG",
    "GGBGGR",
    "GGRGGB",
    "RBGBRG",
};

RawCfaColorEnum
xtransAt(int x,
         int y)
{
    const char c = kXTransRows[((y % 6) + 6) % 6][((x % 6) + 6) % 6];

    return (c == 'R') ? R : ((c == 'G') ? G : B);
}

std::string
xtransPattern()
{
    std::string pattern;

    for (int row = 0; row < 6; ++row) {
        pattern += kXTransRows[row];
    }

    return pattern;
}

} // namespace

TEST(RawMetadata, BayerColourAtMatchesHandTable)
{
    for (const BayerCase& c : kBayerCases) {
        const std::optional<RawLayout> layout = RawLayout::fromPattern(c.pattern);
        ASSERT_TRUE(layout.has_value()) << c.pattern;
        EXPECT_TRUE(layout->hasCfa());
        EXPECT_EQ(2, layout->period());
        EXPECT_EQ(std::string(c.pattern), layout->pattern());
        // Several periods, and negative coordinates, which a region of definition can have.
        for (int y = -4; y < 6; ++y) {
            for (int x = -4; x < 6; ++x) {
                EXPECT_EQ(c.uncropped[((y % 2) + 2) % 2][((x % 2) + 2) % 2], layout->colourAt(x, y)) << c.pattern << " at " << x << "," << y;
            }
        }
    }
}

TEST(RawMetadata, BayerColourAtFollowsAOnePixelCrop)
{
    for (const BayerCase& c : kBayerCases) {
        const std::optional<RawLayout> layout = RawLayout::fromPattern(c.pattern);
        ASSERT_TRUE(layout.has_value()) << c.pattern;
        const RawLayout croppedX = layout->croppedTo(1, 0);
        const RawLayout croppedY = layout->croppedTo(0, 1);
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                EXPECT_EQ(c.croppedOneX[y % 2][x % 2], croppedX.colourAt(x, y)) << c.pattern << " at " << x << "," << y;
                EXPECT_EQ(c.croppedOneY[y % 2][x % 2], croppedY.colourAt(x, y)) << c.pattern << " at " << x << "," << y;
            }
        }
    }
}

TEST(RawMetadata, XTransColourAtMatchesHandTable)
{
    const std::optional<RawLayout> layout = RawLayout::fromPattern(xtransPattern());

    ASSERT_TRUE(layout.has_value());
    EXPECT_TRUE(layout->hasCfa());
    EXPECT_EQ(6, layout->period());
    EXPECT_EQ(xtransPattern(), layout->pattern());
    for (int y = -6; y < 12; ++y) {
        for (int x = -6; x < 12; ++x) {
            EXPECT_EQ(xtransAt(x, y), layout->colourAt(x, y)) << x << "," << y;
        }
    }

    const RawLayout cropped = layout->croppedTo(1, 1);
    for (int y = 0; y < 12; ++y) {
        for (int x = 0; x < 12; ++x) {
            EXPECT_EQ(xtransAt(x + 1, y + 1), cropped.colourAt(x, y)) << x << "," << y;
        }
    }
}

// An odd shift moves the colour under every pixel; shifting back restores it, however many
// periods away the origin went.
TEST(RawMetadata, ShiftsComposeAndUndo)
{
    const std::optional<RawLayout> layout = RawLayout::fromPattern(xtransPattern(), 3, -2);

    ASSERT_TRUE(layout.has_value());
    const RawLayout there = layout->shifted(-7, 13).shifted(101, -55);
    const RawLayout back = there.shifted(7 - 101, -13 + 55);
    EXPECT_TRUE(back == *layout);
    EXPECT_FALSE(there == *layout);
    for (int y = 0; y < 6; ++y) {
        for (int x = 0; x < 6; ++x) {
            EXPECT_EQ(layout->colourAt(x, y), there.colourAt(x + 94, y - 42)) << x << "," << y;
        }
    }
}

TEST(RawMetadata, NoneHasNoCfa)
{
    const std::optional<RawLayout> layout = RawLayout::fromPattern(kRawCfaPatternNone);

    ASSERT_TRUE(layout.has_value());
    EXPECT_FALSE(layout->hasCfa());
    EXPECT_EQ(0, layout->period());
    EXPECT_EQ(eRawCfaColorNone, layout->colourAt(3, 5));
    EXPECT_EQ(std::string(kRawCfaPatternNone), layout->pattern());
}

TEST(RawMetadata, MalformedPatternsAreRejected)
{
    EXPECT_FALSE(RawLayout::fromPattern("").has_value());
    EXPECT_FALSE(RawLayout::fromPattern("RGB").has_value());
    EXPECT_FALSE(RawLayout::fromPattern("RGGX").has_value());
    EXPECT_FALSE(RawLayout::fromPattern("rggb").has_value());
    EXPECT_FALSE(RawLayout::fromPattern("RGGBRGGBR").has_value());
    EXPECT_FALSE(RawLayout::fromPattern(xtransPattern().substr(1)).has_value());

    ImageMetadata metadata;
    EXPECT_FALSE(RawLayout::fromMetadata(metadata).has_value());
    metadata.setString(kRawMetadataCfaPattern, "RGGB");
    metadata.setIntVector(kRawMetadataCfaOrigin, std::vector<int> { 1 });
    EXPECT_FALSE(RawLayout::fromMetadata(metadata).has_value());
}

TEST(RawMetadata, LayoutRoundTripsThroughMetadata)
{
    ImageMetadata metadata;
    const std::optional<RawLayout> layout = RawLayout::fromPattern("GBRG", 1, 0);

    ASSERT_TRUE(layout.has_value());
    layout->writeTo(&metadata);
    EXPECT_EQ(std::optional<std::string>("GBRG"), metadata.getString(kRawMetadataCfaPattern));
    EXPECT_EQ(std::optional<std::vector<int>>(std::vector<int> { 1, 0 }), metadata.getIntVector(kRawMetadataCfaOrigin));

    const std::optional<RawLayout> read = RawLayout::fromMetadata(metadata);
    ASSERT_TRUE(read.has_value());
    EXPECT_TRUE(*read == *layout);
    EXPECT_EQ(1, read->originX());
    EXPECT_EQ(0, read->originY());

    // A pattern without an origin key sits at (0, 0).
    ImageMetadata noOrigin;
    noOrigin.setString(kRawMetadataCfaPattern, "RGGB");
    const std::optional<RawLayout> atZero = RawLayout::fromMetadata(noOrigin);
    ASSERT_TRUE(atZero.has_value());
    EXPECT_EQ(R, atZero->colourAt(0, 0));
}

TEST(RawMetadata, EveryKeyRoundTrips)
{
    ImageMetadata metadata;

    RawMetadata::setBlackLevels(&metadata, std::vector<double> { 512., 513., 514., 515. });
    RawMetadata::setWhiteLevel(&metadata, 16383.);
    RawMetadata::setWbCoeffs(&metadata, std::array<double, 3> { 2.1, 1., 1.6 });
    const std::array<double, 9> matrix = { 0.6, 0.2, 0.15, 0.25, 0.7, 0.05, 0.02, 0.1, 0.9 };
    RawMetadata::setCamToXyz(&metadata, matrix);
    RawMetadata::setCrop(&metadata, RectI(8, 4, 6008, 4004));
    RawMetadata::setIso(&metadata, 400.);
    RawMetadata::setExposure(&metadata, 1. / 125.);
    RawMetadata::setStage(&metadata, eRawStagePrepared);

    EXPECT_EQ(std::optional<std::vector<double>>(std::vector<double> { 512., 513., 514., 515. }), RawMetadata::getBlackLevels(metadata));
    EXPECT_EQ(std::optional<double>(16383.), RawMetadata::getWhiteLevel(metadata));
    EXPECT_EQ((std::optional<std::array<double, 3>>(std::array<double, 3> { 2.1, 1., 1.6 })), RawMetadata::getWbCoeffs(metadata));
    EXPECT_EQ((std::optional<std::array<double, 9>>(matrix)), RawMetadata::getCamToXyz(metadata));
    const std::optional<RectI> crop = RawMetadata::getCrop(metadata);
    ASSERT_TRUE(crop.has_value());
    EXPECT_EQ(RectI(8, 4, 6008, 4004), *crop);
    EXPECT_EQ(std::optional<double>(400.), RawMetadata::getIso(metadata));
    EXPECT_EQ(std::optional<double>(1. / 125.), RawMetadata::getExposure(metadata));
    EXPECT_EQ(std::optional<RawStageEnum>(eRawStagePrepared), RawMetadata::getStage(metadata));
    EXPECT_EQ(std::optional<std::string>(kRawStagePrepared), metadata.getString(kRawMetadataStage));

    RawMetadata::setStage(&metadata, eRawStageRaw);
    EXPECT_EQ(std::optional<RawStageEnum>(eRawStageRaw), RawMetadata::getStage(metadata));
    EXPECT_EQ(std::optional<std::string>(kRawStageRaw), metadata.getString(kRawMetadataStage));
}

TEST(RawMetadata, CameraIdentityLimitAndDngMatrixRoundTrip)
{
    ImageMetadata metadata;

    EXPECT_FALSE(RawMetadata::getMake(metadata).has_value());
    EXPECT_FALSE(RawMetadata::getModel(metadata).has_value());
    EXPECT_FALSE(RawMetadata::getDngD65Matrix(metadata).has_value());
    EXPECT_EQ(1., RawMetadata::getLinearResponseLimit(metadata));

    const std::array<double, 9> matrix = { 0.9, 0.1, 0., 0.2, 0.8, 0.1, 0., 0.05, 0.95 };
    RawMetadata::setMake(&metadata, "Canon");
    RawMetadata::setModel(&metadata, "EOS 5D Mark III");
    RawMetadata::setLinearResponseLimit(&metadata, 0.75);
    RawMetadata::setDngD65Matrix(&metadata, matrix);

    EXPECT_EQ(std::optional<std::string>("Canon"), RawMetadata::getMake(metadata));
    EXPECT_EQ(std::optional<std::string>("EOS 5D Mark III"), RawMetadata::getModel(metadata));
    EXPECT_EQ(0.75, RawMetadata::getLinearResponseLimit(metadata));
    EXPECT_EQ((std::optional<std::array<double, 9>>(matrix)), RawMetadata::getDngD65Matrix(metadata));

    metadata.setDoubleVector(kRawMetadataDngD65Matrix, std::vector<double> { 1., 0., 0. });
    EXPECT_FALSE(RawMetadata::getDngD65Matrix(metadata).has_value());
}

TEST(RawMetadata, WrongShapesReadAsAbsent)
{
    ImageMetadata metadata;

    metadata.setDoubleVector(kRawMetadataWbCoeffs, std::vector<double> { 1., 2. });
    metadata.setDoubleVector(kRawMetadataCamToXyz, std::vector<double>(8, 0.));
    metadata.setIntVector(kRawMetadataCrop, std::vector<int> { 0, 0, 1 });
    metadata.setDoubleVector(kRawMetadataBlackLevels, std::vector<double>());
    metadata.setString(kRawMetadataWhiteLevel, "16383");
    metadata.setString(kRawMetadataStage, "demosaiced");

    EXPECT_FALSE(RawMetadata::getWbCoeffs(metadata).has_value());
    EXPECT_FALSE(RawMetadata::getCamToXyz(metadata).has_value());
    EXPECT_FALSE(RawMetadata::getCrop(metadata).has_value());
    EXPECT_FALSE(RawMetadata::getBlackLevels(metadata).has_value());
    EXPECT_FALSE(RawMetadata::getWhiteLevel(metadata).has_value());
    EXPECT_FALSE(RawMetadata::getStage(metadata).has_value());
    EXPECT_FALSE(RawMetadata::getIso(metadata).has_value());
    EXPECT_FALSE(RawMetadata::getExposure(metadata).has_value());
}

TEST(RawMetadata, RequireStageTexts)
{
    ImageMetadata metadata;

    EXPECT_EQ(std::optional<std::string>("Temperature1 needs raw camera data, but its input does not say which raw stage it is at: connect it below a Read of a raw file."),
              RawMetadata::requireStage(metadata, eRawStagePrepared, "Temperature1"));

    RawMetadata::setStage(&metadata, eRawStageRaw);
    EXPECT_FALSE(RawMetadata::requireStage(metadata, eRawStageRaw, "HotPixels1").has_value());
    EXPECT_EQ(std::optional<std::string>("Temperature1 needs prepared raw data: add a RawPrepare node upstream of it."),
              RawMetadata::requireStage(metadata, eRawStagePrepared, "Temperature1"));

    RawMetadata::setStage(&metadata, eRawStagePrepared);
    EXPECT_FALSE(RawMetadata::requireStage(metadata, eRawStagePrepared, "Temperature1").has_value());
    EXPECT_EQ(std::optional<std::string>("HotPixels1 needs raw data that has not been prepared yet: move it above the RawPrepare node upstream of it."),
              RawMetadata::requireStage(metadata, eRawStageRaw, "HotPixels1"));
}
