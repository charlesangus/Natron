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
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "NativePluginIdentity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Keyer/Difference.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;
// A is B's source shifted, so that the two differ everywhere they overlap.
const int kAOriginX = 2;
const int kAOriginY = 1;

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

RectI
pixelRoD(const NodePtr& node)
{
    EffectInstancePtr effect = node->getEffectInstance();
    RectD rod;
    const StatusEnum stat = effect->getRegionOfDefinition(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod);
    EXPECT_NE(eStatusFailed, stat);

    return rod.toPixelEnclosing(0, 1.);
}

bool
renderRGBA(const NodePtr& node,
           const RectI& window,
           RenderedPlane* plane)
{
    std::list<ImageLayerDesc> layers;
    layers.push_back(ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &planes, &error);
    EXPECT_TRUE(ok) << node->getScriptName() << ": " << error;
    if (!ok || (planes.size() != 1) || (planes[0].channels.size() != 4)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

const float*
pixelAt(const RenderedPlane& plane,
        int x,
        int y)
{
    return &plane.pixels[((std::size_t)(y - plane.window.y1) * plane.window.width() + (x - plane.window.x1)) * 4];
}

// The parity source at (originX, originY) at pixel (x, y), zero outside its region.
float
sourceValue(int channel,
            int originX,
            int originY,
            int x,
            int y)
{
    const int cx = x - originX;
    const int cy = y - originY;
    if ((cx < 0) || (cy < 0) || (cx >= kParitySourceWidth) || (cy >= kParitySourceHeight)) {
        return 0.f;
    }

    return paritySourceColorValue(channel, cx, cy, kTime);
}

// The inputs' regions and a margin around them.
RectI
caseWindow()
{
    return RectI(-4, -4, kParitySourceWidth + 4, kParitySourceHeight + 4);
}

// The matte as the node computes it, in float and in the same order.
float
matte(const float b[4],
      const float a[4],
      float gain,
      float offset)
{
    float sum = 0.f;
    for (int c = 0; c < 3; ++c) {
        const float d = b[c] - a[c];
        sum += d * d;
    }

    return std::max(0.f, gain * sum - offset);
}

} // namespace

class NativeDifferenceTest
    : public BaseTest {
protected:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeDifference64x48", 1.));
    }

    NodePtr makeSource(int originX,
                       int originY)
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
        EXPECT_TRUE(bool(source));
        if (source) {
            setParitySourceOrigin(source, originX, originY);
        }

        return source;
    }

    NodePtr makeDifference(const NodePtr& b,
                           const NodePtr& a)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_DIFFERENCE), PLUGIN_MAJOR_NATRON_DIFFERENCE);
        EXPECT_TRUE(isNative(node));
        if (!node) {
            return node;
        }
        if (b) {
            connectNodes(b, node, kDifferenceInputB, true);
        }
        if (a) {
            connectNodes(a, node, kDifferenceInputA, true);
        }

        return node;
    }

    // Expects B's colour and the matte of B against A (shifted by aOrigin, or B itself without A)
    // everywhere in the window; each reads zero outside its region.
    void expectDifference(const NodePtr& node,
                          bool withA,
                          int aOriginX,
                          int aOriginY,
                          float gain,
                          float offset,
                          float* maxAlpha = NULL)
    {
        const RectI window = caseWindow();
        RenderedPlane plane;
        ASSERT_TRUE(renderRGBA(node, window, &plane));
        float largest = 0.f;
        for (int y = window.y1; y < window.y2; ++y) {
            for (int x = window.x1; x < window.x2; ++x) {
                float b[4];
                float a[4];
                for (int c = 0; c < 4; ++c) {
                    b[c] = sourceValue(c, 0, 0, x, y);
                    a[c] = withA ? sourceValue(c, aOriginX, aOriginY, x, y) : b[c];
                }
                const float* got = pixelAt(plane, x, y);
                for (int c = 0; c < 3; ++c) {
                    ASSERT_EQ(b[c], got[c]) << "channel " << c << " at (" << x << ", " << y << ")";
                }
                const float want = matte(b, a, gain, offset);
                ASSERT_NEAR(want, got[3], 1e-5f * std::max(1.f, std::fabs(want))) << "alpha at (" << x << ", " << y << ")";
                largest = std::max(largest, got[3]);
            }
        }
        if (maxAlpha) {
            *maxAlpha = largest;
        }
    }
};

