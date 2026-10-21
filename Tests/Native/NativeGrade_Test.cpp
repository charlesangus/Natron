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
#include "Engine/AppManager.h"
#include "Engine/ColorWheelMath.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/ColorCorrect.h"
#include "Engine/Nodes/Color/Grade.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Plugin.h"
#include "Engine/Project.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING
NATRON_PYTHON_NAMESPACE_USING

namespace {

const char* const kGradeID = PLUGINID_NATRON_GRADE;
const double kTime = 1.;
const int kOfxGradeMajor = 2;
const int kNativeGradeMajor = PLUGIN_MAJOR_NATRON_GRADE;

// Grade's gamma is a pow(), so the class bound of the transcendental nodes.
const ParityTolerance kGradeTolerance = ParityTolerance::transcendental();

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

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

bool
identityOf(const NodePtr& node,
           const RectI& window,
           int* inputNb)
{
    double inputTime = 0.;
    ViewIdx inputView;

    *inputNb = -1;

    return node->getEffectInstance()->isIdentity_public(false, 0, kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, inputNb);
}

} // namespace

class NativeGradeTest
    : public BaseTest {
protected:
    ParityPair makePair(bool withMask = false)
    {
        ParityPair pair = makeParityPair(getApp(), kGradeID, kOfxGradeMajor, kNativeGradeMajor, withMask ? std::string("Mask") : std::string());

        EXPECT_TRUE(bool(pair.native));
        EXPECT_FALSE(pair.live()) << "the OFX Grade is retired, so parity replays the recorded references";
        EXPECT_TRUE(isNative(pair.native));

        return pair;
    }

    // Compares the pair at mipmap 0 and 1 and prints the largest difference of each, so the
    // tolerance actually needed is on record.
    void expectParity(const ParityPair& pair,
                      const std::string& caseName,
                      bool record)
    {
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const ParityResult r = compareParity(pair, caseName, RectI(), mipmapLevel, kGradeTolerance, record);
            EXPECT_TRUE(r.ok) << caseName << ", mipmap " << mipmapLevel << ": " << describe(r);
            EXPECT_FALSE(r.live);
            EXPECT_GE(r.planesCompared, 1) << caseName;
            std::cout << "[ parity ] Grade " << caseName << " mipmap " << mipmapLevel << ": planes " << r.planesCompared
                      << ", max abs diff " << r.maxAbsDiff << std::endl;
        }
    }
};

TEST_F(NativeGradeTest, MultiplyOffset)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(setKnobOnBoth(pair, "multiply", { 1.5, 0.75, 2., 1.25 }));
    ASSERT_TRUE(setKnobOnBoth(pair, "offset", { 0.1, -0.05, 0.2, 0. }));
    ASSERT_TRUE(setKnobOnBoth(pair, "blackPoint", { 0.05 }));
    ASSERT_TRUE(setKnobOnBoth(pair, "white", { 1.1 }));
    ASSERT_TRUE(setKnobOnBoth(pair, "clampBlack", { 1. }));
    expectParity(pair, "multiply-offset", true);
}

TEST_F(NativeGradeTest, Reverse)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(setKnobOnBoth(pair, "reverse", { 1. }));
    ASSERT_TRUE(setKnobOnBoth(pair, "multiply", { 1.5 }));
    ASSERT_TRUE(setKnobOnBoth(pair, "black", { 0.1 }));
    ASSERT_TRUE(setKnobOnBoth(pair, "gamma", { 0.8 }));
    ASSERT_TRUE(setKnobOnBoth(pair, "clampBlack", { 0. }));
    expectParity(pair, "reverse", true);
}

TEST_F(NativeGradeTest, MaskAndMix)
{
    ParityPair pair = makePair(true);
    ASSERT_TRUE(bool(pair.mask));
    setParitySourceOrigin(pair.mask, 8, 4);
    ASSERT_TRUE(setKnobOnBoth(pair, "enableMask_Mask", { 1. }));
    ASSERT_TRUE(setChannelSelect(pair.native, "maskChannel_Mask", "rgba.A"));
    ASSERT_TRUE(setKnobOnBoth(pair, "mix", { 0.5 }));
    ASSERT_TRUE(setKnobOnBoth(pair, "multiply", { 1.5 }));
    ASSERT_TRUE(setKnobOnBoth(pair, "gamma", { 0.8 }));
    ASSERT_TRUE(setKnobOnBoth(pair, "clampBlack", { 1. }));
    expectParity(pair, "mask-mix", true);
}

