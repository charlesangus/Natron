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
#include <cassert>
#include <deque>

#include <QPointer>
#include <QUndoCommand>

#include "Engine/EffectInstance.h"
#include "Engine/Knob.h"
#include "Engine/Node.h"
#include "Engine/TrackMarker.h"
#include "Engine/TrackerContext.h"

NATRON_NAMESPACE_ENTER

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

    bool isObsolete() const
    {
        return cmd->isObsolete() || (hasOwner && owner.isNull());
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
    bool executing;
    bool clearPending;

    UndoHistoryPrivate(UndoHistory* publicInterface)
        : _publicInterface(publicInterface)
        , entries()
        , undoSeq(0)
        , limit(0)
        , executing(false)
        , clearPending(false)
    {
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
        if (isUndo) {
            cmd->undo();
        } else {
            cmd->redo();
        }
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

    _imp->executing = true;
    cmd->redo();
    _imp->executing = false;
    if (_imp->clearPending) {
        delete cmd;
        _imp->clearNow();
        Q_EMIT changed();

        return;
    }
    if (cmd->isObsolete()) {
        delete cmd;
        Q_EMIT changed();

        return;
    }

    _imp->dropUndone();
    _imp->dropObsolete();

    KnobHolder* owner = scope.getOwner();
    if (!_imp->entries.empty() && (cmd->id() != -1)) {
        UndoEntry& last = _imp->entries.back();
        bool sameOwner = owner ? last.isOwnedBy(owner) : !last.hasOwner;
        if (sameOwner && (last.cmd->id() == cmd->id()) && last.cmd->mergeWith(cmd)) {
            delete cmd;
            const std::vector<KnobHolder*>& touched = scope.getTouched();
            for (std::size_t i = 0; i < touched.size(); ++i) {
                if (!last.touches(touched[i])) {
                    last.touched.push_back(QPointer<KnobHolder>(touched[i]));
                }
            }
            if (last.cmd->isObsolete()) {
                _imp->eraseAt((int)_imp->entries.size() - 1);
            }
            Q_EMIT changed();

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
    _imp->entries.push_back(e);
    _imp->applyLimit();
    Q_EMIT changed();
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

    return (last.done && !last.isObsolete()) ? last.cmd : 0;
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

NATRON_NAMESPACE_EXIT
