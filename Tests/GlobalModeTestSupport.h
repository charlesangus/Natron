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

#ifndef Tests_GlobalModeTestSupport_h
#define Tests_GlobalModeTestSupport_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <map>
#include <memory>
#include <string>

#include <QThreadPool>
#include <QUndoCommand>
#include <QUndoStack>
#include <QWidget>

#include <gtest/gtest.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"

#include "Gui/KnobGuiContainerI.h"
#include "Gui/KnobGuiValue.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief A settings panel's undo stack and knob lookup without the panel: KnobGuiDoubles made
 * through guiFor() are found again by getKnobGui(), and pushes land on a real QUndoStack so
 * undo and redo run as they do from the GUI. guiFor() creates no widget.
 **/
class GlobalModeTestContainer
    : public KnobGuiContainerI {
public:
    explicit GlobalModeTestContainer(QWidget* widget = 0)
        : KnobGuiContainerI(widget)
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

    virtual KnobGuiPtr getKnobGui(const KnobIPtr& knob) const OVERRIDE FINAL
    {
        std::map<KnobI*, KnobGuiPtr>::const_iterator it = guis.find(knob.get());

        return (it == guis.end()) ? KnobGuiPtr() : it->second;
    }

    virtual int getItemsSpacingOnSameLine() const OVERRIDE FINAL
    {
        return 0;
    }

    KnobGuiPtr guiFor(const KnobIPtr& knob)
    {
        KnobGuiPtr existing = getKnobGui(knob);

        if (existing) {
            return existing;
        }

        KnobGuiPtr gui;

        if (std::dynamic_pointer_cast<KnobDouble>(knob)) {
            gui = std::make_shared<KnobGuiDouble>(knob, this);
        } else if (std::dynamic_pointer_cast<KnobInt>(knob)) {
            gui = std::make_shared<KnobGuiInt>(knob, this);
        }
        if (gui) {
            guis[knob.get()] = gui;
        }

        return gui;
    }

    mutable QUndoStack undoStack;
    std::map<KnobI*, KnobGuiPtr> guis;
};

class GlobalModeNodeTest
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
        _container.reset();
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

    KnobDoublePtr doubleKnob(const NodePtr& node,
                             const char* name)
    {
        return std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(name));
    }

    GlobalModeTestContainer& container()
    {
        if (!_container) {
            _container.reset(new GlobalModeTestContainer);
        }

        return *_container;
    }

    AppInstancePtr _app;
    std::unique_ptr<GlobalModeTestContainer> _container;
};

NATRON_NAMESPACE_EXIT

#endif // Tests_GlobalModeTestSupport_h
