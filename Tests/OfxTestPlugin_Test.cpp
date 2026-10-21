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
#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobLayerSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/ParallelRenderArgs.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/Transform.h"
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
    { kNatronTestRGBGeneratorPluginID, kNatronTestRGBGeneratorMajor },
    { kNatronTestRGBPointPluginID, kNatronTestRGBPointMajor },
    { kNatronTestMultiPlanePluginID, kNatronTestMultiPlaneMajor },
    { kNatronTestMatrixWarpPluginID, kNatronTestMatrixWarpMajor },
    { kNatronTestFrameWindowPluginID, kNatronTestFrameWindowMajor },
    { kNatronTestOCIOConfigPluginID, kNatronTestOCIOConfigMajor },
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

    // A matrix warp translating by (tx, ty) in canonical coordinates, which at scale 1 and a
    // pixel aspect ratio of 1 is the same translation in pixels.
    NodePtr createTranslation(double tx,
                              double ty)
    {
        NodePtr warp = createNode(QString::fromUtf8(kNatronTestMatrixWarpPluginID), kNatronTestMatrixWarpMajor);
        if (warp) {
            EXPECT_TRUE(setKnobValues(warp, kNatronTestMatrixWarpParamRow1, { 1., 0., tx }));
            EXPECT_TRUE(setKnobValues(warp, kNatronTestMatrixWarpParamRow2, { 0., 1., ty }));
        }

        return warp;
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
const RGBA kZero = { 0.f, 0.f, 0.f, 0.f };

// Color RGBA (1, 0, 0, 1), diffuse RGB (0, 1, 0) and specular RGB (0, 0, 1), constant over 8x8.
const char* const kThreeLayersFixture = NATRON_TESTS_FIXTURES_DIR "/flat-three-layers.exr";

bool
contains(const std::vector<std::string>& ids,
         const std::string& id)
{
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

std::vector<std::string>
layerIDs(const std::list<ImageLayerDesc>& layers)
{
    std::vector<std::string> ids;

    for (std::list<ImageLayerDesc>::const_iterator it = layers.begin(); it != layers.end(); ++it) {
        ids.push_back(it->getLayerID());
    }

    return ids;
}

std::vector<std::string>
optionIDs(const NodePtr& node,
          const std::string& choiceName)
{
    std::vector<std::string> ids;
    KnobChoice* choice = dynamic_cast<KnobChoice*>(node->getKnobByName(choiceName).get());

    if (choice) {
        const std::vector<ChoiceOption> entries = choice->getEntries_mt_safe();
        for (std::size_t i = 0; i < entries.size(); ++i) {
            ids.push_back(entries[i].id);
        }
    }

    return ids;
}

std::string
activeOptionID(const NodePtr& node,
               const std::string& choiceName)
{
    KnobChoice* choice = dynamic_cast<KnobChoice*>(node->getKnobByName(choiceName).get());

    return choice ? choice->getActiveEntry().id : std::string();
}

int
inputNamed(const NodePtr& node,
           const std::string& label)
{
    for (int i = 0; i < node->getNInputs(); ++i) {
        if (node->getInputLabel(i) == label) {
            return i;
        }
    }

    return -1;
}

void
expectInside(const RenderedPlane& plane,
             const RectI& inside,
             const RGBA& color,
             const std::string& what)
{
    for (int y = plane.window.y1; y < plane.window.y2; ++y) {
        for (int x = plane.window.x1; x < plane.window.x2; ++x) {
            const bool in = (x >= inside.x1) && (x < inside.x2) && (y >= inside.y1) && (y < inside.y2);
            const RGBA expected = in ? color : kZero;
            const RGBA p = pixelAt(plane, x, y);
            EXPECT_FLOAT_EQ(expected.r, p.r) << what << " R at (" << x << ", " << y << ")";
            EXPECT_FLOAT_EQ(expected.g, p.g) << what << " G at (" << x << ", " << y << ")";
            EXPECT_FLOAT_EQ(expected.b, p.b) << what << " B at (" << x << ", " << y << ")";
            EXPECT_FLOAT_EQ(expected.a, p.a) << what << " A at (" << x << ", " << y << ")";
        }
    }
}

void
expectTranslation(const Transform::Matrix3x3& m,
                  double tx,
                  double ty,
                  const std::string& what)
{
    EXPECT_DOUBLE_EQ(1., m.a) << what;
    EXPECT_DOUBLE_EQ(0., m.b) << what;
    EXPECT_DOUBLE_EQ(tx, m.c) << what;
    EXPECT_DOUBLE_EQ(0., m.d) << what;
    EXPECT_DOUBLE_EQ(1., m.e) << what;
    EXPECT_DOUBLE_EQ(ty, m.f) << what;
    EXPECT_DOUBLE_EQ(0., m.g) << what;
    EXPECT_DOUBLE_EQ(0., m.h) << what;
    EXPECT_DOUBLE_EQ(1., m.i) << what;
}

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

// The plug-in rebuilds its plane and channel menus from the planes on its input, and the host's
// twins of those menus list the same layers.
TEST_F(OfxTestPluginTest, MultiPlaneChoicesListTheInputsLayers)
{
    NodePtr reader = createWorkingSpaceRead(kThreeLayersFixture);
    NodePtr multiPlane = createNode(QString::fromUtf8(kNatronTestMultiPlanePluginID), kNatronTestMultiPlaneMajor);
    ASSERT_TRUE(reader && multiPlane);
    EXPECT_TRUE(multiPlane->getEffectInstance()->isMultiPlanar());
    EXPECT_FALSE(bool(multiPlane->getLayerKnob()));
    connectNodes(reader, multiPlane, 0, true);

    const std::vector<std::string> planes = optionIDs(multiPlane, kNatronTestMultiPlaneParamInputPlane);
    EXPECT_TRUE(contains(planes, kNatronColorLayerID));
    EXPECT_TRUE(contains(planes, "diffuse"));
    EXPECT_TRUE(contains(planes, "specular"));
    const std::vector<std::string> channels = optionIDs(multiPlane, kNatronTestMultiPlaneParamByChannel);
    EXPECT_TRUE(contains(channels, std::string(kNatronColorLayerID) + ".A"));
    EXPECT_TRUE(contains(channels, "diffuse.G"));
    EXPECT_TRUE(contains(channels, "specular.B"));
    EXPECT_TRUE(contains(channels, "1"));

    KnobLayerSelectPtr layerTwin = std::dynamic_pointer_cast<KnobLayerSelect>(multiPlane->getKnobByName("hostInputLayer"));
    KnobChannelSelectPtr channelTwin = std::dynamic_pointer_cast<KnobChannelSelect>(multiPlane->getKnobByName("hostUnPremultByChannel"));
    ASSERT_TRUE(layerTwin && channelTwin);
    EXPECT_TRUE(layerTwin->getAllowAll());
    const KnobIPtr twins[2] = { layerTwin, channelTwin };
    for (const KnobIPtr& twin : twins) {
        std::list<ImageLayerDesc> listed;
        multiPlane->listLayersForKnob(twin, &listed);
        const std::vector<std::string> ids = layerIDs(listed);
        EXPECT_TRUE(contains(ids, "diffuse")) << twin->getName();
        EXPECT_TRUE(contains(ids, "specular")) << twin->getName();
    }
}

// Multiplying the diffuse plane by diffuse.R, 0 everywhere, zeroes it, while Color, which the
// plug-in passes through, keeps its (1, 0, 0, 1).
TEST_F(OfxTestPluginTest, MultiPlaneMultipliesOnlyTheChosenPlane)
{
    NodePtr reader = createWorkingSpaceRead(kThreeLayersFixture);
    NodePtr multiPlane = createNode(QString::fromUtf8(kNatronTestMultiPlanePluginID), kNatronTestMultiPlaneMajor);
    ASSERT_TRUE(reader && multiPlane);
    connectNodes(reader, multiPlane, 0, true);

    KnobLayerSelectPtr layerTwin = std::dynamic_pointer_cast<KnobLayerSelect>(multiPlane->getKnobByName("hostInputLayer"));
    KnobChannelSelectPtr channelTwin = std::dynamic_pointer_cast<KnobChannelSelect>(multiPlane->getKnobByName("hostUnPremultByChannel"));
    ASSERT_TRUE(layerTwin && channelTwin);
    layerTwin->setLayer("diffuse");
    channelTwin->set("diffuse.R");
    EXPECT_EQ(std::string("diffuse"), activeOptionID(multiPlane, kNatronTestMultiPlaneParamInputPlane));
    EXPECT_EQ(std::string("diffuse.R"), activeOptionID(multiPlane, kNatronTestMultiPlaneParamByChannel));

    std::list<ImageLayerDesc> listed;
    multiPlane->listLayersForKnob(layerTwin, &listed);
    std::list<ImageLayerDesc> layers(1, ImageLayerDesc::getRGBAComponents());
    for (std::list<ImageLayerDesc>::const_iterator it = listed.begin(); it != listed.end(); ++it) {
        if (it->getLayerID() == "diffuse") {
            layers.push_back(*it);
        }
    }
    ASSERT_EQ(2u, layers.size());

    std::vector<RenderedPlane> planes;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(multiPlane, 1., ViewIdx(0), 0, RectI(0, 0, kWindowSize, kWindowSize), layers, &planes, &error)) << error;
    ASSERT_EQ(2u, planes.size());
    const RGBA red = { 1.f, 0.f, 0.f, 1.f };
    expectEverywhere(planes[0], red, "Color");
    expectEverywhere(planes[1], kZero, "diffuse");
}

// With UV unconnected the warp is its matrix alone, which the host is given in pixels against the
// Source.
TEST_F(OfxTestPluginTest, MatrixWarpReportsItsMatrixAsATransform)
{
    NodePtr source = createConstant(kColorA);
    NodePtr warp = createTranslation(2., 1.);
    ASSERT_TRUE(source && warp);
    const int sourceInput = inputNamed(warp, kOfxImageEffectSimpleSourceClipName);
    ASSERT_GE(sourceInput, 0);
    connectNodes(source, warp, sourceInput, true);

    EffectInstancePtr effect = warp->getEffectInstance();
    EXPECT_TRUE(effect->getCanTransform());
    EXPECT_TRUE(warp->getCurrentCanTransform());

    EffectInstancePtr inputToTransform;
    Transform::Matrix3x3 matrix;
    ASSERT_EQ(eStatusOK, effect->getTransform_public(1., RenderScale::identity, false, ViewIdx(0), &inputToTransform, &matrix));
    EXPECT_EQ(source->getEffectInstance(), inputToTransform);
    expectTranslation(matrix, 2., 1., "reported transform");
}

// The downstream warp reaches past the upstream one: it fetches the upstream warp's input and
// applies the upstream matrix itself. However the host renders it, the Generator's 3x2 format
// lands moved by the sum of the two translations.
TEST_F(OfxTestPluginTest, MatrixWarpsConcatenate)
{
    NodePtr generator = createNode(QString::fromUtf8(kNatronTestGeneratorPluginID), kNatronTestGeneratorMajor);
    NodePtr first = createTranslation(2., 1.);
    NodePtr second = createTranslation(1., 0.);
    ASSERT_TRUE(generator && first && second);
    ASSERT_TRUE(setKnobValues(generator, kNatronTestGeneratorParamColor, { kColorB.r, kColorB.g, kColorB.b, kColorB.a }));
    ASSERT_TRUE(setKnobValues(generator, kNatronParamFormatSize, { 3., 2. }));
    ASSERT_TRUE(setKnobValues(generator, kNatronParamFormatPar, { 1. }));
    ASSERT_TRUE(setKnobValue(generator, kNatronTestGeneratorParamExtent, kNatronTestGeneratorExtentFormat));
    const int sourceInput = inputNamed(second, kOfxImageEffectSimpleSourceClipName);
    ASSERT_GE(sourceInput, 0);
    connectNodes(generator, first, sourceInput, true);
    connectNodes(first, second, sourceInput, true);

    InputMatrixMap concatenated;
    second->getEffectInstance()->tryConcatenateTransforms(1., false, ViewIdx(0), RenderScale::identity, &concatenated);
    InputMatrixMap::const_iterator found = concatenated.find(sourceInput);
    ASSERT_TRUE(found != concatenated.end());
    EXPECT_EQ(first->getEffectInstance(), found->second.newInputEffect);
    EXPECT_EQ(generator->getEffectInstance(), first->getEffectInstance()->getInput(found->second.newInputNbToFetchFrom));
    ASSERT_TRUE(bool(found->second.cat));
    expectTranslation(*found->second.cat, 2., 1., "upstream matrix");

    RenderedPlane plane;
    ASSERT_TRUE(renderRGBA(second, RectI(0, 0, 8, 4), &plane));
    expectInside(plane, RectI(3, 1, 6, 3), kColorB, "two translations");
}

// The U/V/A channel twins list the layers of the UV input. A U offset of 1 from diffuse.G (1
// everywhere) moves the Source one pixel left, and the warp is then no longer a transform.
TEST_F(OfxTestPluginTest, MatrixWarpChannelTwinsReadTheUVInput)
{
    NodePtr generator = createNode(QString::fromUtf8(kNatronTestGeneratorPluginID), kNatronTestGeneratorMajor);
    NodePtr uv = createWorkingSpaceRead(kThreeLayersFixture);
    NodePtr warp = createTranslation(0., 0.);
    ASSERT_TRUE(generator && uv && warp);
    ASSERT_TRUE(setKnobValues(generator, kNatronTestGeneratorParamColor, { kColorB.r, kColorB.g, kColorB.b, kColorB.a }));
    ASSERT_TRUE(setKnobValues(generator, kNatronParamFormatSize, { 3., 2. }));
    ASSERT_TRUE(setKnobValues(generator, kNatronParamFormatPar, { 1. }));
    ASSERT_TRUE(setKnobValue(generator, kNatronTestGeneratorParamExtent, kNatronTestGeneratorExtentFormat));
    const int sourceInput = inputNamed(warp, kOfxImageEffectSimpleSourceClipName);
    const int uvInput = inputNamed(warp, kNatronTestMatrixWarpClipUV);
    ASSERT_GE(sourceInput, 0);
    ASSERT_GE(uvInput, 0);
    connectNodes(generator, warp, sourceInput, true);
    connectNodes(uv, warp, uvInput, true);

    KnobChannelSelectPtr channelU = std::dynamic_pointer_cast<KnobChannelSelect>(warp->getKnobByName("hostChannelU"));
    KnobChannelSelectPtr channelV = std::dynamic_pointer_cast<KnobChannelSelect>(warp->getKnobByName("hostChannelV"));
    ASSERT_TRUE(channelU && channelV);
    EXPECT_EQ(std::string(kNatronColorViewRGBA) + ".R", channelU->get());
    std::list<ImageLayerDesc> listed;
    warp->listLayersForKnob(channelU, &listed);
    EXPECT_TRUE(contains(layerIDs(listed), "diffuse"));
    EXPECT_TRUE(contains(optionIDs(warp, kNatronTestMatrixWarpParamChannelU), "diffuse.G"));

    channelU->set("diffuse.G");
    channelV->setNone();
    EXPECT_EQ(std::string("diffuse.G"), activeOptionID(warp, kNatronTestMatrixWarpParamChannelU));
    EXPECT_EQ(std::string("0"), activeOptionID(warp, kNatronTestMatrixWarpParamChannelV));

    EffectInstancePtr inputToTransform;
    Transform::Matrix3x3 matrix;
    EXPECT_NE(eStatusOK, warp->getEffectInstance()->getTransform_public(1., RenderScale::identity, false, ViewIdx(0), &inputToTransform, &matrix));

    RenderedPlane plane;
    ASSERT_TRUE(renderRGBA(warp, RectI(0, 0, 4, 3), &plane));
    expectInside(plane, RectI(0, 0, 2, 2), kColorB, "U offset of 1");
}

// Relative, the range is placed on the time less the offset; absolute, it is the frames named.
TEST_F(OfxTestPluginTest, FrameWindowAsksForItsKnobsFrameRange)
{
    NodePtr source = createConstant(kColorA);
    NodePtr window = createNode(QString::fromUtf8(kNatronTestFrameWindowPluginID), kNatronTestFrameWindowMajor);
    ASSERT_TRUE(source && window);
    connectNodes(source, window, 0, true);
    ASSERT_TRUE(setKnobValues(window, kNatronTestFrameWindowParamFrameRange, { -3., 1. }));
    ASSERT_TRUE(setKnobValues(window, kNatronTestFrameWindowParamTimeOffset, { 2. }));

    EffectInstancePtr effect = window->getEffectInstance();
    const double time = 10.;
    const auto expectFrames = [&](double first, double last, const std::string& what) {
        const FramesNeededMap needed = effect->getFramesNeeded_public(effect->getRenderHash(), time, ViewIdx(0), 0);
        FramesNeededMap::const_iterator input = needed.find(0);
        ASSERT_TRUE(input != needed.end()) << what;
        FrameRangesMap::const_iterator view = input->second.find(ViewIdx(0));
        ASSERT_TRUE(view != input->second.end()) << what;
        ASSERT_EQ(1u, view->second.size()) << what;
        EXPECT_DOUBLE_EQ(first, view->second[0].min) << what;
        EXPECT_DOUBLE_EQ(last, view->second[0].max) << what;
    };
    expectFrames(5., 9., "relative");

    ASSERT_TRUE(setKnobValues(window, kNatronTestFrameWindowParamAbsolute, { 1. }));
    expectFrames(-3., 1., "absolute");
}

// A one-frame range is an identity of the Source at that frame, as a TimeOffset is.
TEST_F(OfxTestPluginTest, FrameWindowOfOneFrameIsAnIdentityAtThatFrame)
{
    NodePtr source = createConstant(kColorA);
    NodePtr window = createNode(QString::fromUtf8(kNatronTestFrameWindowPluginID), kNatronTestFrameWindowMajor);
    ASSERT_TRUE(source && window);
    connectNodes(source, window, 0, true);
    ASSERT_TRUE(setKnobValues(window, kNatronTestFrameWindowParamTimeOffset, { 3. }));

    EffectInstancePtr effect = window->getEffectInstance();
    double inputTime = 0.;
    ViewIdx inputView(0);
    int inputNb = -1;
    EXPECT_TRUE(effect->isIdentity_public(false, effect->getRenderHash(), 10., RenderScale::identity, RectI(0, 0, kWindowSize, kWindowSize), ViewIdx(0), &inputTime, &inputView, &inputNb));
    EXPECT_EQ(0, inputNb);
    EXPECT_DOUBLE_EQ(7., inputTime);
}

// Red keyed to 0, 0.5 and 1 on frames 0, 1 and 2 averages to 0.5 over frames 0 to 2.
TEST_F(OfxTestPluginTest, FrameWindowAveragesTheFramesInItsRange)
{
    NodePtr source = createConstant(kColorA);
    NodePtr window = createNode(QString::fromUtf8(kNatronTestFrameWindowPluginID), kNatronTestFrameWindowMajor);
    ASSERT_TRUE(source && window);
    KnobColor* color = dynamic_cast<KnobColor*>(source->getKnobByName(kConstantParamColor).get());
    ASSERT_TRUE(color != NULL);
    color->setValueAtTime(0., 0., ViewSpec::all(), 0);
    color->setValueAtTime(1., 0.5, ViewSpec::all(), 0);
    color->setValueAtTime(2., 1., ViewSpec::all(), 0);
    connectNodes(source, window, 0, true);
    ASSERT_TRUE(setKnobValues(window, kNatronTestFrameWindowParamFrameRange, { -1., 1. }));

    RenderedPlane plane;
    ASSERT_TRUE(renderRGBA(window, RectI(0, 0, kWindowSize, kWindowSize), &plane));
    const RGBA average = { 0.5f, kColorA.g, kColorA.b, kColorA.a };
    expectEverywhere(plane, average, "frames 0 to 2");
}

// The project pushes its config and working space into the fixture's params and hides the config,
// over whatever the node held.
TEST_F(OfxTestPluginTest, ProjectPushesItsOCIOConfigIntoTheFixture)
{
    ProjectPtr project = getApp()->getProject();
    NodePtr node = createNode(QString::fromUtf8(kNatronTestOCIOConfigPluginID), kNatronTestOCIOConfigMajor);
    ASSERT_TRUE(bool(node));
    KnobStringBasePtr configFile = std::dynamic_pointer_cast<KnobStringBase>(node->getKnobByName(kNatronTestOCIOConfigParamConfigFile));
    KnobStringBasePtr workingSpace = std::dynamic_pointer_cast<KnobStringBase>(node->getKnobByName(kNatronTestOCIOConfigParamWorkingSpace));
    ASSERT_TRUE(configFile && workingSpace);
    const std::string source = project->getOCIOConfigSource();
    const std::string space = project->getWorkingColorSpace();
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(space.empty());

    EXPECT_EQ(source, configFile->getValue());
    EXPECT_TRUE(configFile->getIsSecret());
    EXPECT_EQ(space, workingSpace->getValue());

    configFile->setValue(std::string("elsewhere.ocio"));
    configFile->setSecret(false);
    workingSpace->setValue(std::string("elsewhere"));
    project->pushOCIOConfigToNode(node);
    EXPECT_EQ(source, configFile->getValue());
    EXPECT_TRUE(configFile->getIsSecret());
    EXPECT_EQ(space, workingSpace->getValue());
}
