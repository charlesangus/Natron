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
#include <list>
#include <memory>
#include <string>
#include <vector>

#include <QString>
#include <QThreadPool>
#include <QUndoCommand>

#include <gtest/gtest.h>

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CreateNodeArgs.h"
#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"
#include "Serialization/NodeSerialization.h"
#include "Serialization/Yaml/PresetYaml.h"

#include "Gui/KnobGuiColor.h"
#include "Gui/KnobGuiContainerI.h"
#include "Gui/KnobGuiValue.h"
#include "Gui/KnobUndoCommand.h"

#include "Gui/UndoGesture.h"
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

TEST_F(UndoHistoryTest, OwnerlessEntriesTouchingADestroyedHolderAreSkipped)
{
    push("before");
    push("move", touching(a.get()));
    take();
    a.reset();

    EXPECT_EQ(QString::fromUtf8("before"), history.undoText());
    history.undo();
    EXPECT_EQ(L({ "-before" }), take());
    EXPECT_EQ(1, history.count());
}

TEST_F(UndoHistoryTest, UndoneEntriesTouchingADestroyedHolderAreNotRedone)
{
    push("move", touching(a.get()));
    history.undo();
    EXPECT_EQ(L({ "+move", "-move" }), take());
    b.reset();
    EXPECT_TRUE(history.canRedo());
    a.reset();

    EXPECT_FALSE(history.canRedo());
    history.redo();
    EXPECT_TRUE(take().empty());
    EXPECT_EQ(0, history.count());
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

TEST_F(UndoHistoryTest, AStepMakesOneEntryOfItsPushesAndRecords)
{
    history.openStep(QString::fromUtf8("script"));
    EXPECT_TRUE(history.isStepOpen());
    EXPECT_TRUE(history.isCapturing());
    push("p1", ownedBy(a.get()));
    history.recordApplied(new LogCommand(&log, "r1"), touching(b.get()));
    push("p2");
    history.closeStep();
    EXPECT_FALSE(history.isStepOpen());
    EXPECT_FALSE(history.isCapturing());

    // Recorded edits are already made: only the pushes ran.
    EXPECT_EQ(L({ "+p1", "+p2" }), take());
    EXPECT_EQ(1, history.count());
    EXPECT_EQ(QString::fromUtf8("script"), history.undoText());

    history.undo();
    EXPECT_EQ(L({ "-p2", "-r1", "-p1" }), take());
    history.redo();
    EXPECT_EQ(L({ "+p1", "+r1", "+p2" }), take());
}

TEST_F(UndoHistoryTest, AStepHoldingOneCommandIsThatCommand)
{
    history.openStep(QString::fromUtf8("script"));
    push("p", ownedBy(a.get()));
    history.closeStep();
    EXPECT_EQ(1, history.count());
    EXPECT_EQ(QString::fromUtf8("p"), history.undoText());
    EXPECT_TRUE(history.canUndo(a.get()));
}

TEST_F(UndoHistoryTest, AStepTouchesWhatItsPartsTouch)
{
    history.openStep(QString::fromUtf8("script"));
    history.recordApplied(new LogCommand(&log, "r1"), touching(a.get()));
    history.recordApplied(new LogCommand(&log, "r2"), touching(a.get()));
    history.closeStep();
    EXPECT_TRUE(history.canUndo(a.get()));
    EXPECT_FALSE(history.canUndo(b.get()));
}

TEST_F(UndoHistoryTest, EmptyAndNestedSteps)
{
    history.openStep(QString::fromUtf8("empty"));
    history.closeStep();
    EXPECT_EQ(0, history.count());

    history.openStep(QString::fromUtf8("outer"));
    history.openStep(QString::fromUtf8("inner"));
    history.recordApplied(new LogCommand(&log, "r1"), UndoScope());
    history.closeStep();
    EXPECT_TRUE(history.isStepOpen());
    history.recordApplied(new LogCommand(&log, "r2"), UndoScope());
    history.closeStep();
    EXPECT_EQ(1, history.count());
    EXPECT_EQ(QString::fromUtf8("outer"), history.undoText());
    history.undo();
    EXPECT_EQ(L({ "-r2", "-r1" }), take());
}

TEST_F(UndoHistoryTest, ARecordOutsideAnyStepOrPushIsDeleted)
{
    bool deleted = false;
    LogCommand* cmd = new LogCommand(&log, "r");
    cmd->deleted = &deleted;
    EXPECT_FALSE(history.isCapturing());
    history.recordApplied(cmd, UndoScope());
    EXPECT_TRUE(deleted);
    EXPECT_EQ(0, history.count());
    EXPECT_TRUE(take().empty());
}

TEST_F(UndoHistoryTest, ClearDuringAStepEmptiesIt)
{
    bool deleted = false;
    history.openStep(QString::fromUtf8("script"));
    LogCommand* cmd = new LogCommand(&log, "r");
    cmd->deleted = &deleted;
    history.recordApplied(cmd, UndoScope());
    history.clear();
    EXPECT_TRUE(deleted);
    history.closeStep();
    EXPECT_EQ(0, history.count());
}

TEST_F(UndoHistoryTest, TheCaptureSerialChangesPerOutermostStep)
{
    quint64 before = history.getCaptureSerial();
    history.openStep(QString());
    quint64 inStep = history.getCaptureSerial();
    EXPECT_NE(before, inStep);
    history.openStep(QString());
    EXPECT_EQ(inStep, history.getCaptureSerial());
    history.closeStep();
    history.closeStep();
    history.openStep(QString());
    EXPECT_NE(inStep, history.getCaptureSerial());
    history.closeStep();
}

namespace {
// Stands for a pushed edit whose Python callback makes an edit of its own, which is recorded
// while the edit executes. Same-id commands merge, and so do their records.
class CallbackCommand
    : public QUndoCommand {
public:
    CallbackCommand(UndoHistory* history,
                    Log* log,
                    const std::string& name,
                    const std::string& callbackName,
                    int mergeId = -1)
        : _history(history)
        , _log(log)
        , _name(name)
        , _callbackName(callbackName)
        , _mergeId(mergeId)
    {
        setText(QString::fromStdString(name));
    }

    virtual void redo() OVERRIDE FINAL
    {
        _log->push_back("+" + _name);
        _history->recordApplied(new LogCommand(_log, _callbackName, _mergeId), UndoScope());
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
        const CallbackCommand* o = dynamic_cast<const CallbackCommand*>(other);
        if (!o) {
            return false;
        }
        _name += o->_name;

        return true;
    }

    UndoHistory* _history;
    Log* _log;
    std::string _name;
    std::string _callbackName;
    int _mergeId;
};
} // anon namespace

TEST_F(UndoHistoryTest, ACallbacksRecordsJoinThePushedEdit)
{
    Log hook;
    history.setReplayHook([&hook](bool on) { hook.push_back(on ? "on" : "off"); });

    CallbackCommand* cmd = new CallbackCommand(&history, &log, "edit", "cb");
    history.push(cmd, ownedBy(a.get()));
    EXPECT_EQ(L({ "+edit" }), take());
    EXPECT_EQ(1, history.count());
    EXPECT_EQ(QString::fromUtf8("edit"), history.undoText());
    EXPECT_TRUE(history.canUndo(a.get()));
    EXPECT_EQ(cmd, history.getLastCommand());
    EXPECT_TRUE(hook.empty());

    history.undo();
    EXPECT_EQ(L({ "-cb", "-edit" }), take());
    EXPECT_EQ(L({ "on", "off" }), hook);

    // Replaying records nothing new: the callback's edit is the recorded one.
    history.redo();
    EXPECT_EQ(L({ "+edit", "+cb" }), take());
    EXPECT_EQ(1, history.count());
    history.undo();
    EXPECT_EQ(L({ "-cb", "-edit" }), take());
}

TEST_F(UndoHistoryTest, MergedPushesMergeTheirRecords)
{
    history.push(new CallbackCommand(&history, &log, "e", "c", 7));
    history.push(new CallbackCommand(&history, &log, "e", "c", 7));
    EXPECT_EQ(1, history.count());
    take();
    history.undo();
    EXPECT_EQ(L({ "-cc", "-ee" }), take());
}

TEST_F(UndoHistoryTest, APushedEditWithRecordsInsideAStepJoinsTheStep)
{
    history.openStep(QString::fromUtf8("script"));
    history.push(new CallbackCommand(&history, &log, "edit", "cb"));
    history.recordApplied(new LogCommand(&log, "r"), UndoScope());
    history.closeStep();
    EXPECT_EQ(1, history.count());
    take();
    history.undo();
    EXPECT_EQ(L({ "-r", "-cb", "-edit" }), take());
}

namespace {
// What a node panel does with a knob GUI's pushes: record them in the window's history, owned by
// the panel's holder when one is given so that the panel's own undo reaches them.
class HistoryKnobGuiContainer
    : public KnobGuiContainerI {
public:
    explicit HistoryKnobGuiContainer(UndoHistory* history,
                                     KnobHolder* owner = 0)
        : KnobGuiContainerI()
        , _history(history)
        , _owner(owner)
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
        _history->push(cmd, _owner ? UndoScope(_owner) : UndoScope());
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
    KnobHolder* _owner;
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

namespace {
const char* const kUserKnob = "userValue";

class NodeKnobUndo
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

    NodePtr createBlur()
    {
        CreateNodeArgs args(PLUGINID_NATRON_BLUR, _app->getProject());

        args.setProperty<int>(kCreateNodeArgsPropPluginVersion, PLUGIN_MAJOR_NATRON_BLUR, 0);
        args.setProperty<int>(kCreateNodeArgsPropPluginVersion, -1, 1);

        return _app->createNode(args);
    }

    static KnobDoublePtr addUserDouble(const NodePtr& node,
                                       const std::string& name,
                                       double value)
    {
        EffectInstancePtr effect = node->getEffectInstance();
        KnobDoublePtr added = effect->createDoubleKnob(name, name, 1);
        if (added) {
            effect->getOrCreateUserPageKnob()->addKnob(added);
            added->setValue(value);
        }

        return added;
    }

    static KnobDoublePtr userKnob(const NodePtr& node)
    {
        return std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kUserKnob));
    }

    static Serialization::NodeSerialization snapshot(const NodePtr& node)
    {
        Serialization::NodeSerialization serialization;
        node->toSerialization(&serialization, false);

        return serialization;
    }

    // What a preset file holds, read back the way Import presets reads it.
    static Serialization::NodeSerialization presetFileOf(const NodePtr& node)
    {
        std::list<Serialization::NodeSerializationPtr> nodes;
        nodes.push_back(std::make_shared<Serialization::NodeSerialization>(snapshot(node)));
        std::list<Serialization::NodeSerializationPtr> decoded = Serialization::decodePresetYaml(Serialization::encodePresetYaml(nodes));
        EXPECT_EQ((std::size_t)1, decoded.size());

        return decoded.empty() ? Serialization::NodeSerialization() : *decoded.front();
    }

    AppInstancePtr _app;
};
} // anon namespace