TEST_F(NativeGradeTest, IdentityConditions)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));
    const RectI window = paritySourceWindow(pair.source, kTime, 0);
    int inputNb = -1;

    EXPECT_TRUE(identityOf(pair.native, window, &inputNb));
    EXPECT_EQ(0, inputNb);

    ASSERT_TRUE(setKnobOnBoth(pair, "clampBlack", { 1. }));
    EXPECT_FALSE(identityOf(pair.native, window, &inputNb));
    ASSERT_TRUE(setKnobOnBoth(pair, "clampBlack", { 0. }));

    ASSERT_TRUE(setKnobOnBoth(pair, "clampWhite", { 1. }));
    EXPECT_FALSE(identityOf(pair.native, window, &inputNb));
    ASSERT_TRUE(setKnobOnBoth(pair, "clampWhite", { 0. }));
    EXPECT_TRUE(identityOf(pair.native, window, &inputNb));

    ASSERT_TRUE(setKnobOnBoth(pair, "gamma", { 0. }));
    EXPECT_FALSE(identityOf(pair.native, window, &inputNb));

    ASSERT_TRUE(setKnobOnBoth(pair, "mix", { 0. }));
    EXPECT_TRUE(identityOf(pair.native, window, &inputNb));
    EXPECT_EQ(0, inputNb);
}

TEST_F(NativeGradeTest, NormalizeSetsTheBlackAndWhitePoints)
{
    ParityPair pair = makePair();
    ASSERT_TRUE(bool(pair.native));

    KnobButton* normalize = dynamic_cast<KnobButton*>(pair.native->getKnobByName(kGradeParamNormalize).get());
    ASSERT_TRUE(normalize != NULL);
    normalize->trigger();
    const char* const names[2] = { kGradeParamBlackPoint, kGradeParamWhitePoint };
    for (int n = 0; n < 2; ++n) {
        KnobColor* native = dynamic_cast<KnobColor*>(pair.native->getKnobByName(names[n]).get());
        ASSERT_TRUE(native != NULL);
        EXPECT_NE(n == 0 ? 0. : 1., native->getValue(0)) << names[n] << " was not set";
    }
}

TEST_F(NativeGradeTest, UnversionedRequestsGetTheNativeGrade)
{
    NodePtr unversioned = createNode(QString::fromUtf8(kGradeID));
    ASSERT_TRUE(bool(unversioned));
    EXPECT_TRUE(isNative(unversioned));
    EXPECT_EQ(kNativeGradeMajor, unversioned->getMajorVersion());

    EXPECT_FALSE(isPluginMajorRegistered(kGradeID, kOfxGradeMajor));

    NodesList before = getApp()->getProject()->getNodes();
    const std::string script = getApp()->getAppIDString() + ".createNode(\"" + std::string(kGradeID) + "\")\n";
    std::string error, output;
    EXPECT_TRUE(interpretPythonScript(script, &error, &output)) << error;
    NodesList after = getApp()->getProject()->getNodes();
    NodePtr created;
    for (NodesList::const_iterator it = after.begin(); it != after.end(); ++it) {
        if (std::find(before.begin(), before.end(), *it) == before.end()) {
            created = *it;
        }
    }
    ASSERT_TRUE(bool(created));
    EXPECT_EQ(std::string(kGradeID), created->getPluginID());
    EXPECT_TRUE(isNative(created));
}

