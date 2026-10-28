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
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Transform/AdjustBBox.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

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

// The source's value at pixel (x, y): with nearest, the nearest pixel of its region's;
// otherwise zero outside it.
float
sourceValue(int channel,
            int x,
            int y,
            bool nearest)
{
    if (nearest) {
        x = std::min(std::max(x, 0), kParitySourceWidth - 1);
        y = std::min(std::max(y, 0), kParitySourceHeight - 1);
    } else if ((x < 0) || (y < 0) || (x >= kParitySourceWidth) || (y >= kParitySourceHeight)) {
        return 0.f;
    }

    return paritySourceColorValue(channel, x, y, kTime);
}

} // namespace

class NativeAdjustBBoxTest
    : public BaseTest {
protected:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeAdjustBBox64x48", 1.));
    }

    NodePtr makeAdjustBBox(double padX,
                           double padY,
                           bool nearest)
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
        EXPECT_TRUE(bool(source));
        if (!source) {
            return NodePtr();
        }
        setParitySourceOrigin(source, 0, 0);
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_ADJUSTBBOX), PLUGIN_MAJOR_NATRON_ADJUSTBBOX);
        EXPECT_TRUE(isNative(node));
        if (!node) {
            return node;
        }
        connectNodes(source, node, 0, true);
        EXPECT_TRUE(setKnobValues(node, kAdjustBBoxParamNumPixels, { padX, padY }));
        EXPECT_TRUE(setKnobValue(node, kAdjustBBoxParamBoundary, nearest ? kAdjustBBoxBoundaryNearest : kAdjustBBoxBoundaryBlack));

        return node;
    }

    // Expects the region of definition and, over window, the source inside it, filled per the
    // boundary, and zero outside it.
    void expectAdjusted(const NodePtr& node,
                        const RectI& expectedRoD,
                        const RectI& window,
                        bool nearest)
    {
        const RectI rod = pixelRoD(node);
        EXPECT_EQ(expectedRoD, rod);
        RenderedPlane plane;
        ASSERT_TRUE(renderRGBA(node, window, &plane));
        for (int y = window.y1; y < window.y2; ++y) {
            for (int x = window.x1; x < window.x2; ++x) {
                const bool inRoD = (x >= rod.x1) && (x < rod.x2) && (y >= rod.y1) && (y < rod.y2);
                const float* got = pixelAt(plane, x, y);
                for (int c = 0; c < 4; ++c) {
                    const float want = inRoD ? sourceValue(c, x, y, nearest) : 0.f;
                    ASSERT_EQ(want, got[c]) << "channel " << c << " at (" << x << ", " << y << ")";
                }
            }
        }
    }
};

TEST_F(NativeAdjustBBoxTest, Identity)
{
    expectNativeIdentity(PLUGINID_NATRON_ADJUSTBBOX, PLUGIN_MAJOR_NATRON_ADJUSTBBOX, "AdjustBBox");
}

TEST_F(NativeAdjustBBoxTest, ZeroPadIsTheSource)
{
    resetProject();
    for (bool nearest : { false, true }) {
        SCOPED_TRACE(nearest ? "nearest" : "black");
        NodePtr node = makeAdjustBBox(0., 0., nearest);
        ASSERT_TRUE(bool(node));
        expectAdjusted(node, RectI(0, 0, kParitySourceWidth, kParitySourceHeight), RectI(-4, -4, kParitySourceWidth + 4, kParitySourceHeight + 4), false);
    }
}

TEST_F(NativeAdjustBBoxTest, PadWithBlack)
{
    resetProject();
    NodePtr node = makeAdjustBBox(5., 3., false);
    ASSERT_TRUE(bool(node));
    expectAdjusted(node, RectI(-5, -3, kParitySourceWidth + 5, kParitySourceHeight + 3), RectI(-8, -6, kParitySourceWidth + 8, kParitySourceHeight + 6), false);
}

TEST_F(NativeAdjustBBoxTest, PadWithNearest)
{
    resetProject();
    NodePtr node = makeAdjustBBox(5., 3., true);
    ASSERT_TRUE(bool(node));
    expectAdjusted(node, RectI(-5, -3, kParitySourceWidth + 5, kParitySourceHeight + 3), RectI(-8, -6, kParitySourceWidth + 8, kParitySourceHeight + 6), true);
}

TEST_F(NativeAdjustBBoxTest, NegativeValuesCrop)
{
    resetProject();
    for (bool nearest : { false, true }) {
        SCOPED_TRACE(nearest ? "nearest" : "black");
        NodePtr node = makeAdjustBBox(-4., -2., nearest);
        ASSERT_TRUE(bool(node));
        expectAdjusted(node, RectI(4, 2, kParitySourceWidth - 4, kParitySourceHeight - 2), RectI(0, 0, kParitySourceWidth, kParitySourceHeight), nearest);
    }
}

TEST_F(NativeAdjustBBoxTest, PadOneAxisAndCropTheOther)
{
    resetProject();
    NodePtr node = makeAdjustBBox(6., -3., true);
    ASSERT_TRUE(bool(node));
    expectAdjusted(node, RectI(-6, 3, kParitySourceWidth + 6, kParitySourceHeight - 3), RectI(-8, 0, kParitySourceWidth + 8, kParitySourceHeight), true);
}

TEST_F(NativeAdjustBBoxTest, CroppingEverythingLeavesNothing)
{
    resetProject();
    NodePtr node = makeAdjustBBox(-40., 0., false);
    ASSERT_TRUE(bool(node));
    const RectI rod = pixelRoD(node);
    EXPECT_TRUE((rod.x2 <= rod.x1) || (rod.y2 <= rod.y1)) << rod.x1 << " " << rod.y1 << " " << rod.x2 << " " << rod.y2;
}

// A window lying wholly in the border, away from the source, must still find its nearest edge
// pixels: rendering it alone gives what it holds in a render of the whole region.
TEST_F(NativeAdjustBBoxTest, NearestBorderDoesNotDependOnTheWindow)
{
    resetProject();
    NodePtr node = makeAdjustBBox(10., 8., true);
    ASSERT_TRUE(bool(node));
    const RectI corners[] = {
        RectI(-10, -8, -6, -4),
        RectI(kParitySourceWidth + 4, kParitySourceHeight + 3, kParitySourceWidth + 10, kParitySourceHeight + 8),
        RectI(20, -8, 30, -2),
    };
    for (const RectI& corner : corners) {
        expectAdjusted(node, RectI(-10, -8, kParitySourceWidth + 10, kParitySourceHeight + 8), corner, true);
    }
}
