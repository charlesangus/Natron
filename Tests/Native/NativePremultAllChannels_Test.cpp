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

#include <cfloat>
#include <cmath>
#include <cstddef>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Premult.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

// A Premult whose Channels take in alpha, the channel it multiplies by, through "All" or through
// picking the rgba layer again in the panel (a layer row with every channel), feeding an
// Unpremult: the user's Constant, Shuffle to a second layer and blurred alpha copied in reduce to
// the parity source with its second layer, whose alpha is soft with bands of exact 0.

namespace {

const double kTime = 1.;

KnobChannelSetPtr
channelsOf(const NodePtr& node)
{
    return node ? std::dynamic_pointer_cast<KnobChannelSet>(node->getKnobByName(kNodeParamChannelSet)) : KnobChannelSetPtr();
}

std::list<ImageLayerDesc>
renderedLayers()
{
    std::list<ImageLayerDesc> layers;

    layers.push_back(ImageLayerDesc::mapNCompsToColorLayer(4));
    layers.push_back(paritySourceExtraLayer());

    return layers;
}

bool
renderPlanes(const NodePtr& node,
             const RectI& window,
             std::vector<RenderedPlane>* planes)
{
    std::string error;
    // The cache is kept, so a result cached under a key that misses the Channels knob shows up.
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, renderedLayers(), planes, &error, false /*byPassCache*/);

    EXPECT_TRUE(ok) << error;
    EXPECT_EQ(2u, planes->size());

    return ok && (planes->size() == 2);
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

// Every value of every plane identical, NaN matching NaN.
void
expectSamePlanes(const std::vector<RenderedPlane>& want,
                 const std::vector<RenderedPlane>& got,
                 const char* what)
{
    ASSERT_EQ(want.size(), got.size()) << what;
    for (std::size_t p = 0; p < want.size(); ++p) {
        ASSERT_EQ(want[p].channels, got[p].channels) << what << ", plane " << p;
        ASSERT_EQ(want[p].pixels.size(), got[p].pixels.size()) << what << ", plane " << p;
        int mismatches = 0;
        std::size_t first = 0;
        for (std::size_t i = 0; i < want[p].pixels.size(); ++i) {
            const float a = want[p].pixels[i];
            const float b = got[p].pixels[i];
            if (!((a == b) || (std::isnan(a) && std::isnan(b)))) {
                if (mismatches == 0) {
                    first = i;
                }
                ++mismatches;
            }
        }
        EXPECT_EQ(0, mismatches) << what << ", plane " << want[p].layer.getLayerID() << ": first at value " << first << ", want "
                                 << want[p].pixels[first] << ", got " << got[p].pixels[first];
    }
}

} // namespace

class NativePremultAllChannelsTest
    : public BaseTest {
protected:
    NodePtr makeSource()
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);

        EXPECT_TRUE(bool(source));
        if (source) {
            setParitySourceExtraLayer(source, true);
        }

        return source;
    }

    NodePtr makeNode(bool divide,
                     const NodePtr& input)
    {
        NodePtr node = createNode(QString::fromUtf8(divide ? PLUGINID_NATRON_UNPREMULT : PLUGINID_NATRON_PREMULT), PLUGIN_MAJOR_NATRON_PREMULT);

        EXPECT_TRUE(node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get()));
        if (node && input) {
            connectNodes(input, node, 0, true);
        }

        return node;
    }
};

