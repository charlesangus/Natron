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
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Transform/Transform.h"

NATRON_NAMESPACE_USING

namespace {

const ValueRoleEnum kAllRoles[] = {
    eValueRoleNone, eValueRoleTranslate, eValueRoleRotate, eValueRoleScale, eValueRoleSize, eValueRoleSkew
};

KnobDoublePtr
doubleKnob(const NodePtr& node,
           const char* name)
{
    return std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(name));
}

} // namespace

TEST(KnobValueRole, DoubleKnobRoundTripsEveryRole)
{
    KnobDoublePtr knob = std::make_shared<KnobDouble>(static_cast<KnobHolder*>(NULL), std::string("value"), 2, false);

    EXPECT_EQ(eValueRoleNone, knob->getValueRole());
    for (ValueRoleEnum role : kAllRoles) {
        knob->setValueRole(role);
        EXPECT_EQ(role, knob->getValueRole());
    }
}

TEST(KnobValueRole, IntKnobRoundTripsEveryRole)
{
    KnobIntPtr knob = std::make_shared<KnobInt>(static_cast<KnobHolder*>(NULL), std::string("value"), 1, false);

    EXPECT_EQ(eValueRoleNone, knob->getValueRole());
    for (ValueRoleEnum role : kAllRoles) {
        knob->setValueRole(role);
        EXPECT_EQ(role, knob->getValueRole());
    }
}

TEST_F(BaseTest, NativeTransformKnobsCarryRolesAndFormTheInteractSet)
{
    NodePtr transform = createNode(QString::fromUtf8(PLUGINID_NATRON_TRANSFORM), PLUGIN_MAJOR_NATRON_TRANSFORM);
    ASSERT_TRUE(bool(transform));

    KnobDoublePtr translate = doubleKnob(transform, kTransformNodeParamTranslate);
    KnobDoublePtr rotate = doubleKnob(transform, kTransformNodeParamRotate);
    KnobDoublePtr scale = doubleKnob(transform, kTransformNodeParamScale);
    KnobDoublePtr skewX = doubleKnob(transform, kTransformNodeParamSkewX);
    KnobDoublePtr skewY = doubleKnob(transform, kTransformNodeParamSkewY);
    KnobDoublePtr center = doubleKnob(transform, kTransformNodeParamCenter);
    ASSERT_TRUE(translate && rotate && scale && skewX && skewY && center);

    EXPECT_EQ(eValueRoleTranslate, translate->getValueRole());
    EXPECT_EQ(eValueRoleRotate, rotate->getValueRole());
    EXPECT_EQ(eValueRoleScale, scale->getValueRole());
    EXPECT_EQ(eValueRoleSkew, skewX->getValueRole());
    EXPECT_EQ(eValueRoleSkew, skewY->getValueRole());
    EXPECT_EQ(eValueRoleNone, center->getValueRole());

    const TransformInteractKnobs knobs = transform->getTransformInteractKnobs();
    EXPECT_FALSE(knobs.isEmpty());
    EXPECT_EQ(translate, knobs.translate);
    EXPECT_EQ(rotate, knobs.rotate);
    EXPECT_EQ(scale, knobs.scale);
    EXPECT_EQ(skewX, knobs.skewX);
    EXPECT_EQ(skewY, knobs.skewY);
    EXPECT_EQ(center, knobs.center);

    const TransformInteractKnobs byMember = transform->getTransformInteractKnobs(skewY.get());
    EXPECT_EQ(translate, byMember.translate);
    EXPECT_TRUE(byMember.contains(center.get()));

    KnobIPtr amount = transform->getKnobByName(kTransformNodeParamAmount);
    ASSERT_TRUE(bool(amount));
    EXPECT_FALSE(knobs.contains(amount.get()));
    EXPECT_TRUE(transform->getTransformInteractKnobs(amount.get()).isEmpty());
}

TEST_F(BaseTest, NativeBlurSizeIsASizeAndHasNoTransformInteract)
{
    NodePtr blur = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR), PLUGIN_MAJOR_NATRON_BLUR);
    ASSERT_TRUE(bool(blur));

    KnobDoublePtr size = doubleKnob(blur, kBlurParamSize);
    ASSERT_TRUE(bool(size));
    EXPECT_EQ(eValueRoleSize, size->getValueRole());
    EXPECT_TRUE(blur->getTransformInteractKnobs().isEmpty());
    EXPECT_TRUE(blur->getTransformInteractKnobs(size.get()).isEmpty());
}
