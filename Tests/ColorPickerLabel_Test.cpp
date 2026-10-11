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

#include <QApplication>
#include <QMouseEvent>
#include <QPointF>
#include <QString>
#include <QUndoCommand>
#include <QUndoStack>

#include <gtest/gtest.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/KnobTypes.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

#include "Gui/KnobGuiColor.h"
#include "Gui/KnobGuiContainerI.h"

NATRON_NAMESPACE_USING

namespace {

// KnobGuiColor needs a KnobGuiContainerI to construct; a real QUndoStack behind it lets a
// test assert on undo-step counts without building any other widget.
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
        // Grade's white is a Gain; left on Auto, this knob's zero defaults and unbounded display
        // minimum would infer an Offset, whose Value edit is a flat lift that moves the plane point.
        knob->setColorRole(eColorKnobRoleGain);
        for (int i = 0; i < 4; ++i) {
            knob->setValue(kBaseline[i], ViewSpec::all(), i, true);
        }
    }

    return knob;
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

TEST(ColorPickerLabel, ThirtyPixelDragChangesNothingAndDoesNotToggle)
{
    KnobColorPtr knob = makeWhiteKnob();
    ASSERT_TRUE(bool(knob));

    FakeKnobGuiContainer container;
    std::shared_ptr<KnobGuiColor> knobGui = std::make_shared<KnobGuiColor>(knob, &container);
    ColorPickerLabel label(knobGui.get());

    pressAt(label, QPointF(0.0, 0.0));
    moveTo(label, QPointF(30.0, 0.0));
    releaseAt(label, QPointF(30.0, 0.0));

    EXPECT_FALSE(label.isPickingEnabled());
    EXPECT_EQ(0, container.undoStack.count());
    for (int i = 0; i < 4; ++i) {
        EXPECT_DOUBLE_EQ(kBaseline[i], knob->getValue(i));
    }
}
