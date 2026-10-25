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

#include "UndoHistory.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <deque>

#include <QPointer>
#include <QUndoCommand>

#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/Node.h"
#include "Engine/TrackMarker.h"
#include "Engine/TrackerContext.h"

#include "Gui/UndoGesture.h"

NATRON_NAMESPACE_ENTER

quint64
UndoGesture::newKey()
{
    // 0 means "no gesture", so the first key handed out is 1.
    static std::atomic<quint64> counter(0);

    return ++counter;
}

UndoScope::UndoScope()
    : _owner(0)
    , _touched()
{
}

UndoScope::UndoScope(KnobHolder* owner)
    : _owner(owner)
    , _touched()
{
    touch(owner);
}

void
UndoScope::touch(KnobHolder* holder)
{
    if (holder && (std::find(_touched.begin(), _touched.end(), holder) == _touched.end())) {
        _touched.push_back(holder);
    }
}

void
UndoScope::touchNode(const NodePtr& node)
{
    touch(holderForNode(node));
}

void
UndoScope::touchKnob(const KnobIPtr& knob)
{
    touch(holderForKnob(knob));
}

void
UndoScope::ownByOnlyTouched()
{
    if (_touched.size() == 1) {
        _owner = _touched.front();
    }
}

KnobHolder*
UndoScope::holderForNode(const NodePtr& node)
{
    if (!node) {
        return 0;
    }

    return node->getEffectInstance().get();
}

KnobHolder*
UndoScope::holderForKnob(const KnobIPtr& knob)
{
    if (!knob) {
        return 0;
    }
    KnobHolder* holder = knob->getHolder();
    TrackMarker* marker = dynamic_cast<TrackMarker*>(holder);
    if (marker) {
        TrackerContextPtr context = marker->getContext();

        return context ? holderForNode(context->getNode()) : 0;
    }

    return holder;
}

namespace {

typedef std::function<void(bool)> ReplayHook;

void
deleteNewestFirst(std::vector<QUndoCommand*>* cmds)
{
    for (std::vector<QUndoCommand*>::reverse_iterator it = cmds->rbegin(); it != cmds->rend(); ++it) {
        delete *it;
    }
    cmds->clear();
}

/*
 * An entry made of several commands: optionally the pushed command, then what was recorded
 * while it executed or what a step gathered. Parts are undone newest first, so each one is
 * undone from the state it left.
 */
class CompoundUndoCommand
    : public QUndoCommand {
public:
    CompoundUndoCommand(const ReplayHook* hook,
                        QUndoCommand* outer,
                        const std::vector<QUndoCommand*>& children,
                        const QString& text)
        : QUndoCommand()
        , _hook(hook)
        , _outer(outer)
        , _children(children)
    {
        setText(text);
        updateObsolete();
    }

    virtual ~CompoundUndoCommand()
    {
        deleteNewestFirst(&_children);
        delete _outer;
    }

    QUndoCommand* getOuter() const
    {
        return _outer;
    }

    // Leaves the commands to the caller.
    void release(QUndoCommand** outer,
                 std::vector<QUndoCommand*>* children)
    {
        *outer = _outer;
        _outer = 0;
        children->swap(_children);
        _children.clear();
    }

    // Folds cmds in after the outer commands merged: each one merges into a part of the same kind
    // when that part accepts it, or is appended.
    void absorb(const std::vector<QUndoCommand*>& cmds)
    {
        for (std::size_t i = 0; i < cmds.size(); ++i) {
            QUndoCommand* cmd = cmds[i];
            bool merged = false;
            if (cmd->id() != -1) {
                for (std::vector<QUndoCommand*>::reverse_iterator it = _children.rbegin(); it != _children.rend(); ++it) {
                    if (((*it)->id() == cmd->id()) && (*it)->mergeWith(cmd)) {
                        merged = true;
                        break;
                    }
                }
            }
            if (merged) {
                delete cmd;
            } else {
                _children.push_back(cmd);
            }
        }
        updateObsolete();
    }

    void updateObsolete()
    {
        bool obsolete = _outer && _outer->isObsolete();
        for (std::size_t i = 0; !obsolete && i < _children.size(); ++i) {
            obsolete = _children[i]->isObsolete();
        }
        if (obsolete) {
            setObsolete(true);
        }
    }

    virtual void undo() OVERRIDE FINAL
    {
        replay(true);
        for (std::vector<QUndoCommand*>::reverse_iterator it = _children.rbegin(); it != _children.rend(); ++it) {
            (*it)->undo();
        }
        if (_outer) {
            _outer->undo();
        }
        replay(false);
        updateObsolete();
    }

    virtual void redo() OVERRIDE FINAL
    {
        replay(true);
        if (_outer) {
            _outer->redo();
        }
        for (std::size_t i = 0; i < _children.size(); ++i) {
            _children[i]->redo();
        }
        replay(false);
        updateObsolete();
    }

private:
    void replay(bool on) const
    {
        if (_hook && *_hook) {
            (*_hook)(on);
        }
    }

    const ReplayHook* _hook;
    QUndoCommand* _outer;
    std::vector<QUndoCommand*> _children;
};

// What a step, or a pushed command while it executes, has gathered so far.
struct CaptureFrame {
    bool isStep;
    QString text;

