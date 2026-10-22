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

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativePluginIdentity.h"
#include "RawTestSource.h"
#include "RenderBothWays.h"

#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Raw/RawHighlights.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const int kWidth = 48;
const int kHeight = 42;
const float kCoeffs[3] = { 2.f, 1.f, 1.5f };

const char* const kXTransPattern = "GGRGGB"
                                   "GGBGGR"
                                   "BRGRBG"
                                   "GGBGGR"
                                   "GGRGGB"
                                   "RBGBRG";

/**
 * @brief A white-balanced mosaic: a smooth scene at 0.3 to 0.5 of the white level times each
 * colour's multiplier, and a disc where the sensor clipped, every photosite at the white level
 * times its multiplier, with a rim where only green clipped.
 **/
RawTestSource::ValueFn
clippedScene(const RawLayout& layout)
{
    return [layout](int x, int y, int) {
        const int colour = layout.colourAt(x, y);
        const float d = std::hypot((float)x - 24.f, (float)y - 20.f);
        float raw = 0.3f + 0.2f * (float)x / (float)kWidth;
        if (d < 8.f) {
            raw = 1.f;
        } else if ((d < 11.f) && (colour == 1)) {
            raw = 1.f;
        }

        return raw * kCoeffs[colour];
    };
}

ImageMetadata
whiteBalancedMetadata(const std::string& pattern)
{
    ImageMetadata metadata = rawTestMetadata(pattern, 0, 0, eRawStagePrepared);

    RawMetadata::setWhiteLevel(&metadata, 1.);
    RawMetadata::setWbApplied(&metadata, { kCoeffs[0], kCoeffs[1], kCoeffs[2] });
    RawMetadata::setProcessedMaximum(&metadata, { kCoeffs[0], kCoeffs[1], kCoeffs[2] });

    return metadata;
}

// Windows a tile render may ask for: inside, on the image's edges, and single pixels.
const RectI kTiles[] = {
    RectI(0, 0, kWidth, kHeight),
    RectI(10, 9, 31, 28),
    RectI(0, 0, 7, 5),
    RectI(kWidth - 9, kHeight - 4, kWidth, kHeight),
    RectI(24, 20, 25, 21),
    RectI(17, 13, 18, 14),
    RectI(30, 0, 48, 13),
};
} // namespace

class NativeRawHighlightsTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRawTestSourceOnce();
        BaseTest::SetUp();
    }

    NodePtr createChain(const std::string& pattern,
                        RawHighlightsModeEnum mode,
                        NodePtr* sourceOut = nullptr)
    {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWHIGHLIGHTS));

        EXPECT_TRUE(source && node);
        if (!source || !node) {
            return NodePtr();
        }
        const ImageMetadata metadata = whiteBalancedMetadata(pattern);
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(RectI(0, 0, kWidth, kHeight), 1, clippedScene(*RawLayout::fromMetadata(metadata)), metadata);
        connectNodes(source, node, 0, true);
        node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamMode)->setValue((int)mode);
        if (sourceOut) {
            *sourceOut = source;
        }

        return node;
    }

    // Every tile of kTiles renders the same pixels as the whole frame.
    void expectTilesMatchTheFullFrame(const NodePtr& node,
                                      const char* what)
    {
        RenderedPlane full;
        std::string error;
        ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &full, &error)) << what << ": " << error;
        for (const RectI& tile : kTiles) {
            RenderedPlane plane;
            ASSERT_TRUE(renderRawTestPlane(node, tile, &plane, &error)) << what << ": " << error;
            for (int y = tile.y1; y < tile.y2; ++y) {
                for (int x = tile.x1; x < tile.x2; ++x) {
                    ASSERT_EQ(rawTestPlaneValue(full, x, y, 0), rawTestPlaneValue(plane, x, y, 0)) << what << " at " << x << "," << y << " in tile " << tile.x1 << "," << tile.y1 << "-" << tile.x2 << "," << tile.y2;
                }
            }
        }
    }
};

