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
#include <functional>
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

using SceneFn = std::function<void(int x, int y, float rgb[3])>;

void
smoothScene(int x,
            int y,
            float rgb[3])
{
    rgb[0] = 0.2f + 0.003f * (float)x;
    rgb[1] = 0.3f + 0.002f * (float)y;
    rgb[2] = 0.25f + 0.001f * (float)(x + y);
}

// A grey Siemens star of 24 spokes around (48, 48): hard edges at every orientation.
void
starScene(int x,
          int y,
          float rgb[3])
{
    const float angle = std::atan2((float)y - 48.f, (float)x - 48.f);
    const float v = (std::sin(angle * 24.f) > 0.f) ? 0.9f : 0.1f;

    rgb[0] = rgb[1] = rgb[2] = v;
}

void
texturedScene(int x,
              int y,
              float rgb[3])
{
    const float r2 = ((float)x - 37.f) * ((float)x - 37.f) + ((float)y - 29.f) * ((float)y - 29.f);

    rgb[0] = 0.4f + 0.3f * std::cos(r2 * 0.02f);
    rgb[1] = 0.5f + 0.25f * std::sin((float)x * 0.4f) * std::cos((float)y * 0.3f);
    rgb[2] = 0.3f + 0.002f * (float)(x * y % 97);
}

RawTestSource::ValueFn
mosaicOf(const SceneFn& scene,
         const RawLayout& layout)
{
    return [scene, layout](int x, int y, int) {
        float rgb[3];
        scene(x, y, rgb);

        return rgb[layout.colourAt(x, y)];
    };
}

struct Method {
    const char* name;
    const char* knob;
    int value;
};

const Method kBayerMethods[] = {
    { "RCD", kRawDemosaicParamBayerMethod, eRawDemosaicBayerRCD },
    { "PPG", kRawDemosaicParamBayerMethod, eRawDemosaicBayerPPG },
    { "AMaZE", kRawDemosaicParamBayerMethod, eRawDemosaicBayerAMaZE },
    { "VNG4", kRawDemosaicParamBayerMethod, eRawDemosaicBayerVNG4 },
};

const Method kXTransMethods[] = {
    { "Markesteijn 1-pass", kRawDemosaicParamXTransMethod, eRawDemosaicXTransMarkesteijn1 },
    { "Markesteijn 3-pass", kRawDemosaicParamXTransMethod, eRawDemosaicXTransMarkesteijn3 },
    { "VNG", kRawDemosaicParamXTransMethod, eRawDemosaicXTransVNG },
};
} // namespace

class NativeRawDemosaicTest
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
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWDEMOSAIC));

        EXPECT_TRUE(source && node);
        if (!source || !node) {
            return NodePtr();
        }
        dynamic_cast<RawTestSource*>(source->getEffectInstance().get())->setRaw(rod, nComps, value, metadata);
        connectNodes(source, node, 0, true);

        return node;
    }

    NodePtr createMosaicChain(const RectI& rod,
                              const std::string& pattern,
                              int originX,
                              int originY,
                              const SceneFn& scene)
    {
        const ImageMetadata metadata = rawTestMetadata(pattern, originX, originY, eRawStagePrepared);

        return createChain(rod, 1, mosaicOf(scene, *RawLayout::fromMetadata(metadata)), metadata);
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

    // The mean and largest absolute difference of RGB to scene, at least margin photosites in.
    static void errorAgainst(const RenderedPlane& plane,
                             const SceneFn& scene,
                             int margin,
                             double* mean,
                             double* maximum)
    {
        double sum = 0.;
        int count = 0;
        *maximum = 0.;
        for (int y = plane.window.y1 + margin; y < plane.window.y2 - margin; ++y) {
            for (int x = plane.window.x1 + margin; x < plane.window.x2 - margin; ++x) {
                float rgb[3];
                scene(x, y, rgb);
                for (int c = 0; c < 3; ++c) {
                    const double d = std::fabs((double)rawTestPlaneValue(plane, x, y, c) - rgb[c]);
                    sum += d;
                    *maximum = std::max(*maximum, d);
                    ++count;
                }
            }
        }
        *mean = (count > 0) ? (sum / count) : 0.;
    }

    // Every tile renders bit for bit the pixels of the full frame, alpha 1 included.
    static void expectTilesMatchTheFullFrame(const NodePtr& node,
                                             const RectI& rod,
                                             const std::string& what)
    {
        const RenderedPlane full = render(node, rod);
        ASSERT_EQ(4u, full.channels.size()) << what;
        const RectI tiles[] = {
            RectI(rod.x1 + 21, rod.y1 + 17, rod.x1 + 52, rod.y1 + 40),
            RectI(rod.x1, rod.y1, rod.x1 + 9, rod.y1 + 7),
            RectI(rod.x2 - 13, rod.y2 - 6, rod.x2, rod.y2),
            RectI(rod.x1 + 30, rod.y1 + 30, rod.x1 + 31, rod.y1 + 31),
            RectI(rod.x1 + 3, rod.y2 - 1, rod.x1 + 4, rod.y2),
            RectI(rod.x1 + 47, rod.y1 + 2, rod.x2 - 1, rod.y1 + 25),
        };
        for (const RectI& tile : tiles) {
            const RenderedPlane plane = render(node, tile);
            ASSERT_EQ(4u, plane.channels.size()) << what;
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

TEST_F(NativeRawDemosaicTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_RAWDEMOSAIC, PLUGIN_MAJOR_NATRON_RAWDEMOSAIC, "RawDemosaic");

    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWDEMOSAIC));
    ASSERT_TRUE(bool(node));
    EXPECT_EQ(eDataKindRaw, node->getEffectInstance()->getInputDataKind(0));
    EXPECT_EQ(eDataKindImage, node->getEffectInstance()->getOutputDataKind());
}

