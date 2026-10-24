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
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include <ofxImageEffect.h>

#include "BaseTest.h"
#include "NativePluginIdentity.h"
#include "RawFixtures.h"
#include "RawTestSource.h"

#include "Engine/EffectInstance.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/IO/NativeRead.h"
#include "Engine/Nodes/IO/RawMetadata.h"
#include "Engine/Nodes/Raw/RawDemosaic.h"
#include "Engine/Nodes/Raw/RawHighlights.h"
#include "Engine/Nodes/Raw/RawKernels.h"
#include "Engine/Nodes/Raw/RawNodeSupport.h"
#include "Engine/Nodes/Raw/RawPrepare.h"
#include "Engine/Nodes/Raw/RawTemperature.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kXTransPattern = "GGRGGB"
                                   "GGBGGR"
                                   "BRGRBG"
                                   "GGBGGR"
                                   "GGRGGB"
                                   "RBGBRG";

// A textured scene: blocks, rings and fine stripes, with a few clipped photosites.
float
texturedScene(int x,
              int y,
              int c)
{
    if ((((x * 7919) ^ (y * 104729)) % 211) == 0) {
        return 1.3f;
    }
    const float r2 = ((float)x - 41.f) * ((float)x - 41.f) + ((float)y - 33.f) * ((float)y - 33.f);
    const float block = ((((x / 11) + (y / 9)) & 1) != 0) ? 0.25f : 0.f;
    switch (c) {
    case 0:
        return 0.3f + 0.2f * std::cos(r2 * 0.003f) + block;
    case 1:
        return 0.4f + 0.2f * std::sin((float)x * 0.9f) * std::cos((float)y * 0.7f) + block;
    default:
        return 0.25f + 0.003f * (float)(x * y % 89) + block;
    }
}

// texturedScene through a Gaussian of sigma 0.8 photosites, as a lens would blur it.
float
blurredScene(int x,
             int y,
             int c)
{
    float sum = 0.f;
    float weights = 0.f;
    for (int dy = -3; dy <= 3; ++dy) {
        for (int dx = -3; dx <= 3; ++dx) {
            const float w = std::exp(-(float)(dx * dx + dy * dy) / (2.f * 0.64f));
            sum += w * std::min(1.f, texturedScene(x + dx, y + dy, c));
            weights += w;
        }
    }

    return sum / weights;
}
} // namespace

class NativeRawDemosaicCaptureTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        registerRawTestSourceOnce();
        BaseTest::SetUp();
    }

    // A RawDemosaic under a source of scene through pattern (kRawCfaPatternNone for one channel
    // without a CFA), with capture sharpening on.
    NodePtr createChain(const RectI& rod,
                        const char* pattern,
                        double iso,
                        const RawTestSource::ValueFn& scene,
                        NodePtr* sourceOut = NULL)
    {
        NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWDEMOSAIC));
        EXPECT_TRUE(source && node);
        if (!source || !node) {
            return NodePtr();
        }
        ImageMetadata metadata = rawTestMetadata(pattern, 0, 0, eRawStagePrepared);
        if (iso > 0.) {
            RawMetadata::setIso(&metadata, iso);
        }
        const std::optional<RawLayout> layout = RawLayout::fromMetadata(metadata);
        RawTestSource::ValueFn mosaic = [scene, layout](int x, int y, int) {
            return scene(x, y, (layout && (layout->period() > 0)) ? layout->colourAt(x, y) : 1);
        };
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(rod, 1, mosaic, metadata);
        connectNodes(source, node, 0, true);
        boolKnob(node, kRawDemosaicParamCaptureSharpen)->setValue(true);
        if (sourceOut) {
            *sourceOut = source;
        }

        return node;
    }

    static KnobBoolPtr boolKnob(const NodePtr& node,
                                const char* name)
    {
        return node->getEffectInstance()->getKnobByNameAndType<KnobBool>(name);
    }

    static KnobDoublePtr doubleKnob(const NodePtr& node,
                                    const char* name)
    {
        return node->getEffectInstance()->getKnobByNameAndType<KnobDouble>(name);
    }

    static KnobIntPtr intKnob(const NodePtr& node,
                              const char* name)
    {
        return node->getEffectInstance()->getKnobByNameAndType<KnobInt>(name);
    }

    static RawDemosaic* demosaicOf(const NodePtr& node)
    {
        return dynamic_cast<RawDemosaic*>(node->getEffectInstance().get());
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

    static void expectTilesMatch(const NodePtr& node,
                                 const RectI& rod,
                                 const char* what)
    {
        const int w = rod.width(), h = rod.height();
        const RectI tiles[] = {
            fromTopLeft(rod, 0, 0, 9, 7),
            fromTopLeft(rod, 40, 30, 17, 13),
            fromTopLeft(rod, 75, 61, 1, 1),
            fromTopLeft(rod, 20, 90, 60, 4),
            fromTopLeft(rod, w - 11, h - 6, 11, 6),
            fromTopLeft(rod, 0, h - 1, w, 1),
        };
        const RenderedPlane full = render(node, rod);
        int sharpened = 0;
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
        boolKnob(node, kRawDemosaicParamCaptureSharpen)->setValue(false);
        const RenderedPlane plain = render(node, rod);
        boolKnob(node, kRawDemosaicParamCaptureSharpen)->setValue(true);
        for (int y = rod.y1; y < rod.y2; ++y) {
            for (int x = rod.x1; x < rod.x2; ++x) {
                sharpened += (rawTestPlaneValue(full, x, y, 1) != rawTestPlaneValue(plain, x, y, 1)) ? 1 : 0;
            }
        }
        EXPECT_GT(sharpened, w * h / 4) << what << ": capture sharpening changes too little";
    }
};

