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
#include "Engine/Nodes/Raw/RawCACorrect.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
// Large enough for the polynomial fit: the correction needs at least 10 blocks of 112 photosites
// with a usable measurement, and fits a cubic field from 32.
const int kWidth = 768;
const int kHeight = 640;
// Red magnified and blue shrunk about the centre by this fraction: lateral chromatic aberration,
// up to 1.5 photosites at the corners.
const float kMagnification = 0.003f;

const char* const kXTransPattern = "GGRGGB"
                                   "GGBGGR"
                                   "BRGRBG"
                                   "GGBGGR"
                                   "GGRGGB"
                                   "RBGBRG";

float
scene(float x,
      float y)
{
    return 0.3f + 0.12f * std::sin(x * 0.21f) * std::cos(y * 0.17f) + 0.08f * std::sin((x + y) * 0.11f);
}

RawTestSource::ValueFn
aberratedMosaic(const RawLayout& layout)
{
    return [layout](int x, int y, int) {
        const int colour = layout.colourAt(x, y);
        const float s = (colour == 0) ? (1.f + kMagnification) : ((colour == 2) ? (1.f - kMagnification) : 1.f);
        const float cx = 0.5f * (float)kWidth;
        const float cy = 0.5f * (float)kHeight;

        return scene(cx + ((float)x - cx) * s, cy + ((float)y - cy) * s);
    };
}

// The root mean square distance of the red and blue photosites to the scene, 16 photosites in.
double
redBlueError(const RawLayout& layout,
             const std::function<float(int, int)>& value)
{
    double sum = 0.;
    int count = 0;

    for (int y = 16; y < kHeight - 16; ++y) {
        for (int x = 16; x < kWidth - 16; ++x) {
            if (layout.colourAt(x, y) == eRawCfaColorGreen) {
                continue;
            }
            const double d = (double)value(x, y) - (double)scene((float)x, (float)y);
            sum += d * d;
            ++count;
        }
    }

    return std::sqrt(sum / count);
}

std::string
persistentMessage(const NodePtr& node,
                  int* type)
{
    QString message;

    node->getPersistentMessage(&message, type, false);

    return message.toStdString();
}
} // namespace

class NativeRawCACorrectTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRawTestSourceOnce();
        BaseTest::SetUp();
    }

    NodePtr createChain(const RectI& rod,
                        int nComps,
                        const RawTestSource::ValueFn& value,
                        const ImageMetadata& metadata)
    {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWCACORRECT));

        EXPECT_TRUE(source && node);
        if (!source || !node) {
            return NodePtr();
        }
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(rod, nComps, value, metadata);
        connectNodes(source, node, 0, true);

        return node;
    }

    static RenderedPlane render(const NodePtr& node,
                                const RectI& window)
    {
        RenderedPlane plane;
        std::string error;

        EXPECT_TRUE(renderRawTestPlane(node, window, &plane, &error)) << error;

        return plane;
    }

    // The node leaves value's data as it is and warns.
    void expectPassThroughWithAWarning(const RectI& rod,
                                       int nComps,
                                       const RawTestSource::ValueFn& value,
                                       const ImageMetadata& metadata,
                                       const std::string& what)
    {
        NodePtr node = createChain(rod, nComps, value, metadata);
        ASSERT_TRUE(bool(node)) << what;
        const RenderedPlane plane = render(node, rod);
        ASSERT_EQ((std::size_t)nComps, plane.channels.size()) << what;
        for (int y = rod.y1; y < rod.y2; ++y) {
            for (int x = rod.x1; x < rod.x2; ++x) {
                for (int c = 0; c < nComps; ++c) {
                    ASSERT_EQ(value(x, y, c), rawTestPlaneValue(plane, x, y, c)) << what << " at " << x << "," << y;
                }
            }
        }
        ASSERT_TRUE(node->hasPersistentMessage()) << what;
        int type = 0;
        const std::string message = persistentMessage(node, &type);
        EXPECT_EQ((int)eMessageTypeWarning, type) << what << ": " << message;
    }
};

