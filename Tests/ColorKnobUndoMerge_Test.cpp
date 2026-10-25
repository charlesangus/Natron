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

#include <list>
#include <memory>
#include <string>

#include <QString>
#include <QUndoCommand>
#include <QUndoStack>

#include <gtest/gtest.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/ColorWheelMath.h"
#include "Engine/KnobTypes.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

#include "Gui/ColorKnobEditSession.h"
#include "Gui/KnobGuiColor.h"
#include "Gui/KnobGuiContainerI.h"
#include "Gui/KnobUndoCommand.h"
#include "Gui/UndoGesture.h"

NATRON_NAMESPACE_USING

namespace {

// Pushes land straight on a real QUndoStack, so KnobUndoCommand's redo(), undo() and
// mergeWith() run exactly as a panel's container would, without creating any widget.
class FakeKnobGuiContainer
    : public KnobGuiContainerI {
public:
    FakeKnobGuiContainer()
        : KnobGuiContainerI()
    {
    }

    virtual Gui* getGui() const OVERRIDE FINAL
    {
        return 0;
    }

    virtual const QUndoCommand* getLastUndoCommand() const OVERRIDE FINAL
    {
        return undoStack.index() > 0 ? undoStack.command(undoStack.index() - 1) : 0;
    }

    virtual void pushUndoCommand(QUndoCommand* cmd) OVERRIDE FINAL
    {
        undoStack.push(cmd);
    }

    virtual KnobGuiPtr getKnobGui(const KnobIPtr& /*knob*/) const OVERRIDE FINAL
    {
        return KnobGuiPtr();
    }

    virtual int getItemsSpacingOnSameLine() const OVERRIDE FINAL
    {
        return 0;
    }

    mutable QUndoStack undoStack;
};

const double kBaseline[4] = { 0.25, 0.5, 0.75, 1.0 };

// A Project is a KnobHolder with a real (background) AppInstance behind it, which is what
// KnobUndoCommand::undo() needs: it dereferences the knob's holder unconditionally.
KnobColorPtr
makeWhiteKnob()
{
    AppInstancePtr app = appPTR->getTopLevelInstance();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();

    if (!project) {
        return KnobColorPtr();
    }

    KnobColorPtr knob = AppManager::createKnob<KnobColor>(project.get(), std::string("White"), 4, false);

    if (knob) {
        for (int i = 0; i < 4; ++i) {
            knob->setValue(kBaseline[i], ViewSpec::all(), i, true);
        }
    }

    return knob;
}

KnobColorValue
makeValue(double r,
          double g,
          double b,
          double a)
{
    KnobColorValue v;

    v.rgb[0] = r;
    v.rgb[1] = g;
    v.rgb[2] = b;
    v.a = a;
    v.hasAlpha = true;

    return v;
}

} // namespace

TEST(ColorKnobUndoMerge, TwoConsecutiveGesturesLeaveTwoUndoSteps)
{
    KnobColorPtr knob = makeWhiteKnob();
    ASSERT_TRUE(bool(knob));

    FakeKnobGuiContainer container;
    KnobGuiPtr knobGui = std::make_shared<KnobGuiColor>(knob, &container);
    ColorKnobEditSession session(knobGui);

    session.begin(QString::fromUtf8("Trackball"));
    session.move(makeValue(0.3, 0.5, 0.75, 1.0), QString::fromUtf8("Trackball"));
    session.end(makeValue(0.3, 0.5, 0.75, 1.0), QString::fromUtf8("Trackball"));
    EXPECT_EQ(1, container.undoStack.count());

    session.begin(QString::fromUtf8("Trackball"));
    session.move(makeValue(0.4, 0.5, 0.75, 1.0), QString::fromUtf8("Trackball"));
    session.end(makeValue(0.4, 0.5, 0.75, 1.0), QString::fromUtf8("Trackball"));
    EXPECT_EQ(2, container.undoStack.count());
}

TEST(ColorKnobUndoMerge, OneGestureOfManyMovesLeavesOneUndoStep)
{
    KnobColorPtr knob = makeWhiteKnob();
    ASSERT_TRUE(bool(knob));

    FakeKnobGuiContainer container;
    KnobGuiPtr knobGui = std::make_shared<KnobGuiColor>(knob, &container);
    ColorKnobEditSession session(knobGui);

    session.begin(QString::fromUtf8("Trackball"));
    session.move(makeValue(0.26, 0.5, 0.75, 1.0), QString::fromUtf8("Trackball"));
    session.move(makeValue(0.27, 0.5, 0.75, 1.0), QString::fromUtf8("Trackball"));
    session.move(makeValue(0.28, 0.5, 0.75, 1.0), QString::fromUtf8("Trackball"));
    session.end(makeValue(0.30, 0.5, 0.75, 1.0), QString::fromUtf8("Trackball"));

    EXPECT_EQ(1, container.undoStack.count());
    EXPECT_DOUBLE_EQ(0.30, knob->getValue(0));
}

