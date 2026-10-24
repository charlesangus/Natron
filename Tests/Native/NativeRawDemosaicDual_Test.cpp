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
#include "Engine/Nodes/Raw/RawDemosaic.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kXTransPattern = "GGRGGB"
                                   "GGBGGR"
                                   "BRGRBG"
                                   "GGBGGR"
                                   "GGRGGB"
                                   "RBGBRG";

float
texturedScene(int x,
              int y,
              int c)
{
    const float r2 = ((float)x - 37.f) * ((float)x - 37.f) + ((float)y - 29.f) * ((float)y - 29.f);
    const float block = ((((x / 23) + (y / 19)) & 1) != 0) ? 0.15f : 0.f;
    switch (c) {
    case 0:
        return 0.4f + 0.3f * std::cos(r2 * 0.0007f) + block;
    case 1:
        return 0.5f + 0.25f * std::sin((float)x * 0.4f) * std::cos((float)y * 0.3f) + block;
    default:
        return 0.3f + 0.002f * (float)(x * y % 97) + block;
    }
}

// Gentle grey waves on the left half, a hard grey pattern on the right half: the blend mask is
// near 0 on the left and 1 on the right.
const int kSplitWidth = 200;

float
splitScene(int x,
           int y,
           int)
{
    if (x < kSplitWidth / 2) {
        return 0.4f + 0.02f * std::sin(0.3f * (float)x) * std::cos(0.2f * (float)y);
    }

    return ((((x / 2) + (y / 3)) & 1) != 0) ? 0.7f : 0.4f;
}

struct DualMode {
    const char* name;
    const char* pattern;
    const char* knob;
    int dual;
    int single;
};

const DualMode kModes[] = {
    { "RCD (dual)", "RGGB", kRawDemosaicParamBayerMethod, eRawDemosaicBayerRCDDual, eRawDemosaicBayerRCD },
    { "AMaZE (dual)", "GBRG", kRawDemosaicParamBayerMethod, eRawDemosaicBayerAMaZEDual, eRawDemosaicBayerAMaZE },
    { "Markesteijn 3-pass (dual)", kXTransPattern, kRawDemosaicParamXTransMethod, eRawDemosaicXTransMarkesteijn3Dual, eRawDemosaicXTransMarkesteijn3 },
};
} // namespace

class NativeRawDemosaicDualTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRawTestSourceOnce();
        BaseTest::SetUp();
    }

    NodePtr createChain(const RectI& rod,
                        const DualMode& mode,
                        int originX,
                        int originY,
                        const RawTestSource::ValueFn& scene,
                        bool whiteBalanced)
    {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWDEMOSAIC));

        EXPECT_TRUE(source && node);
        if (!source || !node) {
            return NodePtr();
        }
        ImageMetadata metadata = rawTestMetadata(mode.pattern, originX, originY, eRawStagePrepared);
        if (whiteBalanced) {
            RawMetadata::setWbApplied(&metadata, { 2.1, 1., 1.6 });
        }
        const RawLayout layout = *RawLayout::fromMetadata(metadata);
        RawTestSource::ValueFn mosaic = [scene, layout](int x, int y, int) {
            return scene(x, y, layout.colourAt(x, y));
        };
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(rod, 1, mosaic, metadata);
        connectNodes(source, node, 0, true);
        setChoice(node, mode.knob, mode.dual);

        return node;
    }

    static void setChoice(const NodePtr& node,
                          const char* knob,
                          int value)
    {
        node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(knob)->setValue(value);
    }

    static void setThreshold(const NodePtr& node,
                             double value)
    {
        node->getEffectInstance()->getKnobByNameAndType<KnobDouble>(kRawDemosaicParamDualThreshold)->setValue(value);
    }

    static RenderedPlane render(const NodePtr& node,
                                const RectI& window)
    {
        RenderedPlane plane;
        std::string error;

        EXPECT_TRUE(renderRawTestPlane(node, window, &plane, &error)) << error;

        return plane;
    }

    static RectI fromTopLeft(const RectI& rod,
                             int left,
                             int top,
                             int width,
                             int height)
    {
        return RectI(rod.x1 + left, rod.y2 - top - height, rod.x1 + left + width, rod.y2 - top);
    }

    static void expectTilesMatchTheFullFrame(const NodePtr& node,
                                             const RectI& rod,
                                             const std::string& what)
    {
        const RenderedPlane full = render(node, rod);
        const int w = rod.width(), h = rod.height();
        // AMaZE's 128-photosite tile seams, the image corners, single pixels and long strips.
        const RectI tiles[] = {
            fromTopLeft(rod, 0, 0, 9, 7),
            fromTopLeft(rod, 3, 2, 5, 40),
            fromTopLeft(rod, 125, 124, 10, 9),
            fromTopLeft(rod, 254, 253, 6, 7),
            fromTopLeft(rod, 127, 128, 1, 1),
            fromTopLeft(rod, 40, 200, 70, 50),
            fromTopLeft(rod, 180, 30, 120, 4),
            fromTopLeft(rod, w - 13, h - 6, 13, 6),
            fromTopLeft(rod, w - 60, 0, 60, 3),
            fromTopLeft(rod, 0, h - 1, w, 1),
        };
        for (const RectI& tile : tiles) {
            const RenderedPlane plane = render(node, tile);
            for (int y = tile.y1; y < tile.y2; ++y) {
                for (int x = tile.x1; x < tile.x2; ++x) {
                    for (int c = 0; c < 4; ++c) {
                        ASSERT_EQ(rawTestPlaneValue(full, x, y, c), rawTestPlaneValue(plane, x, y, c))
                            << what << " at " << x << "," << y << " channel " << c << " in tile " << tile.x1 << "," << tile.y1 << "-" << tile.x2 << "," << tile.y2;
                    }
                }
            }
        }
    }
};

TEST_F(NativeRawDemosaicDualTest, TilesMatchTheFullFrame)
{
    const RectI rod(-3, 5, 357, 305);
    for (const DualMode& mode : kModes) {
        NodePtr node = createChain(rod, mode, 1, -2, texturedScene, true);
        ASSERT_TRUE(bool(node));
        for (double threshold : { 0.2, 0.6 }) {
            setThreshold(node, threshold);
            expectTilesMatchTheFullFrame(node, rod, std::string(mode.name) + " at " + std::to_string(threshold));
        }
    }
}

TEST_F(NativeRawDemosaicDualTest, TilesMatchTheFullFrameWithLocalGreens)
{
    const RectI rod(0, 0, 300, 270);
    for (int m = 0; m < 2; ++m) {
        NodePtr node = createChain(rod, kModes[m], 0, 0, texturedScene, false);
        ASSERT_TRUE(bool(node));
        setChoice(node, kRawDemosaicParamGreenEq, eRawDemosaicGreenEqLocal);
        expectTilesMatchTheFullFrame(node, rod, std::string(kModes[m].name) + " with local greens");
    }
}

// darktable's dual_demosaic() returns before building a mask when the threshold is 0.
TEST_F(NativeRawDemosaicDualTest, ThresholdZeroKeepsTheHighFrequencyMethod)
{
    const RectI rod(0, 0, 160, 150);
    for (const DualMode& mode : kModes) {
        NodePtr node = createChain(rod, mode, 0, 0, texturedScene, false);
        ASSERT_TRUE(bool(node));
        setThreshold(node, 0.);
        const RenderedPlane dual = render(node, rod);
        setChoice(node, mode.knob, mode.single);
        const RenderedPlane single = render(node, rod);
        for (int y = rod.y1; y < rod.y2; ++y) {
            for (int x = rod.x1; x < rod.x2; ++x) {
                for (int c = 0; c < 4; ++c) {
                    ASSERT_EQ(rawTestPlaneValue(single, x, y, c), rawTestPlaneValue(dual, x, y, c)) << mode.name << " at " << x << "," << y << " channel " << c;
                }
            }
        }
    }
}

