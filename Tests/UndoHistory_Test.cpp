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

#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include <QString>
#include <QUndoCommand>

#include <gtest/gtest.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"

#include "Gui/KnobGuiColor.h"
#include "Gui/KnobGuiContainerI.h"
#include "Gui/KnobUndoCommand.h"

#include "Gui/UndoHistory.h"

NATRON_NAMESPACE_USING

namespace {

class FakeHolder
    : public KnobHolder {
public:
    FakeHolder()
        : KnobHolder(AppInstancePtr())
    {
    }

    virtual void initializeKnobs() OVERRIDE FINAL
    {
    }
};

typedef std::vector<std::string> Log;

// Appends "+name" on redo and "-name" on undo. A non -1 merge id lets same-id commands merge,
// and obsoleteAfterRedo marks the command obsolete from its first redo on.
class LogCommand
    : public QUndoCommand {
public:
    LogCommand(Log* log,
               const std::string& name,
               int mergeId = -1)
        : QUndoCommand()
        , _log(log)
        , _name(name)
        , _mergeId(mergeId)
        , obsoleteAfterRedo(false)
        , deleted(0)
    {
        setText(QString::fromStdString(name));
    }

    virtual ~LogCommand()
    {
        if (deleted) {
            *deleted = true;
        }
    }

    virtual void redo() OVERRIDE FINAL
    {
        _log->push_back("+" + _name);
        if (obsoleteAfterRedo) {
            setObsolete(true);
        }
    }

    virtual void undo() OVERRIDE FINAL
    {
        _log->push_back("-" + _name);
    }

    virtual int id() const OVERRIDE FINAL
    {
        return _mergeId;
    }

    virtual bool mergeWith(const QUndoCommand* other) OVERRIDE FINAL
    {
        const LogCommand* o = dynamic_cast<const LogCommand*>(other);
        if (!o) {
            return false;
        }
        _name += o->_name;
        setText(QString::fromStdString(_name));

        return true;
    }

    Log* _log;
    std::string _name;
    int _mergeId;
    bool obsoleteAfterRedo;
    bool* deleted;
};

class UndoHistoryTest
    : public ::testing::Test {
protected:
    UndoHistoryTest()
        : a(new FakeHolder)
        , b(new FakeHolder)
    {
    }

    void push(const std::string& name,
              const UndoScope& scope = UndoScope(),
              int mergeId = -1)
    {
        history.push(new LogCommand(&log, name, mergeId), scope);
    }

    UndoScope ownedBy(KnobHolder* owner)
    {
        return UndoScope(owner);
    }

    UndoScope touching(KnobHolder* holder)
    {
        UndoScope s;
        s.touch(holder);

        return s;
    }

    // The log entries appended since the last call.
    Log take()
    {
        Log ret = log;
        log.clear();

        return ret;
    }

    Log log;
    std::unique_ptr<FakeHolder> a;
    std::unique_ptr<FakeHolder> b;
    UndoHistory history;
};

Log
L(std::initializer_list<const char*> items)
{
    Log ret;
    for (const char* s : items) {
        ret.push_back(s);
    }

    return ret;
}
} // anon namespace

TEST_F(UndoHistoryTest, GlobalOrderAcrossOwners)
{
    push("a1", ownedBy(a.get()));
    push("move");
    push("b1", ownedBy(b.get()));
    push("a2", ownedBy(a.get()));
    EXPECT_EQ(L({ "+a1", "+move", "+b1", "+a2" }), take());
    EXPECT_EQ(4, history.count());

    EXPECT_EQ(QString::fromUtf8("a2"), history.undoText());
    history.undo();
    history.undo();
    history.undo();
    history.undo();
    EXPECT_EQ(L({ "-a2", "-b1", "-move", "-a1" }), take());
    EXPECT_FALSE(history.canUndo());
    EXPECT_TRUE(history.canRedo());

    history.undo();
    EXPECT_TRUE(take().empty());
}

TEST_F(UndoHistoryTest, RedoReplaysMostRecentlyUndone)
{
    push("x");
    push("y");
    push("z");
    take();
    history.undo();
    history.undo();
    EXPECT_EQ(QString::fromUtf8("y"), history.redoText());
    history.redo();
    history.undo();
    history.undo();
    history.redo();
    history.redo();
    history.redo();
    EXPECT_EQ(L({ "-z", "-y", "+y", "-y", "-x", "+x", "+y", "+z" }), take());
    EXPECT_FALSE(history.canRedo());
}

