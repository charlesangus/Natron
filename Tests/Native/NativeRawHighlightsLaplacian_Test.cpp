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
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

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
const int kWidth = 150;
const int kHeight = 120;
const float kCoeffs[3] = { 2.f, 1.f, 1.5f };
// darktable's clip magic for Guided Laplacians.
const float kClipMagic = 0.995f;
// The box mean of the mask reaches 2 photosites, the bilinear RGB under it 1 more.
const int kReach = 3;

const char* const kXTransPattern = "GGRGGB"
                                   "GGBGGR"
                                   "BRGRBG"
                                   "GGBGGR"
                                   "GGRGGB"
                                   "RBGBRG";

// A gently graded reddish scene with a disc where every colour clipped and one where only red did.
RawTestSource::ValueFn
clippedScene(const RawLayout& layout)
{
    return [layout](int x, int y, int) {
        const int colour = layout.colourAt(x, y);
        const float scene[3] = { 3.f, 1.f, 1.f };
        float raw = 0.12f * scene[colour] * (1.f + 0.2f * (float)x / (float)kWidth) * (1.f + 0.1f * (float)y / (float)kHeight);
        if (std::hypot((float)x - 45.f, (float)y - 60.f) < 20.f) {
            raw = 1.05f;
        } else if (std::hypot((float)x - 110.f, (float)y - 60.f) < 16.f) {
            raw *= 6.f;
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

struct LaplacianVariant {
    int iterations;
    double noiseLevel;
    int scales;
    double solidColor;
};

// The defaults, then fewer iterations with noise, a flat colour and a smaller diameter.
const LaplacianVariant kVariants[] = {
    { 30, 0., 6, 0. },
    { 5, 0.3, 4, 0.6 },
};
} // namespace

class NativeRawHighlightsLaplacianTest
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
        node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamMode)->setValue((int)eRawHighlightsModeLaplacian);
        if (sourceOut) {
            *sourceOut = source;
        }

        return node;
    }

    static void setVariant(const NodePtr& node,
                           const LaplacianVariant& variant)
    {
        EffectInstancePtr effect = node->getEffectInstance();

        effect->getKnobByNameAndType<KnobInt>(kRawHighlightsParamIterations)->setValue(variant.iterations);
        effect->getKnobByNameAndType<KnobDouble>(kRawHighlightsParamNoiseLevel)->setValue(variant.noiseLevel);
        effect->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamScales)->setValue(variant.scales);
        effect->getKnobByNameAndType<KnobDouble>(kRawHighlightsParamSolidColor)->setValue(variant.solidColor);
    }

    static void expectFallsBackToOpposed(const NodePtr& node,
                                         int nComps)
    {
        ASSERT_TRUE(node->hasPersistentMessage());
        QString message;
        int type = 0;
        node->getPersistentMessage(&message, &type, false);
        EXPECT_EQ((int)eMessageTypeWarning, type);
        EXPECT_TRUE(message.contains(QString::fromUtf8("falling back to inpaint opposed")));

        RenderedPlane laplacian;
        std::string error;
        ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &laplacian, &error)) << error;
        node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamMode)->setValue((int)eRawHighlightsModeOpposed);
        RenderedPlane opposed;
        ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &opposed, &error)) << error;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                for (int c = 0; c < nComps; ++c) {
                    ASSERT_EQ(rawTestPlaneValue(opposed, x, y, c), rawTestPlaneValue(laplacian, x, y, c)) << x << "," << y << " channel " << c;
                }
            }
        }
    }
};

TEST_F(NativeRawHighlightsLaplacianTest, TheKnobsDefaultToDarktables)
{
    NodePtr node = createChain("RGGB");
    ASSERT_TRUE(bool(node));
    EffectInstancePtr effect = node->getEffectInstance();

    EXPECT_EQ(30, effect->getKnobByNameAndType<KnobInt>(kRawHighlightsParamIterations)->getValue());
    EXPECT_EQ(6, effect->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamScales)->getValue());
    EXPECT_EQ(12, (int)effect->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamScales)->getNumEntries());
    EXPECT_EQ(0., effect->getKnobByNameAndType<KnobDouble>(kRawHighlightsParamSolidColor)->getValue());
    EXPECT_EQ(0., effect->getKnobByNameAndType<KnobDouble>(kRawHighlightsParamNoiseLevel)->getValue());
}