TEST_F(NativeRawDemosaicCaptureTest, KnobDefaultsAndVisibility)
{
    const RectI rod(0, 0, 64, 48);
    NodePtr node = createChain(rod, "RGGB", 100., texturedScene);
    ASSERT_TRUE(bool(node));
    KnobBoolPtr enabled = boolKnob(node, kRawDemosaicParamCaptureSharpen);
    KnobIntPtr iterations = intKnob(node, kRawDemosaicParamCaptureIterations);
    KnobDoublePtr radius = doubleKnob(node, kRawDemosaicParamCaptureRadius);
    KnobDoublePtr threshold = doubleKnob(node, kRawDemosaicParamCaptureThreshold);
    KnobBoolPtr thresholdOverride = boolKnob(node, kRawDemosaicParamCaptureThresholdOverride);
    KnobDoublePtr boost = doubleKnob(node, kRawDemosaicParamCaptureBoost);
    KnobDoublePtr center = doubleKnob(node, kRawDemosaicParamCaptureCenter);
    ASSERT_TRUE(enabled && iterations && radius && threshold && thresholdOverride && boost && center);

    EXPECT_FALSE(enabled->getDefaultValue(0));
    EXPECT_EQ(8, iterations->getDefaultValue(0));
    EXPECT_EQ(1, iterations->getMinimum(0));
    EXPECT_EQ(25, iterations->getMaximum(0));
    EXPECT_EQ(0., radius->getDefaultValue(0));
    EXPECT_EQ(1.5, radius->getMaximum(0));
    EXPECT_EQ(1., threshold->getMaximum(0));
    EXPECT_FALSE(thresholdOverride->getDefaultValue(0));
    EXPECT_EQ(0., boost->getDefaultValue(0));
    EXPECT_EQ(1.5, boost->getMaximum(0));
    EXPECT_EQ(0., center->getDefaultValue(0));
    EXPECT_EQ(1., center->getMaximum(0));

    // ISO 100 and no sensor white level: 0.4 - 0.012 * 5, shown and not editable.
    EXPECT_NEAR(0.34, threshold->getValue(), 1e-6);
    EXPECT_FLOAT_EQ(0.34f, RawKernels::captureVarianceThreshold(0., 100.));
    EXPECT_FLOAT_EQ(0.27f, RawKernels::captureVarianceThreshold(16383., 100.));
    EXPECT_FLOAT_EQ(0.448f, RawKernels::captureVarianceThreshold(4095., 3200.));
    EXPECT_FALSE(threshold->isEnabled(0));
    thresholdOverride->setValue(true);
    EXPECT_TRUE(threshold->isEnabled(0));
    thresholdOverride->setValue(false);
    EXPECT_FALSE(threshold->isEnabled(0));

    EXPECT_EQ(1, RawKernels::captureIterations(1));
    EXPECT_EQ(8, RawKernels::captureIterations(8));
    EXPECT_EQ(50, RawKernels::captureIterations(25));

    // On: every control but Sharp Center, which waits for Corner Boost.
    EXPECT_FALSE(enabled->getIsSecret());
    EXPECT_FALSE(iterations->getIsSecret());
    EXPECT_FALSE(radius->getIsSecret());
    EXPECT_FALSE(threshold->getIsSecret());
    EXPECT_FALSE(boost->getIsSecret());
    EXPECT_TRUE(center->getIsSecret());
    boost->setValue(0.5);
    EXPECT_FALSE(center->getIsSecret());
    enabled->setValue(false);
    EXPECT_FALSE(enabled->getIsSecret());
    EXPECT_TRUE(iterations->getIsSecret());
    EXPECT_TRUE(radius->getIsSecret());
    EXPECT_TRUE(threshold->getIsSecret());
    EXPECT_TRUE(thresholdOverride->getIsSecret());
    EXPECT_TRUE(boost->getIsSecret());
    EXPECT_TRUE(center->getIsSecret());
    enabled->setValue(true);

    // The photosite modes have no capture sharpening.
    node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawDemosaicParamBayerMethod)->setValue((int)eRawDemosaicBayerPhotositeColor);
    EXPECT_TRUE(enabled->getIsSecret());
    EXPECT_TRUE(iterations->getIsSecret());
    node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawDemosaicParamBayerMethod)->setValue((int)eRawDemosaicBayerRCD);
    EXPECT_FALSE(enabled->getIsSecret());

    // One channel without a CFA is darktable's true monochrome; three are not sharpened.
    NodePtr mono = createChain(rod, kRawCfaPatternNone, 100., texturedScene);
    ASSERT_TRUE(bool(mono));
    EXPECT_FALSE(boolKnob(mono, kRawDemosaicParamCaptureSharpen)->getIsSecret());
    NodePtr source = createNode(QString::fromUtf8(kRawTestSourceID));
    NodePtr linear = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWDEMOSAIC));
    ASSERT_TRUE(source && linear);
    dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(rod, 3, [](int, int, int) { return 0.5f; }, rawTestMetadata(kRawCfaPatternNone, 0, 0, eRawStagePrepared));
    connectNodes(source, linear, 0, true);
    EXPECT_TRUE(boolKnob(linear, kRawDemosaicParamCaptureSharpen)->getIsSecret());
} // TEST_F