TEST_F(UndoHistoryTest, PanelUndoOfAnOlderEntry)
{
    push("a1", ownedBy(a.get()));
    push("b1", ownedBy(b.get()));
    push("move");
    take();

    ASSERT_TRUE(history.canUndo(a.get()));
    EXPECT_EQ(QString::fromUtf8("a1"), history.undoText(a.get()));
    history.undo(a.get());
    EXPECT_EQ(L({ "-a1" }), take());
    EXPECT_FALSE(history.canUndo(a.get()));
    EXPECT_TRUE(history.canRedo(a.get()));

    // Global undo still walks the done entries newest first.
    history.undo();
    history.undo();
    EXPECT_EQ(L({ "-move", "-b1" }), take());
    EXPECT_FALSE(history.canUndo());

    // b1 was undone last, so it is redone first.
    history.redo();
    history.redo();
    history.redo();
    EXPECT_EQ(L({ "+b1", "+move", "+a1" }), take());
}

TEST_F(UndoHistoryTest, PanelUndoBlockedByLaterEntryTouchingTheNode)
{
    push("a1", ownedBy(a.get()));
    push("deleteA", touching(a.get()));
    take();

    EXPECT_FALSE(history.canUndo(a.get()));
    history.undo(a.get());
    EXPECT_TRUE(take().empty());

    history.undo();
    EXPECT_EQ(L({ "-deleteA" }), take());
    EXPECT_TRUE(history.canUndo(a.get()));

    // The undone delete now blocks a's panel redo until it is redone: a1 is not undone yet.
    EXPECT_FALSE(history.canRedo(a.get()));
    history.undo(a.get());
    EXPECT_EQ(L({ "-a1" }), take());

    // a1 was undone after deleteA, so it is a's most recent undone entry.
    EXPECT_TRUE(history.canRedo(a.get()));
    history.redo(a.get());
    EXPECT_EQ(L({ "+a1" }), take());
    EXPECT_FALSE(history.canRedo(a.get()));
}

TEST_F(UndoHistoryTest, PanelUndoOwnedByOtherBlocksWhenTouchingNode)
{
    push("a1", ownedBy(a.get()));
    UndoScope bTouchingA(b.get());
    bTouchingA.touch(a.get());
    push("b1", bTouchingA);
    take();

    EXPECT_FALSE(history.canUndo(a.get()));
    EXPECT_TRUE(history.canUndo(b.get()));
}

TEST_F(UndoHistoryTest, PushDropsUndoneEntries)
{
    bool deleted = false;
    LogCommand* cmd = new LogCommand(&log, "x");
    cmd->deleted = &deleted;
    history.push(cmd, ownedBy(a.get()));
    push("y");
    history.undo(a.get());
    take();
    EXPECT_EQ(2, history.count());

    push("z");
    EXPECT_TRUE(deleted);
    EXPECT_EQ(2, history.count());
    EXPECT_FALSE(history.canRedo());
    history.undo();
    history.undo();
    EXPECT_EQ(L({ "+z", "-z", "-y" }), take());
}

TEST_F(UndoHistoryTest, MergeOnlyWithNewestSameOwner)
{
    const int kId = 7;
    push("a", ownedBy(a.get()), kId);
    push("b", ownedBy(a.get()), kId);
    EXPECT_EQ(1, history.count());
    EXPECT_EQ(QString::fromUtf8("ab"), history.undoText());

    push("c", ownedBy(b.get()), kId);
    EXPECT_EQ(2, history.count());

    // The newest entry is b's, so a's command does not reach back to a's entry.
    push("d", ownedBy(a.get()), kId);
    EXPECT_EQ(3, history.count());

    push("e", UndoScope(), kId);
    push("f", UndoScope(), kId);
    EXPECT_EQ(4, history.count());

    push("g", UndoScope(), kId + 1);
    EXPECT_EQ(5, history.count());

    const QUndoCommand* last = history.getLastCommand();
    ASSERT_TRUE(last != 0);
    EXPECT_EQ(QString::fromUtf8("g"), last->text());
    history.undo();
    EXPECT_TRUE(history.getLastCommand() == 0);
}