TEST_F(NativeRawHighlightsTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_RAWHIGHLIGHTS, PLUGIN_MAJOR_NATRON_RAWHIGHLIGHTS, "RawHighlights");

    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWHIGHLIGHTS));
    ASSERT_TRUE(bool(node));
    EXPECT_EQ(eDataKindRaw, node->getEffectInstance()->getInputDataKind(0));
    EXPECT_EQ(eDataKindRaw, node->getEffectInstance()->getOutputDataKind());
}

// Clip mode clips every photosite to the threshold times the white level, whatever its colour,
// which turns the clipped disc white once demosaiced.
TEST_F(NativeRawHighlightsTest, ClipModeClipsToTheThreshold)
{
    NodePtr node = createChain("RGGB", eRawHighlightsModeClip);
    ASSERT_TRUE(bool(node));
    const RawTestSource::ValueFn scene = clippedScene(*RawLayout::fromPattern("RGGB"));

    for (double threshold : { 1., 0.8 }) {
        node->getEffectInstance()->getKnobByNameAndType<KnobDouble>(kRawHighlightsParamClip)->setValue(threshold);
        RenderedPlane plane;
        std::string error;
        ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error)) << error;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                ASSERT_EQ(std::fmin(scene(x, y, 0), (float)threshold), rawTestPlaneValue(plane, x, y, 0)) << x << "," << y << " at threshold " << threshold;
            }
        }
    }
}

TEST_F(NativeRawHighlightsTest, UnclippedLeavesTheData)
{
    NodePtr node = createChain("RGGB", eRawHighlightsModeUnclipped);
    ASSERT_TRUE(bool(node));
    const RawTestSource::ValueFn scene = clippedScene(*RawLayout::fromPattern("RGGB"));

    RenderedPlane plane;
    std::string error;
    ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error)) << error;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            ASSERT_EQ(scene(x, y, 0), rawTestPlaneValue(plane, x, y, 0)) << x << "," << y;
        }
    }
}

// Inpaint Opposed only ever raises a photosite at or above its colour's clipping level, and
// leaves every other one alone; its chrominance is the whole image's, so tiles agree.
TEST_F(NativeRawHighlightsTest, InpaintOpposedOnlyRaisesClippedPhotosites)
{
    for (const char* pattern : { "RGGB", kXTransPattern }) {
        NodePtr node = createChain(pattern, eRawHighlightsModeOpposed);
        ASSERT_TRUE(bool(node));
        const RawLayout layout = *RawLayout::fromPattern(pattern);
        const RawTestSource::ValueFn scene = clippedScene(layout);

        RenderedPlane plane;
        std::string error;
        ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error)) << error;
        int raised = 0;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const float in = scene(x, y, 0);
                const float out = rawTestPlaneValue(plane, x, y, 0);
                const float clip = 0.987f * kCoeffs[layout.colourAt(x, y)];
                if (in < clip) {
                    ASSERT_EQ(in, out) << pattern << " " << x << "," << y;
                } else {
                    ASSERT_GE(out, in) << pattern << " " << x << "," << y;
                    raised += (out > in) ? 1 : 0;
                }
            }
        }
        EXPECT_GT(raised, 0) << pattern;
        expectTilesMatchTheFullFrame(node, pattern);
    }
}

TEST_F(NativeRawHighlightsTest, LChTilesMatchTheFullFrame)
{
    for (const char* pattern : { "GBRG", kXTransPattern }) {
        NodePtr node = createChain(pattern, eRawHighlightsModeLCh);
        ASSERT_TRUE(bool(node));
        const RawTestSource::ValueFn scene = clippedScene(*RawLayout::fromPattern(pattern));

        RenderedPlane plane;
        std::string error;
        ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error)) << error;
        // Far from the disc nothing clips, so nothing changes.
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                ASSERT_EQ(scene(x, y, 0), rawTestPlaneValue(plane, x, y, 0)) << pattern << " " << x << "," << y;
            }
        }
        expectTilesMatchTheFullFrame(node, pattern);
    }
}

