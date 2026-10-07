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
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/ErodeDilate.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const int kOfxMajor = 2;
const double kTime = 1.;

struct MorphologyVariant {
    const char* id;
    const char* name;
    int nativeMajor;
};

const MorphologyVariant kErode = { PLUGINID_NATRON_ERODE, "Erode", PLUGIN_MAJOR_NATRON_ERODE };
const MorphologyVariant kDilate = { PLUGINID_NATRON_DILATE, "Dilate", PLUGIN_MAJOR_NATRON_DILATE };

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

    ParityPair makePair(const MorphologyVariant& variant,
                        bool withMask = false)
    {
        ParityPair pair = makeParityPair(getApp(), variant.id, kOfxMajor, variant.nativeMajor, withMask ? std::string("Mask") : std::string());

        EXPECT_TRUE(bool(pair.native));
        EXPECT_TRUE(isNative(pair.native));
        if (pair.source) {
            setParitySourceOrigin(pair.source, 0, 0);
        }

        return pair;
    }

    void expectParity(const ParityPair& pair,
                      const MorphologyVariant& variant,
                      const std::string& caseName,
                      bool record,
                      bool useOwnRegionOfDefinition = false)
    {
        const std::string fullName = std::string(variant.name) + "-" + caseName;

        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            RectI window;
            if (useOwnRegionOfDefinition) {
                window = regionOfDefinition(pair.native, kTime, RenderScale::fromMipmapLevel(mipmapLevel)).toPixelEnclosing(mipmapLevel, 1.);
            }
            const ParityResult r = compareParity(pair, fullName, window, mipmapLevel, kTolerance, record);
            EXPECT_TRUE(r.ok) << fullName << ", mipmap " << mipmapLevel << ": " << describe(r);
            EXPECT_GE(r.planesCompared, 1) << fullName;
            std::cout << "[ parity ] " << fullName << " mipmap " << mipmapLevel << ": planes " << r.planesCompared
                      << ", max abs diff " << r.maxAbsDiff << std::endl;
        }
    }

    void expectSameRegionOfDefinition(const ParityPair& pair,
                                      const std::string& caseName)
    {
        if (!pair.ofx) {
            return;
        }
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const RenderScale scale = RenderScale::fromMipmapLevel(mipmapLevel);
            const RectD ofx = regionOfDefinition(pair.ofx, kTime, scale);
            const RectD native = regionOfDefinition(pair.native, kTime, scale);
            EXPECT_EQ(ofx.x1, native.x1) << caseName << " mipmap " << mipmapLevel;
            EXPECT_EQ(ofx.y1, native.y1) << caseName << " mipmap " << mipmapLevel;
            EXPECT_EQ(ofx.x2, native.x2) << caseName << " mipmap " << mipmapLevel;
            EXPECT_EQ(ofx.y2, native.y2) << caseName << " mipmap " << mipmapLevel;
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

    void runSizes(const MorphologyVariant& variant)
    {
        resetProject();
        ParityPair pair = makePair(variant);
        ASSERT_TRUE(bool(pair.native));

        expectParity(pair, variant, "size-1x1", true);
        expectSameRegionOfDefinition(pair, "size-1x1");

        ASSERT_TRUE(setKnobOnBoth(pair, kErodeDilateParamSize, { 5., 0. }));
        expectParity(pair, variant, "size-5x0", true);
        expectSameRegionOfDefinition(pair, "size-5x0");

        ASSERT_TRUE(setKnobOnBoth(pair, kErodeDilateParamSize, { -3., -3. }));
        expectParity(pair, variant, "size-neg3x3", true);
        expectSameRegionOfDefinition(pair, "size-neg3x3");

        // The erosion of one axis and the dilation of the other do not commute.
        ASSERT_TRUE(setKnobOnBoth(pair, kErodeDilateParamSize, { 4., -2. }));
        expectParity(pair, variant, "size-4x-2", false);
        ASSERT_TRUE(setKnobOnBoth(pair, kErodeDilateParamSize, { -5., 3. }));
        expectParity(pair, variant, "size-neg5x3", false);

        // A window wider than the image collapses the line to its extremum.
        ASSERT_TRUE(setKnobOnBoth(pair, kErodeDilateParamSize, { 40., 30. }));
        expectParity(pair, variant, "size-40x30", false);
    }

    void runEdges(const MorphologyVariant& variant)
    {
        resetProject();
        ParityPair pair = makePair(variant);
        ASSERT_TRUE(bool(pair.native));
        setParitySourceOrigin(pair.source, 20, -6);
        ASSERT_TRUE(setKnobOnBoth(pair, kErodeDilateParamSize, { -4., 2. }));
        expectSameRegionOfDefinition(pair, "edges");
        expectParity(pair, variant, "edges", false, true);

        ASSERT_TRUE(setKnobOnBoth(pair, kErodeDilateParamExpandRoD, { 0. }));
        expectSameRegionOfDefinition(pair, "edges-no-expand");
        expectParity(pair, variant, "edges-no-expand", false);
    }

    void runAlphaOnly(const MorphologyVariant& variant)
    {
        resetProject();
        ParityPair pair = makePair(variant);
        ASSERT_TRUE(bool(pair.native));
        setParitySourceComponents(pair.source, "alpha");
        ASSERT_TRUE(setKnobOnBoth(pair, kErodeDilateParamSize, { 3., 2. }));
        expectParity(pair, variant, "alpha-3x2", false);
        ASSERT_TRUE(setKnobOnBoth(pair, kErodeDilateParamSize, { -3., -2. }));
        expectParity(pair, variant, "alpha-neg3x2", false);
    }

    void runMaskAndMix(const MorphologyVariant& variant)
    {
        resetProject();
        ParityPair pair = makePair(variant, true);
        ASSERT_TRUE(bool(pair.native));
        ASSERT_TRUE(bool(pair.mask));
        setParitySourceOrigin(pair.mask, 8, 4);
        ASSERT_TRUE(setKnobOnBoth(pair, "enableMask_Mask", { 1. }));
        ASSERT_TRUE(setChannelSelect(pair.native, "maskChannel_Mask", "rgba.A"));
        ASSERT_TRUE(setKnobOnBoth(pair, kOfxMixParamName, { 0.5 }));
        ASSERT_TRUE(setKnobOnBoth(pair, kErodeDilateParamSize, { 3., 2. }));
        expectParity(pair, variant, "mask-mix", false);

        ASSERT_TRUE(setKnobOnBoth(pair, kOfxMaskInvertParamName, { 1. }));
        expectParity(pair, variant, "mask-mix-invert", false);
    }
};

