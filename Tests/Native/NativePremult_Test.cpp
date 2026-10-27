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

#include <bitset>
#include <cfloat>
#include <cmath>
#include <functional>
#include <iostream>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include <ofxNatron.h>

#include "BaseTest.h"
#include "NativeParity.h"
#include "NativePluginIdentity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Premult.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;
const char* const kOfxPremultID = "net.sf.openfx.Premult";
const char* const kOfxUnpremultID = "net.sf.openfx.Unpremult";
const int kOfxPremultMajor = 2;

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

KnobChannelSelect*
divisorSelector(const NodePtr& node)
{
    return node ? dynamic_cast<KnobChannelSelect*>(node->getKnobByName(kUnPremultByKnobName).get()) : NULL;
}

// The colour channels the node's Channels knob processes on an RGBA source, as R, G, B, A bits.
std::bitset<4>
processedRgbaChannels(const NodePtr& node)
{
    KnobChannelSetPtr channels = std::dynamic_pointer_cast<KnobChannelSet>(node->getKnobByName(kNodeParamChannelSet));

    EXPECT_TRUE(bool(channels));
    if (!channels) {
        return std::bitset<4>();
    }
    const std::vector<ResolvedLayer> resolved = channels->resolve(std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()));
    std::bitset<4> bits;
    for (const ResolvedLayer& layer : resolved) {
        if (layer.desc.isColorLayer()) {
            bits |= layer.channels;
        }
    }

    return bits;
}

bool
renderRgba(const NodePtr& node,
           const RectI& window,
           RenderedPlane* plane)
{
    std::list<ImageLayerDesc> layers(1, ImageLayerDesc::mapNCompsToColorLayer(4));
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &planes, &error);

    EXPECT_TRUE(ok) << error;
    if (!ok || (planes.size() != 1)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

float
planeValue(const RenderedPlane& plane,
           int x,
           int y,
           int c)
{
    const std::size_t nComps = plane.channels.size();

    return plane.pixels[(static_cast<std::size_t>(y - plane.window.y1) * plane.window.width() + (x - plane.window.x1)) * nComps + c];
}

// The expected result, written out here rather than taken from the node: a plain product, or a
// quotient wherever the divisor is above FLT_EPSILON and the value itself elsewhere.
float
expectedValue(bool divide,
              float v,
              float d)
{
    if (!divide) {
        return v * d;
    }

    return (d > FLT_EPSILON) ? v / d : v;
}

} // namespace

class NativePremultTest
    : public BaseTest {
protected:
    NodePtr makeSource()
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);

        EXPECT_TRUE(bool(source));

        return source;
    }

    NodePtr makeNode(bool divide,
                     const NodePtr& source)
    {
        NodePtr node = createNode(QString::fromUtf8(divide ? PLUGINID_NATRON_UNPREMULT : PLUGINID_NATRON_PREMULT), PLUGIN_MAJOR_NATRON_PREMULT);

        EXPECT_TRUE(isNative(node));
        if (node && source) {
            connectNodes(source, node, 0, true);
        }

        return node;
    }

    int maskInputOf(const NodePtr& node)
    {
        for (int i = 0; i < node->getNInputs(); ++i) {
            if (node->getInputLabel(i) == "Mask") {
                return i;
            }
        }

        return -1;
    }

    bool identityAt(const NodePtr& node,
                    const RectI& window,
                    int* inputNb)
    {
        double inputTime = 0.;
        ViewIdx inputView(0);

        *inputNb = -1;

        return node->getEffectInstance()->isIdentity_public(false, 0, kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, inputNb);
    }

    // Renders the node and checks every pixel of the colour plane against expectedValue(), the
    // divisor read through `divisorAt`. Returns how many processed values had a divisor at or
    // below FLT_EPSILON.
    int expectKnownPixels(bool divide,
                          const NodePtr& node,
                          const NodePtr& source,
                          const std::function<float(int, int)>& divisorAt)
    {
        const RectI window = paritySourceWindow(source, kTime, 0);
        RenderedPlane plane;
        const ParityTolerance tolerance = ParityTolerance::arithmetic();
        int belowEpsilon = 0;

        EXPECT_TRUE(renderRgba(node, window, &plane));
        EXPECT_EQ(4u, plane.channels.size());
        if (plane.channels.size() != 4) {
            return 0;
        }
        for (int y = window.y1; y < window.y2; ++y) {
            for (int x = window.x1; x < window.x2; ++x) {
                const float d = divisorAt(x, y);
                for (int c = 0; c < 3; ++c) {
                    const float v = paritySourceColorValue(c, x, y, kTime);
                    const float want = expectedValue(divide, v, d);
                    const float got = planeValue(plane, x, y, c);
                    EXPECT_TRUE(tolerance.accepts(want, got)) << (divide ? "Unpremult" : "Premult") << " at " << x << "," << y << " c=" << c << ": divisor " << d << ", value " << v << ", want " << want << ", got " << got;
                    if (d <= FLT_EPSILON) {
                        ++belowEpsilon;
                    }
                }
                // Alpha is not processed by default, so it comes through as it is.
                EXPECT_EQ(paritySourceColorValue(3, x, y, kTime), planeValue(plane, x, y, 3)) << x << "," << y;
            }
        }

        return belowEpsilon;
    }
};