// The mask is 1 where the image is busy, so the high-frequency method shows through there, and
// near 0 where it is flat, where VNG's linear interpolation takes over.
TEST_F(NativeRawDemosaicDualTest, BusyAreasKeepTheHighFrequencyMethodAndFlatOnesDoNot)
{
    const RectI rod(0, 0, kSplitWidth, 160);
    for (const DualMode& mode : kModes) {
        NodePtr node = createChain(rod, mode, 0, 0, splitScene, false);
        ASSERT_TRUE(bool(node));
        setThreshold(node, 0.2);
        const RenderedPlane dual = render(node, rod);
        setChoice(node, mode.knob, mode.single);
        const RenderedPlane single = render(node, rod);
        int differing = 0;
        for (int y = rod.y1 + 12; y < rod.y2 - 12; ++y) {
            for (int x = rod.x1 + 12; x < rod.x2 - 12; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const float d = rawTestPlaneValue(dual, x, y, c);
                    const float s = rawTestPlaneValue(single, x, y, c);
                    if (x >= kSplitWidth / 2 + 12) {
                        ASSERT_NEAR(s, d, 1e-6) << mode.name << " at " << x << "," << y << " channel " << c;
                    } else if ((x < kSplitWidth / 2 - 12) && (d != s)) {
                        ++differing;
                    }
                }
            }
        }
        EXPECT_GT(differing, 0) << mode.name;
    }
}

// darktable's commit_params() turns colour smoothing off for a dual method.
TEST_F(NativeRawDemosaicDualTest, ColorSmoothingIsOffForDualMethods)
{
    const RectI rod(0, 0, 120, 100);
    for (const DualMode& mode : kModes) {
        NodePtr node = createChain(rod, mode, 0, 0, texturedScene, false);
        ASSERT_TRUE(bool(node));
        const RenderedPlane plain = render(node, rod);
        node->getEffectInstance()->getKnobByNameAndType<KnobInt>(kRawDemosaicParamColorSmoothing)->setValue(3);
        const RenderedPlane smoothed = render(node, rod);
        for (int y = rod.y1; y < rod.y2; ++y) {
            for (int x = rod.x1; x < rod.x2; ++x) {
                for (int c = 0; c < 4; ++c) {
                    ASSERT_EQ(rawTestPlaneValue(plain, x, y, c), rawTestPlaneValue(smoothed, x, y, c)) << mode.name << " at " << x << "," << y << " channel " << c;
                }
            }
        }
    }
}

TEST_F(NativeRawDemosaicDualTest, DualThresholdShowsOnlyForDualMethods)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWDEMOSAIC));
    ASSERT_TRUE(bool(node));
    EffectInstancePtr effect = node->getEffectInstance();
    KnobDoublePtr threshold = effect->getKnobByNameAndType<KnobDouble>(kRawDemosaicParamDualThreshold);
    ASSERT_TRUE(bool(threshold));

    EXPECT_DOUBLE_EQ(0.2, threshold->getValue());
    EXPECT_DOUBLE_EQ(0., threshold->getMinimum());
    EXPECT_DOUBLE_EQ(1., threshold->getMaximum());
    EXPECT_TRUE(threshold->getIsSecret());

    setChoice(node, kRawDemosaicParamBayerMethod, eRawDemosaicBayerRCDDual);
    EXPECT_FALSE(threshold->getIsSecret());
    setChoice(node, kRawDemosaicParamBayerMethod, eRawDemosaicBayerAMaZEDual);
    EXPECT_FALSE(threshold->getIsSecret());
    setChoice(node, kRawDemosaicParamBayerMethod, eRawDemosaicBayerAMaZE);
    EXPECT_TRUE(threshold->getIsSecret());

    setChoice(node, kRawDemosaicParamXTransMethod, eRawDemosaicXTransMarkesteijn3Dual);
    EXPECT_FALSE(threshold->getIsSecret());
    setChoice(node, kRawDemosaicParamXTransMethod, eRawDemosaicXTransMarkesteijn3);
    EXPECT_TRUE(threshold->getIsSecret());
}
