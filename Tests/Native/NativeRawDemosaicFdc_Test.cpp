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
} // namespace

class NativeRawDemosaicFdcTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRawTestSourceOnce();
        BaseTest::SetUp();
    }

    // A source of the scene through pattern, unconnected.
    NodePtr createSource(const RectI& rod,
                         const char* pattern,
                         int originX,
                         int originY,
                         double iso,
                         const RawTestSource::ValueFn& scene)
    {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));

        EXPECT_TRUE(bool(source));
        if (!source) {
            return NodePtr();
        }
        setSourceRaw(source, rod, pattern, originX, originY, iso, scene);

        return source;
    }

    static void setSourceRaw(const NodePtr& source,
                             const RectI& rod,
                             const char* pattern,
                             int originX,
                             int originY,
                             double iso,
                             const RawTestSource::ValueFn& scene)
    {
        ImageMetadata metadata = rawTestMetadata(pattern, originX, originY, eRawStagePrepared);
        if (iso > 0.) {
            RawMetadata::setIso(&metadata, iso);
        }
        const RawLayout layout = *RawLayout::fromMetadata(metadata);
        RawTestSource::ValueFn mosaic = [scene, layout](int x, int y, int) {
            return scene(x, y, layout.colourAt(x, y));
        };
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(rod, 1, mosaic, metadata);
    }

    NodePtr createFdc(const NodePtr& source)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWDEMOSAIC));

        EXPECT_TRUE(source && node);
        if (!source || !node) {
            return NodePtr();
        }
        connectNodes(source, node, 0, true);
        setChoice(node, kRawDemosaicParamXTransMethod, eRawDemosaicXTransFDC);

        return node;
    }

    static void setChoice(const NodePtr& node,
                          const char* knob,
                          int value)
    {
        node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(knob)->setValue(value);
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
};

// FDC's tiles are 122 photosites laid every 96 from 13 before the image's top-left: the windows
// sit on and around those seams, at the corners and edges, and in long strips.
TEST_F(NativeRawDemosaicFdcTest, TilesMatchTheFullFrame)
{
    const RectI rod(-3, 5, 357, 305);
    const int w = rod.width(), h = rod.height();
    const RectI tiles[] = {
        fromTopLeft(rod, 0, 0, 9, 7),
        fromTopLeft(rod, 3, 2, 5, 40),
        fromTopLeft(rod, 93, 94, 6, 5),
        fromTopLeft(rod, 191, 190, 3, 4),
        fromTopLeft(rod, 95, 96, 1, 1),
        fromTopLeft(rod, 40, 200, 70, 50),
        fromTopLeft(rod, 180, 30, 120, 4),
        fromTopLeft(rod, 280, 286, 9, 3),
        fromTopLeft(rod, w - 13, h - 6, 13, 6),
        fromTopLeft(rod, w - 60, 0, 60, 3),
        fromTopLeft(rod, 0, h - 1, w, 1),
    };
    RenderedPlane fulls[2];
    int index = 0;
    // Up to ISO 1600 the chroma is the hybrid, above it FDC's alone.
    for (double iso : { 400., 3200. }) {
        NodePtr source = createSource(rod, kXTransPattern, 1, -2, iso, texturedScene);
        NodePtr node = createFdc(source);
        ASSERT_TRUE(bool(node));
        const RenderedPlane full = render(node, rod);
        for (const RectI& tile : tiles) {
            const RenderedPlane plane = render(node, tile);
            for (int y = tile.y1; y < tile.y2; ++y) {
                for (int x = tile.x1; x < tile.x2; ++x) {
                    for (int c = 0; c < 4; ++c) {
                        ASSERT_EQ(rawTestPlaneValue(full, x, y, c), rawTestPlaneValue(plane, x, y, c))
                            << "ISO " << iso << " at " << x << "," << y << " channel " << c << " in tile " << tile.x1 << "," << tile.y1 << "-" << tile.x2 << "," << tile.y2;
                    }
                }
            }
        }
        fulls[index++] = full;
    }

    int differing = 0;
    for (int y = rod.y1; y < rod.y2; ++y) {
        for (int x = rod.x1; x < rod.x2; ++x) {
            differing += (rawTestPlaneValue(fulls[0], x, y, 0) != rawTestPlaneValue(fulls[1], x, y, 0)) ? 1 : 0;
        }
    }
    EXPECT_GT(differing, 0) << "the ISO does not reach the chroma";
}