TEST(ColorKnobUndoMerge, UndoRestoresTheGestureStartValue)
{
    KnobColorPtr knob = makeWhiteKnob();
    ASSERT_TRUE(bool(knob));

    FakeKnobGuiContainer container;
    KnobGuiPtr knobGui = std::make_shared<KnobGuiColor>(knob, &container);
    ColorKnobEditSession session(knobGui);

    session.begin(QString::fromUtf8("Trackball"));
    session.move(makeValue(0.9, 0.1, 0.2, 0.4), QString::fromUtf8("Trackball"));
    session.end(makeValue(0.9, 0.1, 0.2, 0.4), QString::fromUtf8("Trackball"));
    ASSERT_EQ(1, container.undoStack.count());

    container.undoStack.undo();

    EXPECT_DOUBLE_EQ(kBaseline[0], knob->getValue(0));
    EXPECT_DOUBLE_EQ(kBaseline[1], knob->getValue(1));
    EXPECT_DOUBLE_EQ(kBaseline[2], knob->getValue(2));
    EXPECT_DOUBLE_EQ(kBaseline[3], knob->getValue(3));
}

TEST(ColorKnobUndoMerge, NoChangeGestureRecordsNothing)
{
    KnobColorPtr knob = makeWhiteKnob();
    ASSERT_TRUE(bool(knob));

    FakeKnobGuiContainer container;
    KnobGuiPtr knobGui = std::make_shared<KnobGuiColor>(knob, &container);
    ColorKnobEditSession session(knobGui);

    KnobColorValue unchanged = ColorKnobEditSession::readKnobValue(knob);

    session.begin(QString::fromUtf8("Trackball"));
    session.move(unchanged, QString::fromUtf8("Trackball"));
    session.end(unchanged, QString::fromUtf8("Trackball"));

    EXPECT_EQ(0, container.undoStack.count());
}

TEST(ColorKnobUndoMerge, UndoTextNamesTheKnobAndTheAxis)
{
    KnobColorPtr knob = makeWhiteKnob();
    ASSERT_TRUE(bool(knob));

    FakeKnobGuiContainer container;
    KnobGuiPtr knobGui = std::make_shared<KnobGuiColor>(knob, &container);
    ColorKnobEditSession session(knobGui);

    session.begin(QString::fromUtf8("Trackball"));
    session.move(makeValue(0.3, 0.5, 0.75, 1.0), QString::fromUtf8("Trackball"));
    session.end(makeValue(0.3, 0.5, 0.75, 1.0), QString::fromUtf8("Trackball"));

    ASSERT_EQ(1, container.undoStack.count());
    const QUndoCommand* cmd = container.undoStack.command(0);
    ASSERT_TRUE(cmd != 0);
    EXPECT_EQ(QString::fromUtf8("White: trackball"), cmd->text());
}

TEST(ColorKnobUndoMerge, SeparateUnkeyedEditsStayTwoUndoSteps)
{
    KnobColorPtr knob = makeWhiteKnob();
    ASSERT_TRUE(bool(knob));

    FakeKnobGuiContainer container;
    KnobGuiPtr knobGui = std::make_shared<KnobGuiColor>(knob, &container);

    std::list<double> start;
    for (int i = 0; i < 4; ++i) {
        start.push_back(knob->getValue(i));
    }

    std::list<double> mid = start;
    *mid.begin() = 0.4;
    knobGui->pushUndoCommand(new KnobUndoCommand<double>(knobGui, start, mid, false));
    EXPECT_EQ(1, container.undoStack.count());

    std::list<double> last = start;
    *last.begin() = 0.6;
    knobGui->pushUndoCommand(new KnobUndoCommand<double>(knobGui, start, last, false));

    // Key 0 means each push is a separate commit, so editing the same knob twice must leave
    // two undo steps; folding them made one undo jump back past both edits.
    EXPECT_EQ(2, container.undoStack.count());
    EXPECT_DOUBLE_EQ(0.6, knob->getValue(0));
}

TEST(ColorKnobUndoMerge, PushesOfOneGestureFoldIntoOneUndoStep)
{
    KnobColorPtr knob = makeWhiteKnob();
    ASSERT_TRUE(bool(knob));

    FakeKnobGuiContainer container;
    KnobGuiPtr knobGui = std::make_shared<KnobGuiColor>(knob, &container);

    std::list<double> start;
    for (int i = 0; i < 4; ++i) {
        start.push_back(knob->getValue(i));
    }

    UndoGesture drag;
    drag.begin();
    for (int step = 1; step <= 5; ++step) {
        std::list<double> next = start;
        *next.begin() = 0.1 * step;
        knobGui->pushUndoCommand(new KnobUndoCommand<double>(knobGui, start, next, false, drag.key()));
    }
    drag.end();
    EXPECT_EQ(1, container.undoStack.count());
    EXPECT_DOUBLE_EQ(0.5, knob->getValue(0));
}