// Off is the plain demosaic, bit for bit, whatever the other capture controls hold.
TEST_F(NativeRawDemosaicCaptureTest, OffIsBypass)
{
    const RectI rod(0, 0, 72, 56);
    NodePtr plain = createChain(rod, "GRBG", 400., texturedScene);
    NodePtr off = createChain(rod, "GRBG", 400., texturedScene);
    ASSERT_TRUE(plain && off);
    boolKnob(plain, kRawDemosaicParamCaptureSharpen)->setValue(false);
    boolKnob(off, kRawDemosaicParamCaptureSharpen)->setValue(false);
    intKnob(off, kRawDemosaicParamCaptureIterations)->setValue(25);
    doubleKnob(off, kRawDemosaicParamCaptureRadius)->setValue(1.2);
    doubleKnob(off, kRawDemosaicParamCaptureBoost)->setValue(1.);
    doubleKnob(off, kRawDemosaicParamCaptureCenter)->setValue(0.5);
    boolKnob(off, kRawDemosaicParamCaptureThresholdOverride)->setValue(true);
    doubleKnob(off, kRawDemosaicParamCaptureThreshold)->setValue(0.05);
    const RenderedPlane a = render(plain, rod);
    const RenderedPlane b = render(off, rod);
    for (int y = rod.y1; y < rod.y2; ++y) {
        for (int x = rod.x1; x < rod.x2; ++x) {
            for (int c = 0; c < 4; ++c) {
                ASSERT_EQ(rawTestPlaneValue(a, x, y, c), rawTestPlaneValue(b, x, y, c)) << x << "," << y << " channel " << c;
            }
        }
    }
    EXPECT_FALSE(demosaicOf(off)->lastAutoCaptureRadius());
}