TEST_F(NativeRawCACorrectTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_RAWCACORRECT, PLUGIN_MAJOR_NATRON_RAWCACORRECT, "RawCACorrect");

    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWCACORRECT));
    ASSERT_TRUE(bool(node));
    EXPECT_EQ(eDataKindRaw, node->getEffectInstance()->getInputDataKind(0));
    EXPECT_EQ(eDataKindRaw, node->getEffectInstance()->getOutputDataKind());
}

// Red and blue scaled about the centre against green come back to within half their distance to
// the scene, green is never touched, and every tile is cut from the same corrected frame.
TEST_F(NativeRawCACorrectTest, ARadialAberrationIsCorrected)
{
    const RectI rod(0, 0, kWidth, kHeight);
    const ImageMetadata metadata = rawTestMetadata("RGGB", 0, 0, eRawStagePrepared);
    const RawLayout layout = *RawLayout::fromMetadata(metadata);
    const RawTestSource::ValueFn source = aberratedMosaic(layout);
    NodePtr node = createChain(rod, 1, source, metadata);
    ASSERT_TRUE(bool(node));

    const RenderedPlane full = render(node, rod);
    ASSERT_EQ(1u, full.channels.size());
    const double before = redBlueError(layout, [&](int x, int y) { return source(x, y, 0); });
    const double after = redBlueError(layout, [&](int x, int y) { return rawTestPlaneValue(full, x, y, 0); });
    EXPECT_LT(after, 0.5 * before) << "before " << before << ", after " << after;
    EXPECT_FALSE(node->hasPersistentMessage());
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            if (layout.colourAt(x, y) == eRawCfaColorGreen) {
                ASSERT_EQ(source(x, y, 0), rawTestPlaneValue(full, x, y, 0)) << x << "," << y;
            }
        }
    }

    const RectI tiles[] = {
        RectI(100, 90, 357, 301),
        RectI(0, 0, 13, 9),
        RectI(kWidth - 31, kHeight - 17, kWidth, kHeight),
        RectI(384, 320, 385, 321),
        RectI(500, 0, kWidth, 64),
    };
    for (const RectI& tile : tiles) {
        const RenderedPlane plane = render(node, tile);
        for (int y = tile.y1; y < tile.y2; ++y) {
            for (int x = tile.x1; x < tile.x2; ++x) {
                ASSERT_EQ(rawTestPlaneValue(full, x, y, 0), rawTestPlaneValue(plane, x, y, 0)) << x << "," << y << " in tile " << tile.x1 << "," << tile.y1 << "-" << tile.x2 << "," << tile.y2;
            }
        }
    }
}

// The frame is measured again whenever the settings change, and the same settings give the same
// frame.
TEST_F(NativeRawCACorrectTest, ChangingTheSettingsMeasuresAgain)
{
    const RectI rod(0, 0, kWidth, kHeight);
    const ImageMetadata metadata = rawTestMetadata("GRBG", 1, 1, eRawStagePrepared);
    NodePtr node = createChain(rod, 1, aberratedMosaic(*RawLayout::fromMetadata(metadata)), metadata);
    ASSERT_TRUE(bool(node));
    const RectI window(200, 150, 260, 200);
    KnobChoicePtr iterations = node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawCACorrectParamIterations);
    ASSERT_TRUE(bool(iterations));

    iterations->setValue(0);
    const RenderedPlane once = render(node, window);
    iterations->setValue(2);
    const RenderedPlane thrice = render(node, window);
    iterations->setValue(0);
    const RenderedPlane onceAgain = render(node, window);

    EXPECT_NE(once.pixels, thrice.pixels);
    EXPECT_EQ(once.pixels, onceAgain.pixels);

    node->getEffectInstance()->getKnobByNameAndType<KnobBool>(kRawCACorrectParamAvoidShift)->setValue(true);
    const RenderedPlane avoided = render(node, window);
    EXPECT_NE(once.pixels, avoided.pixels);
}