// Premult and Unpremult with alpha among their channels leave alpha as it is and treat the
// other channels as with the default rgb; the second layer, which does not hold the divisor,
// is scaled in full.
TEST_F(NativePremultAllChannelsTest, AllLeavesTheDivisorChannelAlone)
{
    for (int divide = 0; divide < 2; ++divide) {
        NodePtr source = makeSource();
        NodePtr node = makeNode(divide != 0, source);
        ASSERT_TRUE(source && node);
        KnobChannelSetPtr channels = channelsOf(node);
        ASSERT_TRUE(bool(channels));
        channels->setAll();

        const RectI window = paritySourceWindow(source, kTime, 0);
        std::vector<RenderedPlane> planes;
        ASSERT_TRUE(renderPlanes(node, window, &planes));
        ASSERT_EQ(4u, planes[0].channels.size());
        const ParityTolerance tolerance = ParityTolerance::arithmetic();
        for (int y = window.y1; y < window.y2; ++y) {
            for (int x = window.x1; x < window.x2; ++x) {
                const float a = paritySourceColorValue(3, x, y, kTime);
                const bool divides = (divide != 0) && (a > FLT_EPSILON);
                EXPECT_EQ(a, planeValue(planes[0], x, y, 3)) << (divide ? "Unpremult" : "Premult") << " alpha at " << x << "," << y;
                for (int c = 0; c < 3; ++c) {
                    const float v = paritySourceColorValue(c, x, y, kTime);
                    const float want = divide ? (divides ? v / a : v) : v * a;
                    EXPECT_TRUE(tolerance.accepts(want, planeValue(planes[0], x, y, c))) << (divide ? "Unpremult" : "Premult") << " at " << x << "," << y << " c=" << c;
                }
                for (int c = 0; c < (int)planes[1].channels.size(); ++c) {
                    const float v = paritySourceExtraValue(c, x, y, kTime);
                    const float want = divide ? (divides ? v / a : v) : v * a;
                    EXPECT_TRUE(tolerance.accepts(want, planeValue(planes[1], x, y, c))) << (divide ? "Unpremult" : "Premult") << " second layer at " << x << "," << y << " c=" << c;
                }
            }
        }
    }
}

// The user's chain: Premult into Unpremult, the Premult's Channels set to All and back to rgba
// both ways the panel offers (picking the rgba layer, which holds every channel, and the
// default rows). Each round trip renders exactly as before it and as a fresh chain does, and
// "All" itself keeps the colour plane within rounding of it.
TEST_F(NativePremultAllChannelsTest, AllAndBackRendersAsBefore)
{
    NodePtr source = makeSource();
    NodePtr premult = makeNode(false, source);
    NodePtr unpremult = makeNode(true, premult);
    ASSERT_TRUE(source && premult && unpremult);
    KnobChannelSetPtr channels = channelsOf(premult);
    ASSERT_TRUE(bool(channels));
    const std::vector<ChannelSetRow> defaultRows = channels->getRows();

    const RectI window = paritySourceWindow(source, kTime, 0);
    std::vector<RenderedPlane> initial;
    ASSERT_TRUE(renderPlanes(unpremult, window, &initial));

    channels->setAll();
    std::vector<RenderedPlane> all;
    ASSERT_TRUE(renderPlanes(unpremult, window, &all));
    ASSERT_EQ(initial[0].pixels.size(), all[0].pixels.size());
    const ParityTolerance tolerance = ParityTolerance::arithmetic();
    for (std::size_t i = 0; i < initial[0].pixels.size(); ++i) {
        EXPECT_TRUE(tolerance.accepts(initial[0].pixels[i], all[0].pixels[i])) << "colour value " << i << " under All: want " << initial[0].pixels[i] << ", got " << all[0].pixels[i];
    }

    channels->setLayer(0, kNatronColorViewRGBA, NULL);
    {
        // The panel's pick of the rgba layer takes in alpha, unlike the default rows.
        const std::vector<ResolvedLayer> resolved = channels->resolve(std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()));
        ASSERT_EQ(1u, resolved.size());
        EXPECT_TRUE(resolved[0].channels[3]);
    }
    std::vector<RenderedPlane> rgbaPicked;
    ASSERT_TRUE(renderPlanes(unpremult, window, &rgbaPicked));
    expectSamePlanes(initial, rgbaPicked, "All, then the rgba layer picked");

    channels->setAll();
    channels->setRows(defaultRows);
    std::vector<RenderedPlane> restored;
    ASSERT_TRUE(renderPlanes(unpremult, window, &restored));
    expectSamePlanes(initial, restored, "All, then the default rows");

    NodePtr freshPremult = makeNode(false, source);
    NodePtr freshUnpremult = makeNode(true, freshPremult);
    ASSERT_TRUE(freshPremult && freshUnpremult);
    std::vector<RenderedPlane> fresh;
    ASSERT_TRUE(renderPlanes(freshUnpremult, window, &fresh));
    expectSamePlanes(fresh, rgbaPicked, "a fresh chain against the rgba layer picked");
    expectSamePlanes(fresh, restored, "a fresh chain against the default rows");
}