// Undoing a reset whose knob was deleted since must neither crash on the missing holder nor
// leave a dead entry behind.
TEST_F(NodeKnobUndo, AResetOfADeletedKnobIsDroppedOnUndo)
{
    NodePtr blur = createBlur();
    ASSERT_TRUE(bool(blur));
    KnobDoublePtr knob = addUserDouble(blur, kUserKnob, 5.);
    ASSERT_TRUE(bool(knob));

    UndoHistory history;
    history.push(new RestoreDefaultsCommand(false, std::list<KnobIPtr>(1, knob), -1));
    EXPECT_DOUBLE_EQ(knob->getDefaultValue(0), knob->getValue(0));
    ASSERT_EQ(1, history.count());

    blur->deleteUserKnobs(std::list<KnobIWPtr>(1, knob));
    EXPECT_FALSE(bool(userKnob(blur)));
    history.undo();
    EXPECT_EQ(0, history.count());
}

TEST_F(NodeKnobUndo, AResetOfADeletedKnobIsDroppedOnRedo)
{
    NodePtr blur = createBlur();
    ASSERT_TRUE(bool(blur));
    KnobDoublePtr knob = addUserDouble(blur, kUserKnob, 5.);
    ASSERT_TRUE(bool(knob));

    UndoHistory history;
    history.push(new RestoreDefaultsCommand(false, std::list<KnobIPtr>(1, knob), -1));
    history.undo();
    EXPECT_DOUBLE_EQ(5., knob->getValue(0));

    blur->deleteUserKnobs(std::list<KnobIWPtr>(1, knob));
    history.redo();
    EXPECT_EQ(0, history.count());
}