    // Steps opened inside this frame, which add to it.
    int nestedSteps;
    std::vector<QUndoCommand*> children;
    std::vector<UndoScope> scopes;

    CaptureFrame()
        : isStep(false)
        , text()
        , nestedSteps(0)
        , children()
        , scopes()
    {
    }
};

struct UndoEntry {
    QUndoCommand* cmd;
    bool hasOwner;
    QPointer<KnobHolder> owner;
    std::vector<QPointer<KnobHolder>> touched;
    bool done;

    // Order in which undone entries were undone; global redo replays the highest.
    quint64 undoSeq;

    bool touches(const KnobHolder* holder) const
    {
        for (std::size_t i = 0; i < touched.size(); ++i) {
            if (!touched[i].isNull() && (touched[i].data() == holder)) {
                return true;
            }
        }

        return false;
    }

    bool isOwnedBy(const KnobHolder* holder) const
    {
        return hasOwner && !owner.isNull() && (owner.data() == holder);
    }

    // A command cannot be replayed once any holder whose state it changes is gone, owned or not.
    bool isObsolete() const
    {
        if (cmd->isObsolete() || (hasOwner && owner.isNull())) {
            return true;
        }
        for (std::size_t i = 0; i < touched.size(); ++i) {
            if (touched[i].isNull()) {
                return true;
            }
        }

        return false;
    }
};

typedef std::deque<UndoEntry> UndoEntries;

const int kNoEntry = -1;
} // anon namespace

struct UndoHistoryPrivate {
    UndoHistory* _publicInterface;
    UndoEntries entries;
    quint64 undoSeq;
    int limit;
    // Set while a pushed command or an entry executes.
    bool executing;

    // Set while an entry is undone or redone.
    bool replaying;
    bool clearPending;
    std::vector<CaptureFrame> frames;

    // Steps opened while replaying, which gather nothing.
    int ignoredSteps;
    quint64 captureSerial;
    ReplayHook replayHook;

    UndoHistoryPrivate(UndoHistory* publicInterface)
        : _publicInterface(publicInterface)
        , entries()
        , undoSeq(0)
        , limit(0)
        , executing(false)
        , replaying(false)
        , clearPending(false)
        , frames()
        , ignoredSteps(0)
        , captureSerial(0)
        , replayHook()
    {
    }

    void openFrame(bool isStep,
                   const QString& text)
    {
        if (frames.empty()) {
            ++captureSerial;
        }
        CaptureFrame frame;
        frame.isStep = isStep;
        frame.text = text;
        frames.push_back(frame);
    }

    CaptureFrame closeFrame()
    {
        CaptureFrame frame = frames.back();
        frames.pop_back();

        return frame;
    }

    static UndoScope unionOf(const UndoScope& scope,
                             const std::vector<UndoScope>& scopes)
    {
        UndoScope ret = scope;
        for (std::size_t i = 0; i < scopes.size(); ++i) {
            const std::vector<KnobHolder*>& touched = scopes[i].getTouched();
            for (std::size_t j = 0; j < touched.size(); ++j) {
                ret.touch(touched[j]);
            }
        }

        return ret;
    }