TEST_F(NativeRawHighlightsTest, ClipModeOnLinearData)
{
    NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWHIGHLIGHTS));
    ASSERT_TRUE(source && node);
    const RawTestSource::ValueFn linear = [](int x, int y, int c) {
        return 0.1f * (float)(c + 1) + 0.04f * (float)x + 0.01f * (float)y;
    };
    dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(RectI(0, 0, kWidth, kHeight), 3, linear, rawTestMetadata(kRawCfaPatternNone, 0, 0, eRawStagePrepared));
    connectNodes(source, node, 0, true);
    node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamMode)->setValue((int)eRawHighlightsModeClip);

    RenderedPlane plane;
    std::string error;
    ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error)) << error;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            for (int c = 0; c < 3; ++c) {
                ASSERT_EQ(std::fmin(linear(x, y, c), 1.f), rawTestPlaneValue(plane, x, y, c)) << x << "," << y << " channel " << c;
            }
        }
    }
}

// At 26x26 the 3x3 block map has no rounding slack, and its last row and column of photosites
// map one block past it: the reads there must stay inside the mask.
TEST_F(NativeRawHighlightsTest, InpaintOpposedOnASizeWithoutMaskPadding)
{
    for (const char* pattern : { "RGGB", kXTransPattern }) {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWHIGHLIGHTS));
        ASSERT_TRUE(source && node);
        const ImageMetadata metadata = whiteBalancedMetadata(pattern);
        const RawLayout layout = *RawLayout::fromMetadata(metadata);
        const RawTestSource::ValueFn scene = [layout](int x, int y, int) {
            const int colour = layout.colourAt(x, y);
            const float raw = (std::hypot((float)x - 13.f, (float)y - 13.f) < 5.f) ? 1.f : 0.4f + 0.01f * (float)x;

            return raw * kCoeffs[colour];
        };
        const RectI rod(0, 0, 26, 26);
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(rod, 1, scene, metadata);
        connectNodes(source, node, 0, true);
        node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamMode)->setValue((int)eRawHighlightsModeOpposed);

        RenderedPlane plane;
        std::string error;
        ASSERT_TRUE(renderRawTestPlane(node, rod, &plane, &error)) << pattern << ": " << error;
        for (int y = rod.y1; y < rod.y2; ++y) {
            for (int x = rod.x1; x < rod.x2; ++x) {
                ASSERT_GE(rawTestPlaneValue(plane, x, y, 0), scene(x, y, 0)) << pattern << " " << x << "," << y;
            }
        }
    }
}

// The hook for the darktable reference comparison: darktable's own output for each mode on the
// clipped mosaic comes from the reference harness, which does not exist yet, so the test stays
// disabled and fails if forced to run without it.
TEST_F(NativeRawHighlightsTest, DISABLED_MatchesTheDarktableReference)
{
    for (RawHighlightsModeEnum mode : { eRawHighlightsModeClip, eRawHighlightsModeOpposed, eRawHighlightsModeLCh }) {
        NodePtr node = createChain("RGGB", mode);
        ASSERT_TRUE(bool(node));
        RenderedPlane plane;
        std::string error;
        ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error)) << error;
        ADD_FAILURE() << "no darktable reference output for mode " << (int)mode;
    }
}

TEST_F(NativeRawHighlightsTest, UnpreparedDataRaisesTheStageError)
{
    NodePtr source;
    NodePtr node = createChain("RGGB", eRawHighlightsModeClip, &source);
    ASSERT_TRUE(source && node);
    const ImageMetadata metadata = rawTestMetadata("RGGB", 0, 0, eRawStageRaw);
    dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setMetadata(metadata);

    RenderedPlane plane;
    std::string error;
    EXPECT_FALSE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error));
    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);
    EXPECT_EQ(QString::fromUtf8(RawMetadata::requireStage(metadata, eRawStagePrepared, node->getLabel()).value_or(std::string()).c_str()), message);
}
