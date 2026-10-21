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

// clang-format off
CLANG_DIAG_OFF(unknown-pragmas)
CLANG_DIAG_OFF(tautological-undefined-compare)
#include <ofxhPluginCache.h>
#include <ofxhImageEffectAPI.h>
CLANG_DIAG_ON(tautological-undefined-compare)
CLANG_DIAG_ON(unknown-pragmas)
// clang-format on

#include <ofxNatron.h>

#include "BaseTest.h"
#include "NativeParity.h"
#include "OfxTestPlugins/natronTest/NatronTestPluginIDs.h"
#include "RenderBothWays.h"
#include "TestOfxPluginPath.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

struct TestPluginVersion {
    const char* id;
    int major;
};

const TestPluginVersion kTestPlugins[] = {
    { kNatronTestTwoInputCopyPluginID, kNatronTestTwoInputCopyMajor },
    { kNatronTestGeneratorPluginID, kNatronTestGeneratorMajor },
    { kNatronTestPointMatrixPluginID, kNatronTestPointMatrixMajor },
};

const int kWindowSize = 4;

struct RGBA {
    float r, g, b, a;
};

class OfxTestPluginTest
    : public BaseTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        BaseTest::SetUp();
        getApp()->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        getApp()->getProject()->reset(false, true);
        BaseTest::TearDown();
    }

    NodePtr createConstant(const RGBA& color)
    {
        NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);
        if (constant) {
            setKnobValues(constant, kConstantParamColor, { color.r, color.g, color.b, color.a });
        }

        return constant;
    }

    // Renders the RGBA plane of `node` over `window`; false, with a test failure, when it cannot.
    bool renderRGBA(const NodePtr& node,
                    const RectI& window,
                    RenderedPlane* plane)
    {
        std::vector<RenderedPlane> planes;
        std::string error;
        if (!renderNodePlanesDirect(node, 1., ViewIdx(0), 0, window, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error)) {
            ADD_FAILURE() << node->getPluginID() << ": " << error;

            return false;
        }
        if ((planes.size() != 1) || planes[0].pixels.empty()) {
            ADD_FAILURE() << node->getPluginID() << ": no RGBA plane rendered";

            return false;
        }
        *plane = planes[0];

        return true;
    }
};

RGBA
pixelAt(const RenderedPlane& plane,
        int x,
        int y)
{
    static const char* const names[4] = { "R", "G", "B", "A" };
    const int width = plane.window.width();
    const std::size_t nChannels = plane.channels.size();
    const std::size_t offset = ((std::size_t)(y - plane.window.y1) * width + (x - plane.window.x1)) * nChannels;
    float values[4] = { 0.f, 0.f, 0.f, 0.f };

    for (int c = 0; c < 4; ++c) {
        std::vector<std::string>::const_iterator found = std::find(plane.channels.begin(), plane.channels.end(), std::string(names[c]));
        if (found != plane.channels.end()) {
            values[c] = plane.pixels[offset + (found - plane.channels.begin())];
        }
    }
    const RGBA ret = { values[0], values[1], values[2], values[3] };

    return ret;
}

void
expectEverywhere(const RenderedPlane& plane,
                 const RGBA& expected,
                 const std::string& what)
{
    for (int y = plane.window.y1; y < plane.window.y2; ++y) {
        for (int x = plane.window.x1; x < plane.window.x2; ++x) {
            const RGBA p = pixelAt(plane, x, y);
            EXPECT_FLOAT_EQ(expected.r, p.r) << what << " R at (" << x << ", " << y << ")";
            EXPECT_FLOAT_EQ(expected.g, p.g) << what << " G at (" << x << ", " << y << ")";
            EXPECT_FLOAT_EQ(expected.b, p.b) << what << " B at (" << x << ", " << y << ")";
            EXPECT_FLOAT_EQ(expected.a, p.a) << what << " A at (" << x << ", " << y << ")";
        }
    }
}

const RGBA kColorA = { 0.25f, 0.5f, 0.75f, 1.f };
const RGBA kColorB = { 1.f, 0.125f, 0.5f, 0.5f };

} // anonymous namespace

// Each fixture is one OFX plug-in of the natronTest bundle, loaded from the test-only directory
// the test main appends to OFX_PLUGIN_PATH, at a single major version.
TEST_F(OfxTestPluginTest, EveryTestPluginIsLoadedOnceFromTheTestBundle)
{
    const PluginsMap& plugins = appPTR->getPluginsList();
    const std::string testDir = testOfxPluginDir().toStdString();

    for (const TestPluginVersion& expected : kTestPlugins) {
        PluginsMap::const_iterator found = plugins.find(expected.id);
        ASSERT_TRUE(found != plugins.end()) << expected.id << " is not loaded; OFX_PLUGIN_PATH=" << qgetenv("OFX_PLUGIN_PATH").constData();
        ASSERT_EQ(1u, found->second.size()) << expected.id;
        const Plugin* plugin = *found->second.begin();
        EXPECT_EQ(expected.major, plugin->getMajorVersion()) << expected.id;

        OFX::Host::ImageEffect::ImageEffectPlugin* ofxPlugin = plugin->getOfxPlugin();
        ASSERT_TRUE(ofxPlugin != NULL) << expected.id << " is not an OFX plug-in";
        const std::string bundlePath = ofxPlugin->getBinary()->getBundlePath();
        EXPECT_EQ(0u, bundlePath.find(testDir)) << expected.id << " was loaded from " << bundlePath << ", not from " << testDir;
    }
}

