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

#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "BaseTest.h"

#include "Engine/EffectInstance.h"
#include "Engine/GlobalModeKind.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Transform/Transform.h"

NATRON_NAMESPACE_USING

TEST(GlobalModeKind, NameHeuristicMatchesWholeWordsCaseInsensitively)
{
    EXPECT_EQ(eValueRoleTranslate, valueRoleFromName("translate", ""));
    EXPECT_EQ(eValueRoleTranslate, valueRoleFromName("boxPosition", ""));
    EXPECT_EQ(eValueRoleTranslate, valueRoleFromName("Offset", ""));
    EXPECT_EQ(eValueRoleRotate, valueRoleFromName("rotation", ""));
    EXPECT_EQ(eValueRoleRotate, valueRoleFromName("lightAngle", ""));
    EXPECT_EQ(eValueRoleScale, valueRoleFromName("SCALE", ""));
    EXPECT_EQ(eValueRoleSize, valueRoleFromName("blur_size", ""));
    EXPECT_EQ(eValueRoleSkew, valueRoleFromName("skewX", ""));
    EXPECT_EQ(eValueRoleSkew, valueRoleFromName("skew2", ""));

    EXPECT_EQ(eValueRoleNone, valueRoleFromName("resize", ""));
    EXPECT_EQ(eValueRoleNone, valueRoleFromName("rotated", ""));
    EXPECT_EQ(eValueRoleNone, valueRoleFromName("upscaled", ""));
    EXPECT_EQ(eValueRoleNone, valueRoleFromName("", ""));
}

TEST(GlobalModeKind, NameHeuristicTriesTheNameBeforeTheLabel)
{
    EXPECT_EQ(eValueRoleSize, valueRoleFromName("param1", "Kernel Size"));
    EXPECT_EQ(eValueRoleRotate, valueRoleFromName("angle", "Size"));
    EXPECT_EQ(eValueRoleScale, valueRoleFromName("scaleSize", ""));
}

TEST_F(BaseTest, GlobalModeKindClassifiesEveryBranch)
{
    NodePtr transform = createNode(QString::fromUtf8(PLUGINID_NATRON_TRANSFORM), PLUGIN_MAJOR_NATRON_TRANSFORM);
    ASSERT_TRUE(bool(transform));
    EffectInstancePtr effect = transform->getEffectInstance();
    ASSERT_TRUE(bool(effect));

    EXPECT_EQ(eGlobalModeKindIneligible, classifyGlobalModeKind(KnobIPtr()));

    // Explicit roles from the transform interact.
    EXPECT_EQ(eGlobalModeKindTranslate, classifyGlobalModeKind(transform->getKnobByName(kTransformNodeParamTranslate)));
    EXPECT_EQ(eGlobalModeKindRotate, classifyGlobalModeKind(transform->getKnobByName(kTransformNodeParamRotate)));
    EXPECT_EQ(eGlobalModeKindScale, classifyGlobalModeKind(transform->getKnobByName(kTransformNodeParamScale)));
    EXPECT_EQ(eGlobalModeKindSkew, classifyGlobalModeKind(transform->getKnobByName(kTransformNodeParamSkewX)));
    EXPECT_EQ(eGlobalModeKindSkew, classifyGlobalModeKind(transform->getKnobByName(kTransformNodeParamSkewY)));

    // Center has no role and no matching name, but is a 2D spatial double.
    EXPECT_EQ(eGlobalModeKindGenericSpatial2D, classifyGlobalModeKind(transform->getKnobByName(kTransformNodeParamCenter)));

    KnobColorPtr colour = effect->createColorKnob("tint", "Tint", 3);
    ASSERT_TRUE(bool(colour));
    EXPECT_EQ(eGlobalModeKindColor, classifyGlobalModeKind(colour));

    KnobBoolPtr toggle = effect->createBoolKnob("scaleToggle", "Scale Toggle");
    ASSERT_TRUE(bool(toggle));
    EXPECT_EQ(eGlobalModeKindIneligible, classifyGlobalModeKind(toggle));

    KnobDoublePtr heuristic = effect->createDoubleKnob("kernelSize", "Kernel Size", 1);
    ASSERT_TRUE(bool(heuristic));
    EXPECT_EQ(eGlobalModeKindSize, classifyGlobalModeKind(heuristic));

    KnobIntPtr heuristicInt = effect->createIntKnob("stepAngle", "Step Angle", 1);
    ASSERT_TRUE(bool(heuristicInt));
    EXPECT_EQ(eGlobalModeKindRotate, classifyGlobalModeKind(heuristicInt));

    KnobDoublePtr misleading = effect->createDoubleKnob("offsetAmount", "Offset Amount", 1);
    ASSERT_TRUE(bool(misleading));
    misleading->setValueRole(eValueRoleScale);
    EXPECT_EQ(eGlobalModeKindScale, classifyGlobalModeKind(misleading));

    KnobDoublePtr plainSpatial1D = effect->createDoubleKnob("pivotX", "Pivot X", 1);
    ASSERT_TRUE(bool(plainSpatial1D));
    plainSpatial1D->setSpatial(true);
    EXPECT_EQ(eGlobalModeKindGeneric, classifyGlobalModeKind(plainSpatial1D));

    KnobDoublePtr plain2D = effect->createDoubleKnob("gain2", "Gain 2", 2);
    ASSERT_TRUE(bool(plain2D));
    EXPECT_EQ(eGlobalModeKindGeneric, classifyGlobalModeKind(plain2D));

    KnobIntPtr plainInt = effect->createIntKnob("iterations", "Iterations", 1);
    ASSERT_TRUE(bool(plainInt));
    EXPECT_EQ(eGlobalModeKindGeneric, classifyGlobalModeKind(plainInt));
}

TEST_F(BaseTest, GlobalModeKindSkipsKnobsWhoseEveryDimensionIsDriven)
{
    NodePtr blur = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR), PLUGIN_MAJOR_NATRON_BLUR);
    ASSERT_TRUE(bool(blur));
    KnobIPtr size = blur->getKnobByName(kBlurParamSize);
    ASSERT_TRUE(bool(size));
    EXPECT_EQ(eGlobalModeKindSize, classifyGlobalModeKind(size));

    ASSERT_NO_THROW(size->setExpression(0, "3", false, true));
    EXPECT_FALSE(isGlobalModeDimensionWritable(size, 0));
    EXPECT_TRUE(isGlobalModeDimensionWritable(size, 1));
    EXPECT_EQ(eGlobalModeKindSize, classifyGlobalModeKind(size));

    KnobDoublePtr master = blur->getEffectInstance()->createDoubleKnob("sizeMaster", "Size Master", 1);
    ASSERT_TRUE(bool(master));
    ASSERT_TRUE(size->slaveTo(1, master, 0));
    EXPECT_FALSE(isGlobalModeDimensionWritable(size, 1));
    EXPECT_EQ(eGlobalModeKindIneligible, classifyGlobalModeKind(size));
}
