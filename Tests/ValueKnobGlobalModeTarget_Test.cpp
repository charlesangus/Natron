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
#include "Engine/Nodes/Filter/Blur.h"

#include "Gui/ColorTrackballHud.h"
#include "Gui/ColorTrackballTarget.h"
#include "Gui/ValueKnobGlobalModeTarget.h"

#include "GlobalModeTestSupport.h"

NATRON_NAMESPACE_USING

namespace {

class ValueKnobGlobalModeTargetTest
    : public GlobalModeNodeTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        GlobalModeNodeTest::SetUp();
        _blur = create(PLUGINID_NATRON_BLUR, PLUGIN_MAJOR_NATRON_BLUR);
        ASSERT_TRUE(bool(_blur));
        _size = doubleKnob(_blur, kBlurParamSize);
        ASSERT_TRUE(bool(_size));
        ASSERT_EQ(2, _size->getDimension());
        _size->setValue(10., ViewSpec::all(), 0);
        _size->setValue(10., ViewSpec::all(), 1);
        _sizeGui = container().guiFor(_size);
        ASSERT_TRUE(bool(_sizeGui));

        _target = std::dynamic_pointer_cast<ValueKnobGlobalModeTarget>(makeGlobalModeTarget(_sizeGui.get()));
        ASSERT_TRUE(bool(_target));
    }

    virtual void TearDown() OVERRIDE
    {
        _target.reset();
        _sizeGui.reset();
        _size.reset();
        _blur.reset();
        GlobalModeNodeTest::TearDown();
    }

    void stroke(double dx,
                double dy)
    {
        ASSERT_TRUE(_target->beginStroke());
        _target->moveStroke(dx, dy, 1.);
        _target->endStroke();
    }

    NodePtr _blur;
    KnobDoublePtr _size;
    KnobGuiPtr _sizeGui;
    std::shared_ptr<ValueKnobGlobalModeTarget> _target;
};

TEST_F(ValueKnobGlobalModeTargetTest, BlurSizeIsALoneValueTarget)
{
    EXPECT_EQ(_sizeGui.get(), _target->getKnobGui());
    EXPECT_NE(eGlobalModeKindIneligible, _target->getKind());
    EXPECT_NE(eGlobalModeKindColor, _target->getKind());
    EXPECT_EQ(kGlobalModeLockAll, _target->getLockDimension());
}

TEST_F(ValueKnobGlobalModeTargetTest, StrokeMovesBothDimensionsUniformly)
{
    stroke(40., 0.);

    const double x = _size->getValue(0);
    const double y = _size->getValue(1);

    EXPECT_NE(10., x);
    EXPECT_DOUBLE_EQ(x, y);
    EXPECT_EQ(1, container().undoStack.count());

    container().undoStack.undo();
    EXPECT_DOUBLE_EQ(10., _size->getValue(0));
    EXPECT_DOUBLE_EQ(10., _size->getValue(1));
}

TEST_F(ValueKnobGlobalModeTargetTest, QLocksTheStrokeToX)
{
    EXPECT_TRUE(_target->handleKey(Qt::Key_Q, Qt::NoModifier, false));
    EXPECT_EQ(0, _target->getLockDimension());

    stroke(40., 0.);

    EXPECT_NE(10., _size->getValue(0));
    EXPECT_DOUBLE_EQ(10., _size->getValue(1));

    EXPECT_TRUE(_target->handleKey(Qt::Key_A, Qt::NoModifier, false));
    EXPECT_EQ(kGlobalModeLockAll, _target->getLockDimension());
}

TEST_F(ValueKnobGlobalModeTargetTest, LockKeysPastTheDimensionCountAndModifiedKeysAreIgnored)
{
    EXPECT_FALSE(_target->handleKey(Qt::Key_E, Qt::NoModifier, false));
    EXPECT_FALSE(_target->handleKey(Qt::Key_R, Qt::NoModifier, false));
    EXPECT_FALSE(_target->handleKey(Qt::Key_Q, Qt::ControlModifier, false));
    EXPECT_EQ(kGlobalModeLockAll, _target->getLockDimension());
}

TEST_F(ValueKnobGlobalModeTargetTest, HudShowsTheLockChipsAndARateLine)
{
    ColorTrackballHudState state;

    _target->fillHudState(&state);

    ASSERT_FALSE(state.chipRows.empty());
    EXPECT_FALSE(state.rateLine.isEmpty());
    EXPECT_TRUE(state.knobLabel.contains(QString::fromUtf8(kBlurParamSize)));
}

} // namespace
