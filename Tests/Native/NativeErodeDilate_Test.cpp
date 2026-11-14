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

#include <functional>
#include <iostream>
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
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/ErodeDilate.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/EdgeModeKnob.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

// Rectangular minimum and maximum are exact in float.
const ParityTolerance kTolerance = ParityTolerance::exact();

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

bool
setChannelSelect(const NodePtr& node,
                 const std::string& name,
                 const std::string& value)
{
    KnobChannelSelect* select = node ? dynamic_cast<KnobChannelSelect*>(node->getKnobByName(name).get()) : NULL;

    EXPECT_TRUE(select != NULL) << name;
    if (!select) {
        return false;
    }
    select->set(value);

    return true;
}

RectD
regionOfDefinition(const NodePtr& node,
                   double time,
                   const RenderScale& scale = RenderScale::identity)
{
    RectD rod;
    EffectInstancePtr effect = node ? node->getEffectInstance() : EffectInstancePtr();

    EXPECT_TRUE(bool(effect));
    if (!effect) {
        return rod;
    }
    // Called directly rather than through the cached action: a knob edit does not always change
    // the render hash.
    const StatusEnum stat = effect->getRegionOfDefinition(effect->getRenderHash(), time, scale, ViewIdx(0), &rod);
    EXPECT_NE(eStatusFailed, stat) << node->getPluginID();

    return rod;
}

} // namespace

class NativeErodeDilateTest
    : public BaseTest {
protected:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeErodeDilate64x48", 1.));
    }

    ParityPair makePair()
    {
        ParityPair pair = makeParityPair(getApp(), PLUGINID_NATRON_ERODEDILATE, PLUGIN_MAJOR_NATRON_ERODEDILATE);

        EXPECT_TRUE(bool(pair.native));
        EXPECT_TRUE(isNative(pair.native));
        if (pair.source) {
            setParitySourceOrigin(pair.source, 0, 0);
        }

        return pair;
    }

    void expectParity(const ParityPair& pair,
                      const std::string& caseName,
                      bool record)
    {
        const std::string fullName = std::string("ErodeDilate-") + caseName;

        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const ParityResult r = compareParity(pair, fullName, RectI(), mipmapLevel, kTolerance, record);
            EXPECT_TRUE(r.ok) << fullName << ", mipmap " << mipmapLevel << ": " << describe(r);
            EXPECT_GE(r.planesCompared, 1) << fullName;
            std::cout << "[ parity ] " << fullName << " mipmap " << mipmapLevel << ": planes " << r.planesCompared
                      << ", max abs diff " << r.maxAbsDiff << std::endl;
        }
    }

    void expectSameBothWays(const NodePtr& node,
                            const std::string& caseName)
    {
        std::vector<int> poolSizes;
        poolSizes.push_back(1);
        poolSizes.push_back(4);
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            std::vector<int> unplannedPulls;
            const RectI window = regionOfDefinition(node, kTime, RenderScale::fromMipmapLevel(mipmapLevel)).toPixelEnclosing(mipmapLevel, 1.);
            const RenderMismatch m = renderBothWaysDirect(node, kTime, ViewIdx(0), mipmapLevel, window, poolSizes, std::function<void()>(), 0.f, &unplannedPulls);
            EXPECT_FALSE(m.any) << caseName << ", mipmap " << mipmapLevel << ": " << describe(m);
            EXPECT_EQ(poolSizes.size(), unplannedPulls.size()) << caseName;
            for (std::size_t p = 0; p < unplannedPulls.size(); ++p) {
                EXPECT_EQ(0, unplannedPulls[p]) << caseName << ", mipmap " << mipmapLevel << ", pool " << poolSizes[p];
            }
        }
    }

    // The recorded cases, eroding at the sizes named, or dilating at them when `dilate` negates
    // them.
    void runSizes(bool dilate)
    {
        resetProject();
        ParityPair pair = makePair();
        ASSERT_TRUE(bool(pair.native));
        const double sign = dilate ? -1. : 1.;
        const std::string mode = dilate ? "dilate-" : "erode-";

        ASSERT_TRUE(setKnobOnBoth(pair, kErodeDilateParamSize, { sign, sign }));
        expectParity(pair, mode + "size-1x1", true);

        ASSERT_TRUE(setKnobOnBoth(pair, kErodeDilateParamSize, { sign * 5., 0. }));
        expectParity(pair, mode + "size-5x0", true);

        ASSERT_TRUE(setKnobOnBoth(pair, kErodeDilateParamSize, { sign * -3., sign * -3. }));
        expectParity(pair, mode + "size-neg3x3", true);
    }

    bool renderRGBA(const NodePtr& node,
                    const RectI& window,
                    std::vector<float>* pixels)
    {
        const std::list<ImageLayerDesc> layers(1, ImageLayerDesc::getRGBAComponents());
        std::vector<RenderedPlane> planes;
        std::string error;
        if (!renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &planes, &error) || (planes.size() != 1)) {
            ADD_FAILURE() << error;

            return false;
        }
        *pixels = planes[0].pixels;

        return true;
    }

    NodePtr makeNode(const NodePtr& source,
                     double sizeX,
                     double sizeY)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_ERODEDILATE), PLUGIN_MAJOR_NATRON_ERODEDILATE);

        EXPECT_TRUE(isNative(node));
        if (node) {
            connectNodes(source, node, 0, true);
            EXPECT_TRUE(setKnobValues(node, kErodeDilateParamSize, { sizeX, sizeY }));
        }

        return node;
    }
};