// Undoing and redoing a preset deletes and recreates the user knobs it adds, the way
// LoadNodePresetsCommand does; an earlier edit of such a knob must land on the recreated one.
TEST_F(NodeKnobUndo, AnEditOfAUserKnobSurvivesThePresetRecreatingIt)
{
    NodePtr source = createBlur();
    NodePtr target = createBlur();
    ASSERT_TRUE(source && target);
    ASSERT_TRUE(bool(addUserDouble(source, kUserKnob, 6.)));
    const Serialization::NodeSerialization preset = presetFileOf(source);
    const Serialization::NodeSerialization before = snapshot(target);

    std::list<KnobIPtr> created;
    target->applyPresetSerialization(preset, Node::ePresetApplyModePreset, &created);
    KnobDoublePtr first = userKnob(target);
    ASSERT_TRUE(bool(first));
    EXPECT_DOUBLE_EQ(6., first->getValue(0));

    UndoHistory history;
    HistoryKnobGuiContainer container(&history);
    KnobGuiPtr knobGui = std::make_shared<KnobGuiDouble>(first, &container);
    knobGui->pushUndoCommand(new KnobUndoCommand<double>(knobGui, 6., 8., 0, false));
    EXPECT_DOUBLE_EQ(8., first->getValue(0));
    ASSERT_EQ(1, history.count());

    history.undo();
    EXPECT_DOUBLE_EQ(6., first->getValue(0));

    // Undo, then redo, the preset.
    target->deleteUserKnobs(std::list<KnobIWPtr>(created.begin(), created.end()));
    created.clear();
    target->applyPresetSerialization(before, Node::ePresetApplyModeSnapshot, NULL);
    EXPECT_FALSE(bool(userKnob(target)));
    target->applyPresetSerialization(preset, Node::ePresetApplyModePreset, &created);
    KnobDoublePtr recreated = userKnob(target);
    ASSERT_TRUE(bool(recreated));
    ASSERT_NE(first.get(), recreated.get());

    ASSERT_TRUE(history.canRedo());
    history.redo();
    EXPECT_DOUBLE_EQ(8., recreated->getValue(0));
    EXPECT_EQ(1, history.count());
    history.undo();
    EXPECT_DOUBLE_EQ(6., recreated->getValue(0));
}