TEST_F(NativeRawDemosaicCaptureTest, TilesMatchTheFullFrameWithAFixedRadius)
{
    const RectI rod(-5, 3, 147, 121);
    struct Case {
        const char* pattern;
        double radius;
        double boost;
    };
    const Case cases[] = {
        { "RGGB", 0.4, 0. },
        { "GBRG", 1.1, 0.8 },
        { kXTransPattern, 0.7, 0.4 },
        { kRawCfaPatternNone, 0.9, 0. },
    };
    for (const Case& k : cases) {
        NodePtr node = createChain(rod, k.pattern, 800., blurredScene);
        ASSERT_TRUE(bool(node));
        doubleKnob(node, kRawDemosaicParamCaptureRadius)->setValue(k.radius);
        doubleKnob(node, kRawDemosaicParamCaptureBoost)->setValue(k.boost);
        doubleKnob(node, kRawDemosaicParamCaptureCenter)->setValue(0.3);
        expectTilesMatch(node, rod, k.pattern);
        EXPECT_FALSE(demosaicOf(node)->lastAutoCaptureRadius()) << k.pattern << ": a fixed radius measures nothing";
    }
}

TEST_F(NativeRawDemosaicCaptureTest, TilesMatchTheFullFrameWithTheAutomaticRadius)
{
    const RectI rod(-5, 3, 147, 121);
    for (const char* pattern : { "RGGB", kXTransPattern, kRawCfaPatternNone }) {
        NodePtr node = createChain(rod, pattern, 800., blurredScene);
        ASSERT_TRUE(bool(node));
        intKnob(node, kRawDemosaicParamCaptureIterations)->setValue(4);
        expectTilesMatch(node, rod, pattern);
        const std::optional<double> radius = demosaicOf(node)->lastAutoCaptureRadius();
        ASSERT_TRUE(bool(radius)) << pattern;
        EXPECT_GT(*radius, 0.) << pattern;
        EXPECT_LE(*radius, 1.5) << pattern;
    }
}

// The automatic radius is the frame's, whatever window asks for it, and is
// _calc_auto_radius() of the frame's centre.
TEST_F(NativeRawDemosaicCaptureTest, TheAutomaticRadiusIsDeterministic)
{
    const RectI rod(0, 0, 200, 150);
    for (const char* pattern : { "RGGB", kXTransPattern }) {
        NodePtr node = createChain(rod, pattern, 800., blurredScene);
        ASSERT_TRUE(bool(node));
        RawDemosaic* demosaic = demosaicOf(node);
        (void)render(node, fromTopLeft(rod, 0, 0, 8, 8));
        const std::optional<double> corner = demosaic->lastAutoCaptureRadius();
        // A different window under a different render hash measures again.
        doubleKnob(node, kRawDemosaicParamCaptureBoost)->setValue(0.1);
        (void)render(node, fromTopLeft(rod, 150, 120, 20, 20));
        const std::optional<double> other = demosaic->lastAutoCaptureRadius();
        ASSERT_TRUE(corner && other) << pattern;
        EXPECT_EQ(*corner, *other) << pattern;

        const RawKernels::CaptureRadiusArea area = RawKernels::captureRadiusArea(rod.width(), rod.height());
        ASSERT_TRUE(area.reliable);
        const RawLayout layout = *RawLayout::fromPattern(pattern);
        std::vector<float> values((std::size_t)area.width * area.height);
        for (int r = 0; r < area.height; ++r) {
            for (int c = 0; c < area.width; ++c) {
                const int x = rod.x1 + area.x + c;
                const int y = rod.y2 - 1 - area.y - r;
                values[(std::size_t)r * area.width + c] = blurredScene(x, y, layout.colourAt(x, y));
            }
        }
        const std::uint32_t filters = (layout.period() == 2) ? RawNodeSupport::bayerFiltersAt(layout, rod) : RawKernels::kFiltersXTrans;
        std::uint8_t xtrans[6][6] = {};
        if (layout.period() == 6) {
            RawNodeSupport::xtransAt(layout, rod, xtrans);
        }
        const float ones[3] = { 1.f, 1.f, 1.f };
        const float expected = RawKernels::captureAutoRadius(values.data(), area.width, area.height, filters, xtrans, ones, RawKernels::ParallelFor(&RawKernels::serialFor));
        EXPECT_EQ((double)expected, *corner) << pattern;
    }
}

