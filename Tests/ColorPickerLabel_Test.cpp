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

#include <cmath>
#include <memory>
#include <string>

#include <QApplication>
#include <QEnterEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointF>
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

NATRON_NAMESPACE_USING

namespace {

// Pushes land straight on a real QUndoStack, so a drag's KnobUndoCommand runs exactly as a
// panel's container would, without creating any other widget.
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
baselineValue()
{
    KnobColorValue v;

    v.rgb[0] = kBaseline[0];
    v.rgb[1] = kBaseline[1];
    v.rgb[2] = kBaseline[2];
    v.a = kBaseline[3];
    v.hasAlpha = true;

    return v;
}

void
sendMouse(ColorPickerLabel& label,
          QEvent::Type type,
          const QPointF& pos,
          Qt::MouseButton button,
          Qt::MouseButtons buttons)
{
    QMouseEvent e(type, pos, label.mapToGlobal(pos), button, buttons, Qt::NoModifier);

    QApplication::sendEvent(&label, &e);
}

void
pressAt(ColorPickerLabel& label,
        const QPointF& pos)
{
    sendMouse(label, QEvent::MouseButtonPress, pos, Qt::LeftButton, Qt::LeftButton);
}

void
moveTo(ColorPickerLabel& label,
       const QPointF& pos)
{
    sendMouse(label, QEvent::MouseMove, pos, Qt::NoButton, Qt::LeftButton);
}

void
releaseAt(ColorPickerLabel& label,
          const QPointF& pos)
{
    sendMouse(label, QEvent::MouseButtonRelease, pos, Qt::LeftButton, Qt::NoButton);
}

void
enterLabel(ColorPickerLabel& label)
{
    QPointF pos(5.0, 5.0);
    QEnterEvent e(pos, pos, label.mapToGlobal(pos));

    QApplication::sendEvent(&label, &e);
}

void
sendKey(ColorPickerLabel& label,
        QEvent::Type type,
        int key)
{
    QKeyEvent e(type, key, Qt::NoModifier);

    QApplication::sendEvent(&label, &e);
}

} // namespace

TEST(ColorPickerLabel, ZeroPixelClickTogglesPickingAndRecordsNothing)
{
    KnobColorPtr knob = makeWhiteKnob();
    ASSERT_TRUE(bool(knob));

    FakeKnobGuiContainer container;
    std::shared_ptr<KnobGuiColor> knobGui = std::make_shared<KnobGuiColor>(knob, &container);
    ColorPickerLabel label(knobGui.get());

    EXPECT_FALSE(label.isPickingEnabled());

    pressAt(label, QPointF(5.0, 5.0));
    releaseAt(label, QPointF(5.0, 5.0));

    EXPECT_TRUE(label.isPickingEnabled());
    EXPECT_EQ(0, container.undoStack.count());
    EXPECT_DOUBLE_EQ(kBaseline[0], knob->getValue(0));
}

TEST(ColorPickerLabel, JitterUnderDragThresholdStillCountsAsAClick)
{
    KnobColorPtr knob = makeWhiteKnob();
    ASSERT_TRUE(bool(knob));

    FakeKnobGuiContainer container;
    std::shared_ptr<KnobGuiColor> knobGui = std::make_shared<KnobGuiColor>(knob, &container);
    ColorPickerLabel label(knobGui.get());

    pressAt(label, QPointF(5.0, 5.0));
    moveTo(label, QPointF(7.0, 5.0)); // 2 px, well under QApplication::startDragDistance()
    releaseAt(label, QPointF(7.0, 5.0));

    EXPECT_TRUE(label.isPickingEnabled());
    EXPECT_EQ(0, container.undoStack.count());
}

TEST(ColorPickerLabel, ThirtyPixelDragChangesChromaKeepsYLeavesPickingAloneAndRecordsOneUndoStep)
{
    KnobColorPtr knob = makeWhiteKnob();
    ASSERT_TRUE(bool(knob));

    FakeKnobGuiContainer container;
    std::shared_ptr<KnobGuiColor> knobGui = std::make_shared<KnobGuiColor>(knob, &container);
    ColorPickerLabel label(knobGui.get());
    LumaBasis basis = knobGui->getEditBasis();

    pressAt(label, QPointF(0.0, 0.0));
    moveTo(label, QPointF(30.0, 0.0));
    releaseAt(label, QPointF(30.0, 0.0));

    EXPECT_FALSE(label.isPickingEnabled());
    EXPECT_EQ(1, container.undoStack.count());

    KnobColorValue after = ColorKnobEditSession::readKnobValue(knob);
    double chromaMove = std::abs(after.rgb[0] - kBaseline[0])
        + std::abs(after.rgb[1] - kBaseline[1])
        + std::abs(after.rgb[2] - kBaseline[2]);

    EXPECT_GT(chromaMove, 1e-6);
    EXPECT_NEAR(luma(basis, kBaseline), luma(basis, after.rgb), 1e-9);
    EXPECT_DOUBLE_EQ(kBaseline[3], after.a);
}

TEST(ColorPickerLabel, VHeldDuringTheDragChangesOnlyY)
{
    KnobColorPtr knob = makeWhiteKnob();
    ASSERT_TRUE(bool(knob));

    FakeKnobGuiContainer container;
    std::shared_ptr<KnobGuiColor> knobGui = std::make_shared<KnobGuiColor>(knob, &container);
    ColorPickerLabel label(knobGui.get());
    LumaBasis basis = knobGui->getEditBasis();
    ColorKnobRoleEnum role = knobGui->getColorRole();

    enterLabel(label);
    EXPECT_EQ(&label, label.focusWidget());
    sendKey(label, QEvent::KeyPress, Qt::Key_V);

    pressAt(label, QPointF(0.0, 0.0));
    moveTo(label, QPointF(0.0, -30.0));
    releaseAt(label, QPointF(0.0, -30.0));
    sendKey(label, QEvent::KeyRelease, Qt::Key_V);

    EXPECT_EQ(1, container.undoStack.count());

    KnobColorValue after = ColorKnobEditSession::readKnobValue(knob);
    PlanePoint before = toPlane(role, basis, baselineValue());
    PlanePoint afterPoint = toPlane(role, basis, after);

    ASSERT_TRUE(before.valid);
    ASSERT_TRUE(afterPoint.valid);
    EXPECT_NEAR(before.x, afterPoint.x, 1e-9);
    EXPECT_NEAR(before.y, afterPoint.y, 1e-9);
    EXPECT_GT(std::abs(luma(basis, after.rgb) - luma(basis, kBaseline)), 1e-6);
}
