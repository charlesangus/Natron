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
#include <string>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativePluginIdentity.h"
#include "RawTestSource.h"

#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Raw/RawHighlights.h"

NATRON_NAMESPACE_USING

namespace {
const int kWidth = 150;
const int kHeight = 120;
const float kCoeffs[3] = { 2.f, 1.f, 1.5f };

const char* const kXTransPattern = "GGRGGB"
                                   "GGBGGR"
                                   "BRGRBG"
                                   "GGBGGR"
                                   "GGRGGB"
                                   "RBGBRG";

// A flat reddish scene, flat so that the segmentation finds candidates next to its segments, with
// a disc where every colour clipped, large enough for Rebuild, and a disc where only red did.
RawTestSource::ValueFn
clippedScene(const RawLayout& layout)
{
    return [layout](int x, int y, int) {
        const int colour = layout.colourAt(x, y);
        const float scene[3] = { 3.f, 1.f, 1.f };
        float raw = 0.12f * scene[colour] * (1.f + 0.001f * (float)x / (float)kWidth);
        if (std::hypot((float)x - 45.f, (float)y - 60.f) < 28.f) {
            raw = 1.05f;
        } else if (std::hypot((float)x - 110.f, (float)y - 60.f) < 20.f) {
            raw *= 8.f;
        }

        return std::fmin(raw, 1.05f) * kCoeffs[colour];
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

const RectI kTiles[] = {
    RectI(30, 40, 70, 85),
    RectI(0, 0, 9, 7),
    RectI(kWidth - 11, kHeight - 5, kWidth, kHeight),
    RectI(45, 60, 46, 61),
    RectI(100, 50, 101, 51),
    RectI(90, 0, 150, 64),
    RectI(23, 38, 68, 82),
};

struct Rebuild {
    int recovery;
    double strength;
    double noiseLevel;
};

// Off, Generic with noise, and Flat Large Segments, by their place in the Rebuild menu.
const Rebuild kRebuilds[] = {
    { 0, 0., 0. },
    { 1, 0.6, 0.1 },
    { 6, 0.8, 0. },
};
} // namespace

class NativeRawHighlightsSegmentsTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRawTestSourceOnce();
        BaseTest::SetUp();
    }

    NodePtr createChain(const std::string& pattern,
                        int nComps = 1)
    {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWHIGHLIGHTS));

        EXPECT_TRUE(source && node);
        if (!source || !node) {
            return NodePtr();
        }
        const ImageMetadata metadata = whiteBalancedMetadata(pattern);
        const RawLayout layout = RawLayout::fromMetadata(metadata).value_or(RawLayout());
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(RectI(0, 0, kWidth, kHeight), nComps, clippedScene(layout), metadata);
        connectNodes(source, node, 0, true);
        node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamMode)->setValue((int)eRawHighlightsModeSegments);

        return node;
    }

    static void setRebuild(const NodePtr& node,
                           const Rebuild& rebuild)
    {
        EffectInstancePtr effect = node->getEffectInstance();

        effect->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamRecovery)->setValue(rebuild.recovery);
        effect->getKnobByNameAndType<KnobDouble>(kRawHighlightsParamStrength)->setValue(rebuild.strength);
        effect->getKnobByNameAndType<KnobDouble>(kRawHighlightsParamNoiseLevel)->setValue(rebuild.noiseLevel);
    }
};

TEST_F(NativeRawHighlightsSegmentsTest, TilesMatchTheFullFrameAndOnlyClippedPhotositesChange)
{
    for (const char* pattern : { "RGGB", "GBRG", kXTransPattern }) {
        NodePtr node = createChain(pattern);
        ASSERT_TRUE(bool(node));
        const RawLayout layout = *RawLayout::fromPattern(pattern);
        const RawTestSource::ValueFn scene = clippedScene(layout);

        RenderedPlane off;
        for (const Rebuild& rebuild : kRebuilds) {
            setRebuild(node, rebuild);
            RenderedPlane full;
            std::string error;
            ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &full, &error)) << pattern << ": " << error;
            int changed = 0;
            int rebuilt = 0;
            for (int y = 0; y < kHeight; ++y) {
                for (int x = 0; x < kWidth; ++x) {
                    const float in = scene(x, y, 0);
                    const float out = rawTestPlaneValue(full, x, y, 0);
                    ASSERT_TRUE(std::isfinite(out)) << pattern << " " << x << "," << y;
                    if (in < 0.98f * kCoeffs[layout.colourAt(x, y)]) {
                        ASSERT_EQ(in, out) << pattern << " rebuild " << rebuild.recovery << " at " << x << "," << y;
                    } else {
                        changed += (in != out) ? 1 : 0;
                    }
                    if (rebuild.recovery != 0) {
                        rebuilt += (out != rawTestPlaneValue(off, x, y, 0)) ? 1 : 0;
                    }
                }
            }
            EXPECT_GT(changed, 0) << pattern << " rebuild " << rebuild.recovery;
            if (rebuild.recovery == 0) {
                off = full;
            } else {
                EXPECT_GT(rebuilt, 0) << pattern << " rebuild " << rebuild.recovery;
            }

            for (const RectI& tile : kTiles) {
                RenderedPlane plane;
                ASSERT_TRUE(renderRawTestPlane(node, tile, &plane, &error)) << pattern << ": " << error;
                for (int y = tile.y1; y < tile.y2; ++y) {
                    for (int x = tile.x1; x < tile.x2; ++x) {
                        ASSERT_EQ(rawTestPlaneValue(full, x, y, 0), rawTestPlaneValue(plane, x, y, 0)) << pattern << " rebuild " << rebuild.recovery << " at " << x << "," << y << " in tile " << tile.x1 << "," << tile.y1 << "-" << tile.x2 << "," << tile.y2;
                    }
                }
            }
        }
    }
}

