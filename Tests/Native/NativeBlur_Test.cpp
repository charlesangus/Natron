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
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kBlurID = PLUGINID_NATRON_BLUR;
const int kOfxBlurMajor = 4;
const int kNativeBlurMajor = PLUGIN_MAJOR_NATRON_BLUR;
const double kTime = 1.;

// The IIR filters run in double with float storage between passes, as CImg does; the box family
// shares the class so every case of the node is held to one bound.
const ParityTolerance kBlurTolerance = ParityTolerance::iir();

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

class NativeBlurTest
    : public BaseTest {
protected:
    // A fresh project whose default format is the parity source's 64x48, so that cropToFormat
    // keeps the blurred region of definition, and the recorded references, at the source's size.
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeBlur64x48", 1.));
    }

    ParityPair makePair(bool withMask = false)
    {
        ParityPair pair = makeParityPair(getApp(), kBlurID, kOfxBlurMajor, kNativeBlurMajor, withMask ? std::string("Mask") : std::string());

        EXPECT_TRUE(bool(pair.native));
        EXPECT_TRUE(isNative(pair.native));
        EXPECT_FALSE(pair.live()) << "the OFX Blur is retired, so parity replays the recorded references";
        if (pair.source) {
            setParitySourceOrigin(pair.source, 0, 0);
        }

        return pair;
    }

    // Compares the pair at mipmap 0 and 1 over the source's region of definition and prints the
    // largest difference of each, so the tolerance actually needed is on record.
    void expectParity(const ParityPair& pair,
                      const std::string& caseName,
                      const ParityTolerance& tolerance,
                      bool record)
    {
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const ParityResult r = compareParity(pair, caseName, RectI(), mipmapLevel, tolerance, record);
            EXPECT_TRUE(r.ok) << caseName << ", mipmap " << mipmapLevel << ": " << describe(r);
            EXPECT_FALSE(r.live);
            EXPECT_GE(r.planesCompared, 1) << caseName;
            std::cout << "[ parity ] Blur " << caseName << " mipmap " << mipmapLevel << ": planes " << r.planesCompared
                      << ", max abs diff " << r.maxAbsDiff << std::endl;
        }
    }

    // Renders `node` in Legacy mode and as a task graph at pool sizes 1, 4 and 16 over its own
    // region of definition: bit-exact, and the task graph pulls nothing it did not plan.
    void expectSameBothWays(const NodePtr& node,
                            const std::string& caseName)
    {
        std::vector<int> poolSizes;
        poolSizes.push_back(1);
        poolSizes.push_back(4);
        poolSizes.push_back(16);
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
};

TEST_F(NativeBlurTest, UnversionedRequestsGetTheNativeBlur)
{
    NodePtr unversioned = createNode(QString::fromUtf8(kBlurID));
    ASSERT_TRUE(bool(unversioned));
    EXPECT_TRUE(isNative(unversioned));
    EXPECT_EQ(kNativeBlurMajor, unversioned->getMajorVersion());
}

TEST_F(NativeBlurTest, InputsAreSourceAndMask)
{
    resetProject();
    NodePtr node = createNode(QString::fromUtf8(kBlurID), kNativeBlurMajor);
    ASSERT_TRUE(isNative(node));
    EXPECT_EQ(std::string("Source"), node->getInputLabel(0));
    EXPECT_EQ(std::string("Mask"), node->getInputLabel(1));
    EXPECT_TRUE(node->getEffectInstance()->isInputMask(1));
    EXPECT_TRUE(bool(node->getKnobByName("enableMask_Mask")));
    EXPECT_TRUE(bool(node->getKnobByName("hostUnPremultBy")));
}

