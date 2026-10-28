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

#include <list>
#include <memory>
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
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Merge/KeyMix.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;
// B at the origin, A shifted so that each covers pixels the other does not.
const int kAOriginX = 8;
const int kAOriginY = 4;

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
    const StatusEnum stat = effect->getRegionOfDefinition(effect->getRenderHash(), kTime, RenderScale::fromMipmapLevel(0), ViewIdx(0), &rod);
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

// The union of A and B and a margin around it.
RectI
caseWindow()
{
    return RectI(-4, -4, kParitySourceWidth + kAOriginX + 4, kParitySourceHeight + kAOriginY + 4);
}

} // namespace

class NativeKeyMixTest
    : public BaseTest {
protected:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeKeyMix64x48", 1.));
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

    // KeyMix with B at the origin and, when withA, A at (kAOriginX, kAOriginY).
    NodePtr makeKeyMix(bool withB,
                       bool withA)
    {
        NodePtr keyMix = createNode(QString::fromUtf8(PLUGINID_NATRON_KEYMIX), PLUGIN_MAJOR_NATRON_KEYMIX);
        EXPECT_TRUE(isNative(keyMix));
        if (!keyMix) {
            return keyMix;
        }
        if (withB) {
            NodePtr b = makeSource(0, 0);
            if (b) {
                connectNodes(b, keyMix, kKeyMixInputB, true);
            }
        }
        if (withA) {
            NodePtr a = makeSource(kAOriginX, kAOriginY);
            if (a) {
                connectNodes(a, keyMix, kKeyMixInputA, true);
            }
        }

        return keyMix;
    }

    // Connects a parity source at the origin as the mask, read from its alpha: bands of 0, 1 and
    // a [0, 1] ramp.
    void connectSoftMask(const NodePtr& keyMix)
    {
        NodePtr mask = makeSource(0, 0);
        ASSERT_TRUE(bool(mask));
        connectNodes(mask, keyMix, kKeyMixInputMask, true);
        ASSERT_TRUE(setKnobValues(keyMix, "enableMask_Mask", { 1. }));
        KnobChannelSelect* select = dynamic_cast<KnobChannelSelect*>(keyMix->getKnobByName("maskChannel_Mask").get());
        ASSERT_TRUE(select != NULL);
        select->set("rgba.A");
    }

    // Expects every pixel of the window to be B + (A - B) x mask x mix on the processed channels
    // and B on the others, as the host's mask/mix computes it.
    void expectKeyMix(const NodePtr& keyMix,
                      const bool processed[4],
                      bool masked,
                      float mix)
    {
        const RectI window = caseWindow();
        RenderedPlane plane;
        ASSERT_TRUE(renderRGBA(keyMix, window, &plane));
        const RectI rod = pixelRoD(keyMix);
        for (int y = window.y1; y < window.y2; ++y) {
            for (int x = window.x1; x < window.x2; ++x) {
                const bool inRoD = (x >= rod.x1) && (x < rod.x2) && (y >= rod.y1) && (y < rod.y2);
                const float m = masked ? sourceValue(3, 0, 0, x, y) : 1.f;
                const float alpha = m * mix;
                const float* got = pixelAt(plane, x, y);
                for (int c = 0; c < 4; ++c) {
                    const float b = sourceValue(c, 0, 0, x, y);
                    const float a = sourceValue(c, kAOriginX, kAOriginY, x, y);
                    float want = b;
                    if (processed[c] && (alpha == 1.f)) {
                        want = a;
                    } else if (processed[c] && (alpha != 0.f)) {
                        want = a * alpha + (1.f - alpha) * b;
                    }
                    if (!inRoD) {
                        want = 0.f;
                    }
                    ASSERT_NEAR(want, got[c], 1e-6f) << "channel " << c << " at (" << x << ", " << y << ")";
                }
            }
        }
    }
};

TEST_F(NativeKeyMixTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_KEYMIX, PLUGIN_MAJOR_NATRON_KEYMIX, "KeyMix");
}

TEST_F(NativeKeyMixTest, AWithoutAMaskCoversBAndKeepsHdrValues)
{
    resetProject();
    NodePtr keyMix = makeKeyMix(true, true);
    ASSERT_TRUE(bool(keyMix));
    const bool all[4] = { true, true, true, true };
    expectKeyMix(keyMix, all, false, 1.f);

    // The parity source's colour ramps run from -0.25 to 1.75; KeyMix must not clamp them.
    RenderedPlane plane;
    ASSERT_TRUE(renderRGBA(keyMix, caseWindow(), &plane));
    bool sawNegative = false;
    bool sawAboveOne = false;
    for (float v : plane.pixels) {
        sawNegative = sawNegative || (v < 0.f);
        sawAboveOne = sawAboveOne || (v > 1.f);
    }
    EXPECT_TRUE(sawNegative);
    EXPECT_TRUE(sawAboveOne);
}

TEST_F(NativeKeyMixTest, SoftMaskBlendsAOverB)
{
    resetProject();
    NodePtr keyMix = makeKeyMix(true, true);
    ASSERT_TRUE(bool(keyMix));
    connectSoftMask(keyMix);
    const bool all[4] = { true, true, true, true };
    expectKeyMix(keyMix, all, true, 1.f);
}

TEST_F(NativeKeyMixTest, MixScalesTheMask)
{
    resetProject();
    NodePtr keyMix = makeKeyMix(true, true);
    ASSERT_TRUE(bool(keyMix));
    connectSoftMask(keyMix);
    ASSERT_TRUE(setKnobValues(keyMix, "mix", { 0.5 }));
    const bool all[4] = { true, true, true, true };
    expectKeyMix(keyMix, all, true, 0.5f);
}