    // Records cmd as a new done entry, merging it into the newest entry when allowed and possible.
    void addEntry(QUndoCommand* cmd,
                  const UndoScope& scope,
                  bool mayMerge)
    {
        dropUndone();
        dropObsolete();

        KnobHolder* owner = scope.getOwner();
        CompoundUndoCommand* compound = dynamic_cast<CompoundUndoCommand*>(cmd);
        QUndoCommand* outer = compound ? compound->getOuter() : cmd;
        if (mayMerge && !entries.empty() && outer && (outer->id() != -1)) {
            UndoEntry& last = entries.back();
            CompoundUndoCommand* lastCompound = dynamic_cast<CompoundUndoCommand*>(last.cmd);
            QUndoCommand* lastOuter = lastCompound ? lastCompound->getOuter() : last.cmd;
            bool sameOwner = owner ? last.isOwnedBy(owner) : !last.hasOwner;
            if (sameOwner && lastOuter && (lastOuter->id() == outer->id()) && lastOuter->mergeWith(outer)) {
                if (compound) {
                    QUndoCommand* released = 0;
                    std::vector<QUndoCommand*> children;
                    compound->release(&released, &children);
                    delete compound;
                    delete released;
                    if (!lastCompound) {
                        lastCompound = new CompoundUndoCommand(&replayHook, last.cmd, std::vector<QUndoCommand*>(), last.cmd->text());
                        last.cmd = lastCompound;
                    }
                    lastCompound->absorb(children);
                } else {
                    delete cmd;
                    if (lastCompound) {
                        lastCompound->updateObsolete();
                    }
                }
                const std::vector<KnobHolder*>& touched = scope.getTouched();
                for (std::size_t i = 0; i < touched.size(); ++i) {
                    if (!last.touches(touched[i])) {
                        last.touched.push_back(QPointer<KnobHolder>(touched[i]));
                    }
                }
                if (last.cmd->isObsolete()) {
                    eraseAt((int)entries.size() - 1);
                }
                Q_EMIT _publicInterface->changed();

                return;
            }
        }

        UndoEntry e;
        e.cmd = cmd;
        e.hasOwner = (owner != 0);
        e.owner = owner;
        const std::vector<KnobHolder*>& touched = scope.getTouched();
        for (std::size_t i = 0; i < touched.size(); ++i) {
            e.touched.push_back(QPointer<KnobHolder>(touched[i]));
        }
        e.done = true;
        e.undoSeq = 0;
        entries.push_back(e);
        applyLimit();
        Q_EMIT _publicInterface->changed();
    }

    // Newest done entry; with an owner, the newest done entry touching it.
    int findUndoable(const KnobHolder* owner) const
    {
        for (int i = (int)entries.size() - 1; i >= 0; --i) {
            const UndoEntry& e = entries[i];
            if (!e.done || e.isObsolete()) {
                continue;
            }
            if (!owner || e.touches(owner)) {
                return i;
            }
        }

        return kNoEntry;
    }

    // Most recently undone entry; with an owner, the most recently undone entry touching it.
    int findRedoable(const KnobHolder* owner) const
    {
        int found = kNoEntry;
        for (int i = 0; i < (int)entries.size(); ++i) {
            const UndoEntry& e = entries[i];
            if (e.done || e.isObsolete()) {
                continue;
            }
            if (owner && !e.touches(owner)) {
                continue;
            }
            if ((found == kNoEntry) || (e.undoSeq > entries[found].undoSeq)) {
                found = i;
            }
        }

        return found;
    }

    int findForOwner(const KnobHolder* owner,
                     bool undoable) const
    {
        if (!owner) {
            return kNoEntry;
        }
        int i = undoable ? findUndoable(owner) : findRedoable(owner);
        if ((i == kNoEntry) || !entries[i].isOwnedBy(owner)) {
            return kNoEntry;
        }

        return i;
    }

    void deleteEntries(UndoEntries& removed)
    {
        // Newest first, so that a command never outlives one pushed before it.
        for (UndoEntries::reverse_iterator it = removed.rbegin(); it != removed.rend(); ++it) {
            delete it->cmd;
        }
        removed.clear();
    }

    void eraseAt(int i)
    {
        UndoEntries removed;
        removed.push_back(entries[i]);
        entries.erase(entries.begin() + i);
        deleteEntries(removed);
    }

    void dropObsolete()
    {
        UndoEntries removed;
        UndoEntries kept;
        for (UndoEntries::iterator it = entries.begin(); it != entries.end(); ++it) {
            if (it->isObsolete()) {
                removed.push_back(*it);
            } else {
                kept.push_back(*it);
            }
        }
        entries.swap(kept);
        deleteEntries(removed);
    }

    void dropUndone()
    {
        UndoEntries removed;
        UndoEntries kept;
        for (UndoEntries::iterator it = entries.begin(); it != entries.end(); ++it) {
            if (it->done) {
                kept.push_back(*it);
            } else {
                removed.push_back(*it);
            }
        }
        entries.swap(kept);
        deleteEntries(removed);
    }