// Typing three values into the same knob, each committed on its own, must take three undos to
// unwind, from the global undo and from the panel's own undo alike. Folding them sent one undo
// straight back to the value before the first edit.
TEST_F(NodeKnobUndo, SeparateCommitsOnOneKnobAreSeparateGlobalUndoSteps)
{
    NodePtr blur = createBlur();
    ASSERT_TRUE(bool(blur));
    KnobDoublePtr mix = std::dynamic_pointer_cast<KnobDouble>(blur->getKnobByName(kOfxMixParamName));
    ASSERT_TRUE(bool(mix));
    ASSERT_DOUBLE_EQ(1., mix->getValue(0));

    UndoHistory history;
    HistoryKnobGuiContainer container(&history, UndoScope::holderForNode(blur));
    KnobGuiPtr knobGui = std::make_shared<KnobGuiDouble>(mix, &container);

    knobGui->pushUndoCommand(new KnobUndoCommand<double>(knobGui, 1., 0.5, 0, false));
    knobGui->pushUndoCommand(new KnobUndoCommand<double>(knobGui, 0.5, 0.25, 0, false));
    knobGui->pushUndoCommand(new KnobUndoCommand<double>(knobGui, 0.25, 0.1, 0, false));
    EXPECT_DOUBLE_EQ(0.1, mix->getValue(0));
    ASSERT_EQ(3, history.count());

    history.undo();
    EXPECT_DOUBLE_EQ(0.25, mix->getValue(0));
    history.undo();
    EXPECT_DOUBLE_EQ(0.5, mix->getValue(0));
    history.undo();
    EXPECT_DOUBLE_EQ(1., mix->getValue(0));
    EXPECT_FALSE(history.canUndo());
}