TEST_F(NativePremultTest, DefaultsAreRgbByTheAlphaChannel)
{
    for (int divide = 0; divide < 2; ++divide) {
        NodePtr node = makeNode(divide != 0, NodePtr());
        ASSERT_TRUE(bool(node));
        KnobChannelSelect* select = divisorSelector(node);
        ASSERT_TRUE(select != NULL);
        EXPECT_EQ(std::string(kNatronColorViewRGBA) + ".A", select->get());
        EXPECT_EQ(std::string(divide ? "Unpremult by" : "Premult by"), select->getLabel());
        EXPECT_EQ(std::bitset<4>(0x7), processedRgbaChannels(node));
    }
}

TEST_F(NativePremultTest, PremultKnownPixels)
{
    NodePtr source = makeSource();
    NodePtr node = makeNode(false, source);
    ASSERT_TRUE(bool(source) && bool(node));
    expectKnownPixels(false, node, source, [](int x, int y) { return paritySourceColorValue(3, x, y, kTime); });
}

// The parity source's alpha has bands of exact 0, so the pass-through below FLT_EPSILON is hit.
TEST_F(NativePremultTest, UnpremultKnownPixelsAndEpsilonPassThrough)
{
    NodePtr source = makeSource();
    NodePtr node = makeNode(true, source);
    ASSERT_TRUE(bool(source) && bool(node));
    const int belowEpsilon = expectKnownPixels(true, node, source, [](int x, int y) { return paritySourceColorValue(3, x, y, kTime); });
    EXPECT_GT(belowEpsilon, 0) << "the source has no alpha at or below FLT_EPSILON";
}

// Any layer's channel can be the divisor: here the G channel of the source's second layer.
TEST_F(NativePremultTest, AlternateAlphaChannel)
{
    for (int divide = 0; divide < 2; ++divide) {
        NodePtr source = makeSource();
        NodePtr node = makeNode(divide != 0, source);
        ASSERT_TRUE(bool(source) && bool(node));
        setParitySourceExtraLayer(source, true);
        KnobChannelSelect* select = divisorSelector(node);
        ASSERT_TRUE(select != NULL);
        select->set(kParitySourceExtraLayerID ".G");
        expectKnownPixels(divide != 0, node, source, [](int x, int y) { return paritySourceExtraValue(1, x, y, kTime); });
    }
}

TEST_F(NativePremultTest, IdentityConditions)
{
    for (int divide = 0; divide < 2; ++divide) {
        NodePtr source = makeSource();
        NodePtr node = makeNode(divide != 0, source);
        ASSERT_TRUE(bool(source) && bool(node));
        const RectI window = paritySourceWindow(source, kTime, 0);
        KnobChannelSelect* select = divisorSelector(node);
        ASSERT_TRUE(select != NULL);
        int inputNb = -1;

        EXPECT_FALSE(identityAt(node, window, &inputNb));

        select->setNone();
        EXPECT_TRUE(identityAt(node, window, &inputNb));
        EXPECT_EQ(0, inputNb);

        select->set("rgba.A");
        EXPECT_FALSE(identityAt(node, window, &inputNb));
        ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, std::vector<double>(1, 0.)));
        EXPECT_TRUE(identityAt(node, window, &inputNb));
        EXPECT_EQ(0, inputNb);
    }
}