TEST_F(NativeRawDemosaicTest, OutputsAnImageWithoutTheCfaKeys)
{
    const RectI rod(0, 0, 64, 48);
    ImageMetadata metadata = rawTestMetadata("RGGB", 0, 0, eRawStagePrepared);
    const std::array<double, 9> camToXyz = { 0.6, 0.2, 0.15, 0.3, 0.8, -0.1, 0.02, -0.15, 1.1 };
    RawMetadata::setCamToXyz(&metadata, camToXyz);
    NodePtr node = createChain(rod, 1, mosaicOf(smoothScene, *RawLayout::fromPattern("RGGB")), metadata);
    ASSERT_TRUE(bool(node));
    NodePtr imageSink = createNode(QString::fromUtf8("test.natron.built-in.DataKindImageSink"));
    if (imageSink) {
        EXPECT_EQ(Node::eCanConnectInput_ok, imageSink->canConnectInput(node, 0));
    }

    const ImageMetadata out = dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get())->getOutputMetadata(1., ViewIdx(0));
    EXPECT_FALSE(out.contains(kRawMetadataCfaPattern));
    EXPECT_FALSE(out.contains(kRawMetadataCfaOrigin));
    EXPECT_FALSE(out.contains(kRawMetadataStage));
    EXPECT_EQ(camToXyz, RawMetadata::getCamToXyz(out).value_or(std::array<double, 9> {}));
}

// A smooth scene sampled through the CFA comes back within a bound of the scene, for every
// method and Bayer phase.
TEST_F(NativeRawDemosaicTest, ASmoothGradientReconstructs)
{
    const RectI rod(0, 0, 80, 64);
    for (const char* pattern : { "RGGB", "BGGR", "GRBG", "GBRG" }) {
        for (const Method& method : kBayerMethods) {
            NodePtr node = createMosaicChain(rod, pattern, 0, 0, smoothScene);
            ASSERT_TRUE(bool(node));
            setChoice(node, method.knob, method.value);
            const RenderedPlane plane = render(node, rod);
            double mean = 0., maximum = 0.;
            errorAgainst(plane, smoothScene, 12, &mean, &maximum);
            EXPECT_LT(maximum, (method.value == eRawDemosaicBayerVNG4) ? 2e-2 : 1e-3) << method.name << " on " << pattern;
            for (int y = rod.y1; y < rod.y2; ++y) {
                for (int x = rod.x1; x < rod.x2; ++x) {
                    ASSERT_EQ(1.f, rawTestPlaneValue(plane, x, y, 3)) << method.name;
                }
            }
        }
    }
}

TEST_F(NativeRawDemosaicTest, TilesMatchTheFullFrameBayer)
{
    const RectI rod(-3, 5, 97, 85);
    for (const Method& method : kBayerMethods) {
        NodePtr node = createMosaicChain(rod, "GBRG", 1, -2, texturedScene);
        ASSERT_TRUE(bool(node));
        setChoice(node, method.knob, method.value);
        expectTilesMatchTheFullFrame(node, rod, method.name);
    }
}

TEST_F(NativeRawDemosaicTest, TilesMatchTheFullFrameWithGreensAndSmoothing)
{
    const RectI rod(0, 0, 90, 70);
    const int greens[] = { eRawDemosaicGreenEqLocal, eRawDemosaicGreenEqFull, eRawDemosaicGreenEqBoth };
    for (int green : greens) {
        for (const Method& method : { kBayerMethods[0], kBayerMethods[2] }) {
            NodePtr node = createMosaicChain(rod, "RGGB", 0, 0, texturedScene);
            ASSERT_TRUE(bool(node));
            setChoice(node, method.knob, method.value);
            setChoice(node, kRawDemosaicParamGreenEq, green);
            node->getEffectInstance()->getKnobByNameAndType<KnobInt>(kRawDemosaicParamColorSmoothing)->setValue(2);
            expectTilesMatchTheFullFrame(node, rod, std::string(method.name) + " with green equilibration " + std::to_string(green));
        }
    }
}

