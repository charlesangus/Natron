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

#include <gtest/gtest.h>

#include "Engine/GlobalModeKind.h"
#include "Engine/Nodes/Transform/Transform.h"

#include "Gui/ColorTrackballHud.h"
#include "Gui/ColorTrackballTarget.h"
#include "Gui/TransformGroupGlobalModeTarget.h"

#include "GlobalModeTestSupport.h"

NATRON_NAMESPACE_USING

namespace {

class TransformGroupGlobalModeTargetTest
    : public GlobalModeNodeTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        GlobalModeNodeTest::SetUp();
        _transform = create(PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM);
        ASSERT_TRUE(bool(_transform));

        _translate = doubleKnob(_transform, kTransformNodeParamTranslate);
        _rotate = doubleKnob(_transform, kTransformNodeParamRotate);
        _scale = doubleKnob(_transform, kTransformNodeParamScale);
        _skewX = doubleKnob(_transform, kTransformNodeParamSkewX);
        _skewY = doubleKnob(_transform, kTransformNodeParamSkewY);
        _center = doubleKnob(_transform, kTransformNodeParamCenter);
        ASSERT_TRUE(_translate && _rotate && _scale && _skewX && _skewY && _center);

        // The group target finds its members' GUIs through the container, as a panel would.
        container().guiFor(_translate);
        container().guiFor(_scale);
        container().guiFor(_skewX);
        container().guiFor(_skewY);
        container().guiFor(_center);

        KnobGuiPtr rotateGui = container().guiFor(_rotate);

        _target = std::dynamic_pointer_cast<TransformGroupGlobalModeTarget>(makeGlobalModeTarget(rotateGui.get()));
        ASSERT_TRUE(bool(_target));
    }

    virtual void TearDown() OVERRIDE
    {
        _target.reset();
        _translate.reset();
        _rotate.reset();
        _scale.reset();
        _skewX.reset();
        _skewY.reset();
        _center.reset();
        _transform.reset();
        GlobalModeNodeTest::TearDown();
    }

    void stroke(double dx,
                double dy)
    {
        ASSERT_TRUE(_target->beginStroke());
        _target->moveStroke(dx, dy, 1.);
        _target->endStroke();
    }

    NodePtr _transform;
    KnobDoublePtr _translate;
    KnobDoublePtr _rotate;
    KnobDoublePtr _scale;
    KnobDoublePtr _skewX;
    KnobDoublePtr _skewY;
    KnobDoublePtr _center;
    std::shared_ptr<TransformGroupGlobalModeTarget> _target;
};

TEST_F(TransformGroupGlobalModeTargetTest, EngagingRotateStrokesRotateOnly)
{
    EXPECT_EQ(eGlobalModeGroupMemberRotate, _target->getMember());
    EXPECT_EQ(_rotate.get(), _target->getKnobGui()->getKnob().get());

    const double rotate = _rotate->getValue(0);
    const double tx = _translate->getValue(0);

    stroke(20., 0.);

    EXPECT_NE(rotate, _rotate->getValue(0));
    EXPECT_DOUBLE_EQ(tx, _translate->getValue(0));
}

TEST_F(TransformGroupGlobalModeTargetTest, QSwitchesToTranslate)
{
    const double rotate = _rotate->getValue(0);
    const double tx = _translate->getValue(0);
    const double ty = _translate->getValue(1);

    EXPECT_TRUE(_target->handleKey(Qt::Key_Q, Qt::NoModifier, false));
    EXPECT_EQ(eGlobalModeGroupMemberTranslate, _target->getMember());

    stroke(10., -5.);

    EXPECT_DOUBLE_EQ(tx + 10., _translate->getValue(0));
    EXPECT_DOUBLE_EQ(ty + 5., _translate->getValue(1));
    EXPECT_DOUBLE_EQ(rotate, _rotate->getValue(0));
}

TEST_F(TransformGroupGlobalModeTargetTest, RStrokesBothSkewsInOneUndoStep)
{
    const double skewX = _skewX->getValue(0);
    const double skewY = _skewY->getValue(0);

    EXPECT_TRUE(_target->handleKey(Qt::Key_R, Qt::NoModifier, false));
    EXPECT_EQ(eGlobalModeGroupMemberSkew, _target->getMember());

    const int before = container().undoStack.count();

    stroke(10., -10.);

    EXPECT_NE(skewX, _skewX->getValue(0));
    EXPECT_NE(skewY, _skewY->getValue(0));
    ASSERT_EQ(before + 1, container().undoStack.count());

    container().undoStack.undo();
    EXPECT_DOUBLE_EQ(skewX, _skewX->getValue(0));
    EXPECT_DOUBLE_EQ(skewY, _skewY->getValue(0));
}

TEST_F(TransformGroupGlobalModeTargetTest, TwoLocksTheStrokeToY)
{
    EXPECT_TRUE(_target->handleKey(Qt::Key_R, Qt::NoModifier, false));
    EXPECT_TRUE(_target->handleKey(Qt::Key_2, Qt::NoModifier, false));
    EXPECT_EQ(1, _target->getLockDimension());

    const double skewX = _skewX->getValue(0);
    const double skewY = _skewY->getValue(0);

    stroke(10., -10.);

    EXPECT_DOUBLE_EQ(skewX, _skewX->getValue(0));
    EXPECT_NE(skewY, _skewY->getValue(0));

    EXPECT_TRUE(_target->handleKey(Qt::Key_Q, Qt::NoModifier, false));
    EXPECT_EQ(1, _target->getLockDimension());

    const double tx = _translate->getValue(0);
    const double ty = _translate->getValue(1);

    stroke(10., -10.);

    EXPECT_DOUBLE_EQ(tx, _translate->getValue(0));
    EXPECT_DOUBLE_EQ(ty + 10., _translate->getValue(1));
}

TEST_F(TransformGroupGlobalModeTargetTest, SwitchingToTheActiveMemberIsIgnored)
{
    EXPECT_FALSE(_target->handleKey(Qt::Key_W, Qt::NoModifier, false));
    EXPECT_FALSE(_target->handleKey(Qt::Key_W, Qt::ControlModifier, false));
    EXPECT_EQ(eGlobalModeGroupMemberRotate, _target->getMember());
}

TEST_F(TransformGroupGlobalModeTargetTest, HudLightsTheActiveMember)
{
    ColorTrackballHudState state;

    _target->fillHudState(&state);

    ASSERT_EQ(2u, state.chipRows.size());
    ASSERT_EQ(5u, state.chipRows[0].size());
    EXPECT_FALSE(state.chipRows[0][0].lit);
    EXPECT_TRUE(state.chipRows[0][1].lit);
    EXPECT_TRUE(state.chipRows[1][0].dimmed);
    EXPECT_FALSE(state.rateLine.isEmpty());
}

} // namespace
