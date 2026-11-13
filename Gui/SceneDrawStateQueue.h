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

#ifndef NATRON_GUI_SCENEDRAWSTATEQUEUE_H
#define NATRON_GUI_SCENEDRAWSTATEQUEUE_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "Engine/Nodes/Scene/SceneDrawStates.h"

NATRON_NAMESPACE_ENTER

// One edit of a viewer's draw states, as it goes to its renderer.
struct SceneDrawChange {
    SceneDrawDelta delta;
    // Loads an earlier change asked for that no row asks for any more.
    std::vector<std::string> cancelledLoads;
    std::chrono::steady_clock::time_point startedAt;
};

/**
 * @brief A viewer's draw states and the edits made to them. An edit runs on the calling thread
 * when none is waiting and few rows are stored or brought in; otherwise it runs on a worker, so that clearing or
 * setting many rows does not stall the interface. Each edit's change is delivered through post()
 * in the order the edits were made. Not a QObject: post() decides the thread deliveries run on.
 **/
class SceneDrawStateQueue {
public:
    typedef std::function<SceneDrawDelta(SceneDrawStates*)> Edit;
    typedef std::function<void(const SceneDrawChange&)> Deliver;
    // Runs a function later on the thread that edits, in the order given.
    typedef std::function<void(const std::function<void()>&)> Post;
    typedef std::vector<std::pair<std::string, SceneDrawStates::Stored>> SetStates;

    static const std::size_t kDefaultInlineRows = 2000;

    SceneDrawStateQueue(const Post& post,
                        const Deliver& deliver,
                        std::size_t inlineRows = kDefaultInlineRows);

    // Waits for the edit under way on the worker; the ones not started are dropped.
    ~SceneDrawStateQueue();

    /**
     * @brief Queues an edit. rows is how many rows the edit itself brings, such as the states a
     * replaceSetStates() puts in: above the inline limit it goes to the worker however few rows are
     * stored now.
     **/
    void edit(const Edit& edit,
              std::chrono::steady_clock::time_point startedAt = std::chrono::steady_clock::time_point(),
              std::size_t rows = 0);

    // False, without calling read, while the worker edits.
    bool tryRead(const std::function<void(const SceneDrawStates&)>& read) const;

    // Waits for the worker's edit under way, if any.
    void read(const std::function<void(const SceneDrawStates&)>& read) const;

    // Edits made and not yet delivered.
    int getOutstandingCount() const;

    // SceneDrawStates::getSetStates() as the last edit run left them. Any thread; never waits on an edit.
    std::shared_ptr<const SetStates> getSetStates() const;

    // The stage has taken every load asked for so far: none is left for a later edit to cancel.
    void forgetIssuedLoads();

private:
    SceneDrawChange run(const Edit& edit, std::chrono::steady_clock::time_point startedAt);
    void workerLoop();

    struct Job {
        Edit edit;
        std::chrono::steady_clock::time_point startedAt;
    };

    Post _post;
    Deliver _deliver;
    std::size_t _inlineRows;
    mutable std::mutex _statesMutex;
    SceneDrawStates _states;
    typedef std::map<std::string, SceneDrawLoadRequest, SceneDrawStates::PathLess> IssuedMap;
    // The loads asked for that the stage may not have taken yet, by path, a subtree's together;
    // under _statesMutex.
    IssuedMap _issued;
    std::atomic<bool> _forgetIssued { false };
    mutable std::mutex _setStatesMutex;
    std::shared_ptr<const SetStates> _setStates;
    // Only the editing thread touches it.
    int _outstanding = 0;
    std::mutex _jobsMutex;
    std::condition_variable _jobsChanged;
    std::deque<Job> _jobs;
    bool _quit = false;
    std::thread _worker;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_GUI_SCENEDRAWSTATEQUEUE_H