TEST_F(NativeRawHighlightsSegmentsTest, ARenderRepeatsBitForBit)
{
    for (const char* pattern : { "RGGB", kXTransPattern }) {
        NodePtr node = createChain(pattern);
        ASSERT_TRUE(bool(node));
        setRebuild(node, kRebuilds[1]);
        RenderedPlane first;
        std::string error;
        ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &first, &error)) << error;
        // A different setting in between drops the cached frame, so the second render recomputes.
        setRebuild(node, kRebuilds[2]);
        RenderedPlane other;
        ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &other, &error)) << error;
        setRebuild(node, kRebuilds[1]);
        RenderedPlane second;
        ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &second, &error)) << error;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                ASSERT_EQ(rawTestPlaneValue(first, x, y, 0), rawTestPlaneValue(second, x, y, 0)) << pattern << " " << x << "," << y;
            }
        }
    }
}

TEST_F(NativeRawHighlightsSegmentsTest, WithoutACfaItFallsBackToOpposedWithAWarning)
{
    const RawLayout bayer = *RawLayout::fromPattern("RGGB");
    const RawLayout linear;
    bool fellBack = false;

    EXPECT_EQ(eRawHighlightsRequiresCfa, rawHighlightsModeRequirement(eRawHighlightsModeSegments));
    EXPECT_TRUE(rawHighlightsModeAvailable(eRawHighlightsModeSegments, bayer));
    EXPECT_FALSE(rawHighlightsModeAvailable(eRawHighlightsModeSegments, linear));
    EXPECT_EQ(eRawHighlightsModeOpposed, sanitiseRawHighlightsMode(eRawHighlightsModeSegments, linear, &fellBack));
    EXPECT_TRUE(fellBack);

    NodePtr node = createChain(std::string(), 3);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(node->hasPersistentMessage());
    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("falling back to inpaint opposed")));

    RenderedPlane segments;
    std::string error;
    ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &segments, &error)) << error;
    node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamMode)->setValue((int)eRawHighlightsModeOpposed);
    RenderedPlane opposed;
    ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &opposed, &error)) << error;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            for (int c = 0; c < 3; ++c) {
                ASSERT_EQ(rawTestPlaneValue(opposed, x, y, c), rawTestPlaneValue(segments, x, y, c)) << x << "," << y << " channel " << c;
            }
        }
    }
}

TEST_F(NativeRawHighlightsSegmentsTest, TheSegmentationKnobsFollowTheMethodAndRebuild)
{
    NodePtr node = createChain("RGGB");
    ASSERT_TRUE(bool(node));
    EffectInstancePtr effect = node->getEffectInstance();
    KnobChoicePtr mode = effect->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamMode);

    EXPECT_FALSE(effect->getKnobByNameAndType<KnobInt>(kRawHighlightsParamCombine)->getIsSecret());
    EXPECT_FALSE(effect->getKnobByNameAndType<KnobDouble>(kRawHighlightsParamCandidating)->getIsSecret());
    EXPECT_FALSE(effect->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamRecovery)->getIsSecret());
    EXPECT_TRUE(effect->getKnobByNameAndType<KnobDouble>(kRawHighlightsParamStrength)->getIsSecret());
    EXPECT_TRUE(effect->getKnobByNameAndType<KnobDouble>(kRawHighlightsParamNoiseLevel)->getIsSecret());

    setRebuild(node, kRebuilds[1]);
    EXPECT_FALSE(effect->getKnobByNameAndType<KnobDouble>(kRawHighlightsParamStrength)->getIsSecret());
    EXPECT_FALSE(effect->getKnobByNameAndType<KnobDouble>(kRawHighlightsParamNoiseLevel)->getIsSecret());

    mode->setValue((int)eRawHighlightsModeOpposed);
    EXPECT_TRUE(effect->getKnobByNameAndType<KnobInt>(kRawHighlightsParamCombine)->getIsSecret());
    EXPECT_TRUE(effect->getKnobByNameAndType<KnobDouble>(kRawHighlightsParamCandidating)->getIsSecret());
    EXPECT_TRUE(effect->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamRecovery)->getIsSecret());
    EXPECT_TRUE(effect->getKnobByNameAndType<KnobDouble>(kRawHighlightsParamStrength)->getIsSecret());
    EXPECT_TRUE(effect->getKnobByNameAndType<KnobDouble>(kRawHighlightsParamNoiseLevel)->getIsSecret());
}

TEST_F(NativeRawHighlightsSegmentsTest, ThereIsNoGpuPath)
{
    for (const char* pattern : { "RGGB", kXTransPattern }) {
        NodePtr node = createChain(pattern);
        ASSERT_TRUE(bool(node));
        StagedImageEffect* effect = dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get());
        ASSERT_TRUE(effect != nullptr);
        GpuQuery query;
        query.time = 0.;
        query.view = ViewIdx(0);
        query.nComps = 1;
        query.bitDepth = eImageBitDepthFloat;
        EXPECT_FALSE(effect->canRenderOnGpu(query)) << pattern;
    }
}