TEST_F(NativeErodeDilateTest, ErodeKnobsMatchTheOfxPlugin)
{
    resetProject();
    ParityPair pair = makePair(kErode);
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(pair.live());
    expectKnobParity(pair.ofx, pair.native);
}

TEST_F(NativeErodeDilateTest, DilateKnobsMatchTheOfxPlugin)
{
    resetProject();
    ParityPair pair = makePair(kDilate);
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(pair.live());
    expectKnobParity(pair.ofx, pair.native);
}

TEST_F(NativeErodeDilateTest, UnversionedRequestsGetTheNativeNodes)
{
    NodePtr erode = createNode(QString::fromUtf8(kErode.id));
    ASSERT_TRUE(bool(erode));
    EXPECT_TRUE(isNative(erode));
    EXPECT_EQ(kErode.nativeMajor, erode->getMajorVersion());

    NodePtr dilate = createNode(QString::fromUtf8(kDilate.id));
    ASSERT_TRUE(bool(dilate));
    EXPECT_TRUE(isNative(dilate));
    EXPECT_EQ(kDilate.nativeMajor, dilate->getMajorVersion());
}

TEST_F(NativeErodeDilateTest, InputsAreSourceAndMask)
{
    resetProject();
    NodePtr node = createNode(QString::fromUtf8(kErode.id), kErode.nativeMajor);
    ASSERT_TRUE(isNative(node));
    EXPECT_EQ(std::string("Source"), node->getInputLabel(0));
    EXPECT_EQ(std::string("Mask"), node->getInputLabel(1));
    EXPECT_TRUE(node->getEffectInstance()->isInputMask(1));
    EXPECT_TRUE(bool(node->getKnobByName("enableMask_Mask")));
    EXPECT_TRUE(bool(node->getKnobByName("hostUnPremultBy")));
}