TEST_F(NativeRawCACorrectTest, XTransAndLinearDataPassThrough)
{
    const RectI rod(0, 0, 64, 48);
    const ImageMetadata xtrans = rawTestMetadata(kXTransPattern, 0, 0, eRawStagePrepared);
    const RawLayout layout = *RawLayout::fromMetadata(xtrans);
    expectPassThroughWithAWarning(rod, 1, aberratedMosaic(layout), xtrans, "X-Trans");

    const ImageMetadata linear = rawTestMetadata("none", 0, 0, eRawStagePrepared);
    const RawTestSource::ValueFn rgb = [](int x, int y, int c) {
        return 0.2f + 0.01f * (float)x + 0.02f * (float)y + 0.1f * (float)c;
    };
    expectPassThroughWithAWarning(rod, 3, rgb, linear, "linear");
}

// Too small to hold a tile, or too flat to measure anything in: the data passes through.
TEST_F(NativeRawCACorrectTest, SmallOrFlatImagesPassThrough)
{
    const ImageMetadata metadata = rawTestMetadata("RGGB", 0, 0, eRawStagePrepared);
    const RawTestSource::ValueFn flat = [](int, int, int) {
        return 0.25f;
    };
    expectPassThroughWithAWarning(RectI(0, 0, 24, 20), 1, flat, metadata, "small");
    expectPassThroughWithAWarning(RectI(0, 0, 160, 128), 1, flat, metadata, "flat");
}

TEST_F(NativeRawCACorrectTest, UnpreparedDataRaisesTheStageError)
{
    const ImageMetadata metadata = rawTestMetadata("RGGB", 0, 0, eRawStageRaw);
    NodePtr node = createChain(RectI(0, 0, 64, 48), 1, aberratedMosaic(*RawLayout::fromMetadata(metadata)), metadata);
    ASSERT_TRUE(bool(node));

    RenderedPlane plane;
    std::string error;
    EXPECT_FALSE(renderRawTestPlane(node, RectI(0, 0, 64, 48), &plane, &error));
    int type = 0;
    EXPECT_EQ(RawMetadata::requireStage(metadata, eRawStagePrepared, node->getLabel()).value_or(std::string()), persistentMessage(node, &type));
}

// The corrected frame is kept across renders of the frame, and Clear Caches drops it, so the next
// render corrects the frame again.
TEST_F(NativeRawCACorrectTest, ClearCachesDropsTheCorrectedFrame)
{
    const RectI rod(0, 0, kWidth, kHeight);
    const ImageMetadata metadata = rawTestMetadata("RGGB", 0, 0, eRawStagePrepared);
    NodePtr node = createChain(rod, 1, aberratedMosaic(*RawLayout::fromMetadata(metadata)), metadata);
    ASSERT_TRUE(bool(node));

    const FrameComputedCounter computed;
    // Emptying the node cache makes each window render, and leaves the node's own results.
    const auto renderWindow = [&node](const RectI& window) {
        appPTR->clearNodeCache();
        RenderedPlane plane;
        std::string error;
        EXPECT_TRUE(renderRawTestPlane(node, window, &plane, &error, false)) << error;

        return plane;
    };
    const RectI window(100, 80, 164, 128);
    const RenderedPlane first = renderWindow(window);
    (void)renderWindow(RectI(kWidth - 64, kHeight - 48, kWidth, kHeight));
    EXPECT_EQ(1, computed.count(kRawCACorrectFrame));

    node->purgeAllInstancesCaches();
    const RenderedPlane again = renderWindow(window);
    EXPECT_EQ(2, computed.count(kRawCACorrectFrame));
    EXPECT_EQ(first.pixels, again.pixels);
}