TEST_F(NativeErodeDilateTest, UnversionedRequestsGetTheNativeNode)
{
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_ERODEDILATE));
    ASSERT_TRUE(bool(node));
    EXPECT_TRUE(isNative(node));
    EXPECT_EQ(PLUGIN_MAJOR_NATRON_ERODEDILATE, node->getMajorVersion());
}

TEST_F(NativeErodeDilateTest, InputsAreSourceAndMask)
{
    resetProject();
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_ERODEDILATE), PLUGIN_MAJOR_NATRON_ERODEDILATE);
    ASSERT_TRUE(isNative(node));
    EXPECT_EQ(std::string("Source"), node->getInputLabel(0));
    EXPECT_EQ(std::string("Mask"), node->getInputLabel(1));
    EXPECT_TRUE(node->getEffectInstance()->isInputMask(1));
    EXPECT_TRUE(bool(node->getKnobByName("enableMask_Mask")));
    EXPECT_FALSE(bool(node->getKnobByName("hostUnPremultBy")));
}

TEST_F(NativeErodeDilateTest, SourceRegionOfInterestIdentityAndExpansion)
{
    ErodeDilateParams params;
    const RectI rect(10, 20, 30, 40);

    params.sizeX = -3;
    params.sizeY = 5;
    RectI roi = ErodeDilate::getSourceRoI(rect, RenderScale::identity, params);
    EXPECT_EQ(10 - 3, roi.x1);
    EXPECT_EQ(30 + 3, roi.x2);
    EXPECT_EQ(20 - 5, roi.y1);
    EXPECT_EQ(40 + 5, roi.y2);

    // ceil(3 * 0.5) = 2 at mipmap level 1.
    roi = ErodeDilate::getSourceRoI(rect, RenderScale::fromMipmapLevel(1), params);
    EXPECT_EQ(10 - 2, roi.x1);
    EXPECT_EQ(40 + 3, roi.y2);

    params.sizeX = 0;
    params.sizeY = 0;
    EXPECT_TRUE(ErodeDilate::paramsAreIdentity(RenderScale::identity, params));
    params.sizeX = 1;
    EXPECT_FALSE(ErodeDilate::paramsAreIdentity(RenderScale::identity, params));
    // floor(|size| * 0.5) = 0 is identity at mipmap level 1, whichever the sign.
    EXPECT_TRUE(ErodeDilate::paramsAreIdentity(RenderScale::fromMipmapLevel(1), params));
    params.sizeX = -1;
    EXPECT_FALSE(ErodeDilate::paramsAreIdentity(RenderScale::identity, params));
    EXPECT_TRUE(ErodeDilate::paramsAreIdentity(RenderScale::fromMipmapLevel(1), params));

    // Only a negative, dilating, size expands the region.
    int deltaX = -1;
    int deltaY = -1;
    params.sizeX = -3;
    params.sizeY = 5;
    ErodeDilate::getExpansion(RenderScale::identity, params, &deltaX, &deltaY);
    EXPECT_EQ(3, deltaX);
    EXPECT_EQ(0, deltaY);
    params.sizeX = 3;
    params.sizeY = -5;
    ErodeDilate::getExpansion(RenderScale::identity, params, &deltaX, &deltaY);
    EXPECT_EQ(0, deltaX);
    EXPECT_EQ(5, deltaY);
}

// A size of 0 passes the source through bit for bit.
TEST_F(NativeErodeDilateTest, SizeZeroIsIdentity)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 0, 0);
    NodePtr node = makeNode(source, 0., 0.);
    ASSERT_TRUE(bool(node));

    const RectI window(0, 0, kParitySourceWidth, kParitySourceHeight);
    std::vector<float> expected, actual;
    ASSERT_TRUE(renderRGBA(source, window, &expected));
    ASSERT_TRUE(renderRGBA(node, window, &actual));
    EXPECT_EQ(expected, actual);
}

