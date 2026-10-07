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

#include <ofxNatron.h>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Transform/Reformat.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kReformatID = PLUGINID_NATRON_REFORMAT;
const int kOfxReformatMajor = 2;
const int kNativeReformatMajor = PLUGIN_MAJOR_NATRON_REFORMAT;
const double kTime = 1.;

// Both resample through the same filter arithmetic in double, accumulating in float.
const ParityTolerance kReformatTolerance = ParityTolerance::resampling();

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
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

bool
isSecret(const NodePtr& node,
         const std::string& name)
{
    KnobIPtr knob = node->getKnobByName(name);

    EXPECT_TRUE(bool(knob)) << name;

    return knob && knob->getIsSecret();
}

} // namespace

class NativeReformatTest
    : public BaseTest {
protected:
    // A fresh project whose default format is width x height at the given pixel aspect ratio.
    void resetProject(int width = 64,
                      int height = 48,
                      double par = 1.)
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        const std::string name = "nativeReformat" + std::to_string(width) + "x" + std::to_string(height) + "par" + std::to_string(par);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, width, height, name, par));
    }

    ParityPair makePair()
    {
        ParityPair pair = makeParityPair(getApp(), kReformatID, kOfxReformatMajor, kNativeReformatMajor);

        EXPECT_TRUE(bool(pair.native));
        EXPECT_TRUE(isNative(pair.native));
        EXPECT_FALSE(pair.live()) << "the OFX Reformat is retired, so parity replays the recorded references";
        if (pair.source) {
            setParitySourceOrigin(pair.source, 0, 0);
        }

        return pair;
    }

    // The native output's region of definition at the level, in its pixels, grown by two pixels so
    // the edges and the zero fill past them are compared too.
    RectI caseWindow(const ParityPair& pair,
                     unsigned mipmapLevel)
    {
        const RectD rod = regionOfDefinition(pair.native, kTime, RenderScale::fromMipmapLevel(mipmapLevel));
        RectI window = rod.toPixelEnclosing(mipmapLevel, pair.native->getEffectInstance()->getAspectRatio(-1));

        window.x1 -= 2;
        window.y1 -= 2;
        window.x2 += 2;
        window.y2 += 2;

        return window;
    }

    void expectParity(const ParityPair& pair,
                      const std::string& caseName,
                      bool record)
    {
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const ParityResult r = compareParity(pair, caseName, caseWindow(pair, mipmapLevel), mipmapLevel, kReformatTolerance, record);
            EXPECT_TRUE(r.ok) << caseName << ", mipmap " << mipmapLevel << ": " << describe(r);
            EXPECT_FALSE(r.live);
            EXPECT_GE(r.planesCompared, 1) << caseName;
            std::cout << "[ parity ] Reformat " << caseName << " mipmap " << mipmapLevel << ": planes " << r.planesCompared
                      << ", max abs diff " << r.maxAbsDiff << std::endl;
        }
    }

    // The To Box type with a 50x20 box the output is cropped to.
    bool setFixedBox(const ParityPair& pair)
    {
        return setKnobOnBoth(pair, kReformatParamType, std::string(kReformatParamTypeOptionToBox)) && setKnobOnBoth(pair, kReformatParamBoxSize, { 50., 20. }) && setKnobOnBoth(pair, kReformatParamBoxFixed, { 1. });
    }
};

