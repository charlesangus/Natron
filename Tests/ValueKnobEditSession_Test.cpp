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

#include <vector>

#include <QString>

#include <gtest/gtest.h>

#include "Engine/Curve.h"
#include "Engine/Nodes/Transform/Transform.h"
#include "Engine/ViewIdx.h"

#include "Gui/ValueKnobEditSession.h"

#include "GlobalModeTestSupport.h"

NATRON_NAMESPACE_USING

namespace {

class ValueKnobEditSessionTest
    : public GlobalModeNodeTest {
protected:
    virtual void SetUp() OVERRIDE
    {
        GlobalModeNodeTest::SetUp();
        _transform = create(PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM);
        ASSERT_TRUE(bool(_transform));
    }

    virtual void TearDown() OVERRIDE
    {
        _transform.reset();
        GlobalModeNodeTest::TearDown();
    }

    NodePtr _transform;
};

TEST_F(ValueKnobEditSessionTest, TwoKnobStrokeIsOneUndoStep)
{
    KnobDoublePtr skewX = doubleKnob(_transform, kTransformNodeParamSkewX);
    KnobDoublePtr skewY = doubleKnob(_transform, kTransformNodeParamSkewY);

    ASSERT_TRUE(skewX && skewY);

    const double startX = skewX->getValue(0);
    const double startY = skewY->getValue(0);
    std::vector<KnobGuiPtr> guis;

    guis.push_back(container().guiFor(skewX));
    guis.push_back(container().guiFor(skewY));

    ValueKnobEditSession session;

    ASSERT_TRUE(session.begin(guis, QString::fromUtf8("Global edit")));
    EXPECT_EQ(2, session.getSlotCount());

    std::vector<double> values;

    values.push_back(startX + 0.1);
    values.push_back(startY - 0.2);
    session.move(values);
    values[0] = startX + 0.25;
    values[1] = startY - 0.4;
    session.move(values);
    session.end(QString::fromUtf8("Global edit"));

    EXPECT_FALSE(session.isActive());
    EXPECT_DOUBLE_EQ(startX + 0.25, skewX->getValue(0));
    EXPECT_DOUBLE_EQ(startY - 0.4, skewY->getValue(0));
    ASSERT_EQ(1, container().undoStack.count());

    container().undoStack.undo();
    EXPECT_DOUBLE_EQ(startX, skewX->getValue(0));
    EXPECT_DOUBLE_EQ(startY, skewY->getValue(0));

    container().undoStack.redo();
    EXPECT_DOUBLE_EQ(startX + 0.25, skewX->getValue(0));
    EXPECT_DOUBLE_EQ(startY - 0.4, skewY->getValue(0));
}

TEST_F(ValueKnobEditSessionTest, DrivenDimensionIsLeftToItsDriver)
{
    KnobDoublePtr translate = doubleKnob(_transform, kTransformNodeParamTranslate);
    NodePtr other = create(PLUGINID_NATRON_TRANSFORM, PLUGIN_MAJOR_NATRON_TRANSFORM);

    ASSERT_TRUE(bool(translate));
    ASSERT_TRUE(bool(other));

    KnobDoublePtr master = doubleKnob(other, kTransformNodeParamTranslate);

    ASSERT_TRUE(bool(master));
    master->setValue(7., ViewSpec::all(), 1);
    ASSERT_TRUE(translate->slaveTo(1, master, 1));

    const double startX = translate->getValue(0);
    const double startY = translate->getValue(1);
    std::vector<KnobGuiPtr> guis(1, container().guiFor(translate));
    ValueKnobEditSession session;

    ASSERT_TRUE(session.begin(guis, QString::fromUtf8("Global edit")));
    EXPECT_TRUE(session.isSlotWritable(0));
    EXPECT_FALSE(session.isSlotWritable(1));

    std::vector<double> values;

    values.push_back(startX + 5.);
    values.push_back(startY + 5.);
    session.move(values);
    session.end(QString::fromUtf8("Global edit"));

    EXPECT_DOUBLE_EQ(startX + 5., translate->getValue(0));
    EXPECT_DOUBLE_EQ(startY, translate->getValue(1));
    EXPECT_TRUE(translate->isSlave(1));
    EXPECT_DOUBLE_EQ(7., master->getValue(1));
}

TEST_F(ValueKnobEditSessionTest, AnimatedKnobGetsAKeyAtTheCurrentTime)
{
    KnobDoublePtr rotate = doubleKnob(_transform, kTransformNodeParamRotate);

    ASSERT_TRUE(bool(rotate));

    const double now = rotate->getCurrentTime();

    rotate->setValueAtTime(now - 5., 0., ViewSpec::all(), 0);
    rotate->setValueAtTime(now + 5., 10., ViewSpec::all(), 0);
    ASSERT_EQ(2, rotate->getKeyFramesCount(ViewIdx(0), 0));

    const double start = rotate->getValue(0);
    std::vector<KnobGuiPtr> guis(1, container().guiFor(rotate));
    ValueKnobEditSession session;

    ASSERT_TRUE(session.begin(guis, QString::fromUtf8("Global edit")));
    session.move(std::vector<double>(1, start + 30.));
    session.end(QString::fromUtf8("Global edit"));

    ASSERT_EQ(3, rotate->getKeyFramesCount(ViewIdx(0), 0));

    CurvePtr curve = rotate->getCurve(ViewIdx(0), 0);
    KeyFrame key;

    ASSERT_TRUE(bool(curve));
    ASSERT_TRUE(curve->getKeyFrameWithTime(now, &key));
    EXPECT_DOUBLE_EQ(start + 30., key.getValue());
    EXPECT_EQ(1, container().undoStack.count());

    container().undoStack.undo();
    EXPECT_EQ(2, rotate->getKeyFramesCount(ViewIdx(0), 0));
    EXPECT_DOUBLE_EQ(start, rotate->getValue(0));
}

TEST_F(ValueKnobEditSessionTest, AbandonedStrokeRestoresWithoutAnUndoStep)
{
    KnobDoublePtr rotate = doubleKnob(_transform, kTransformNodeParamRotate);

    ASSERT_TRUE(bool(rotate));

    const double start = rotate->getValue(0);
    std::vector<KnobGuiPtr> guis(1, container().guiFor(rotate));
    ValueKnobEditSession session;

    ASSERT_TRUE(session.begin(guis, QString::fromUtf8("Global edit")));
    session.move(std::vector<double>(1, start + 12.));
    session.abandon();

    EXPECT_DOUBLE_EQ(start, rotate->getValue(0));
    EXPECT_EQ(0, container().undoStack.count());
}

} // namespace