// Sizes of opposite signs filter X and then Y: the same as an ErodeDilate on X alone followed by
// one on Y alone.
TEST_F(NativeErodeDilateTest, OppositeSignsFilterXThenY)
{
    const double sizes[][2] = { { 3., -2. }, { -4., 1. } };
    for (const double* size : sizes) {
        resetProject();
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
        ASSERT_TRUE(bool(source));
        setParitySourceOrigin(source, 0, 0);
        NodePtr node = makeNode(source, size[0], size[1]);
        NodePtr first = makeNode(source, size[0], 0.);
        NodePtr second = first ? makeNode(first, 0., size[1]) : NodePtr();
        ASSERT_TRUE(bool(node) && bool(second));

        const RectI window(-8, -8, kParitySourceWidth + 8, kParitySourceHeight + 8);
        std::vector<float> expected, actual;
        ASSERT_TRUE(renderRGBA(second, window, &expected));
        ASSERT_TRUE(renderRGBA(node, window, &actual));
        EXPECT_EQ(expected, actual) << size[0] << "x" << size[1];
    }
}

TEST_F(NativeErodeDilateTest, ErodeSizes)
{
    runSizes(false);
}

TEST_F(NativeErodeDilateTest, DilateSizes)
{
    runSizes(true);
}

// Erosion reads the fill beyond the source: Stretch and the generator's own Stretch keep a
// constant to the border, Black pulls the border pixels to zero.
TEST_F(NativeErodeDilateTest, EdgeModeDecidesTheBorderOfAnErodedConstant)
{
    const float kValue = 0.5f;
    const char* modes[] = { "input", "stretch", "black" };
    const RectI format(0, 0, kParitySourceWidth, kParitySourceHeight);

    for (const char* mode : modes) {
        resetProject();
        NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);
        ASSERT_TRUE(bool(constant));
        ASSERT_TRUE(setKnobValue(constant, kNativeGeneratorParamExtent, std::string(kNativeGeneratorExtentProject)));
        ASSERT_TRUE(setKnobValues(constant, kConstantParamColor, { kValue, kValue, kValue, kValue }));
        NodePtr node = makeNode(constant, 3., 3.);
        ASSERT_TRUE(bool(node));
        ASSERT_TRUE(setKnobValue(node, kEdgeModeParam, std::string(mode)));

        std::vector<float> pixels;
        ASSERT_TRUE(renderRGBA(node, format, &pixels)) << mode;
        ASSERT_EQ(static_cast<std::size_t>(format.width()) * format.height() * 4, pixels.size()) << mode;
        const auto at = [&](int x, int y) {
            return pixels[(static_cast<std::size_t>(y - format.y1) * format.width() + (x - format.x1)) * 4];
        };

        if (std::string(mode) == "black") {
            EXPECT_EQ(0.f, at(0, 0)) << mode << ": the corner reads the black outside";
            EXPECT_EQ(0.f, at(format.x2 - 1, format.y2 / 2)) << mode << ": the right edge reads the black outside";
            EXPECT_EQ(kValue, at(format.x2 / 2, format.y2 / 2)) << mode << ": the centre is out of the window's reach";
        } else {
            for (std::size_t i = 0; i < pixels.size(); ++i) {
                ASSERT_EQ(kValue, pixels[i]) << mode << ": the value holds to the border, float " << i;
            }
        }
    }
}

TEST_F(NativeErodeDilateTest, RendersTheSameInBothSchedulerModesAtAnyPoolSize)
{
    resetProject();
    for (double sign : { 1., -1. }) {
        const std::string name = (sign > 0.) ? "6x-3" : "-6x3";
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_ERODEDILATE), PLUGIN_MAJOR_NATRON_ERODEDILATE);
        ASSERT_TRUE(bool(source));
        ASSERT_TRUE(isNative(node));
        connectNodes(source, node, 0, true);
        ASSERT_TRUE(setKnobValues(node, kErodeDilateParamSize, { sign * 6., sign * -3. }));
        expectSameBothWays(node, name);

        NodePtr mask = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
        ASSERT_TRUE(bool(mask));
        setParitySourceOrigin(mask, 8, 4);
        connectNodes(mask, node, 1, true);
        ASSERT_TRUE(setKnobValues(node, "enableMask_Mask", { 1. }));
        ASSERT_TRUE(setChannelSelect(node, "maskChannel_Mask", "rgba.A"));
        ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { 0.5 }));
        expectSameBothWays(node, name + "-mask-mix");
    }
}

TEST_F(NativeErodeDilateTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_ERODEDILATE, PLUGIN_MAJOR_NATRON_ERODEDILATE, "ErodeDilate");
}
