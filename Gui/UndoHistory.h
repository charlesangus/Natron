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

#ifndef NATRON_GUI_UNDOHISTORY_H
#define NATRON_GUI_UNDOHISTORY_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <memory>
#include <vector>

#include <QObject>
#include <QString>

#include "Engine/EngineFwd.h"

#include "Gui/GuiFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Who an undo entry belongs to and which nodes' state it changes.
 *
 * The owner is the knob holder whose settings panel offers the entry through its own
 * undo/redo buttons, or null for entries only the global undo reaches. The touched set lists
 * the holders whose state the entry changes; the owner is always part of it. Nodes are keyed by
 * their effect instance, see holderForNode().
 **/
class UndoScope {
public:
    UndoScope();

    explicit UndoScope(KnobHolder* owner);

    KnobHolder* getOwner() const
    {
        return _owner;
    }

    const std::vector<KnobHolder*>& getTouched() const
    {
        return _touched;
    }

    void touch(KnobHolder* holder);

    void touchNode(const NodePtr& node);

    void touchKnob(const KnobIPtr& knob);

    /**
     * @brief Makes the only touched holder the owner, for edits that a single panel may offer.
     * Does nothing when several holders, or none, are touched.
     **/
    void ownByOnlyTouched();

    /**
     * @brief The holder that keys a node in scopes: its effect instance.
     **/
    static KnobHolder* holderForNode(const NodePtr& node);

    /**
     * @brief The holder that keys the knob's owner in scopes: the node's effect instance for
     * node knobs (including knobs held by a node's tracks), otherwise the knob's own holder.
     **/
    static KnobHolder* holderForKnob(const KnobIPtr& knob);

private:
    KnobHolder* _owner;
    std::vector<KnobHolder*> _touched;
};

struct UndoHistoryPrivate;

/**
 * @brief One chronological undo history for a project window.
 *
 * Commands are kept in push order. Global undo undoes the newest done entry and global redo
 * redoes the most recently undone one. A holder's filtered undo undoes its own newest entry,
 * but only when no later done entry touches that holder; filtered redo mirrors it on the undone
 * entries. Pushing a command drops every undone entry.
 *
 * A command is merged (QUndoCommand::id()/mergeWith()) only into the newest entry, and only when
 * both have the same owner. Obsolete commands, and commands whose owner or any touched holder
 * has been destroyed, are skipped and dropped instead of being executed.
 **/
class UndoHistory
    : public QObject {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

public:
    explicit UndoHistory(QObject* parent = 0);

    virtual ~UndoHistory();

    /**
     * @brief Runs cmd->redo() and records it, taking ownership of it. A push made while another
     * command is being executed by this history is executed but not recorded: the outer command
     * already covers it.
     **/
    void push(QUndoCommand* cmd, const UndoScope& scope = UndoScope());

    bool canUndo() const;
    bool canRedo() const;
    void undo();
    void redo();
    QString undoText() const;
    QString redoText() const;

    bool canUndo(KnobHolder* owner) const;
    bool canRedo(KnobHolder* owner) const;
    void undo(KnobHolder* owner);
    void redo(KnobHolder* owner);
    QString undoText(KnobHolder* owner) const;
    QString redoText(KnobHolder* owner) const;

    /**
     * @brief The newest entry if it is done, which is the only one a new command may merge into.
     **/
    const QUndoCommand* getLastCommand() const;

    /**
     * @brief Caps the number of entries, dropping the oldest first. 0 means no limit.
     **/
    void setUndoLimit(int limit);
    int getUndoLimit() const;

    int count() const;

    /**
     * @brief Deletes every entry. Called while a command is executing, it takes effect once that
     * command returns, and the pushed command is deleted with the rest.
     **/
    void clear();

Q_SIGNALS:

    void changed();

private:
    std::unique_ptr<UndoHistoryPrivate> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_UNDOHISTORY_H