TEST_F(NativeRawDemosaicTest, TilesMatchTheFullFrameXTrans)
{
    const RectI rod(0, 0, 96, 84);
    for (const Method& method : kXTransMethods) {
        NodePtr node = createMosaicChain(rod, kXTransPattern, 4, 1, texturedScene);
        ASSERT_TRUE(bool(node));
        setChoice(node, method.knob, method.value);
        expectTilesMatchTheFullFrame(node, rod, method.name);
    }
}

// A flat grey X-Trans patch must not pick up the 6x6 pattern.
TEST_F(NativeRawDemosaicTest, AFlatXTransPatchHasNoPatternArtefacts)
{
    const RectI rod(0, 0, 60, 54);
    const SceneFn flat = [](int, int, float rgb[3]) {
        rgb[0] = rgb[1] = rgb[2] = 0.35f;
    };
    for (const Method& method : kXTransMethods) {
        NodePtr node = createMosaicChain(rod, kXTransPattern, 0, 0, flat);
        ASSERT_TRUE(bool(node));
        setChoice(node, method.knob, method.value);
        const RenderedPlane plane = render(node, rod);
        double mean = 0., maximum = 0.;
        errorAgainst(plane, flat, 8, &mean, &maximum);
        EXPECT_LT(maximum, 1e-5) << method.name;
    }
}

TEST_F(NativeRawDemosaicTest, AMaZEMatchesPPGOnASmoothPatchAndIsSharperOnEdges)
{
    const RectI rod(0, 0, 96, 96);
    double smooth[2] = { 0., 0. };
    double edges[2] = { 0., 0. };
    const int methods[2] = { eRawDemosaicBayerPPG, eRawDemosaicBayerAMaZE };
    RenderedPlane smoothPlanes[2];
    for (int m = 0; m < 2; ++m) {
        NodePtr smoothNode = createMosaicChain(rod, "RGGB", 0, 0, smoothScene);
        NodePtr edgeNode = createMosaicChain(rod, "RGGB", 0, 0, starScene);
        ASSERT_TRUE(smoothNode && edgeNode);
        setChoice(smoothNode, kRawDemosaicParamBayerMethod, methods[m]);
        setChoice(edgeNode, kRawDemosaicParamBayerMethod, methods[m]);
        double maximum = 0.;
        smoothPlanes[m] = render(smoothNode, rod);
        errorAgainst(smoothPlanes[m], smoothScene, 12, &smooth[m], &maximum);
        errorAgainst(render(edgeNode, rod), starScene, 12, &edges[m], &maximum);
    }
    for (int y = rod.y1 + 12; y < rod.y2 - 12; ++y) {
        for (int x = rod.x1 + 12; x < rod.x2 - 12; ++x) {
            for (int c = 0; c < 3; ++c) {
                ASSERT_NEAR(rawTestPlaneValue(smoothPlanes[0], x, y, c), rawTestPlaneValue(smoothPlanes[1], x, y, c), 1e-3) << x << "," << y;
            }
        }
    }
    EXPECT_LT(edges[1], 0.75 * edges[0]) << "AMaZE mean error " << edges[1] << ", PPG " << edges[0];
}

TEST_F(NativeRawDemosaicTest, LinearDataPassesThroughAsAnImage)
{
    const RectI rod(0, 0, 32, 24);
    const RawTestSource::ValueFn linear = [](int x, int y, int c) {
        return 0.1f * (float)(c + 1) + 0.01f * (float)x - 0.02f * (float)y;
    };
    NodePtr node = createChain(rod, 3, linear, rawTestMetadata(kRawCfaPatternNone, 0, 0, eRawStagePrepared));
    ASSERT_TRUE(bool(node));
    const RenderedPlane plane = render(node, rod);
    ASSERT_EQ(4u, plane.channels.size());
    for (int y = rod.y1; y < rod.y2; ++y) {
        for (int x = rod.x1; x < rod.x2; ++x) {
            for (int c = 0; c < 3; ++c) {
                ASSERT_EQ(linear(x, y, c), rawTestPlaneValue(plane, x, y, c)) << x << "," << y;
            }
            ASSERT_EQ(1.f, rawTestPlaneValue(plane, x, y, 3));
        }
    }
}

