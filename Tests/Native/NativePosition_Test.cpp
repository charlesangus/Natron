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

#include <iostream>
#include <string>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Transform/Position.h"
#include "Engine/OfxEffectInstance.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kPositionID = PLUGINID_NATRON_POSITION;
const int kOfxPositionMajor = 1;
const int kNativePositionMajor = PLUGIN_MAJOR_NATRON_POSITION;
const double kTime = 1.;

// A whole-pixel shift copies values, so nothing is computed.
const ParityTolerance kPositionTolerance = ParityTolerance::exact();

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

bool
isOfx(const NodePtr& node)
{
    return node && dynamic_cast<OfxEffectInstance*>(node->getEffectInstance().get());
}

// The source's region and a margin on every side, so the shifted edges and the zero fill outside
// them are compared too.
RectI
caseWindow(unsigned mipmapLevel,
           double par)
{
    return RectD(-20. * par, -16., 84. * par, 64.).toPixelEnclosing(mipmapLevel, par);
}

} // namespace

class NativePositionTest
    : public BaseTest {
protected:
    // A fresh project whose default format is 64x48 at the given pixel aspect ratio.
    void resetProject(double par = 1.)
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, 64, 48, "nativePositionPar" + std::to_string(par), par));
    }

    ParityPair makePair()
    {
        ParityPair pair = makeParityPair(getApp(), kPositionID, kOfxPositionMajor, kNativePositionMajor);

        EXPECT_TRUE(bool(pair.native));
        EXPECT_TRUE(isNative(pair.native));
        EXPECT_TRUE(pair.live()) << "the OFX Position must be loadable at major " << kOfxPositionMajor;

        return pair;
    }

    void expectParity(const ParityPair& pair,
                      const std::string& caseName,
                      bool record,
                      double par = 1.)
    {
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const ParityResult r = compareParity(pair, caseName, caseWindow(mipmapLevel, par), mipmapLevel, kPositionTolerance, record);
            EXPECT_TRUE(r.ok) << caseName << ", mipmap " << mipmapLevel << ": " << describe(r);
            EXPECT_GE(r.planesCompared, 1) << caseName;
            std::cout << "[ parity ] Position " << caseName << " mipmap " << mipmapLevel << ": planes " << r.planesCompared
                      << ", max abs diff " << r.maxAbsDiff << std::endl;
        }
    }

    RectD regionOfDefinition(const NodePtr& node,
                             unsigned mipmapLevel)
    {
        EffectInstancePtr effect = node->getEffectInstance();
        RectD rod;
        bool isProjectFormat = false;

        EXPECT_NE(eStatusFailed, effect->getRegionOfDefinition_public(effect->getRenderHash(), kTime, RenderScale::fromMipmapLevel(mipmapLevel), ViewIdx(0), &rod, &isProjectFormat));

        return rod;
    }
};

TEST_F(NativePositionTest, KnobsMatchTheOfxPosition)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    ASSERT_TRUE(pair.live());

    expectKnobParity(pair.ofx, pair.native);
}

TEST_F(NativePositionTest, TranslateOverlayIsAHostPositionOverlay)
{
    resetProject();
    NodePtr node = createNode(QString::fromUtf8(kPositionID), kNativePositionMajor);
    ASSERT_TRUE(isNative(node));
    KnobDouble* translate = dynamic_cast<KnobDouble*>(node->getKnobByName(kPositionParamTranslate).get());
    ASSERT_TRUE(translate != NULL);
    EXPECT_TRUE(translate->getHasHostOverlayHandle());
    EXPECT_EQ(2, translate->getDimension());
}

TEST_F(NativePositionTest, TranslateZero)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    expectParity(pair, "translate-0", true);
}

TEST_F(NativePositionTest, TranslateIsRoundedToThePixel)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    ASSERT_TRUE(setKnobOnBoth(pair, kPositionParamTranslate, { 10.4, -3.6 }));
    expectParity(pair, "translate-10.4-3.6", true);
}

TEST_F(NativePositionTest, NegativeTranslate)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    ASSERT_TRUE(setKnobOnBoth(pair, kPositionParamTranslate, { -17.5, 21.5 }));
    expectParity(pair, "translate-neg", false);
}

TEST_F(NativePositionTest, PixelAspectRatioTwo)
{
    resetProject(2.);
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    ASSERT_TRUE(setKnobOnBoth(pair, kPositionParamTranslate, { 10.4, -3.6 }));
    expectParity(pair, "par2", true, 2.);
}

TEST_F(NativePositionTest, TranslateBeyondTheSourceIsBlack)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    // The window must still meet the shifted RoD: a render entirely outside it produces no plane.
    ASSERT_TRUE(setKnobOnBoth(pair, kPositionParamTranslate, { 60., 20. }));
    expectParity(pair, "translate-far", false);
}

TEST_F(NativePositionTest, RegionOfDefinitionIsShifted)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    ASSERT_TRUE(setKnobOnBoth(pair, kPositionParamTranslate, { 10.4, -3.6 }));
    for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
        const RectD ofx = regionOfDefinition(pair.ofx, mipmapLevel);
        const RectD native = regionOfDefinition(pair.native, mipmapLevel);

        EXPECT_EQ(ofx.x1, native.x1) << mipmapLevel;
        EXPECT_EQ(ofx.y1, native.y1) << mipmapLevel;
        EXPECT_EQ(ofx.x2, native.x2) << mipmapLevel;
        EXPECT_EQ(ofx.y2, native.y2) << mipmapLevel;
    }
}

TEST_F(NativePositionTest, ZeroShiftIsIdentityOfTheSource)
{
    resetProject();
    ParityPair pair = makePair();
    ASSERT_TRUE(pair.live());
    EffectInstancePtr effect = pair.native->getEffectInstance();
    const RectI window = caseWindow(0, 1.);
    double inputTime = 0.;
    ViewIdx inputView(0);
    int inputNb = -1;

    EXPECT_TRUE(effect->isIdentity_public(false, effect->getRenderHash(), kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb));
    EXPECT_EQ(0, inputNb);

    ASSERT_TRUE(setKnobOnBoth(pair, kPositionParamTranslate, { 3., 0. }));
    EXPECT_FALSE(effect->isIdentity_public(false, effect->getRenderHash(), kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, &inputNb));
}

TEST_F(NativePositionTest, UnversionedRequestsGetTheNativePosition)
{
    NodePtr unversioned = createNode(QString::fromUtf8(kPositionID));
    ASSERT_TRUE(bool(unversioned));
    EXPECT_TRUE(isNative(unversioned));
    EXPECT_EQ(kNativePositionMajor, unversioned->getMajorVersion());

    if (isPluginMajorRegistered(kPositionID, kOfxPositionMajor)) {
        NodePtr ofx = createNode(QString::fromUtf8(kPositionID), kOfxPositionMajor);
        ASSERT_TRUE(bool(ofx));
        EXPECT_TRUE(isOfx(ofx));
    }
}