// The hybrid keeps a flat field to float rounding. Pure FDC's 13x13 filters are tabulated to 7
// digits and do not sum to exactly 0 on a flat field. Measured over all 36 phases of a 50x40
// patch at 0.35, darktable's own C and this port agree bit for bit: hybrid (ISO 400) deviates by
// at most 2.98e-8, pure FDC (ISO 3200) by at most 1.2089e-3. The pure tolerance is that x 1.5.
TEST_F(NativeRawDemosaicFdcTest, AFlatFieldStaysNeutralAtEveryPhase)
{
    const RectI rod(0, 0, 50, 40);
    const float grey = 0.35f;
    RawTestSource::ValueFn flat = [grey](int, int, int) {
        return grey;
    };
    struct Mode {
        double iso;
        float tolerance;
    };
    const Mode modes[] = { { 400., 1e-6f }, { 3200., 1.82e-3f } };
    for (const Mode& mode : modes) {
        NodePtr source = createSource(rod, kXTransPattern, 0, 0, mode.iso, flat);
        NodePtr node = createFdc(source);
        ASSERT_TRUE(bool(node));
        for (int phase = 0; phase < 36; ++phase) {
            disconnectNodes(source, node, true);
            setSourceRaw(source, rod, kXTransPattern, phase % 6, phase / 6, mode.iso, flat);
            connectNodes(source, node, 0, true);
            const RenderedPlane plane = render(node, rod);
            for (int y = rod.y1; y < rod.y2; ++y) {
                for (int x = rod.x1; x < rod.x2; ++x) {
                    for (int c = 0; c < 3; ++c) {
                        ASSERT_NEAR(grey, rawTestPlaneValue(plane, x, y, c), mode.tolerance)
                            << "ISO " << mode.iso << " phase " << phase << " at " << x << "," << y << " channel " << c;
                    }
                    ASSERT_EQ(1.f, rawTestPlaneValue(plane, x, y, 3));
                }
            }
        }
    }
}

