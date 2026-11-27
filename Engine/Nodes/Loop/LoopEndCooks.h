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

#ifndef Engine_Nodes_Loop_LoopEndCooks_h
#define Engine_Nodes_Loop_LoopEndCooks_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <list>
#include <set>
#include <vector>

#include <QtCore/QMutex>

#include "Engine/EngineFwd.h"
#include "Global/GlobalDefines.h"

NATRON_NAMESPACE_ENTER

/** @brief The begin and end nodes of one kind of loop that pairs its ends by walking the graph. */
struct LoopEndKind {
    bool (*isBegin)(const NodePtr& node);
    bool (*isEnd)(const NodePtr& node);
};

/**
 * @brief The begins of kind found walking up from from. It counts the loops it enters: an end
 * met going up opens a nested loop, whose own begins close it, so only a begin met at depth 0
 * pairs. A nested end's first input is its body; its other inputs are outside its loop and are
 * walked at the depth outside it.
 **/
void collectLoopPairedBegins(const LoopEndKind& kind,
                             const NodePtr& from,
                             std::set<Node*>* found);

/** @brief The ends of kind found walking down from begin, counting nested loops the same way. */
void collectLoopPairedEnds(const LoopEndKind& kind,
                           const NodePtr& begin,
                           std::set<Node*>* found);

/** @brief The one end of kind begin opens a loop of, or null for none or several. */
NodePtr loopPairedEnd(const LoopEndKind& kind,
                      const NodePtr& begin) WARN_UNUSED_RETURN;

/**
 * @brief The begins of kind found walking up from an end's first input that pair with that end
 * alone, by script name, kept until the graph changes (LoopEnd::getGraphGeneration()). MT-safe.
 **/
class LoopEndPairing {
public:
    explicit LoopEndPairing(const LoopEndKind& kind);

    std::vector<NodePtr> getBegins(const NodePtr& end) const WARN_UNUSED_RETURN;

private:
    const LoopEndKind _kind;
    mutable QMutex _mutex;
    mutable U64 _generation = 0;
    mutable bool _valid = false;
    mutable std::vector<NodeWPtr> _begins;
};

/**
 * @brief The viewer cooks of a loop end and the hash each started under. An edit keeps the
 * oldest viewer render running, and a loop can cook for minutes, so a cook is aborted once the
 * hash moves or an edit of the end or its body is held back by the render it cooks in. Not
 * during a drag, which keeps its oldest render as it keeps any render. MT-safe.
 **/
class LoopEndCooks {
public:
    explicit LoopEndCooks(EffectInstance* end);

    LoopEndCooks(const LoopEndCooks&) = delete;
    LoopEndCooks& operator=(const LoopEndCooks&) = delete;

private:
    struct RunningCook {
        U64 startHash = 0;
        AbortableRenderInfoPtr abortInfo;
    };

public:
    /** @brief Registers the cook running on this thread, when it is an abortable viewer cook. */
    class Scope {
    public:
        explicit Scope(LoopEndCooks* cooks);
        ~Scope();

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

        /**
         * @brief Aborts the cook and returns true when it is stale. onHashChanged() only hears of
         * an edit once something rehashes the node, so this asks.
         **/
        bool editedSinceStart() const WARN_UNUSED_RETURN;

    private:
        LoopEndCooks* _cooks;
        RunningCook _cook;
        std::list<RunningCook>::iterator _entry;
        bool _registered = false;
    };

    /** @brief Aborts the cooks started under another hash. */
    void onHashChanged(U64 hash);

    /** @brief For an edit held back by the renders reading the end: aborts every cook. */
    void abortStaleAfterEdit();

private:
    bool abortIfEditedSince(const RunningCook& cook) const WARN_UNUSED_RETURN;
    void abortRunning(const U64* keepHash);

    EffectInstance* const _end;
    QMutex _mutex;
    std::list<RunningCook> _running;
    // See noteEditAtLatestRenderSequence().
    std::atomic<U64> _editedAtRenderSequence { 0 };
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Loop_LoopEndCooks_h