TEST_F(NativeReformatTest, VisibleKnobsFollowTheType)
{
    resetProject();
    NodePtr node = createNode(QString::fromUtf8(kReformatID), kNativeReformatMajor);
    ASSERT_TRUE(isNative(node));

    KnobChoice* type = dynamic_cast<KnobChoice*>(node->getKnobByName(kReformatParamType).get());
    ASSERT_TRUE(type != NULL);
    EXPECT_EQ((int)Reformat::eReformatTypeToProjectFormat, type->getValue());
    KnobBool* blackOutside = dynamic_cast<KnobBool*>(node->getKnobByName(kResamplerParamFilterBlackOutside).get());
    ASSERT_TRUE(blackOutside != NULL);
    EXPECT_FALSE(blackOutside->getValue());

    EXPECT_TRUE(isSecret(node, kNatronParamFormatChoice));
    EXPECT_TRUE(isSecret(node, kReformatParamBoxSize));
    EXPECT_TRUE(isSecret(node, kReformatParamScale));
    EXPECT_TRUE(isSecret(node, kNatronParamFormatSize));
    EXPECT_TRUE(isSecret(node, kNatronParamFormatPar));

    ASSERT_TRUE(setKnobValue(node, kReformatParamType, kReformatParamTypeOptionToFormat));
    EXPECT_FALSE(isSecret(node, kNatronParamFormatChoice));
    EXPECT_TRUE(isSecret(node, kReformatParamBoxSize));
    EXPECT_TRUE(isSecret(node, kReformatParamScale));

    ASSERT_TRUE(setKnobValue(node, kReformatParamType, kReformatParamTypeOptionToBox));
    EXPECT_TRUE(isSecret(node, kNatronParamFormatChoice));
    EXPECT_FALSE(isSecret(node, kReformatParamBoxSize));
    EXPECT_FALSE(isSecret(node, kReformatParamBoxPar));
    EXPECT_FALSE(isSecret(node, kReformatParamBoxFixed));
    EXPECT_TRUE(isSecret(node, kReformatParamScale));

    ASSERT_TRUE(setKnobValue(node, kReformatParamType, kReformatParamTypeOptionScale));
    EXPECT_TRUE(isSecret(node, kReformatParamBoxSize));
    EXPECT_FALSE(isSecret(node, kReformatParamScale));
    EXPECT_FALSE(isSecret(node, kReformatParamScaleUniform));
    EXPECT_TRUE(isSecret(node, kNatronParamFormatSize));
}

TEST_F(NativeReformatTest, ScaleWritesTheBoxBack)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setKnobOnBoth(pair, kReformatParamType, std::string(kReformatParamTypeOptionScale)));
    ASSERT_TRUE(setKnobOnBoth(pair, kReformatParamScale, { 0.5, 0.25 }));

    KnobInt* boxSize = dynamic_cast<KnobInt*>(pair.native->getKnobByName(kReformatParamBoxSize).get());
    ASSERT_TRUE(boxSize != NULL);
    EXPECT_EQ(32, boxSize->getValue(0));
    EXPECT_EQ(12, boxSize->getValue(1));
    KnobBool* boxFixed = dynamic_cast<KnobBool*>(pair.native->getKnobByName(kReformatParamBoxFixed).get());
    ASSERT_TRUE(boxFixed != NULL);
    EXPECT_TRUE(boxFixed->getValue());
}

TEST_F(NativeReformatTest, ToProjectFormatFromTheSourceRoD)
{
    resetProject(40, 30);
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    // The source is 64x48 while its format is the 40x30 project one, so only useRoD resamples.
    ASSERT_TRUE(setKnobOnBoth(pair, kReformatParamUseRoD, { 1. }));
    ASSERT_TRUE(setKnobOnBoth(pair, kReformatParamResize, std::string(kReformatParamResizeOptionFit)));
    expectParity(pair, "project", true);

    EffectInstancePtr effect = pair.native->getEffectInstance();
    EXPECT_EQ(1., effect->getAspectRatio(-1));
    const RectI format = effect->getOutputFormat();
    EXPECT_EQ(0, format.x1);
    EXPECT_EQ(0, format.y1);
    EXPECT_EQ(40, format.x2);
    EXPECT_EQ(30, format.y2);
}

TEST_F(NativeReformatTest, ToFormatWithAPixelAspectRatio)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setKnobOnBoth(pair, kReformatParamType, std::string(kReformatParamTypeOptionToFormat)));
    // What the host writes when a 40x30 format at PAR 1.09 is picked, the way a PAL format is.
    ASSERT_TRUE(setKnobOnBoth(pair, kNatronParamFormatSize, { 40., 30. }));
    ASSERT_TRUE(setKnobOnBoth(pair, kNatronParamFormatPar, { 1.09 }));
    expectParity(pair, "format-par", true);
    EXPECT_EQ(1.09, pair.native->getEffectInstance()->getAspectRatio(-1));
}