TEST_F(NodeKnobUndo, SeparateCommitsOnOneKnobAreSeparatePanelUndoSteps)
{
    NodePtr blur = createBlur();
    ASSERT_TRUE(bool(blur));
    KnobDoublePtr mix = std::dynamic_pointer_cast<KnobDouble>(blur->getKnobByName(kOfxMixParamName));
    ASSERT_TRUE(bool(mix));

    KnobHolder* owner = UndoScope::holderForNode(blur);
    ASSERT_NE(owner, (KnobHolder*)0);
    UndoHistory history;
    HistoryKnobGuiContainer container(&history, owner);
    KnobGuiPtr knobGui = std::make_shared<KnobGuiDouble>(mix, &container);

    knobGui->pushUndoCommand(new KnobUndoCommand<double>(knobGui, 1., 0.5, 0, false));
    knobGui->pushUndoCommand(new KnobUndoCommand<double>(knobGui, 0.5, 0.25, 0, false));
    knobGui->pushUndoCommand(new KnobUndoCommand<double>(knobGui, 0.25, 0.1, 0, false));
    ASSERT_EQ(3, history.count());

    ASSERT_TRUE(history.canUndo(owner));
    history.undo(owner);
    EXPECT_DOUBLE_EQ(0.25, mix->getValue(0));
    history.undo(owner);
    EXPECT_DOUBLE_EQ(0.5, mix->getValue(0));
    history.undo(owner);
    EXPECT_DOUBLE_EQ(1., mix->getValue(0));
    EXPECT_FALSE(history.canUndo(owner));

    history.redo(owner);
    EXPECT_DOUBLE_EQ(0.5, mix->getValue(0));
}

// A drag pushes once per mouse move; all of one drag is one undo step, and the next drag is
// another even though it edits the same knob.
TEST_F(NodeKnobUndo, PushesOfOneGestureAreOneUndoStep)
{
    NodePtr blur = createBlur();
    ASSERT_TRUE(bool(blur));
    KnobDoublePtr mix = std::dynamic_pointer_cast<KnobDouble>(blur->getKnobByName(kOfxMixParamName));
    ASSERT_TRUE(bool(mix));

    KnobHolder* owner = UndoScope::holderForNode(blur);
    UndoHistory history;
    HistoryKnobGuiContainer container(&history, owner);
    KnobGuiPtr knobGui = std::make_shared<KnobGuiDouble>(mix, &container);

    UndoGesture drag;
    drag.begin();
    double previous = 1.;
    for (int step = 1; step <= 6; ++step) {
        const double next = 1. - 0.1 * step;
        KnobUndoCommand<double>* cmd = new KnobUndoCommand<double>(knobGui, previous, next, 0, false);
        cmd->setGestureKey(drag.key());
        knobGui->pushUndoCommand(cmd);
        previous = next;
    }
    drag.end();
    EXPECT_EQ(1, history.count());
    EXPECT_DOUBLE_EQ(0.4, mix->getValue(0));

    drag.begin();
    for (int step = 1; step <= 3; ++step) {
        const double next = 0.4 - 0.1 * step;
        KnobUndoCommand<double>* cmd = new KnobUndoCommand<double>(knobGui, previous, next, 0, false);
        cmd->setGestureKey(drag.key());
        knobGui->pushUndoCommand(cmd);
        previous = next;
    }
    drag.end();
    EXPECT_EQ(2, history.count());

    history.undo(owner);
    EXPECT_DOUBLE_EQ(0.4, mix->getValue(0));
    history.undo(owner);
    EXPECT_DOUBLE_EQ(1., mix->getValue(0));
}