// darktable gives the CR2 fixture an automatic radius of 0.61 and the RAF one 0.86.
TEST_F(NativeRawDemosaicCaptureTest, TheFixturesGiveDarktablesAutomaticRadius)
{
    struct Expectation {
        const RawFixture& fixture;
        double radius;
    };
    const Expectation expectations[] = { { bayerFixture(), 0.61 }, { xtransFixture(), 0.86 } };
    for (const Expectation& e : expectations) {
        NodePtr read = createNode(QString::fromUtf8(PLUGINID_NATRON_READ), PLUGIN_MAJOR_NATRON_READ);
        ASSERT_TRUE(bool(read));
        KnobFile* file = dynamic_cast<KnobFile*>(read->getKnobByName(kOfxImageEffectFileParamName).get());
        ASSERT_TRUE(file != NULL);
        file->setValue(e.fixture.path());
        NodePtr input = read;
        NodePtr demosaic;
        for (const char* id : { PLUGINID_NATRON_RAWPREPARE, PLUGINID_NATRON_RAWTEMPERATURE, PLUGINID_NATRON_RAWHIGHLIGHTS, PLUGINID_NATRON_RAWDEMOSAIC }) {
            NodePtr node = createNode(QString::fromUtf8(id));
            ASSERT_TRUE(bool(node)) << id;
            connectNodes(input, node, 0, true);
            input = node;
            demosaic = node;
        }
        boolKnob(demosaic, kRawDemosaicParamCaptureSharpen)->setValue(true);
        RectI rod;
        ASSERT_TRUE(RawNodeSupport::getInputPixelRoD(*demosaic->getEffectInstance(), 0, 0., ViewIdx(0), &rod));
        // A small window: the radius measures the frame's centre whatever the window.
        (void)render(demosaic, fromTopLeft(rod, rod.width() / 2, rod.height() / 2, 16, 16));
        const std::optional<double> radius = demosaicOf(demosaic)->lastAutoCaptureRadius();
        ASSERT_TRUE(bool(radius)) << e.fixture.file;
        EXPECT_NEAR(e.radius, *radius, 1e-2) << e.fixture.file;
    }
}

// The automatic radius and the full green equilibration ratio are kept across renders of the
// frame, and Clear Caches drops them, so the next render measures them again; the radius last
// measured is still reported in between.
TEST_F(NativeRawDemosaicCaptureTest, ClearCachesDropsTheFrameMeasurements)
{
    const RectI rod(0, 0, 200, 150);
    NodePtr node = createChain(rod, "RGGB", 800., blurredScene);
    ASSERT_TRUE(bool(node));
    KnobChoicePtr greenEq = node->getEffectInstance()->getKnobByNameAndType<KnobChoice>(kRawDemosaicParamGreenEq);
    ASSERT_TRUE(bool(greenEq));
    greenEq->setValue((int)eRawDemosaicGreenEqFull);

    const FrameComputedCounter computed;
    // Emptying the node cache makes each window render, and leaves the node's own results.
    const auto renderWindow = [&node](const RectI& window) {
        appPTR->clearNodeCache();
        RenderedPlane plane;
        std::string error;
        EXPECT_TRUE(renderRawTestPlane(node, window, &plane, &error, false)) << error;
    };
    renderWindow(fromTopLeft(rod, 0, 0, 16, 16));
    renderWindow(fromTopLeft(rod, 150, 120, 20, 20));
    EXPECT_EQ(1, computed.count(kRawDemosaicFrameCaptureRadius));
    EXPECT_EQ(1, computed.count(kRawDemosaicFrameGreenRatio));
    const std::optional<double> radius = demosaicOf(node)->lastAutoCaptureRadius();
    ASSERT_TRUE(bool(radius));

    node->purgeAllInstancesCaches();
    EXPECT_EQ(radius, demosaicOf(node)->lastAutoCaptureRadius());
    renderWindow(fromTopLeft(rod, 80, 60, 16, 16));
    EXPECT_EQ(2, computed.count(kRawDemosaicFrameCaptureRadius));
    EXPECT_EQ(2, computed.count(kRawDemosaicFrameGreenRatio));
    EXPECT_EQ(radius, demosaicOf(node)->lastAutoCaptureRadius());
}