TEST_F(UndoHistoryTest, MergedScopeTouchesBothNodes)
{
    const int kId = 3;
    push("m1", touching(a.get()), kId);
    push("m2", touching(b.get()), kId);
    push("b1", ownedBy(b.get()));
    history.undo(b.get());
    take();

    EXPECT_EQ(2, history.count());
    EXPECT_FALSE(history.canUndo(b.get()));
}

TEST_F(UndoHistoryTest, LimitTrimsOldestFirst)
{
    history.setUndoLimit(3);
    push("1");
    push("2");
    push("3");
    push("4");
    push("5");
    take();
    EXPECT_EQ(3, history.count());
    history.undo();
    history.undo();
    history.undo();
    history.undo();
    EXPECT_EQ(L({ "-5", "-4", "-3" }), take());

    history.redo();
    history.redo();
    history.redo();
    take();
    history.setUndoLimit(1);
    EXPECT_EQ(1, history.count());
    history.undo();
    EXPECT_EQ(L({ "-5" }), take());
    EXPECT_FALSE(history.canUndo());
}

TEST_F(UndoHistoryTest, ObsoleteEntriesAreSkipped)
{
    LogCommand* gone = new LogCommand(&log, "gone");
    gone->obsoleteAfterRedo = true;
    history.push(gone);
    EXPECT_EQ(0, history.count());

    push("keep");
    LogCommand* later = new LogCommand(&log, "later");
    history.push(later);
    later->setObsolete(true);
    take();

    EXPECT_EQ(QString::fromUtf8("keep"), history.undoText());
    history.undo();
    EXPECT_EQ(L({ "-keep" }), take());
    EXPECT_EQ(1, history.count());
}

TEST_F(UndoHistoryTest, EntriesOfADestroyedOwnerAreSkipped)
{
    push("before");
    push("a1", ownedBy(a.get()));
    take();
    a.reset();

    EXPECT_EQ(QString::fromUtf8("before"), history.undoText());
    history.undo();
    EXPECT_EQ(L({ "-before" }), take());
    EXPECT_EQ(1, history.count());
}

TEST_F(UndoHistoryTest, ClearDeletesEverythingAndSignals)
{
    int changes = 0;
    QObject::connect(&history, &UndoHistory::changed, [&changes]() { ++changes; });
    push("x");
    push("y");
    history.undo();
    EXPECT_EQ(3, changes);
    history.clear();
    EXPECT_EQ(4, changes);
    EXPECT_EQ(0, history.count());
    EXPECT_FALSE(history.canUndo());
    EXPECT_FALSE(history.canRedo());
}

namespace {
// Pushes a nested command and clears the history from inside its own redo().
class ReentrantCommand
    : public QUndoCommand {
public:
    ReentrantCommand(UndoHistory* history,
                     Log* log,
                     bool clearInRedo)
        : _history(history)
        , _log(log)
        , _clearInRedo(clearInRedo)
    {
    }

    virtual void redo() OVERRIDE FINAL
    {
        _history->push(new LogCommand(_log, "nested"));
        if (_clearInRedo) {
            _history->clear();
        }
    }

    virtual void undo() OVERRIDE FINAL
    {
    }

    UndoHistory* _history;
    Log* _log;
    bool _clearInRedo;
};
} // anon namespace

TEST_F(UndoHistoryTest, NestedPushIsExecutedButNotRecorded)
{
    history.push(new ReentrantCommand(&history, &log, false));
    EXPECT_EQ(L({ "+nested" }), take());
    EXPECT_EQ(1, history.count());
}

TEST_F(UndoHistoryTest, ClearDuringPushIsDeferred)
{
    push("x");
    history.push(new ReentrantCommand(&history, &log, true));
    EXPECT_EQ(0, history.count());
}