TEST_F(NativeReformatTest, FitIntoAFixedBox)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setFixedBox(pair));
    ASSERT_TRUE(setKnobOnBoth(pair, kReformatParamResize, std::string(kReformatParamResizeOptionFit)));
    expectParity(pair, "box-fit", true);
}

TEST_F(NativeReformatTest, PreserveBoundingBox)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(setFixedBox(pair));
    ASSERT_TRUE(setKnobOnBoth(pair, kReformatParamResize, std::string(kReformatParamResizeOptionHeight)));
    EXPECT_FALSE(pair.native->getEffectInstance()->getCanTransform());

    ASSERT_TRUE(setKnobOnBoth(pair, kReformatParamPreserveBoundingBox, { 1. }));
    EXPECT_TRUE(pair.native->getEffectInstance()->getCanTransform());
    EXPECT_TRUE(pair.native->getCurrentCanTransform());
}

TEST_F(NativeReformatTest, NoResizeNoCenterIsIdentity)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    EffectInstancePtr effect = pair.native->getEffectInstance();
    const RectI window(0, 0, 64, 48);
    double inputTime = 0.;
    ViewIdx inputView(0);
    int inputNb = -1;

    EXPECT_FALSE(effect->isIdentity_public(false, effect->getRenderHash(), kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb));

    ASSERT_TRUE(setKnobOnBoth(pair, kReformatParamResize, std::string(kReformatParamResizeOptionNone)));
    ASSERT_TRUE(setKnobOnBoth(pair, kReformatParamCenter, { 0. }));
    EXPECT_TRUE(effect->isIdentity_public(false, effect->getRenderHash(), kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb));
    EXPECT_EQ(0, inputNb);

    ASSERT_TRUE(setKnobOnBoth(pair, kResamplerParamFilterClamp, { 1. }));
    EXPECT_FALSE(effect->isIdentity_public(false, effect->getRenderHash(), kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb));
}

TEST_F(NativeReformatTest, UnversionedRequestsGetTheNativeReformat)
{
    NodePtr unversioned = createNode(QString::fromUtf8(kReformatID));
    ASSERT_TRUE(bool(unversioned));
    EXPECT_TRUE(isNative(unversioned));
    EXPECT_EQ(kNativeReformatMajor, unversioned->getMajorVersion());
}

TEST_F(NativeReformatTest, RendersTheSameInBothSchedulerModes)
{
    resetProject();
    std::vector<int> poolSizes;
    poolSizes.push_back(1);
    poolSizes.push_back(4);

    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    NodePtr reformat = createNode(QString::fromUtf8(kReformatID), kNativeReformatMajor);
    ASSERT_TRUE(bool(source));
    ASSERT_TRUE(isNative(reformat));
    connectNodes(source, reformat, 0, true);
    ASSERT_TRUE(setKnobValue(reformat, kReformatParamType, kReformatParamTypeOptionToBox));
    ASSERT_TRUE(setKnobValues(reformat, kReformatParamBoxSize, { 50., 20. }));
    ASSERT_TRUE(setKnobValues(reformat, kReformatParamBoxFixed, { 1. }));
    ASSERT_TRUE(setKnobValue(reformat, kReformatParamResize, kReformatParamResizeOptionFit));
    ASSERT_TRUE(setKnobValues(reformat, kReformatParamTurn, { 1. }));

    for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
        std::vector<int> unplannedPulls;
        const RectI window = regionOfDefinition(reformat, kTime, RenderScale::fromMipmapLevel(mipmapLevel)).toPixelEnclosing(mipmapLevel, 1.);
        const RenderMismatch m = renderBothWaysDirect(reformat, kTime, ViewIdx(0), mipmapLevel, window, poolSizes, std::function<void()>(), 0.f, &unplannedPulls);
        EXPECT_FALSE(m.any) << "mipmap " << mipmapLevel << ": " << describe(m);
        for (std::size_t p = 0; p < unplannedPulls.size(); ++p) {
            EXPECT_EQ(0, unplannedPulls[p]) << "mipmap " << mipmapLevel << ", pool " << poolSizes[p];
        }
    }
}
