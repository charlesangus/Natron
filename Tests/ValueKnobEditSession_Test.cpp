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

#include <QHBoxLayout>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

#include <gtest/gtest.h>

#include "Engine/Curve.h"
#include "Engine/EffectInstance.h"
#include "Engine/Nodes/Transform/Transform.h"
#include "Engine/ViewIdx.h"

#include "Gui/KnobGuiValue.h"
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

TEST_F(ValueKnobEditSessionTest, DisabledDimensionIsNotWritten)
{
    KnobDoublePtr translate = doubleKnob(_transform, kTransformNodeParamTranslate);

    ASSERT_TRUE(bool(translate));
    translate->setEnabled(1, false);

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
}

TEST_F(ValueKnobEditSessionTest, SwitchingKnobsMidGestureIsOneUndoStep)
{
    KnobDoublePtr rotate = doubleKnob(_transform, kTransformNodeParamRotate);
    KnobDoublePtr translate = doubleKnob(_transform, kTransformNodeParamTranslate);

    ASSERT_TRUE(rotate && translate);

    const double startR = rotate->getValue(0);
    const double startX = translate->getValue(0);
    const double startY = translate->getValue(1);
    const std::vector<KnobGuiPtr> rotateGuis(1, container().guiFor(rotate));
    const std::vector<KnobGuiPtr> translateGuis(1, container().guiFor(translate));
    ValueKnobEditSession session;

    ASSERT_TRUE(session.begin(rotateGuis, QString::fromUtf8("Global edit")));
    session.move(std::vector<double>(1, startR + 10.));

    ASSERT_TRUE(session.switchKnobs(translateGuis));
    EXPECT_EQ(2, session.getSlotCount());
    EXPECT_DOUBLE_EQ(startX, session.getStartValues()[0]);

    std::vector<double> values;

    values.push_back(startX + 3.);
    values.push_back(startY - 4.);
    session.move(values);

    // Back on rotate, the gesture carries on from its latest value but keeps its first start.
    ASSERT_TRUE(session.switchKnobs(rotateGuis));
    EXPECT_DOUBLE_EQ(startR, session.getStartValues()[0]);
    EXPECT_DOUBLE_EQ(startR + 10., session.getCurrentValues()[0]);
    session.move(std::vector<double>(1, startR + 20.));
    session.end(QString::fromUtf8("Global edit"));

    EXPECT_DOUBLE_EQ(startR + 20., rotate->getValue(0));
    EXPECT_DOUBLE_EQ(startX + 3., translate->getValue(0));
    EXPECT_DOUBLE_EQ(startY - 4., translate->getValue(1));
    ASSERT_EQ(1, container().undoStack.count());

    container().undoStack.undo();
    EXPECT_DOUBLE_EQ(startR, rotate->getValue(0));
    EXPECT_DOUBLE_EQ(startX, translate->getValue(0));
    EXPECT_DOUBLE_EQ(startY, translate->getValue(1));
}

TEST_F(ValueKnobEditSessionTest, AKnobDeletedMidGestureIsSkipped)
{
    KnobDoublePtr rotate = doubleKnob(_transform, kTransformNodeParamRotate);
    KnobDoublePtr doomed = _transform->getEffectInstance()->createDoubleKnob("doomed", "Doomed", 1);

    ASSERT_TRUE(rotate && doomed);

    const double startR = rotate->getValue(0);
    std::vector<KnobGuiPtr> guis;

    guis.push_back(container().guiFor(rotate));
    guis.push_back(container().guiFor(doomed));

    ValueKnobEditSession session;

    ASSERT_TRUE(session.begin(guis, QString::fromUtf8("Global edit")));

    std::vector<double> values;

    values.push_back(startR + 5.);
    values.push_back(1.);
    session.move(values);

    _transform->getEffectInstance()->deleteKnob(doomed.get(), false);
    doomed.reset();

    values[0] = startR + 10.;
    values[1] = 2.;
    session.move(values);
    session.end(QString::fromUtf8("Global edit"));

    EXPECT_DOUBLE_EQ(startR + 10., rotate->getValue(0));
    ASSERT_EQ(1, container().undoStack.count());

    container().undoStack.undo();
    EXPECT_DOUBLE_EQ(startR, rotate->getValue(0));
}

TEST_F(ValueKnobEditSessionTest, UniformScaleStrokeKeepsTheRowFolded)
{
    KnobDoublePtr scale = doubleKnob(_transform, kTransformNodeParamScale);

    ASSERT_TRUE(bool(scale));

    // A row with real widgets, the way a settings panel builds it: only then do value refreshes
    // reach KnobGuiValue::updateGUI(), which records a knob as unfolded.
    QWidget page;

    new QVBoxLayout(&page);

    GlobalModeTestContainer panel(&page);
    KnobGuiPtr gui = std::make_shared<KnobGuiDouble>(scale, &panel);

    gui->initialize();

    QWidget* label = new QWidget(&page);
    QWidget* field = new QWidget(&page);
    QHBoxLayout* fieldLayout = new QHBoxLayout(field);

    fieldLayout->setContentsMargins(0, 0, 0, 0);
    gui->createGUI(field, label, 0, 0, fieldLayout, true, 0, std::vector<KnobIPtr>());
    page.layout()->addWidget(field);
    panel.guis[scale.get()] = gui;
    ASSERT_FALSE(scale->getDimensionsUnfoldedInGui());

    const double start = scale->getValue(0);
    ValueKnobEditSession session;

    ASSERT_TRUE(session.begin(std::vector<KnobGuiPtr>(1, gui), QString::fromUtf8("Global edit")));
    session.move(std::vector<double>(2, start * 1.5));
    session.move(std::vector<double>(2, start * 2.));
    session.end(QString::fromUtf8("Global edit"));

    EXPECT_DOUBLE_EQ(start * 2., scale->getValue(0));
    EXPECT_DOUBLE_EQ(start * 2., scale->getValue(1));
    EXPECT_FALSE(scale->getDimensionsUnfoldedInGui());
    ASSERT_EQ(1, panel.undoStack.count());

    panel.undoStack.undo();
    EXPECT_DOUBLE_EQ(start, scale->getValue(0));
    EXPECT_DOUBLE_EQ(start, scale->getValue(1));
    EXPECT_FALSE(scale->getDimensionsUnfoldedInGui());

    panel.undoStack.redo();
    EXPECT_DOUBLE_EQ(start * 2., scale->getValue(1));
    EXPECT_FALSE(scale->getDimensionsUnfoldedInGui());

    panel.undoStack.clear();
    panel.guis.clear();
}

} // namespace