TEST_F(NativeDifferenceTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_DIFFERENCE, PLUGIN_MAJOR_NATRON_DIFFERENCE, "Difference");
}

TEST_F(NativeDifferenceTest, AlphaIsTheSquaredColourDistance)
{
    resetProject();
    NodePtr node = makeDifference(makeSource(0, 0), makeSource(kAOriginX, kAOriginY));
    ASSERT_TRUE(bool(node));
    expectDifference(node, true, kAOriginX, kAOriginY, 1.f, 0.f);
}

TEST_F(NativeDifferenceTest, GainAndOffset)
{
    resetProject();
    NodePtr node = makeDifference(makeSource(0, 0), makeSource(kAOriginX, kAOriginY));
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kDifferenceParamGain, { 3. }));
    ASSERT_TRUE(setKnobValues(node, kDifferenceParamOffset, { 0.01 }));
    expectDifference(node, true, kAOriginX, kAOriginY, 3.f, 0.01f);
}

TEST_F(NativeDifferenceTest, MattesAboveOneAreNotClamped)
{
    resetProject();
    NodePtr node = makeDifference(makeSource(0, 0), makeSource(kAOriginX, kAOriginY));
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kDifferenceParamGain, { 1000. }));
    float largest = 0.f;
    expectDifference(node, true, kAOriginX, kAOriginY, 1000.f, 0.f, &largest);
    EXPECT_GT(largest, 1.f);
}

TEST_F(NativeDifferenceTest, AZeroDifferenceGivesAlphaZero)
{
    resetProject();
    NodePtr source = makeSource(0, 0);
    ASSERT_TRUE(bool(source));
    NodePtr node = makeDifference(source, source);
    ASSERT_TRUE(bool(node));
    float largest = 1.f;
    expectDifference(node, true, 0, 0, 1.f, 0.f, &largest);
    EXPECT_EQ(0.f, largest);
}

TEST_F(NativeDifferenceTest, WithoutACleanPlateTheDifferenceIsZero)
{
    resetProject();
    NodePtr node = makeDifference(makeSource(0, 0), NodePtr());
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kDifferenceParamGain, { 5. }));
    float largest = 1.f;
    expectDifference(node, false, 0, 0, 5.f, 0.f, &largest);
    EXPECT_EQ(0.f, largest);
}

TEST_F(NativeDifferenceTest, WithoutBItIsBlack)
{
    resetProject();
    NodePtr node = makeDifference(NodePtr(), makeSource(kAOriginX, kAOriginY));
    ASSERT_TRUE(bool(node));

    // Black is an identity of no input, as for Switch and STMap: renderRoI succeeds without
    // producing a plane, which the helper reports as "no plane".
    EffectInstancePtr effect = node->getEffectInstance();
    double inputTime = 0.;
    ViewIdx inputView(0);
    int inputNb = -3;
    EXPECT_TRUE(effect->isIdentity_public(false, effect->getRenderHash(), kTime, RenderScale::identity, RectI(0, 0, 16, 16), ViewIdx(0), &inputTime, &inputView, &inputNb));
    EXPECT_EQ(-1, inputNb);

    std::list<ImageLayerDesc> layers(1, ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> planes;
    std::string error;
    EXPECT_FALSE(renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, RectI(0, 0, 16, 16), layers, &planes, &error));
    EXPECT_EQ(std::string(kRenderProducedNoPlane), error);
    EXPECT_TRUE(planes.empty());
}
