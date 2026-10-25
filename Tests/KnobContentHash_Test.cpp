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

#include "Engine/AppManager.h"
#include "Engine/ContentHasher.h"
#include "Engine/Curve.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

KnobDoublePtr
makeDoubleKnob(const NodePtr& node,
               const std::string& name)
{
    return AppManager::createKnob<KnobDouble>(node->getEffectInstance().get(), name, 1, false);
}

} // namespace

TEST_F(BaseTest, KnobContentHashIsStableAndIgnoresIdentity)
{
    NodePtr nodeA = createNode(_generatorPluginID);
    NodePtr nodeB = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(nodeA));
    ASSERT_TRUE(bool(nodeB));

    KnobDoublePtr a = makeDoubleKnob(nodeA, "hashA");
    KnobDoublePtr b = makeDoubleKnob(nodeB, "hashB");
    ASSERT_TRUE(bool(a));
    ASSERT_TRUE(bool(b));

    const U64 first = a->getContentHash();
    EXPECT_NE(0u, first);
    EXPECT_EQ(first, a->getContentHash());

    a->setValue(2.5);
    b->setValue(2.5);
    EXPECT_EQ(a->getContentHash(), b->getContentHash());

    ContentHasher viaAppend;
    a->appendContentHash(viaAppend);
    ContentHasher viaValue;
    viaValue.appendU64(a->getContentHash());
    EXPECT_EQ(viaValue.finalize(), viaAppend.finalize());
}

TEST_F(BaseTest, KnobContentHashFollowsStaticValue)
{
    NodePtr node = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(node));
    KnobDoublePtr knob = makeDoubleKnob(node, "hashStatic");
    ASSERT_TRUE(bool(knob));

    knob->setValue(1.);
    const U64 before = knob->getContentHash();

    knob->setValue(2.);
    EXPECT_NE(before, knob->getContentHash());

    knob->setValue(1.);
    EXPECT_EQ(before, knob->getContentHash());
}

TEST_F(BaseTest, KnobContentHashFollowsKeyframes)
{
    NodePtr node = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(node));
    KnobDoublePtr knob = makeDoubleKnob(node, "hashKeys");
    ASSERT_TRUE(bool(knob));
    KnobIPtr knobI = knob;

    const U64 noKeys = knob->getContentHash();

    knob->setValueAtTime(10, 3., ViewSpec::all(), 0);
    const U64 oneKey = knob->getContentHash();
    EXPECT_NE(noKeys, oneKey);

    KeyFrame moved;
    ASSERT_TRUE(knob->moveValueAtTime(eCurveChangeReasonInternal, 10, ViewSpec::all(), 0, 5, 0, &moved));
    EXPECT_NE(oneKey, knob->getContentHash());
    ASSERT_TRUE(knob->moveValueAtTime(eCurveChangeReasonInternal, 15, ViewSpec::all(), 0, -5, 0, &moved));
    EXPECT_EQ(oneKey, knob->getContentHash());

    KeyFrame changed;
    ASSERT_TRUE(knob->setInterpolationAtTime(eCurveChangeReasonInternal, ViewSpec::all(), 0, 10, eKeyframeTypeConstant, &changed));
    EXPECT_NE(oneKey, knob->getContentHash());
    ASSERT_TRUE(knob->setInterpolationAtTime(eCurveChangeReasonInternal, ViewSpec::all(), 0, 10, eKeyframeTypeSmooth, &changed));
    EXPECT_EQ(oneKey, knob->getContentHash());

    knobI->deleteValueAtTime(eCurveChangeReasonInternal, 10, ViewSpec::all(), 0, false);
    EXPECT_EQ(noKeys, knob->getContentHash());
}

TEST_F(BaseTest, KnobContentHashFollowsExpression)
{
    NodePtr node = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(node));
    KnobDoublePtr knob = makeDoubleKnob(node, "hashExpr");
    ASSERT_TRUE(bool(knob));

    const U64 before = knob->getContentHash();

    knob->setExpression(0, "1", false, false);
    const U64 withExpr = knob->getContentHash();
    EXPECT_NE(before, withExpr);

    knob->setExpression(0, "2", false, false);
    EXPECT_NE(withExpr, knob->getContentHash());

    knob->clearExpression(0, true);
    EXPECT_EQ(before, knob->getContentHash());
}

TEST_F(BaseTest, SlavedKnobHashesAsItsMaster)
{
    NodePtr node = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(node));
    KnobDoublePtr master = makeDoubleKnob(node, "hashMaster");
    KnobDoublePtr slave = makeDoubleKnob(node, "hashSlave");
    ASSERT_TRUE(bool(master));
    ASSERT_TRUE(bool(slave));

    master->setValue(3.);
    slave->setValue(4.);
    EXPECT_NE(master->getContentHash(), slave->getContentHash());

    ASSERT_TRUE(slave->slaveTo(0, master, 0));
    EXPECT_EQ(master->getContentHash(), slave->getContentHash());

    master->setValue(7.);
    EXPECT_EQ(master->getContentHash(), slave->getContentHash());

    master->setValueAtTime(5, 1., ViewSpec::all(), 0);
    EXPECT_EQ(master->getContentHash(), slave->getContentHash());
}

