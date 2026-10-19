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

#include <QThreadPool>
#include <QUndoCommand>
#include <QUndoStack>

#include <gtest/gtest.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

#include "Gui/KnobGuiContainerI.h"
#include "Gui/KnobGuiValue.h"
#include "Gui/KnobUndoCommand.h"

NATRON_NAMESPACE_USING

namespace {

// A settings panel's undo stack without the panel: pushes land on a real QUndoStack, so
// KnobUndoCommand's redo() and undo() run as they do from the GUI, without creating a widget.
class UndoStackKnobGuiContainer
    : public KnobGuiContainerI {
public:
    UndoStackKnobGuiContainer()
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

class KnobUndoHashTest
    : public ::testing::Test {
protected:
    virtual void SetUp() OVERRIDE
    {
        _app = appPTR->getTopLevelInstance();
        ASSERT_TRUE(bool(_app));
        _app->getProject()->reset(false, true);
    }

    virtual void TearDown() OVERRIDE
    {
        if (_app) {
            _app->getProject()->reset(false, true);
        }
        QThreadPool::globalInstance()->waitForDone();
    }

    NodePtr create(const char* pluginID,
                   int majorVersion)
    {
        CreateNodeArgs args(pluginID, _app->getProject());

        args.setProperty<int>(kCreateNodeArgsPropPluginVersion, majorVersion, 0);
        args.setProperty<int>(kCreateNodeArgsPropPluginVersion, -1, 1);

        return _app->createNode(args);
    }

    AppInstancePtr _app;
};

} // namespace

// The cache is keyed by the node hash, so a return to the exact earlier hash, on the node and
// downstream, is a return to the entries cached for that state.
TEST_F(KnobUndoHashTest, UndoAndRedoOfAKnobEditRestoreTheHashesUpToDownstream)
{
    NodePtr constant = create(PLUGINID_NATRON_CONSTANT, -1);
    NodePtr blur = create(PLUGINID_NATRON_BLUR, PLUGIN_MAJOR_NATRON_BLUR);
    NodePtr downstream = create(PLUGINID_NATRON_BLUR, PLUGIN_MAJOR_NATRON_BLUR);
    ASSERT_TRUE(constant && blur && downstream);
    ASSERT_TRUE(_app->getProject()->connectNodes(0, constant, blur));
    ASSERT_TRUE(_app->getProject()->connectNodes(0, blur, downstream));
    KnobDoublePtr size = std::dynamic_pointer_cast<KnobDouble>(blur->getKnobByName(kBlurParamSize));
    ASSERT_TRUE(bool(size));
    size->setValue(4., ViewSpec::all(), 0);

    UndoStackKnobGuiContainer container;
    KnobGuiPtr knobGui = std::make_shared<KnobGuiDouble>(size, &container);

    const U64 before = blur->getHashValue();
    const U64 downstreamBefore = downstream->getHashValue();

    knobGui->pushUndoCommand(new KnobUndoCommand<double>(knobGui, 4., 7., 0, false));
    ASSERT_EQ(1, container.undoStack.count());
    ASSERT_DOUBLE_EQ(7., size->getValue(0));
    const U64 edited = blur->getHashValue();
    const U64 downstreamEdited = downstream->getHashValue();
    EXPECT_NE(before, edited);
    EXPECT_NE(downstreamBefore, downstreamEdited);

    container.undoStack.undo();
    ASSERT_DOUBLE_EQ(4., size->getValue(0));
    EXPECT_EQ(before, blur->getHashValue());
    EXPECT_EQ(downstreamBefore, downstream->getHashValue());

    container.undoStack.redo();
    ASSERT_DOUBLE_EQ(7., size->getValue(0));
    EXPECT_EQ(edited, blur->getHashValue());
    EXPECT_EQ(downstreamEdited, downstream->getHashValue());

    container.undoStack.undo();
    EXPECT_EQ(before, blur->getHashValue());
    EXPECT_EQ(downstreamBefore, downstream->getHashValue());
}
