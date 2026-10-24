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
#include <memory>
#include <random>
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
#include "Engine/Nodes/Raw/RawDenoise.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kXTransPattern = "GGRGGB"
                                   "GGBGGR"
                                   "BRGRBG"
                                   "GGBGGR"
                                   "GGRGGB"
                                   "RBGBRG";

// Two smooth halves with a vertical step of about 0.4 between them at x = width / 2.
float
stepScene(int x,
          int y,
          int width)
{
    return (x < width / 2) ? (0.15f + 0.0005f * (float)y) : (0.55f + 0.0004f * (float)x);
}

/**
 * @brief stepScene with Gaussian noise of standard deviation sigma over rod, the same noise for
 * the same seed.
 **/
RawTestSource::ValueFn
noisyScene(const RectI& rod,
           float sigma,
           unsigned seed)
{
    std::shared_ptr<std::vector<float>> values = std::make_shared<std::vector<float>>((std::size_t)rod.width() * (std::size_t)rod.height());
    std::mt19937 rng(seed);
    std::normal_distribution<float> noise(0.f, sigma);
    const int width = rod.width();

    for (int y = rod.y1; y < rod.y2; ++y) {
        for (int x = rod.x1; x < rod.x2; ++x) {
            (*values)[(std::size_t)(y - rod.y1) * (std::size_t)width + (std::size_t)(x - rod.x1)] = stepScene(x - rod.x1, y - rod.y1, width) + noise(rng);
        }
    }

    return [values, rod](int x, int y, int) {
        return (*values)[(std::size_t)(y - rod.y1) * (std::size_t)rod.width() + (std::size_t)(x - rod.x1)];
    };
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

class NativeRawDenoiseTest
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
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWDENOISE));

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

    static void expectTilesMatchTheFullFrame(const NodePtr& node,
                                             const RectI& rod,
                                             const std::string& what)
    {
        const RenderedPlane full = render(node, rod);
        const RectI tiles[] = {
            RectI(rod.x1 + 70, rod.y1 + 60, rod.x1 + 101, rod.y1 + 83),
            RectI(rod.x1, rod.y1, rod.x1 + 9, rod.y1 + 7),
            RectI(rod.x2 - 13, rod.y2 - 6, rod.x2, rod.y2),
            RectI(rod.x2 - 1, rod.y1 + 40, rod.x2, rod.y1 + 47),
            RectI(rod.x1 + 100, rod.y1 + 80, rod.x1 + 101, rod.y1 + 81),
            RectI(rod.x1 + 3, rod.y2 - 1, rod.x1 + 4, rod.y2),
            RectI(rod.x1 + 120, rod.y1 + 2, rod.x2 - 1, rod.y1 + 25),
        };
        for (const RectI& tile : tiles) {
            const RenderedPlane plane = render(node, tile);
            for (int y = tile.y1; y < tile.y2; ++y) {
                for (int x = tile.x1; x < tile.x2; ++x) {
                    ASSERT_EQ(rawTestPlaneValue(full, x, y, 0), rawTestPlaneValue(plane, x, y, 0)) << what << " at " << x << "," << y << " in tile " << tile.x1 << "," << tile.y1 << "-" << tile.x2 << "," << tile.y2;
                }
            }
        }
    }
};

TEST_F(NativeRawDenoiseTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_RAWDENOISE, PLUGIN_MAJOR_NATRON_RAWDENOISE, "RawDenoise");

    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_RAWDENOISE));
    ASSERT_TRUE(bool(node));
    EXPECT_EQ(eDataKindRaw, node->getEffectInstance()->getInputDataKind(0));
    EXPECT_EQ(eDataKindRaw, node->getEffectInstance()->getOutputDataKind());
    for (int channel = 0; channel < 4; ++channel) {
        for (int band = 0; band < RawKernels::kRawDenoiseBands; ++band) {
            EXPECT_TRUE(bool(node->getEffectInstance()->getKnobByNameAndType<KnobDouble>(RawDenoise::strengthKnobName(channel, band))));
        }
    }
}

// At the default settings, noise of 0.01 on a mosaic between 0.15 and 0.6 loses at least 3 dB,
// and the photosites next to a step of 0.4 stay within 0.06 of it.
TEST_F(NativeRawDenoiseTest, NoiseIsReducedAndEdgesKept)
{
    const RectI rod(0, 0, 96, 80);
    for (const char* pattern : { "RGGB", kXTransPattern }) {
        const ImageMetadata metadata = rawTestMetadata(pattern, 0, 0, eRawStagePrepared);
        const RawTestSource::ValueFn noisy = noisyScene(rod, 0.01f, 7);
        NodePtr node = createChain(rod, 1, noisy, metadata);
        ASSERT_TRUE(bool(node));
        const RenderedPlane plane = render(node, rod);
        ASSERT_EQ(1u, plane.channels.size()) << pattern;

        double noisyError = 0.;
        double denoisedError = 0.;
        double edgeError = 0.;
        for (int y = rod.y1; y < rod.y2; ++y) {
            for (int x = rod.x1; x < rod.x2; ++x) {
                const double clean = stepScene(x, y, rod.width());
                const double out = rawTestPlaneValue(plane, x, y, 0);
                noisyError += (noisy(x, y, 0) - clean) * (noisy(x, y, 0) - clean);
                denoisedError += (out - clean) * (out - clean);
                if ((y >= 4) && (y < rod.y2 - 4) && (x >= rod.width() / 2 - 2) && (x < rod.width() / 2 + 2)) {
                    edgeError = std::max(edgeError, std::fabs(out - clean));
                }
            }
        }
        const double gain = 10. * std::log10(noisyError / denoisedError);
        EXPECT_GT(gain, 3.) << pattern;
        EXPECT_LT(edgeError, 0.06) << pattern;
        EXPECT_FALSE(node->hasPersistentMessage()) << pattern;
    }
}

