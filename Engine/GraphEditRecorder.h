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

#ifndef Engine_GraphEditRecorder_h
#define Engine_GraphEditRecorder_h

#include "Global/Macros.h"

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include "Engine/EngineFwd.h"

#include "Serialization/Ops/GraphOp.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Turns the edits made to a project into graph operations for its subscribers.
 *
 * Edits made during one turn of the event loop are delivered together at the end of the turn,
 * or when flush() is called. Repeated edits of one knob, one node's layout, user knobs, roto
 * shapes or tracks, or the project's formats and layers, make one operation carrying the state
 * at delivery, in the place of the first edit. Creating or reactivating a node makes a
 * CreateNode carrying its whole state, followed by a SetInput for each of its connections.
 *
 * Nothing is recorded while the project loads or closes, or under Suppress. The hooks are
 * called by the edited classes only when isActive() is true.
 **/
class GraphEditRecorder {
public:
    typedef std::function<void(const std::vector<Serialization::GraphOp>&)> Subscriber;

    explicit GraphEditRecorder(Project* project);
    ~GraphEditRecorder();

    GraphEditRecorder(const GraphEditRecorder&) = delete;
    GraphEditRecorder& operator=(const GraphEditRecorder&) = delete;

    /**
     * @brief Returns the ID unsubscribe() takes. subscriber is called on the main thread.
     **/
    int subscribe(const Subscriber& subscriber);
    void unsubscribe(int subscription);

    /**
     * @brief Delivers the operations recorded so far now. Main thread only.
     **/
    void flush();

    /**
     * @brief Whether any recorder has a subscriber.
     **/
    static bool isActive()
    {
        return _subscriptions.load(std::memory_order_relaxed) > 0;
    }

    /**
     * @brief While one exists on a thread, the graph edits made on that thread are not recorded:
     * they replay operations that are already known.
     **/
    class Suppress {
    public:
        Suppress();
        ~Suppress();

        Suppress(const Suppress&) = delete;
        Suppress& operator=(const Suppress&) = delete;
    };

    static bool isSuppressed();

    /**
     * @brief The nodes added or activated while the outermost scope of a thread exists are
     * recorded when it ends, as CreateNode operations for those not inside another one, in
     * place of the edits made to them meanwhile.
     **/
    class CreationScope {
    public:
        explicit CreationScope(const AppInstance* app);
        explicit CreationScope(Node* activatedNode);
        ~CreationScope();

        CreationScope(const CreationScope&) = delete;
        CreationScope& operator=(const CreationScope&) = delete;

    private:
        void begin(const AppInstance* app);

        bool _owner = false;
    };

    static void onNodeAdded(const NodePtr& node);
    static void onNodeDeactivating(Node* node);
    static void onNodeRenamed(Node* node);
    static void onNodeLayoutChanged(Node* node);
    static void onInputChanged(Node* node, int inputNb);
    static void onKnobContentChanged(KnobI* knob);
    static void onKnobListChanged(KnobHolder* holder);
    static void onRotoChanged(const NodePtr& node);
    static void onTrackerChanged(const NodePtr& node);
    static void onProjectDataChanged(Project* project);

private:
    struct Implementation;

    static GraphEditRecorder* recorderFor(const AppInstance* app);

    static std::atomic<int> _subscriptions;

    std::unique_ptr<Implementation> _imp;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_GraphEditRecorder_h
