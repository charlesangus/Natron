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
float
texturedScene(int x,
              int y,
              int c)
{
    const float r2 = ((float)x - 37.f) * ((float)x - 37.f) + ((float)y - 29.f) * ((float)y - 29.f);
    switch (c) {
    case 0:
        return 0.4f + 0.3f * std::cos(r2 * 0.0007f);
    case 1:
        return 0.5f + 0.25f * std::sin((float)x * 0.4f) * std::cos((float)y * 0.3f);
    default:
        return 0.3f + 0.002f * (float)(x * y % 97);
    }
}

const int kRefineLevels[] = {
    eRawDemosaicLmmseBasic,
    eRawDemosaicLmmseMedian,
    eRawDemosaicLmmse3xMedian,
    eRawDemosaicLmmseRefineMedians,
    eRawDemosaicLmmse2xRefineMedians,
};
} // namespace

class NativeRawDemosaicLmmseTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRawTestSourceOnce();
        BaseTest::SetUp();
    }

    NodePtr createChain(const RectI& rod,
                        const std::string& pattern,
                        int originX,
                        int originY,
                        const RawTestSource::ValueFn& scene)
    {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWDEMOSAIC));

        EXPECT_TRUE(source && node);
        if (!source || !node) {
            return NodePtr();
        }
        const ImageMetadata metadata = rawTestMetadata(pattern, originX, originY, eRawStagePrepared);
        const RawLayout layout = *RawLayout::fromMetadata(metadata);
        RawTestSource::ValueFn mosaic = [scene, layout](int x, int y, int) {
            return scene(x, y, layout.colourAt(x, y));
        };
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(rod, 1, mosaic, metadata);
        connectNodes(source, node, 0, true);
        setChoice(node, kRawDemosaicParamBayerMethod, eRawDemosaicBayerLMMSE);

        return node;
    }

    static void setChoice(const NodePtr& node,
                          const char* knob,
                          int value)
    {
        node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(knob)->setValue(value);
    }

    static void render(const NodePtr& node,
                       const RectI& window,
                       RenderedPlane* plane)
    {
        std::string error;

        ASSERT_TRUE(renderRawTestPlane(node, window, plane, &error)) << error;
    }

    // A window placed from the top-left of rod, in the top-down rows LMMSE lays its tiles in.
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
                                             const char* what)
    {
        RenderedPlane full;
        ASSERT_NO_FATAL_FAILURE(render(node, rod, &full));
        const int w = rod.width(), h = rod.height();
        // LMMSE writes rows and columns [112t + 8, 112t + 120) of tile t, and the first and last
        // tiles of the image differ from the inner ones.
        const RectI tiles[] = {
            fromTopLeft(rod, 2, 2, 9, 7),
            fromTopLeft(rod, 115, 117, 10, 6),
            fromTopLeft(rod, 229, 231, 6, 5),
            fromTopLeft(rod, 341, 118, 8, 120),
            fromTopLeft(rod, 119, 230, 1, 1),
            fromTopLeft(rod, 120, 120, 1, 1),
            fromTopLeft(rod, 40, 200, 70, 50),
            fromTopLeft(rod, w - 13, h - 6, 13, 6),
            fromTopLeft(rod, w - 60, 0, 60, 3),
        };
        for (const RectI& tile : tiles) {
            // A window outside the image renders nothing.
            ASSERT_TRUE(rod.contains(tile)) << what << ": tile " << tile.x1 << "," << tile.y1 << "-" << tile.x2 << "," << tile.y2 << " is outside the image";
            RenderedPlane plane;
            ASSERT_NO_FATAL_FAILURE(render(node, tile, &plane));
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

TEST_F(NativeRawDemosaicLmmseTest, TilesMatchTheFullFrameAtEveryRefineLevel)
{
    const RectI rod(-3, 5, 397, 365);
    const struct {
        const char* pattern;
        int originX, originY;
    } phases[] = { { "RGGB", 0, 0 }, { "GBRG", 1, -2 } };
    for (const auto& phase : phases) {
        NodePtr node = createChain(rod, phase.pattern, phase.originX, phase.originY, texturedScene);
        ASSERT_TRUE(bool(node));
        for (int level : kRefineLevels) {
            setChoice(node, kRawDemosaicParamLmmseRefine, level);
            ASSERT_NO_FATAL_FAILURE(expectTilesMatchTheFullFrame(node, rod, (std::string(phase.pattern) + " refine " + std::to_string(level)).c_str()));
        }
    }
}

TEST_F(NativeRawDemosaicLmmseTest, TilesMatchTheFullFrameWithLocalGreensAndSmoothing)
{
    const RectI rod(0, 0, 360, 250);
    NodePtr node = createChain(rod, "GRBG", 0, 0, texturedScene);
    ASSERT_TRUE(bool(node));
    setChoice(node, kRawDemosaicParamLmmseRefine, eRawDemosaicLmmseRefineMedians);
    setChoice(node, kRawDemosaicParamGreenEq, eRawDemosaicGreenEqLocal);
    node->getEffectInstance()->getKnobByNameAndType<KnobInt>(kRawDemosaicParamColorSmoothing)->setValue(2);
    ASSERT_NO_FATAL_FAILURE(expectTilesMatchTheFullFrame(node, rod, "local greens, 2 smoothing passes"));
}

// darktable's tiles read zeros past the image edge, so the photosites just inside its 4-photosite
// PPG frame are not neutral; everything 8 photosites in is.
TEST_F(NativeRawDemosaicLmmseTest, AFlatFieldComesOutNeutral)
{
    const RectI rod(0, 0, 260, 240);
    const float value = 0.37f;
    for (const char* pattern : { "RGGB", "GBRG" }) {
        NodePtr node = createChain(rod, pattern, 0, 0, [value](int, int, int) { return value; });
        ASSERT_TRUE(bool(node));
        for (int level : kRefineLevels) {
            setChoice(node, kRawDemosaicParamLmmseRefine, level);
            RenderedPlane plane;
            ASSERT_NO_FATAL_FAILURE(render(node, rod, &plane));
            for (int y = rod.y1 + 8; y < rod.y2 - 8; ++y) {
                for (int x = rod.x1 + 8; x < rod.x2 - 8; ++x) {
                    for (int c = 0; c < 3; ++c) {
                        ASSERT_NEAR(value, rawTestPlaneValue(plane, x, y, c), 1e-4)
                            << pattern << " refine " << level << " at " << x << "," << y << " channel " << c;
                    }
                }
            }
        }
    }
}

TEST_F(NativeRawDemosaicLmmseTest, RefineShowsOnlyForLmmse)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWDEMOSAIC));
    ASSERT_TRUE(bool(node));
    EffectInstancePtr effect = node->getEffectInstance();
    KnobChoicePtr refine = effect->getKnobByNameAndType<KnobChoice>(kRawDemosaicParamLmmseRefine);
    ASSERT_TRUE(bool(refine));

    EXPECT_EQ((int)eRawDemosaicLmmseMedian, refine->getValue());
    EXPECT_TRUE(refine->getIsSecret());

    setChoice(node, kRawDemosaicParamBayerMethod, eRawDemosaicBayerLMMSE);
    EXPECT_FALSE(refine->getIsSecret());

    setChoice(node, kRawDemosaicParamBayerMethod, eRawDemosaicBayerAMaZE);
    EXPECT_TRUE(refine->getIsSecret());
}