TEST_F(NativeRawDenoiseTest, TilesMatchTheFullFrame)
{
    const RectI rod(-5, 3, 195, 163);
    for (const char* pattern : { "GBRG", kXTransPattern }) {
        const ImageMetadata metadata = rawTestMetadata(pattern, 1, -2, eRawStagePrepared);
        NodePtr node = createChain(rod, 1, noisyScene(rod, 0.02f, 11), metadata);
        ASSERT_TRUE(bool(node));
        node->getEffectInstance()->getKnobByNameAndType<KnobDouble>(kRawDenoiseParamThreshold)->setValue(0.05);
        expectTilesMatchTheFullFrame(node, rod, pattern);
    }
}

// Zero strength in every band of one channel leaves that channel's noise; the others still lose it.
TEST_F(NativeRawDenoiseTest, ChannelStrengthsApplyToTheirColour)
{
    const RectI rod(0, 0, 96, 80);
    const ImageMetadata metadata = rawTestMetadata("RGGB", 0, 0, eRawStagePrepared);
    const RawLayout layout = *RawLayout::fromMetadata(metadata);
    const RawTestSource::ValueFn noisy = noisyScene(rod, 0.01f, 5);
    NodePtr node = createChain(rod, 1, noisy, metadata);
    ASSERT_TRUE(bool(node));
    for (int band = 0; band < RawKernels::kRawDenoiseBands; ++band) {
        node->getEffectInstance()->getKnobByNameAndType<KnobDouble>(RawDenoise::strengthKnobName(1, band))->setValue(0.);
    }
    const RenderedPlane plane = render(node, rod);
    double redChange = 0.;
    double greenChange = 0.;
    for (int y = rod.y1; y < rod.y2; ++y) {
        for (int x = rod.x1; x < rod.x2; ++x) {
            const double change = std::fabs(rawTestPlaneValue(plane, x, y, 0) - noisy(x, y, 0));
            if (layout.colourAt(x, y) == eRawCfaColorRed) {
                redChange = std::max(redChange, change);
            } else if (layout.colourAt(x, y) == eRawCfaColorGreen) {
                greenChange = std::max(greenChange, change);
            }
        }
    }
    EXPECT_LT(redChange, 1e-5);
    EXPECT_GT(greenChange, 1e-3);
}

TEST_F(NativeRawDenoiseTest, ZeroThresholdIsAnIdentity)
{
    const RectI rod(0, 0, 64, 48);
    const ImageMetadata metadata = rawTestMetadata("RGGB", 0, 0, eRawStagePrepared);
    NodePtr node = createChain(rod, 1, noisyScene(rod, 0.01f, 3), metadata);
    ASSERT_TRUE(bool(node));
    node->getEffectInstance()->getKnobByNameAndType<KnobDouble>(kRawDenoiseParamThreshold)->setValue(0.);

    double inputTime = 0.;
    ViewIdx inputView(0);
    int inputNb = -1;
    const EffectInstancePtr effect = node->getEffectInstance();
    EXPECT_TRUE(effect->isIdentity_public(false, effect->getRenderHash(), 1., RenderScale::identity, rod, ViewIdx(0), &inputTime, &inputView, &inputNb));
    EXPECT_EQ(0, inputNb);
}

TEST_F(NativeRawDenoiseTest, LinearDataPassesThroughWithAWarning)
{
    const RectI rod(0, 0, 64, 48);
    const ImageMetadata metadata = rawTestMetadata("none", 0, 0, eRawStagePrepared);
    const RawTestSource::ValueFn rgb = [](int x, int y, int c) {
        return 0.2f + 0.01f * (float)x + 0.02f * (float)y + 0.1f * (float)c;
    };
    NodePtr node = createChain(rod, 3, rgb, metadata);
    ASSERT_TRUE(bool(node));
    const RenderedPlane plane = render(node, rod);
    ASSERT_EQ(3u, plane.channels.size());
    for (int y = rod.y1; y < rod.y2; ++y) {
        for (int x = rod.x1; x < rod.x2; ++x) {
            for (int c = 0; c < 3; ++c) {
                ASSERT_EQ(rgb(x, y, c), rawTestPlaneValue(plane, x, y, c)) << x << "," << y;
            }
        }
    }
    ASSERT_TRUE(node->hasPersistentMessage());
    int type = 0;
    const std::string message = persistentMessage(node, &type);
    EXPECT_EQ((int)eMessageTypeWarning, type) << message;
}

TEST_F(NativeRawDenoiseTest, UnpreparedDataRaisesTheStageError)
{
    const RectI rod(0, 0, 64, 48);
    const ImageMetadata metadata = rawTestMetadata("RGGB", 0, 0, eRawStageRaw);
    NodePtr node = createChain(rod, 1, noisyScene(rod, 0.01f, 3), metadata);
    ASSERT_TRUE(bool(node));

    RenderedPlane plane;
    std::string error;
    EXPECT_FALSE(renderRawTestPlane(node, rod, &plane, &error));
    int type = 0;
    EXPECT_EQ(RawMetadata::requireStage(metadata, eRawStagePrepared, node->getLabel()).value_or(std::string()), persistentMessage(node, &type));
}