// The task graph renders exactly what renderRoI does, with every source pixel declared up front.
TEST_F(NativeRawDemosaicTest, TaskGraphMatchesWithNoUnplannedPulls)
{
    const RectI rod(0, 0, 64, 48);
    for (const Method& method : kBayerMethods) {
        NodePtr node = createMosaicChain(rod, "RGGB", 0, 0, texturedScene);
        ASSERT_TRUE(bool(node));
        setChoice(node, method.knob, method.value);
        std::vector<int> unplannedPulls;
        const std::vector<int> poolSizes = { 1, 4 };
        const RenderMismatch m = renderBothWaysDirect(node, 1., ViewIdx(0), 0, RectI(9, 7, 41, 33), poolSizes, std::function<void()>(), 0.f, &unplannedPulls);
        EXPECT_FALSE(m.any) << method.name << ": " << describe(m);
        for (int pulls : unplannedPulls) {
            EXPECT_EQ(0, pulls) << method.name;
        }
    }
}

TEST_F(NativeRawDemosaicTest, UnpreparedDataRaisesTheStageError)
{
    const RectI rod(0, 0, 32, 24);
    const ImageMetadata metadata = rawTestMetadata("RGGB", 0, 0, eRawStageRaw);
    NodePtr node = createChain(rod, 1, mosaicOf(smoothScene, *RawLayout::fromPattern("RGGB")), metadata);
    ASSERT_TRUE(bool(node));
    RenderedPlane plane;
    std::string error;
    EXPECT_FALSE(renderRawTestPlane(node, rod, &plane, &error));
    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);
    EXPECT_EQ(QString::fromUtf8(RawMetadata::requireStage(metadata, eRawStagePrepared, node->getLabel()).value_or(std::string()).c_str()), message);
}

// Images too small for a method's reach fall back on VNG instead of reading past their edges.
TEST_F(NativeRawDemosaicTest, SmallImagesStayInsideTheirBuffers)
{
    const SceneFn flat = [](int, int, float rgb[3]) {
        rgb[0] = rgb[1] = rgb[2] = 0.35f;
    };
    for (const RectI& rod : { RectI(0, 0, 18, 17), RectI(0, 0, 12, 40), RectI(0, 0, 19, 19) }) {
        for (const Method& method : kXTransMethods) {
            NodePtr node = createMosaicChain(rod, kXTransPattern, 0, 0, flat);
            ASSERT_TRUE(bool(node));
            setChoice(node, method.knob, method.value);
            const RenderedPlane plane = render(node, rod);
            double mean = 0., maximum = 0.;
            errorAgainst(plane, flat, 0, &mean, &maximum);
            EXPECT_LT(maximum, 1e-5) << method.name << " on " << rod.width() << "x" << rod.height();
        }
    }
    for (const RectI& rod : { RectI(0, 0, 15, 30), RectI(0, 0, 32, 20) }) {
        for (const Method& method : kBayerMethods) {
            NodePtr node = createMosaicChain(rod, "RGGB", 0, 0, smoothScene);
            ASSERT_TRUE(bool(node));
            setChoice(node, method.knob, method.value);
            const RenderedPlane plane = render(node, rod);
            for (float v : plane.pixels) {
                ASSERT_TRUE(std::isfinite(v)) << method.name << " on " << rod.width() << "x" << rod.height();
            }
        }
    }
}

// AMaZE lays 128-photosite tiles from the image's corner; windows ending just before a tile
// boundary, with local green matching in front, still give the full frame's pixels.
TEST_F(NativeRawDemosaicTest, AMaZETilesAtItsGridBoundariesWithLocalGreens)
{
    const RectI rod(0, 0, 300, 280);
    NodePtr node = createMosaicChain(rod, "RGGB", 0, 0, texturedScene);
    ASSERT_TRUE(bool(node));
    setChoice(node, kRawDemosaicParamBayerMethod, eRawDemosaicBayerAMaZE);
    setChoice(node, kRawDemosaicParamGreenEq, eRawDemosaicGreenEqLocal);
    const RenderedPlane full = render(node, rod);
    const RectI tiles[] = {
        RectI(100, rod.y2 - 128, 128, rod.y2 - 100),
        RectI(230, rod.y2 - 256, 256, rod.y2 - 230),
        RectI(127, rod.y2 - 129, 129, rod.y2 - 127),
        RectI(120, 10, 136, 30),
    };
    for (const RectI& tile : tiles) {
        const RenderedPlane plane = render(node, tile);
        for (int y = tile.y1; y < tile.y2; ++y) {
            for (int x = tile.x1; x < tile.x2; ++x) {
                for (int c = 0; c < 3; ++c) {
                    ASSERT_EQ(rawTestPlaneValue(full, x, y, c), rawTestPlaneValue(plane, x, y, c))
                        << x << "," << y << " channel " << c << " in tile " << tile.x1 << "," << tile.y1 << "-" << tile.x2 << "," << tile.y2;
                }
            }
        }
    }
}