TEST_F(NativeRawHighlightsLaplacianTest, OnlyClippedAreasChange)
{
    for (const char* pattern : { "RGGB", "GBRG" }) {
        NodePtr node = createChain(pattern);
        ASSERT_TRUE(bool(node));
        const RawLayout layout = *RawLayout::fromPattern(pattern);
        const RawTestSource::ValueFn scene = clippedScene(layout);
        std::vector<char> clipped((std::size_t)kWidth * kHeight, 0);
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const int colour = layout.colourAt(x, y);
                clipped[(std::size_t)y * kWidth + x] = (scene(x, y, 0) > kClipMagic * kCoeffs[colour]) ? 1 : 0;
            }
        }
        const auto nearClipped = [&](int x, int y) {
            for (int v = std::max(0, y - kReach); v <= std::min(kHeight - 1, y + kReach); ++v) {
                for (int u = std::max(0, x - kReach); u <= std::min(kWidth - 1, x + kReach); ++u) {
                    if (clipped[(std::size_t)v * kWidth + u]) {
                        return true;
                    }
                }
            }

            return false;
        };

        for (const LaplacianVariant& variant : kVariants) {
            setVariant(node, variant);
            RenderedPlane full;
            std::string error;
            ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &full, &error)) << pattern << ": " << error;
            int changed = 0;
            int clippedCount = 0;
            for (int y = 0; y < kHeight; ++y) {
                for (int x = 0; x < kWidth; ++x) {
                    const float in = scene(x, y, 0);
                    const float out = rawTestPlaneValue(full, x, y, 0);
                    ASSERT_TRUE(std::isfinite(out)) << pattern << " " << x << "," << y;
                    if (!nearClipped(x, y)) {
                        ASSERT_EQ(in, out) << pattern << " iterations " << variant.iterations << " at " << x << "," << y;
                    }
                    if (clipped[(std::size_t)y * kWidth + x]) {
                        ++clippedCount;
                        changed += (in != out) ? 1 : 0;
                    }
                }
            }
            ASSERT_GT(clippedCount, 0);
            EXPECT_GT(changed, 0) << pattern << " iterations " << variant.iterations;
        }
    }
}

// Every window of a frame at an offset origin and of odd size renders the pixels of the whole
// frame, with each variant; on X-Trans, through the fallback to inpaint opposed.
TEST_F(NativeRawHighlightsLaplacianTest, RegionIndependentCpu)
{
    const RectI frame(-5, -3, kWidth + 2, kHeight + 4);

    for (const char* pattern : { "RGGB", "GBRG", kXTransPattern }) {
        for (const LaplacianVariant& variant : kVariants) {
            RegionIndependenceTest::expectRawRegionIndependent([&]() {
                NodePtr source;
                NodePtr node = createChain(pattern, 1, &source, frame);
                if (node) {
                    setVariant(node, variant);
                }

                return std::make_pair(node, source);
            },
                                                               frame, std::string(pattern) + " iterations " + std::to_string(variant.iterations));
        }
    }
}

TEST_F(NativeRawHighlightsLaplacianTest, ARenderWithNoiseRepeatsBitForBit)
{
    NodePtr node = createChain("RGGB");
    ASSERT_TRUE(bool(node));
    setVariant(node, kVariants[1]);
    RenderedPlane first;
    std::string error;
    ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &first, &error)) << error;
    // A different setting in between drops the cached reconstruction, so the second render recomputes.
    setVariant(node, kVariants[0]);
    RenderedPlane other;
    ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &other, &error)) << error;
    setVariant(node, kVariants[1]);
    RenderedPlane second;
    ASSERT_TRUE(renderRawTestPlane(node, RectI(0, 0, kWidth, kHeight), &second, &error)) << error;
    int differs = 0;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            ASSERT_EQ(rawTestPlaneValue(first, x, y, 0), rawTestPlaneValue(second, x, y, 0)) << x << "," << y;
            differs += (rawTestPlaneValue(first, x, y, 0) != rawTestPlaneValue(other, x, y, 0)) ? 1 : 0;
        }
    }
    EXPECT_GT(differs, 0);
}