TEST(ColorKnobUndoMerge, NonZeroMergeKeysOnlyMergeWhenEqual)
{
    KnobColorPtr knob = makeWhiteKnob();
    ASSERT_TRUE(bool(knob));

    FakeKnobGuiContainer container;
    KnobGuiPtr knobGui = std::make_shared<KnobGuiColor>(knob, &container);

    std::list<double> start;
    for (int i = 0; i < 4; ++i) {
        start.push_back(knob->getValue(i));
    }

    std::list<double> a = start;
    *a.begin() = 0.4;
    std::list<double> b = start;
    *b.begin() = 0.5;

    knobGui->pushUndoCommand(new KnobUndoCommand<double>(knobGui, start, a, true, 5));
    knobGui->pushUndoCommand(new KnobUndoCommand<double>(knobGui, start, b, true, 5));
    EXPECT_EQ(1, container.undoStack.count());

    std::list<double> c = start;
    *c.begin() = 0.7;
    knobGui->pushUndoCommand(new KnobUndoCommand<double>(knobGui, start, c, true, 6));
    EXPECT_EQ(2, container.undoStack.count());
}

TEST(ColorKnobUndoMerge, ZeroKeyNeverMergesWithANonZeroKey)
{
    KnobColorPtr knob = makeWhiteKnob();
    ASSERT_TRUE(bool(knob));

    FakeKnobGuiContainer container;
    KnobGuiPtr knobGui = std::make_shared<KnobGuiColor>(knob, &container);

    std::list<double> start;
    for (int i = 0; i < 4; ++i) {
        start.push_back(knob->getValue(i));
    }

    std::list<double> a = start;
    *a.begin() = 0.4;
    std::list<double> b = start;
    *b.begin() = 0.5;

    knobGui->pushUndoCommand(new KnobUndoCommand<double>(knobGui, start, a, true, 0));
    knobGui->pushUndoCommand(new KnobUndoCommand<double>(knobGui, start, b, true, 9));

    EXPECT_EQ(2, container.undoStack.count());
}

TEST(ColorKnobUndoMerge, AGestureThatLeavesAlphaAloneNeverWritesIt)
{
    KnobColorPtr knob = makeWhiteKnob();
    ASSERT_TRUE(bool(knob));

    FakeKnobGuiContainer container;
    KnobGuiPtr knobGui = std::make_shared<KnobGuiColor>(knob, &container);
    ColorKnobEditSession session(knobGui);

    session.begin(QString::fromUtf8("Hue"));
    session.move(makeValue(0.3, 0.5, 0.7, kBaseline[3]), QString::fromUtf8("Hue"));
    session.end(makeValue(0.3, 0.5, 0.7, kBaseline[3]), QString::fromUtf8("Hue"));
    ASSERT_EQ(1, container.undoStack.count());

    // Set outside the undo stack, so an undo that wrote alpha would put the baseline back.
    knob->setValue(0.9, ViewSpec::all(), 3, true);
    container.undoStack.undo();

    EXPECT_DOUBLE_EQ(kBaseline[0], knob->getValue(0));
    EXPECT_DOUBLE_EQ(kBaseline[2], knob->getValue(2));
    EXPECT_DOUBLE_EQ(0.9, knob->getValue(3));
}

TEST(ColorKnobUndoMerge, AlphaJoiningMidGestureStaysOneUndoStep)
{
    KnobColorPtr knob = makeWhiteKnob();
    ASSERT_TRUE(bool(knob));

    FakeKnobGuiContainer container;
    KnobGuiPtr knobGui = std::make_shared<KnobGuiColor>(knob, &container);
    ColorKnobEditSession session(knobGui);

    session.begin(QString::fromUtf8("Value"));
    session.move(makeValue(0.3, 0.5, 0.75, kBaseline[3]), QString::fromUtf8("Value"));
    session.move(makeValue(0.5, 1.0, 1.5, 2.0), QString::fromUtf8("Value"));
    session.end(makeValue(0.5, 1.0, 1.5, 2.0), QString::fromUtf8("Value"));

    ASSERT_EQ(1, container.undoStack.count());
    EXPECT_DOUBLE_EQ(2.0, knob->getValue(3));

    container.undoStack.undo();

    for (int i = 0; i < 4; ++i) {
        EXPECT_DOUBLE_EQ(kBaseline[i], knob->getValue(i)) << "dimension " << i;
    }

    container.undoStack.redo();

    EXPECT_DOUBLE_EQ(0.5, knob->getValue(0));
    EXPECT_DOUBLE_EQ(2.0, knob->getValue(3));
}