namespace {
// What a node panel does with a knob GUI's pushes: record them in the window's history.
class HistoryKnobGuiContainer
    : public KnobGuiContainerI {
public:
    explicit HistoryKnobGuiContainer(UndoHistory* history)
        : KnobGuiContainerI()
        , _history(history)
    {
    }

    virtual Gui* getGui() const OVERRIDE FINAL
    {
        return 0;
    }

    virtual const QUndoCommand* getLastUndoCommand() const OVERRIDE FINAL
    {
        return _history->getLastCommand();
    }

    virtual void pushUndoCommand(QUndoCommand* cmd) OVERRIDE FINAL
    {
        _history->push(cmd);
    }

    virtual KnobGuiPtr getKnobGui(const KnobIPtr& /*knob*/) const OVERRIDE FINAL
    {
        return KnobGuiPtr();
    }

    virtual int getItemsSpacingOnSameLine() const OVERRIDE FINAL
    {
        return 0;
    }

private:
    UndoHistory* _history;
};

// A project knob: its holder has the real (background) AppInstance that KnobUndoCommand needs.
template <typename K>
std::shared_ptr<K>
makeProjectKnob(const std::string& label,
                int dimension)
{
    AppInstancePtr app = appPTR->getTopLevelInstance();
    ProjectPtr project = app ? app->getProject() : ProjectPtr();

    if (!project) {
        return std::shared_ptr<K>();
    }

    return AppManager::createKnob<K>(project.get(), label, dimension, false);
}
} // anon namespace

TEST(KnobUndoRecording, EditOfANonRenderingProjectKnobIsRecorded)
{
    KnobColorPtr knob = makeProjectKnob<KnobColor>("Non-rendering colour", 3);
    ASSERT_TRUE(bool(knob));
    knob->setEvaluateOnChange(false);
    knob->setValue(0.25, ViewSpec::all(), 0, true);

    UndoHistory history;
    HistoryKnobGuiContainer container(&history);
    KnobGuiPtr knobGui = std::make_shared<KnobGuiColor>(knob, &container);

    knobGui->pushUndoCommand(new KnobUndoCommand<double>(knobGui, 0.25, 0.75, 0, false));
    EXPECT_DOUBLE_EQ(0.75, knob->getValue(0));
    ASSERT_EQ(1, history.count());

    history.undo();
    EXPECT_DOUBLE_EQ(0.25, knob->getValue(0));
    history.redo();
    EXPECT_DOUBLE_EQ(0.75, knob->getValue(0));
}

TEST(KnobUndoRecording, EditOfANonUndoableKnobIsAppliedButNotRecorded)
{
    KnobColorPtr knob = makeProjectKnob<KnobColor>("Non-undoable colour", 3);
    ASSERT_TRUE(bool(knob));
    knob->setCanUndo(false);
    knob->setValue(0.25, ViewSpec::all(), 0, true);

    UndoHistory history;
    HistoryKnobGuiContainer container(&history);
    KnobGuiPtr knobGui = std::make_shared<KnobGuiColor>(knob, &container);

    knobGui->pushUndoCommand(new KnobUndoCommand<double>(knobGui, 0.25, 0.75, 0, false));
    EXPECT_DOUBLE_EQ(0.75, knob->getValue(0));
    EXPECT_EQ(0, history.count());
}

TEST(KnobUndoRecording, WhichKnobEditsAreUndoable)
{
    KnobColorPtr rendering = makeProjectKnob<KnobColor>("Rendering colour", 3);
    KnobColorPtr nonRendering = makeProjectKnob<KnobColor>("Saved colour", 3);
    KnobColorPtr transient = makeProjectKnob<KnobColor>("Transient colour", 3);
    KnobButtonPtr button = makeProjectKnob<KnobButton>("Action", 1);
    KnobButtonPtr renderingButton = makeProjectKnob<KnobButton>("Rendering toggle", 1);
    ASSERT_TRUE(rendering && nonRendering && transient && button && renderingButton);

    nonRendering->setEvaluateOnChange(false);
    transient->setEvaluateOnChange(false);
    transient->setIsPersistent(false);
    button->setEvaluateOnChange(false);
    renderingButton->setEvaluateOnChange(true);

    EXPECT_TRUE(KnobGui::isUndoableEdit(rendering));
    EXPECT_TRUE(KnobGui::isUndoableEdit(nonRendering));
    EXPECT_FALSE(KnobGui::isUndoableEdit(transient));
    EXPECT_FALSE(KnobGui::isUndoableEdit(button));
    EXPECT_TRUE(KnobGui::isUndoableEdit(renderingButton));
    EXPECT_FALSE(KnobGui::isUndoableEdit(KnobIPtr()));
}