TEST_F(NativeRawHighlightsLaplacianTest, XTransFallsBackToOpposedWithAWarning)
{
    const RawLayout bayer = *RawLayout::fromPattern("RGGB");
    const RawLayout xtrans = *RawLayout::fromPattern(kXTransPattern);
    bool fellBack = false;

    EXPECT_EQ(eRawHighlightsRequiresBayer, rawHighlightsModeRequirement(eRawHighlightsModeLaplacian));
    EXPECT_TRUE(rawHighlightsModeAvailable(eRawHighlightsModeLaplacian, bayer));
    EXPECT_FALSE(rawHighlightsModeAvailable(eRawHighlightsModeLaplacian, xtrans));
    EXPECT_EQ(eRawHighlightsModeOpposed, sanitiseRawHighlightsMode(eRawHighlightsModeLaplacian, xtrans, &fellBack));
    EXPECT_TRUE(fellBack);

    NodePtr node = createChain(kXTransPattern);
    ASSERT_TRUE(bool(node));
    expectFallsBackToOpposed(node, 1);
}

TEST_F(NativeRawHighlightsLaplacianTest, WithoutACfaItFallsBackToOpposedWithAWarning)
{
    EXPECT_FALSE(rawHighlightsModeAvailable(eRawHighlightsModeLaplacian, RawLayout()));

    NodePtr node = createChain(std::string(), 3);
    ASSERT_TRUE(bool(node));
    expectFallsBackToOpposed(node, 3);
}

TEST_F(NativeRawHighlightsLaplacianTest, TheLaplacianKnobsFollowTheMethod)
{
    NodePtr node = createChain("RGGB");
    ASSERT_TRUE(bool(node));
    EffectInstancePtr effect = node->getEffectInstance();
    KnobChoicePtr mode = effect->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamMode);
    const auto laplacianKnobsSecret = [&](bool secret) {
        EXPECT_EQ(secret, effect->getKnobByNameAndType<KnobInt>(kRawHighlightsParamIterations)->getIsSecret());
        EXPECT_EQ(secret, effect->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamScales)->getIsSecret());
        EXPECT_EQ(secret, effect->getKnobByNameAndType<KnobDouble>(kRawHighlightsParamSolidColor)->getIsSecret());
    };

    laplacianKnobsSecret(false);
    EXPECT_FALSE(effect->getKnobByNameAndType<KnobDouble>(kRawHighlightsParamNoiseLevel)->getIsSecret());
    EXPECT_TRUE(effect->getKnobByNameAndType<KnobInt>(kRawHighlightsParamCombine)->getIsSecret());
    EXPECT_TRUE(effect->getKnobByNameAndType<KnobDouble>(kRawHighlightsParamCandidating)->getIsSecret());
    EXPECT_TRUE(effect->getKnobByNameAndType<KnobChoice>(kRawHighlightsParamRecovery)->getIsSecret());
    EXPECT_TRUE(effect->getKnobByNameAndType<KnobDouble>(kRawHighlightsParamStrength)->getIsSecret());

    mode->setValue((int)eRawHighlightsModeSegments);
    laplacianKnobsSecret(true);
    EXPECT_TRUE(effect->getKnobByNameAndType<KnobDouble>(kRawHighlightsParamNoiseLevel)->getIsSecret());

    mode->setValue((int)eRawHighlightsModeOpposed);
    laplacianKnobsSecret(true);
    EXPECT_TRUE(effect->getKnobByNameAndType<KnobDouble>(kRawHighlightsParamNoiseLevel)->getIsSecret());

    mode->setValue((int)eRawHighlightsModeLaplacian);
    laplacianKnobsSecret(false);
    EXPECT_FALSE(effect->getKnobByNameAndType<KnobDouble>(kRawHighlightsParamNoiseLevel)->getIsSecret());
}

TEST_F(NativeRawHighlightsLaplacianTest, ThereIsNoGpuPath)
{
    NodePtr node = createChain("RGGB");
    ASSERT_TRUE(bool(node));
    StagedImageEffect* effect = dynamic_cast<StagedImageEffect*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect != nullptr);
    GpuQuery query;
    query.time = 0.;
    query.view = ViewIdx(0);
    query.nComps = 1;
    query.bitDepth = eImageBitDepthFloat;
    EXPECT_FALSE(effect->canRenderOnGpu(query));
}