    void applyLimit()
    {
        if (limit <= 0) {
            return;
        }
        UndoEntries removed;
        while ((int)entries.size() > limit) {
            removed.push_back(entries.front());
            entries.pop_front();
        }
        deleteEntries(removed);
    }

    void clearNow()
    {
        clearPending = false;
        // Open frames stay open, emptied: their steps are still to be closed.
        for (std::vector<CaptureFrame>::reverse_iterator it = frames.rbegin(); it != frames.rend(); ++it) {
            deleteNewestFirst(&it->children);
            it->scopes.clear();
        }
        UndoEntries removed;
        removed.swap(entries);
        deleteEntries(removed);
    }

    // Runs the undo or redo of entries[i], then drops it if it became obsolete.
    void execute(int i,
                 bool isUndo)
    {
        QUndoCommand* cmd = entries[i].cmd;
        executing = true;
        replaying = true;
        if (isUndo) {
            cmd->undo();
        } else {
            cmd->redo();
        }
        replaying = false;
        executing = false;
        if (clearPending) {
            clearNow();

            return;
        }

        // Nested pushes are not recorded and clear() is deferred, so entries[i] is still cmd.
        UndoEntry& e = entries[i];
        assert(e.cmd == cmd);
        e.done = !isUndo;
        if (isUndo) {
            e.undoSeq = ++undoSeq;
        }
        if (cmd->isObsolete()) {
            eraseAt(i);
        }
    }

    void undoOrRedo(const KnobHolder* owner,
                    bool isUndo)
    {
        if (executing) {
            return;
        }
        dropObsolete();
        int i = owner ? findForOwner(owner, isUndo) : (isUndo ? findUndoable(0) : findRedoable(0));
        if (i != kNoEntry) {
            execute(i, isUndo);
        }
        Q_EMIT _publicInterface->changed();
    }

    static QString textOf(const UndoEntries& entries,
                          int i)
    {
        return (i == kNoEntry) ? QString() : entries[i].cmd->actionText();
    }
};

UndoHistory::UndoHistory(QObject* parent)
    : QObject(parent)
    , _imp(new UndoHistoryPrivate(this))
{
}

UndoHistory::~UndoHistory()
{
    _imp->clearNow();
}

void
UndoHistory::push(QUndoCommand* cmd,
                  const UndoScope& scope)
{
    if (!cmd) {
        return;
    }
    if (_imp->executing) {
        cmd->redo();
        delete cmd;

        return;
    }

    _imp->openFrame(false, QString());
    _imp->executing = true;
    cmd->redo();
    _imp->executing = false;
    CaptureFrame frame = _imp->closeFrame();
    if (_imp->clearPending) {
        deleteNewestFirst(&frame.children);
        delete cmd;
        _imp->clearNow();
        Q_EMIT changed();

        return;
    }

    QUndoCommand* entryCmd = cmd;
    if (!frame.children.empty()) {
        entryCmd = new CompoundUndoCommand(&_imp->replayHook, cmd, frame.children, cmd->text());
    }
    if (entryCmd->isObsolete()) {
        delete entryCmd;
        Q_EMIT changed();

        return;
    }

    UndoScope entryScope = UndoHistoryPrivate::unionOf(scope, frame.scopes);
    if (!_imp->frames.empty()) {
        // Pushed inside a step, which makes the entry.
        _imp->frames.back().children.push_back(entryCmd);
        _imp->frames.back().scopes.push_back(entryScope);

        return;
    }
    _imp->addEntry(entryCmd, entryScope, true);
}

bool
UndoHistory::canUndo() const
{
    return _imp->findUndoable(0) != kNoEntry;
}

bool
UndoHistory::canRedo() const
{
    return _imp->findRedoable(0) != kNoEntry;
}

void
UndoHistory::undo()
{
    _imp->undoOrRedo(0, true);
}

void
UndoHistory::redo()
{
    _imp->undoOrRedo(0, false);
}

QString
UndoHistory::undoText() const
{
    return UndoHistoryPrivate::textOf(_imp->entries, _imp->findUndoable(0));
}

QString
UndoHistory::redoText() const
{
    return UndoHistoryPrivate::textOf(_imp->entries, _imp->findRedoable(0));
}

bool
UndoHistory::canUndo(KnobHolder* owner) const
{
    return _imp->findForOwner(owner, true) != kNoEntry;
}