// passThrough on routes by identity, as Switch does; off, the plug-in renders a copy of the
// chosen input, as KeyMix and CopyRectangle render rather than route.
TEST_F(OfxTestPluginTest, TwoInputCopyOutputsTheInputWhichChooses)
{
    NodePtr a = createConstant(kColorA);
    NodePtr b = createConstant(kColorB);
    NodePtr copy = createNode(QString::fromUtf8(kNatronTestTwoInputCopyPluginID), kNatronTestTwoInputCopyMajor);
    ASSERT_TRUE(a && b && copy);
    connectNodes(a, copy, 0, true);
    connectNodes(b, copy, 1, true);

    const RectI window(0, 0, kWindowSize, kWindowSize);
    for (int passThrough = 1; passThrough >= 0; --passThrough) {
        ASSERT_TRUE(setKnobValues(copy, kNatronTestTwoInputCopyParamPassThrough, { (double)passThrough }));
        const std::string mode = passThrough ? "pass-through" : "rendered copy";

        ASSERT_TRUE(setKnobValues(copy, kNatronTestTwoInputCopyParamWhich, { 0. }));
        RenderedPlane plane;
        ASSERT_TRUE(renderRGBA(copy, window, &plane));
        expectEverywhere(plane, kColorA, mode + ", which 0");

        ASSERT_TRUE(setKnobValues(copy, kNatronTestTwoInputCopyParamWhich, { 1. }));
        ASSERT_TRUE(renderRGBA(copy, window, &plane));
        expectEverywhere(plane, kColorB, mode + ", which 1");
    }
}

// With the Format extent the colour covers exactly the chosen size, and the region past it is
// outside the node's region of definition, so it renders as zero.
TEST_F(OfxTestPluginTest, GeneratorFillsItsColourOverItsFormat)
{
    NodePtr generator = createNode(QString::fromUtf8(kNatronTestGeneratorPluginID), kNatronTestGeneratorMajor);
    ASSERT_TRUE(bool(generator));
    EXPECT_TRUE(generator->getEffectInstance()->isGenerator());
    ASSERT_TRUE(setKnobValues(generator, kNatronTestGeneratorParamColor, { kColorB.r, kColorB.g, kColorB.b, kColorB.a }));

    RenderedPlane plane;
    ASSERT_TRUE(renderRGBA(generator, RectI(0, 0, kWindowSize, kWindowSize), &plane));
    expectEverywhere(plane, kColorB, "project extent");

    const int formatWidth = 3;
    const int formatHeight = 2;
    ASSERT_TRUE(setKnobValues(generator, kNatronParamFormatSize, { (double)formatWidth, (double)formatHeight }));
    ASSERT_TRUE(setKnobValues(generator, kNatronParamFormatPar, { 1. }));
    ASSERT_TRUE(setKnobValue(generator, kNatronTestGeneratorParamExtent, kNatronTestGeneratorExtentFormat));

    ASSERT_TRUE(renderRGBA(generator, RectI(0, 0, formatWidth + 2, formatHeight + 2), &plane));
    const RGBA zero = { 0.f, 0.f, 0.f, 0.f };
    for (int y = plane.window.y1; y < plane.window.y2; ++y) {
        for (int x = plane.window.x1; x < plane.window.x2; ++x) {
            const bool inside = (x < formatWidth) && (y < formatHeight);
            const RGBA expected = inside ? kColorB : zero;
            const RGBA p = pixelAt(plane, x, y);
            EXPECT_FLOAT_EQ(expected.r, p.r) << "R at (" << x << ", " << y << ")";
            EXPECT_FLOAT_EQ(expected.g, p.g) << "G at (" << x << ", " << y << ")";
            EXPECT_FLOAT_EQ(expected.b, p.b) << "B at (" << x << ", " << y << ")";
            EXPECT_FLOAT_EQ(expected.a, p.a) << "A at (" << x << ", " << y << ")";
        }
    }
}

// The rows swizzle R <- G, G <- B, B <- R and halve alpha; every weight is 0, 1 or a half, so the
// products are exact.
TEST_F(OfxTestPluginTest, PointMatrixAppliesItsMatrixAndMix)
{
    NodePtr source = createConstant(kColorA);
    NodePtr matrix = createNode(QString::fromUtf8(kNatronTestPointMatrixPluginID), kNatronTestPointMatrixMajor);
    ASSERT_TRUE(source && matrix);
    ASSERT_EQ(kNatronTestPointMatrixMajor, matrix->getPlugin()->getMajorVersion());
    connectNodes(source, matrix, 0, true);

    ASSERT_TRUE(setKnobValues(matrix, kNatronTestPointMatrixParamOutputRed, { 0., 1., 0., 0. }));
    ASSERT_TRUE(setKnobValues(matrix, kNatronTestPointMatrixParamOutputGreen, { 0., 0., 1., 0. }));
    ASSERT_TRUE(setKnobValues(matrix, kNatronTestPointMatrixParamOutputBlue, { 1., 0., 0., 0. }));
    ASSERT_TRUE(setKnobValues(matrix, kNatronTestPointMatrixParamOutputAlpha, { 0., 0., 0., 0.5 }));

    const RectI window(0, 0, kWindowSize, kWindowSize);
    const RGBA swizzled = { kColorA.g, kColorA.b, kColorA.r, kColorA.a * 0.5f };
    RenderedPlane plane;
    ASSERT_TRUE(renderRGBA(matrix, window, &plane));
    expectEverywhere(plane, swizzled, "mix 1");

    ASSERT_TRUE(setKnobValues(matrix, kNatronTestPointMatrixParamMix, { 0.5 }));
    const RGBA halfway = { (kColorA.r + swizzled.r) * 0.5f, (kColorA.g + swizzled.g) * 0.5f,
                           (kColorA.b + swizzled.b) * 0.5f, (kColorA.a + swizzled.a) * 0.5f };
    ASSERT_TRUE(renderRGBA(matrix, window, &plane));
    expectEverywhere(plane, halfway, "mix 0.5");
}