TEST_F(NativeKeyMixTest, ChannelSubsetsTakeOnlyTheSelectedChannelsFromA)
{
    resetProject();
    NodePtr keyMix = makeKeyMix(true, true);
    ASSERT_TRUE(bool(keyMix));
    connectSoftMask(keyMix);
    KnobChannelSetPtr channels = std::dynamic_pointer_cast<KnobChannelSet>(keyMix->getKnobByName(kNodeParamChannelSet));
    ASSERT_TRUE(bool(channels));

    const std::vector<std::string> red = { "R" };
    channels->setLayer(0, kNatronColorViewRGBA, &red);
    const bool onlyRed[4] = { true, false, false, false };
    expectKeyMix(keyMix, onlyRed, true, 1.f);

    const std::vector<std::string> greenAlpha = { "G", "A" };
    channels->setLayer(0, kNatronColorViewRGBA, &greenAlpha);
    const bool greenAndAlpha[4] = { false, true, false, true };
    expectKeyMix(keyMix, greenAndAlpha, true, 1.f);
}

TEST_F(NativeKeyMixTest, WithoutAItIsB)
{
    resetProject();
    NodePtr keyMix = makeKeyMix(true, false);
    ASSERT_TRUE(bool(keyMix));
    connectSoftMask(keyMix);

    EXPECT_EQ(RectI(0, 0, kParitySourceWidth, kParitySourceHeight), pixelRoD(keyMix));
    const RectI window = caseWindow();
    RenderedPlane plane;
    ASSERT_TRUE(renderRGBA(keyMix, window, &plane));
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const float* got = pixelAt(plane, x, y);
            for (int c = 0; c < 4; ++c) {
                ASSERT_EQ(sourceValue(c, 0, 0, x, y), got[c]) << "channel " << c << " at (" << x << ", " << y << ")";
            }
        }
    }
}

TEST_F(NativeKeyMixTest, WithoutInputsItIsBlack)
{
    resetProject();
    NodePtr keyMix = makeKeyMix(false, false);
    ASSERT_TRUE(bool(keyMix));

    // Black is an identity of no input, as for Switch and STMap: renderRoI succeeds without
    // producing a plane, which the helper reports as "no plane".
    EffectInstancePtr effect = keyMix->getEffectInstance();
    double inputTime = 0.;
    ViewIdx inputView(0);
    int inputNb = -3;
    EXPECT_TRUE(effect->isIdentity_public(false, effect->getRenderHash(), kTime, RenderScale::identity, RectI(0, 0, 16, 16), ViewIdx(0), &inputTime, &inputView, &inputNb));
    EXPECT_EQ(-1, inputNb);

    std::list<ImageLayerDesc> layers(1, ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> planes;
    std::string error;
    EXPECT_FALSE(renderNodePlanesDirect(keyMix, kTime, ViewIdx(0), 0, RectI(0, 0, 16, 16), layers, &planes, &error));
    EXPECT_EQ(std::string(kRenderProducedNoPlane), error);
    EXPECT_TRUE(planes.empty());
}

TEST_F(NativeKeyMixTest, WithoutBAIsKeyedOverBlack)
{
    resetProject();
    NodePtr keyMix = makeKeyMix(false, true);
    ASSERT_TRUE(bool(keyMix));
    connectSoftMask(keyMix);

    const RectI window = caseWindow();
    RenderedPlane plane;
    ASSERT_TRUE(renderRGBA(keyMix, window, &plane));
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const float m = sourceValue(3, 0, 0, x, y);
            const float* got = pixelAt(plane, x, y);
            for (int c = 0; c < 4; ++c) {
                const float a = sourceValue(c, kAOriginX, kAOriginY, x, y);
                const float want = (m == 0.f) ? 0.f : (m == 1.f) ? a
                                                                 : (a * m + (1.f - m) * 0.f);
                ASSERT_NEAR(want, got[c], 1e-6f) << "channel " << c << " at (" << x << ", " << y << ")";
            }
        }
    }
}

TEST_F(NativeKeyMixTest, SetBBoxChoosesTheRegion)
{
    resetProject();
    NodePtr keyMix = makeKeyMix(true, true);
    ASSERT_TRUE(bool(keyMix));

    EXPECT_EQ(RectI(0, 0, kParitySourceWidth + kAOriginX, kParitySourceHeight + kAOriginY), pixelRoD(keyMix));
    ASSERT_TRUE(setKnobValue(keyMix, kKeyMixParamSetBBox, kKeyMixBBoxB));
    EXPECT_EQ(RectI(0, 0, kParitySourceWidth, kParitySourceHeight), pixelRoD(keyMix));
    ASSERT_TRUE(setKnobValue(keyMix, kKeyMixParamSetBBox, kKeyMixBBoxA));
    EXPECT_EQ(RectI(kAOriginX, kAOriginY, kParitySourceWidth + kAOriginX, kParitySourceHeight + kAOriginY), pixelRoD(keyMix));

    // A mix of 0 is B, whatever setBBox says.
    ASSERT_TRUE(setKnobValues(keyMix, "mix", { 0. }));
    EXPECT_EQ(RectI(0, 0, kParitySourceWidth, kParitySourceHeight), pixelRoD(keyMix));
}

TEST_F(NativeKeyMixTest, UsesTheHostChannelAndMaskKnobs)
{
    resetProject();
    NodePtr keyMix = makeKeyMix(true, true);
    ASSERT_TRUE(bool(keyMix));

    for (const char* name : { "processR", "processG", "processB", "processA" }) {
        EXPECT_FALSE(bool(keyMix->getKnobByName(name))) << name;
    }
    EXPECT_TRUE(bool(keyMix->getKnobByName(kNodeParamChannelSet)));
    EXPECT_TRUE(bool(keyMix->getKnobByName("enableMask_Mask")));
}