bool
UndoHistory::canRedo(KnobHolder* owner) const
{
    return _imp->findForOwner(owner, false) != kNoEntry;
}

void
UndoHistory::undo(KnobHolder* owner)
{
    if (owner) {
        _imp->undoOrRedo(owner, true);
    }
}

void
UndoHistory::redo(KnobHolder* owner)
{
    if (owner) {
        _imp->undoOrRedo(owner, false);
    }
}

QString
UndoHistory::undoText(KnobHolder* owner) const
{
    return UndoHistoryPrivate::textOf(_imp->entries, _imp->findForOwner(owner, true));
}

QString
UndoHistory::redoText(KnobHolder* owner) const
{
    return UndoHistoryPrivate::textOf(_imp->entries, _imp->findForOwner(owner, false));
}

const QUndoCommand*
UndoHistory::getLastCommand() const
{
    if (_imp->entries.empty()) {
        return 0;
    }
    const UndoEntry& last = _imp->entries.back();
    if (!last.done || last.isObsolete()) {
        return 0;
    }
    // Callers compare it with the command they would push next, which is never a compound.
    const CompoundUndoCommand* compound = dynamic_cast<const CompoundUndoCommand*>(last.cmd);

    return (compound && compound->getOuter()) ? compound->getOuter() : last.cmd;
}

void
UndoHistory::setUndoLimit(int limit)
{
    _imp->limit = std::max(0, limit);
    if (!_imp->executing) {
        _imp->applyLimit();
        Q_EMIT changed();
    }
}

int
UndoHistory::getUndoLimit() const
{
    return _imp->limit;
}

int
UndoHistory::count() const
{
    return (int)_imp->entries.size();
}

void
UndoHistory::clear()
{
    if (_imp->executing) {
        _imp->clearPending = true;

        return;
    }
    _imp->clearNow();
    Q_EMIT changed();
}

void
UndoHistory::openStep(const QString& text)
{
    if (_imp->replaying) {
        ++_imp->ignoredSteps;

        return;
    }
    if (!_imp->frames.empty()) {
        ++_imp->frames.back().nestedSteps;

        return;
    }
    _imp->openFrame(true, text);
}

void
UndoHistory::closeStep()
{
    // Steps nest strictly, so the innermost open one is the one closing.
    if (_imp->ignoredSteps > 0) {
        --_imp->ignoredSteps;

        return;
    }
    if (_imp->frames.empty()) {
        return;
    }
    CaptureFrame& top = _imp->frames.back();
    if (top.nestedSteps > 0) {
        --top.nestedSteps;

        return;
    }
    if (!top.isStep) {
        return;
    }
    CaptureFrame frame = _imp->closeFrame();
    if (frame.children.empty()) {
        return;
    }

    QUndoCommand* entryCmd;
    UndoScope entryScope;
    if (frame.children.size() == 1) {
        // A step holding a single command is that command, owner included.
        entryCmd = frame.children.front();
        entryScope = frame.scopes.front();
    } else {
        QString text = frame.text.isEmpty() ? frame.children.front()->text() : frame.text;
        entryCmd = new CompoundUndoCommand(&_imp->replayHook, 0, frame.children, text);
        entryScope = UndoHistoryPrivate::unionOf(UndoScope(), frame.scopes);
        entryScope.ownByOnlyTouched();
    }
    if (entryCmd->isObsolete()) {
        delete entryCmd;
        Q_EMIT changed();

        return;
    }
    _imp->addEntry(entryCmd, entryScope, false);
}

bool
UndoHistory::isStepOpen() const
{
    for (std::size_t i = 0; i < _imp->frames.size(); ++i) {
        if (_imp->frames[i].isStep) {
            return true;
        }
    }

    return false;
}

bool
UndoHistory::isCapturing() const
{
    return !_imp->replaying && !_imp->frames.empty();
}

quint64
UndoHistory::getCaptureSerial() const
{
    return _imp->captureSerial;
}

void
UndoHistory::recordApplied(QUndoCommand* cmd,
                           const UndoScope& scope)
{
    if (!cmd) {
        return;
    }
    if (!isCapturing()) {
        delete cmd;

        return;
    }
    _imp->frames.back().children.push_back(cmd);
    _imp->frames.back().scopes.push_back(scope);
}

void
UndoHistory::setReplayHook(const std::function<void(bool)>& hook)
{
    _imp->replayHook = hook;
}

NATRON_NAMESPACE_EXIT