TEST_F(BaseTest, NodeUiKnobsAreNotRenderHashed)
{
    NodePtr node = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(node));
    EffectInstancePtr effect = node->getEffectInstance();

    const char* const uiKnobs[] = { kUserLabelKnobName, kEnablePreviewKnobName, "hideInputs", "nodeInfos" };
    for (const char* name : uiKnobs) {
        KnobIPtr knob = effect->getKnobByName(name);
        ASSERT_TRUE(bool(knob)) << name;
        EXPECT_TRUE(knob->isHashExempt()) << name;
        EXPECT_FALSE(knob->isRenderHashed()) << name;
    }
}

TEST_F(BaseTest, ValueKnobsAreRenderHashed)
{
    NodePtr node = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(node));

    KnobDoublePtr normal = makeDoubleKnob(node, "hashNormal");
    ASSERT_TRUE(bool(normal));
    normal->setEvaluateOnChange(true);
    EXPECT_TRUE(normal->isRenderHashed());

    KnobDoublePtr secretState = makeDoubleKnob(node, "hashSecretState");
    ASSERT_TRUE(bool(secretState));
    secretState->setSecret(true);
    secretState->setIsPersistent(true);
    secretState->setEvaluateOnChange(false);
    EXPECT_TRUE(secretState->isRenderHashed());

    KnobDoublePtr transient = makeDoubleKnob(node, "hashTransient");
    ASSERT_TRUE(bool(transient));
    transient->setIsPersistent(false);
    transient->setEvaluateOnChange(false);
    EXPECT_FALSE(transient->isRenderHashed());

    KnobDoublePtr exempt = makeDoubleKnob(node, "hashExempt");
    ASSERT_TRUE(bool(exempt));
    exempt->setEvaluateOnChange(true);
    exempt->setHashExempt(true);
    EXPECT_FALSE(exempt->isRenderHashed());
}

TEST_F(BaseTest, StructuralKnobsAreNotRenderHashed)
{
    NodePtr node = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(node));
    KnobHolder* holder = node->getEffectInstance().get();

    KnobPagePtr page = AppManager::createKnob<KnobPage>(holder, std::string("hashPage"), 1, false);
    KnobSeparatorPtr separator = AppManager::createKnob<KnobSeparator>(holder, std::string("hashSeparator"), 1, false);
    KnobButtonPtr button = AppManager::createKnob<KnobButton>(holder, std::string("hashButton"), 1, false);
    ASSERT_TRUE(bool(page));
    ASSERT_TRUE(bool(separator));
    ASSERT_TRUE(bool(button));
    EXPECT_FALSE(page->isRenderHashed());
    EXPECT_FALSE(separator->isRenderHashed());
    EXPECT_FALSE(button->isRenderHashed());
}

TEST_F(BaseTest, KnobTangentWeightChangesValueSignalsAndHash)
{
    NodePtr node = createNode(_generatorPluginID);
    ASSERT_TRUE(bool(node));
    KnobDoublePtr knob = makeDoubleKnob(node, "hashTangents");
    ASSERT_TRUE(bool(knob));

    knob->setValueAtTime(0, 0., ViewSpec::all(), 0);
    knob->setValueAtTime(10, 5., ViewSpec::all(), 0);
    knob->setValueAtTime(20, 0., ViewSpec::all(), 0);
    const double midpoint = knob->getValueAtTime(15., 0);
    const U64 before = knob->getContentHash();

    int moved = 0;
    KnobSignalSlotHandlerPtr handler = knob->getSignalSlotHandler();
    ASSERT_TRUE(bool(handler));
    QObject::connect(handler.get(), &KnobSignalSlotHandler::derivativeMoved, [&moved](double time, ViewSpec, int dimension) {
        EXPECT_EQ(10., time);
        EXPECT_EQ(0, dimension);
        ++moved;
    });

    KeyFrame key;
    ASSERT_TRUE(knob->getCurve(ViewIdx(0), 0)->getKeyFrameWithTime(10., &key));
    KeyTangents tangents;
    tangents.left = key.getLeftDerivative();
    tangents.right = tangents.left;
    tangents.rightWeight = 0.8;
    KeyFrame changed;
    ASSERT_TRUE(knob->setTangentsAtTime(eCurveChangeReasonInternal, ViewSpec::all(), 0, 10., tangents, &changed));

    EXPECT_EQ(1, moved);
    EXPECT_EQ(eKeyframeTypeFree, changed.getInterpolation());
    EXPECT_EQ(Interpolation::kDefaultTangentWeight, changed.getLeftWeight());
    EXPECT_EQ(0.8, changed.getRightWeight());
    EXPECT_EQ(5., knob->getValueAtTime(10., 0));
    EXPECT_NE(midpoint, knob->getValueAtTime(15., 0));
    EXPECT_NE(before, knob->getContentHash());

    tangents.rightWeight = Interpolation::kDefaultTangentWeight;
    ASSERT_TRUE(knob->setTangentsAtTime(eCurveChangeReasonInternal, ViewSpec::all(), 0, 10., tangents, &changed));
    EXPECT_EQ(2, moved);
    EXPECT_EQ(midpoint, knob->getValueAtTime(15., 0));

    tangents.right = tangents.left + 1.;
    ASSERT_TRUE(knob->setTangentsAtTime(eCurveChangeReasonInternal, ViewSpec::all(), 0, 10., tangents, &changed));
    EXPECT_EQ(eKeyframeTypeBroken, changed.getInterpolation());
}