// While the OpenFX plug-ins are still loadable, the default render matches them. Recording
// writes their output as references for replay once they are retired.
TEST_F(NativePremultTest, MatchesTheOpenFXPlugins)
{
    for (int divide = 0; divide < 2; ++divide) {
        const char* ofxID = divide ? kOfxUnpremultID : kOfxPremultID;
        if (!isPluginMajorRegistered(ofxID, kOfxPremultMajor)) {
            std::cout << "[ skipped ] " << ofxID << " " << kOfxPremultMajor << " is not loaded" << std::endl;
            continue;
        }
        ParityPair pair;
        pair.id = divide ? PLUGINID_NATRON_UNPREMULT : PLUGINID_NATRON_PREMULT;
        pair.ofxMajor = kOfxPremultMajor;
        pair.nativeMajor = PLUGIN_MAJOR_NATRON_PREMULT;
        pair.app = getApp();
        pair.source = makeSource();
        pair.native = makeNode(divide != 0, pair.source);
        pair.ofx = createNodeAtMajor(getApp(), ofxID, kOfxPremultMajor);
        ASSERT_TRUE(bool(pair.source) && bool(pair.native) && bool(pair.ofx));
        connectNodes(pair.source, pair.ofx, 0, true);
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const ParityResult r = compareParity(pair, "defaults", RectI(), mipmapLevel, ParityTolerance::arithmetic(), true);
            EXPECT_TRUE(r.ok) << pair.id << ", mipmap " << mipmapLevel << ": " << describe(r);
            EXPECT_GE(r.planesCompared, 1) << pair.id;
        }
    }
}

TEST_F(NativePremultTest, BothSchedulerModesAgreeWithNoUnplannedPull)
{
    for (int divide = 0; divide < 2; ++divide) {
        NodePtr source = makeSource();
        NodePtr mask = makeSource();
        NodePtr node = makeNode(divide != 0, source);
        ASSERT_TRUE(bool(source) && bool(mask) && bool(node));
        setParitySourceExtraLayer(source, true);
        setParitySourceOrigin(mask, 8, 4);
        const int maskInput = maskInputOf(node);
        ASSERT_GE(maskInput, 0);
        connectNodes(mask, node, maskInput, true);
        ASSERT_TRUE(setKnobValues(node, "enableMask_Mask", std::vector<double>(1, 1.)));
        KnobChannelSelect* maskChannel = dynamic_cast<KnobChannelSelect*>(node->getKnobByName("maskChannel_Mask").get());
        ASSERT_TRUE(maskChannel != NULL);
        maskChannel->set("rgba.A");
        ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, std::vector<double>(1, 0.75)));
        KnobChannelSelect* select = divisorSelector(node);
        ASSERT_TRUE(select != NULL);

        const char* const divisors[2] = { kParitySourceExtraLayerID ".G", "rgba.A" };
        std::vector<int> poolSizes;
        poolSizes.push_back(1);
        poolSizes.push_back(4);
        for (int d = 0; d < 2; ++d) {
            select->set(divisors[d]);
            for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
                const RectI window = paritySourceWindow(source, kTime, mipmapLevel);
                std::vector<int> unplannedPulls;
                const RenderMismatch m = renderBothWaysDirect(node, kTime, ViewIdx(0), mipmapLevel, window, poolSizes, std::function<void()>(), 0.f, &unplannedPulls);
                EXPECT_FALSE(m.any) << divisors[d] << ", mipmap " << mipmapLevel << ": " << describe(m);
                ASSERT_EQ(poolSizes.size(), unplannedPulls.size()) << divisors[d];
                for (std::size_t i = 0; i < unplannedPulls.size(); ++i) {
                    EXPECT_EQ(0, unplannedPulls[i]) << divisors[d] << ", mipmap " << mipmapLevel << ", pool " << poolSizes[i];
                }
            }
        }
    }
}

TEST_F(NativePremultTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_PREMULT, PLUGIN_MAJOR_NATRON_PREMULT, "Premult");
    expectNativeIdentity(PLUGINID_NATRON_UNPREMULT, PLUGIN_MAJOR_NATRON_PREMULT, "Unpremult");
}
