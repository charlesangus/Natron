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
#include <utility>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativePluginIdentity.h"
#include "RawTestSource.h"
#include "RegionIndependenceSupport.h"

#include "Engine/EffectInstance.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Image/GpuRender.h"
#include "Engine/Nodes/Image/StagedImageEffect.h"
#include "Engine/Nodes/Raw/RawHighlights.h"

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

// A smooth scene with a disc where the sensor clipped every colour and a rim where only green did.
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
} // namespace

class NativeRawHighlightsInpaintTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRawTestSourceOnce();
        BaseTest::SetUp();
    }

    NodePtr createChain(const std::string& pattern,
                        int nComps = 1,
                        NodePtr* sourceOut = nullptr,
                        const RectI& rod = RectI(0, 0, kWidth, kHeight))
    {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWHIGHLIGHTS));

        EXPECT_TRUE(source && node);
        if (!source || !node) {
            return NodePtr();
        }
        const ImageMetadata metadata = whiteBalancedMetadata(pattern);
        const RawLayout layout = RawLayout::fromMetadata(metadata).value_or(RawLayout());
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(rod, nComps, clippedScene(layout), metadata);
        connectNodes(source, node, 0, true);
        node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamMode)->setValue((int)eRawHighlightsModeInpaint);
        if (sourceOut) {
            *sourceOut = source;
        }

        return node;
    }
};

TEST_F(NativeRawHighlightsInpaintTest, OnlyClippedPhotositesChange)
{
    for (const char* pattern : { "RGGB", "GBRG", kXTransPattern }) {
        NodePtr node = createChain(pattern);
        ASSERT_TRUE(bool(node));
        const RawLayout layout = *RawLayout::fromPattern(pattern);
        const RawTestSource::ValueFn scene = clippedScene(layout);

        RenderedPlane full;
        std::string error;
        ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &full, &error)) << pattern << ": " << error;
        int changed = 0;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const float in = scene(x, y, 0);
                const float out = rawTestPlaneValue(full, x, y, 0);
                ASSERT_TRUE(std::isfinite(out)) << pattern << " " << x << "," << y;
                if (in < 0.9f * kCoeffs[layout.colourAt(x, y)]) {
                    ASSERT_EQ(in, out) << pattern << " " << x << "," << y;
                } else {
                    changed += (in != out) ? 1 : 0;
                }
            }
        }
        EXPECT_GT(changed, 0) << pattern;
    }
}

// Every window of a frame at an offset origin and of odd size renders the pixels of the whole
// frame.
TEST_F(NativeRawHighlightsInpaintTest, RegionIndependentCpu)
{
    const RectI frame(-5, -3, kWidth + 2, kHeight + 4);

    for (const char* pattern : { "RGGB", "GBRG", kXTransPattern }) {
        RegionIndependenceTest::expectRawRegionIndependent([&]() {
            NodePtr source;
            NodePtr node = createChain(pattern, 1, &source, frame);

            return std::make_pair(node, source);
        },
                                                           frame, pattern);
    }
}

TEST_F(NativeRawHighlightsInpaintTest, ARenderRepeatsBitForBit)
{
    for (const char* pattern : { "RGGB", kXTransPattern }) {
        NodePtr node = createChain(pattern);
        ASSERT_TRUE(bool(node));
        RenderedPlane first;
        RenderedPlane second;
        std::string error;
        ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &first, &error)) << error;
        ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &second, &error)) << error;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                ASSERT_EQ(rawTestPlaneValue(first, x, y, 0), rawTestPlaneValue(second, x, y, 0)) << pattern << " " << x << "," << y;
            }
        }
    }
}

TEST_F(NativeRawHighlightsInpaintTest, WithoutACfaItFallsBackToOpposedWithAWarning)
{
    const RawLayout bayer = *RawLayout::fromPattern("RGGB");
    const RawLayout linear;
    bool fellBack = false;

    EXPECT_EQ(eRawHighlightsRequiresCfa, rawHighlightsModeRequirement(eRawHighlightsModeInpaint));
    EXPECT_TRUE(rawHighlightsModeAvailable(eRawHighlightsModeInpaint, bayer));
    EXPECT_FALSE(rawHighlightsModeAvailable(eRawHighlightsModeInpaint, linear));
    EXPECT_EQ(eRawHighlightsModeOpposed, sanitiseRawHighlightsMode(eRawHighlightsModeInpaint, linear, &fellBack));
    EXPECT_TRUE(fellBack);

    NodePtr node = createChain(std::string(), 3);
    ASSERT_TRUE(bool(node));
    node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamMode)->setValue((int)eRawHighlightsModeInpaint);
    ASSERT_TRUE(node->hasPersistentMessage());
    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("falling back to inpaint opposed")));

    RenderedPlane plane;
    std::string error;
    EXPECT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &plane, &error)) << error;
}

TEST_F(NativeRawHighlightsInpaintTest, ThereIsNoGpuPath)
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