TEST_F(NativeErodeDilateTest, SourceRegionOfInterestAndIdentityFollowTheOfxPlugin)
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
    // floor(1 * 0.5) = 0 is identity, but floor(-1 * 0.5) = -1 is not: the plug-in's quirk.
    EXPECT_TRUE(ErodeDilate::paramsAreIdentity(RenderScale::fromMipmapLevel(1), params));
    params.sizeX = -1;
    EXPECT_FALSE(ErodeDilate::paramsAreIdentity(RenderScale::fromMipmapLevel(1), params));

    int deltaX = -1;
    int deltaY = -1;
    params.sizeX = -3;
    params.sizeY = 5;
    ErodeDilate::getExpansion(RenderScale::identity, params, false, &deltaX, &deltaY);
    EXPECT_EQ(3, deltaX);
    EXPECT_EQ(0, deltaY);
    ErodeDilate::getExpansion(RenderScale::identity, params, true, &deltaX, &deltaY);
    EXPECT_EQ(0, deltaX);
    EXPECT_EQ(5, deltaY);
}

TEST_F(NativeErodeDilateTest, ErodeSizes)
{
    runSizes(kErode);
}

TEST_F(NativeErodeDilateTest, DilateSizes)
{
    runSizes(kDilate);
}

TEST_F(NativeErodeDilateTest, ErodeImageEdges)
{
    runEdges(kErode);
}

TEST_F(NativeErodeDilateTest, DilateImageEdges)
{
    runEdges(kDilate);
}

TEST_F(NativeErodeDilateTest, ErodeAlphaOnlyInput)
{
    runAlphaOnly(kErode);
}

TEST_F(NativeErodeDilateTest, DilateAlphaOnlyInput)
{
    runAlphaOnly(kDilate);
}

TEST_F(NativeErodeDilateTest, ErodeMaskAndMix)
{
    runMaskAndMix(kErode);
}

TEST_F(NativeErodeDilateTest, DilateMaskAndMix)
{
    runMaskAndMix(kDilate);
}

TEST_F(NativeErodeDilateTest, RendersTheSameInBothSchedulerModesAtAnyPoolSize)
{
    resetProject();
    const MorphologyVariant variants[] = { kErode, kDilate };
    for (std::size_t v = 0; v < 2; ++v) {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
        NodePtr node = createNode(QString::fromUtf8(variants[v].id), variants[v].nativeMajor);
        ASSERT_TRUE(bool(source));
        ASSERT_TRUE(isNative(node));
        connectNodes(source, node, 0, true);
        ASSERT_TRUE(setKnobValues(node, kErodeDilateParamSize, { 6., -3. }));
        expectSameBothWays(node, std::string(variants[v].name) + "-6x-3");

        NodePtr mask = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
        ASSERT_TRUE(bool(mask));
        setParitySourceOrigin(mask, 8, 4);
        connectNodes(mask, node, 1, true);
        ASSERT_TRUE(setKnobValues(node, "enableMask_Mask", { 1. }));
        ASSERT_TRUE(setChannelSelect(node, "maskChannel_Mask", "rgba.A"));
        ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { 0.5 }));
        expectSameBothWays(node, std::string(variants[v].name) + "-6x-3-mask-mix");
    }
}