TEST_F(NativeBlurTest, SourceRegionOfInterestIsTheOfxHalo)
{
    BlurParams params;
    const RectI rect(10, 20, 30, 40);

    params.filter = BlurKernels::eFilterGaussian;
    params.sizeX = params.sizeY = 0.2;
    EXPECT_TRUE(Blur::paramsAreIdentity(RenderScale::identity, params));
    RectI roi = Blur::getSourceRoI(rect, RenderScale::identity, params);
    EXPECT_EQ(rect.x1, roi.x1);
    EXPECT_EQ(rect.y2, roi.y2);

    // max(3, ceil(1.5 * 25)) = 38, plus the derivative order.
    params.sizeX = params.sizeY = 25.;
    params.orderX = 1;
    EXPECT_FALSE(Blur::paramsAreIdentity(RenderScale::identity, params));
    roi = Blur::getSourceRoI(rect, RenderScale::identity, params);
    EXPECT_EQ(10 - 38 - 1, roi.x1);
    EXPECT_EQ(30 + 38 + 1, roi.x2);
    EXPECT_EQ(20 - 38, roi.y1);
    EXPECT_EQ(40 + 38, roi.y2);

    // Quadratic: 3 * (floor((25 - 1) / 2) + 1) = 39, plus one for any derivative.
    params.filter = BlurKernels::eFilterQuadratic;
    roi = Blur::getSourceRoI(rect, RenderScale::identity, params);
    EXPECT_EQ(10 - 39 - 1, roi.x1);
    EXPECT_EQ(20 - 39, roi.y1);

    // A box no wider than one pixel and no derivative leaves the image alone.
    params.filter = BlurKernels::eFilterBox;
    params.orderX = 0;
    params.sizeX = params.sizeY = 1.;
    EXPECT_TRUE(Blur::paramsAreIdentity(RenderScale::identity, params));
    params.sizeX = params.sizeY = 2.;
    EXPECT_TRUE(Blur::paramsAreIdentity(RenderScale::fromMipmapLevel(1), params));
    EXPECT_FALSE(Blur::paramsAreIdentity(RenderScale::identity, params));
}

TEST_F(NativeBlurTest, GaussianSize3)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setKnobOnBoth(pair, kBlurParamFilter, std::string(kBlurParamFilterGaussian)));
    ASSERT_TRUE(setKnobOnBoth(pair, kBlurParamSize, { 3. }));
    expectParity(pair, "gaussian-3", kBlurTolerance, true);
}

TEST_F(NativeBlurTest, AnisotropicSize40By5)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setKnobOnBoth(pair, kBlurParamFilter, std::string(kBlurParamFilterGaussian)));
    ASSERT_TRUE(setKnobOnBoth(pair, kBlurParamSize, { 40., 5. }));
    expectParity(pair, "gaussian-40x5", kBlurTolerance, true);
}

TEST_F(NativeBlurTest, MaskAndMix)
{
    resetProject();
    ParityPair pair = makePair(true);
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(bool(pair.mask));
    setParitySourceOrigin(pair.mask, 8, 4);
    ASSERT_TRUE(setKnobOnBoth(pair, "enableMask_Mask", { 1. }));
    ASSERT_TRUE(setChannelSelect(pair.native, "maskChannel_Mask", "rgba.A"));
    ASSERT_TRUE(setKnobOnBoth(pair, kOfxMixParamName, { 0.5 }));
    ASSERT_TRUE(setKnobOnBoth(pair, kBlurParamFilter, std::string(kBlurParamFilterGaussian)));
    ASSERT_TRUE(setKnobOnBoth(pair, kBlurParamSize, { 6. }));
    expectParity(pair, "mask-mix", kBlurTolerance, true);
}

TEST_F(NativeBlurTest, RendersTheSameInBothSchedulerModesAtAnyPoolSize)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    NodePtr blur = createNode(QString::fromUtf8(kBlurID), kNativeBlurMajor);
    ASSERT_TRUE(bool(source));
    ASSERT_TRUE(isNative(blur));
    connectNodes(source, blur, 0, true);
    ASSERT_TRUE(setKnobValues(blur, kBlurParamSize, { 9., 4. }));
    ASSERT_TRUE(setKnobValues(blur, kBlurParamCropToFormat, { 0. }));
    expectSameBothWays(blur, "fir-gaussian-9x4");

    ASSERT_TRUE(setKnobValue(blur, kBlurParamBoundary, std::string(kBlurParamBoundaryNearest)));
    ASSERT_TRUE(setKnobValue(blur, kBlurParamFilter, std::string(kBlurParamFilterQuadratic)));
    expectSameBothWays(blur, "quadratic-9x4-nearest");

    NodePtr mask = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(mask));
    setParitySourceOrigin(mask, 8, 4);
    connectNodes(mask, blur, 1, true);
    ASSERT_TRUE(setKnobValues(blur, "enableMask_Mask", { 1. }));
    ASSERT_TRUE(setChannelSelect(blur, "maskChannel_Mask", "rgba.A"));
    ASSERT_TRUE(setKnobValues(blur, kOfxMixParamName, { 0.5 }));
    expectSameBothWays(blur, "quadratic-9x4-mask-mix");
}