TEST_F(NativeGradeTest, ChainOfThirtyRendersTheSameInBothSchedulerModes)
{
    getApp()->getProject()->reset(false, true);
    Format format(0, 0, 128, 96, "nativeGradeChainFormat", 1.);
    getApp()->getProject()->setOrAddProjectFormat(format);

    NodePtr previous = createNode(QString::fromUtf8("net.sf.openfx.CheckerBoardPlugin"));
    ASSERT_TRUE(bool(previous));
    for (int i = 0; i < 30; ++i) {
        NodePtr grade = createNode(QString::fromUtf8(kGradeID));
        ASSERT_TRUE(isNative(grade));
        KnobColor* multiply = dynamic_cast<KnobColor*>(grade->getKnobByName(kGradeParamMultiply).get());
        ASSERT_TRUE(multiply != NULL);
        const double factor = (i % 2) ? 1.05 : 0.97;
        multiply->setValues(factor, factor, factor, 1., ViewSpec::all(), eValueChangedReasonNatronInternalEdited);
        connectNodes(previous, grade, 0, true);
        previous = grade;
    }
    NodePtr writer = createNativeWriter();
    ASSERT_TRUE(bool(writer));
    connectNodes(previous, writer, 0, true);

    std::vector<int> poolSizes;
    poolSizes.push_back(1);
    poolSizes.push_back(4);
    const RenderMismatch m = renderBothWays(writer, 1, 2, poolSizes);
    EXPECT_FALSE(m.any) << describe(m);
}

TEST_F(NativeGradeTest, ColorKnobRoles)
{
    struct RoleCheck {
        const char* name;
        ColorKnobRoleEnum role;
    };

    NodePtr grade = createNode(QString::fromUtf8(kGradeID));
    ASSERT_TRUE(isNative(grade));
    const RoleCheck kGradeRoles[] = {
        { kGradeParamBlackPoint, eColorKnobRoleOffset },
        { kGradeParamWhitePoint, eColorKnobRoleGain },
        { kGradeParamBlack, eColorKnobRoleOffset },
        { kGradeParamWhite, eColorKnobRoleGain },
        { kGradeParamMultiply, eColorKnobRoleGain },
        { kGradeParamOffset, eColorKnobRoleOffset },
        { kGradeParamGamma, eColorKnobRolePower },
    };
    for (size_t i = 0; i < sizeof(kGradeRoles) / sizeof(kGradeRoles[0]); ++i) {
        KnobColor* knob = dynamic_cast<KnobColor*>(grade->getKnobByName(kGradeRoles[i].name).get());
        ASSERT_TRUE(knob != NULL) << kGradeRoles[i].name;
        EXPECT_EQ(kGradeRoles[i].role, knob->getColorRole()) << kGradeRoles[i].name;
    }

    NodePtr colorCorrect = createNode(QString::fromUtf8(PLUGINID_NATRON_COLORCORRECT));
    ASSERT_TRUE(isNative(colorCorrect));
    const char* const kGroups[] = { kColorCorrectGroupMaster, kColorCorrectGroupShadows, kColorCorrectGroupMidtones, kColorCorrectGroupHighlights };
    const RoleCheck kScaleRoles[] = {
        { kColorCorrectParamSaturation, eColorKnobRoleGain },
        { kColorCorrectParamContrast, eColorKnobRoleGain },
        { kColorCorrectParamGain, eColorKnobRoleGain },
        { kColorCorrectParamOffset, eColorKnobRoleOffset },
        { kColorCorrectParamGamma, eColorKnobRolePower },
    };
    for (size_t g = 0; g < sizeof(kGroups) / sizeof(kGroups[0]); ++g) {
        for (size_t i = 0; i < sizeof(kScaleRoles) / sizeof(kScaleRoles[0]); ++i) {
            const std::string name = std::string(kGroups[g]) + kScaleRoles[i].name;
            KnobColor* knob = dynamic_cast<KnobColor*>(colorCorrect->getKnobByName(name).get());
            ASSERT_TRUE(knob != NULL) << name;
            EXPECT_EQ(kScaleRoles[i].role, knob->getColorRole()) << name;
        }
    }

    // Everything that never calls setColorRole, such as Constant's colour, stays Auto and is
    // resolved by inference: all-zero defaults with a non-negative display minimum is Colour,
    // not Offset, since Offset's defining trait is a *negative* display minimum.
    NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
    ASSERT_TRUE(isNative(constant));
    KnobColor* constantColor = dynamic_cast<KnobColor*>(constant->getKnobByName(kConstantParamColor).get());
    ASSERT_TRUE(constantColor != NULL);
    EXPECT_EQ(eColorKnobRoleColour, constantColor->getColorRole());
}

TEST_F(NativeGradeTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_GRADE, PLUGIN_MAJOR_NATRON_GRADE, "Grade");
}