// darktable's gui_changed(): the source's sensor picks the method list and the knobs its method
// uses; with no source both lists show.
TEST_F(NativeRawDemosaicFdcTest, KnobsFollowTheSourceSensor)
{
    const RectI rod(0, 0, 40, 40);
    NodePtr bayerSource = createSource(rod, "RGGB", 0, 0, 0., texturedScene);
    NodePtr xtransSource = createSource(rod, kXTransPattern, 0, 0, 0., texturedScene);
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWDEMOSAIC));
    ASSERT_TRUE(bayerSource && xtransSource && node);
    EffectInstancePtr effect = node->getEffectInstance();
    KnobChoicePtr bayerMethod = effect->getKnobByNameAndType<KnobChoice>(kRawDemosaicParamBayerMethod);
    KnobChoicePtr xtransMethod = effect->getKnobByNameAndType<KnobChoice>(kRawDemosaicParamXTransMethod);
    KnobChoicePtr greenEq = effect->getKnobByNameAndType<KnobChoice>(kRawDemosaicParamGreenEq);
    KnobDoublePtr edgeThreshold = effect->getKnobByNameAndType<KnobDouble>(kRawDemosaicParamEdgeThreshold);
    KnobIntPtr colorSmoothing = effect->getKnobByNameAndType<KnobInt>(kRawDemosaicParamColorSmoothing);
    KnobChoicePtr lmmseRefine = effect->getKnobByNameAndType<KnobChoice>(kRawDemosaicParamLmmseRefine);
    KnobDoublePtr dualThreshold = effect->getKnobByNameAndType<KnobDouble>(kRawDemosaicParamDualThreshold);
    ASSERT_TRUE(bayerMethod && xtransMethod && greenEq && edgeThreshold && colorSmoothing && lmmseRefine && dualThreshold);

    // No source: as before the sensor was known.
    EXPECT_FALSE(bayerMethod->getIsSecret());
    EXPECT_FALSE(xtransMethod->getIsSecret());
    EXPECT_FALSE(greenEq->getIsSecret());
    EXPECT_FALSE(edgeThreshold->getIsSecret());
    EXPECT_FALSE(colorSmoothing->getIsSecret());
    EXPECT_TRUE(lmmseRefine->getIsSecret());
    EXPECT_TRUE(dualThreshold->getIsSecret());
    setChoice(node, kRawDemosaicParamXTransMethod, eRawDemosaicXTransMarkesteijn3Dual);
    EXPECT_FALSE(dualThreshold->getIsSecret());
    EXPECT_FALSE(colorSmoothing->getIsSecret());
    setChoice(node, kRawDemosaicParamXTransMethod, eRawDemosaicXTransMarkesteijn1);

    connectNodes(bayerSource, node, 0, true);
    EXPECT_FALSE(bayerMethod->getIsSecret());
    EXPECT_TRUE(xtransMethod->getIsSecret());
    EXPECT_FALSE(greenEq->getIsSecret());
    EXPECT_TRUE(edgeThreshold->getIsSecret());
    EXPECT_FALSE(colorSmoothing->getIsSecret());
    EXPECT_TRUE(lmmseRefine->getIsSecret());
    EXPECT_TRUE(dualThreshold->getIsSecret());
    setChoice(node, kRawDemosaicParamBayerMethod, eRawDemosaicBayerPPG);
    EXPECT_FALSE(edgeThreshold->getIsSecret());
    setChoice(node, kRawDemosaicParamBayerMethod, eRawDemosaicBayerLMMSE);
    EXPECT_FALSE(lmmseRefine->getIsSecret());
    EXPECT_TRUE(edgeThreshold->getIsSecret());
    setChoice(node, kRawDemosaicParamBayerMethod, eRawDemosaicBayerRCDDual);
    EXPECT_FALSE(dualThreshold->getIsSecret());
    EXPECT_TRUE(colorSmoothing->getIsSecret());
    EXPECT_TRUE(lmmseRefine->getIsSecret());
    setChoice(node, kRawDemosaicParamBayerMethod, eRawDemosaicBayerPassthroughMonochrome);
    EXPECT_TRUE(dualThreshold->getIsSecret());
    EXPECT_TRUE(colorSmoothing->getIsSecret());
    EXPECT_TRUE(greenEq->getIsSecret());
    // A dual X-Trans method means nothing to a Bayer source.
    setChoice(node, kRawDemosaicParamBayerMethod, eRawDemosaicBayerRCD);
    setChoice(node, kRawDemosaicParamXTransMethod, eRawDemosaicXTransMarkesteijn3Dual);
    EXPECT_TRUE(dualThreshold->getIsSecret());
    EXPECT_FALSE(colorSmoothing->getIsSecret());

    disconnectNodes(bayerSource, node, true);
    connectNodes(xtransSource, node, 0, true);
    EXPECT_TRUE(bayerMethod->getIsSecret());
    EXPECT_FALSE(xtransMethod->getIsSecret());
    EXPECT_TRUE(greenEq->getIsSecret());
    EXPECT_TRUE(edgeThreshold->getIsSecret());
    EXPECT_TRUE(lmmseRefine->getIsSecret());
    EXPECT_FALSE(dualThreshold->getIsSecret());
    EXPECT_TRUE(colorSmoothing->getIsSecret());
    setChoice(node, kRawDemosaicParamXTransMethod, eRawDemosaicXTransFDC);
    EXPECT_TRUE(dualThreshold->getIsSecret());
    EXPECT_FALSE(colorSmoothing->getIsSecret());
    // A Bayer method means nothing to an X-Trans source.
    setChoice(node, kRawDemosaicParamBayerMethod, eRawDemosaicBayerLMMSE);
    EXPECT_TRUE(lmmseRefine->getIsSecret());
    setChoice(node, kRawDemosaicParamXTransMethod, eRawDemosaicXTransPhotositeColor);
    EXPECT_TRUE(colorSmoothing->getIsSecret());

    disconnectNodes(xtransSource, node, true);
    EXPECT_FALSE(bayerMethod->getIsSecret());
    EXPECT_FALSE(xtransMethod->getIsSecret());
    EXPECT_FALSE(greenEq->getIsSecret());
    EXPECT_FALSE(edgeThreshold->getIsSecret());
    EXPECT_FALSE(colorSmoothing->getIsSecret());
    EXPECT_FALSE(lmmseRefine->getIsSecret());
}
