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

#include "SceneDrawStateQueue.h"

#include <algorithm>
#include <set>
#include <utility>

NATRON_NAMESPACE_ENTER

SceneDrawStateQueue::SceneDrawStateQueue(const Post& post,
                                         const Deliver& deliver,
                                         std::size_t inlineRows)
    : _post(post)
    , _deliver(deliver)
    , _inlineRows(inlineRows)
{
}

SceneDrawStateQueue::~SceneDrawStateQueue()
{
    {
        std::lock_guard<std::mutex> lock(_jobsMutex);
        _quit = true;
        _jobs.clear();
    }
    _jobsChanged.notify_all();
    if (_worker.joinable()) {
        _worker.join();
    }
}

namespace {
bool
isBelow(const std::string& path,
        const std::string& ancestor)
{
    if (ancestor == "/") {
        return path.size() > 1;
    }

    return (path.size() > ancestor.size()) && (path[ancestor.size()] == '/') && (path.compare(0, ancestor.size(), ancestor) == 0);
}

// Empty for the root.
std::string
parentOf(const std::string& path)
{
    if (path.empty() || (path == "/")) {
        return std::string();
    }
    const std::size_t slash = path.rfind('/');

    return (slash == 0) ? std::string("/") : path.substr(0, slash);
}
} // anonymous namespace

SceneDrawChange
SceneDrawStateQueue::run(const Edit& edit,
                         std::chrono::steady_clock::time_point startedAt)
{
    SceneDrawChange change;
    change.startedAt = startedAt;
    change.delta = edit(&_states);
    if (_forgetIssued.exchange(false)) {
        _issued.clear();
    }
    if (!change.delta.isEmpty() && !_issued.empty()) {
        // A row the edit boxed, hid or cleared may have been all that asked for a load still queued.
        // Only loads at or below the edit's roots can have changed, and those of an issued load above
        // a root, whose exclusions may have: the check widens each root to its topmost issued ancestor.
        const SceneDrawStates::PathLess less;
        // A delta that names no roots may have changed anything.
        const std::vector<std::string> named = change.delta.roots.empty() ? std::vector<std::string>(1, std::string("/")) : change.delta.roots;
        std::vector<std::string> roots;
        for (const std::string& root : named) {
            std::string widest = root;
            for (std::string ancestor = parentOf(root); !ancestor.empty(); ancestor = parentOf(ancestor)) {
                if (_issued.count(ancestor) != 0) {
                    widest = ancestor;
                }
            }
            roots.push_back(widest);
        }
        std::sort(roots.begin(), roots.end(), less);
        std::set<std::string> asked;
        for (const SceneDrawLoadRequest& load : change.delta.loads) {
            asked.insert(load.path);
        }
        std::string covered;
        for (const std::string& root : roots) {
            if (!covered.empty() && ((root == covered) || isBelow(root, covered))) {
                continue;
            }
            covered = root;
            IssuedMap::iterator it = _issued.lower_bound(root);
            if ((it == _issued.end()) || ((it->first != root) && !isBelow(it->first, root))) {
                continue;
            }
            std::map<std::string, SceneDrawLoadRequest> wanted;
            for (SceneDrawLoadRequest& load : _states.wantedLoadsUnder(root)) {
                std::string path = load.path;
                wanted.emplace(std::move(path), std::move(load));
            }
            while ((it != _issued.end()) && ((it->first == root) || isBelow(it->first, root))) {
                const std::map<std::string, SceneDrawLoadRequest>::const_iterator found = wanted.find(it->first);
                if (found == wanted.end()) {
                    change.cancelledLoads.push_back(it->first);
                    it = _issued.erase(it);
                    continue;
                }
                const SceneDrawLoadRequest& want = found->second;
                const bool differs = (want.withDescendants != it->second.withDescendants) || (want.excluded != it->second.excluded);
                if (differs && (asked.count(want.path) == 0)) {
                    change.delta.loads.push_back(want);
                    asked.insert(want.path);
                }
                ++it;
            }
        }
    }
    for (const SceneDrawLoadRequest& load : change.delta.loads) {
        _issued[load.path] = load;
    }
    std::shared_ptr<const SetStates> setStates = std::make_shared<const SetStates>(_states.getSetStates());
    {
        std::lock_guard<std::mutex> lock(_setStatesMutex);
        _setStates.swap(setStates);
    }

    return change;
}

void
SceneDrawStateQueue::edit(const Edit& edit,
                          std::chrono::steady_clock::time_point startedAt,
                          std::size_t rows)
{
    if ((_outstanding == 0) && (rows <= _inlineRows)) {
        std::unique_lock<std::mutex> lock(_statesMutex);
        // Policy rows are walked like set ones: a big policy is as slow to edit.
        if ((_states.storedCount() + _states.getPolicyStates().size()) <= _inlineRows) {
            const SceneDrawChange change = run(edit, startedAt);
            lock.unlock();
            _deliver(change);

            return;
        }
    }
    ++_outstanding;
    {
        std::lock_guard<std::mutex> lock(_jobsMutex);
        _jobs.push_back(Job { edit, startedAt });
        if (!_worker.joinable()) {
            _worker = std::thread([this]() { workerLoop(); });
        }
    }
    _jobsChanged.notify_one();
}

void
SceneDrawStateQueue::workerLoop()
{
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(_jobsMutex);
            _jobsChanged.wait(lock, [this]() { return _quit || !_jobs.empty(); });
            if (_quit) {
                return;
            }
            job = std::move(_jobs.front());
            _jobs.pop_front();
        }
        SceneDrawChange change;
        {
            std::lock_guard<std::mutex> lock(_statesMutex);
            change = run(job.edit, job.startedAt);
        }
        _post([this, change]() {
            --_outstanding;
            _deliver(change);
        });
    }
}

bool
SceneDrawStateQueue::tryRead(const std::function<void(const SceneDrawStates&)>& read) const
{
    std::unique_lock<std::mutex> lock(_statesMutex, std::try_to_lock);

    if (!lock.owns_lock()) {
        return false;
    }
    read(_states);

    return true;
}

void
SceneDrawStateQueue::read(const std::function<void(const SceneDrawStates&)>& read) const
{
    std::lock_guard<std::mutex> lock(_statesMutex);

    read(_states);
}

int
SceneDrawStateQueue::getOutstandingCount() const
{
    return _outstanding;
}

std::shared_ptr<const SceneDrawStateQueue::SetStates>
SceneDrawStateQueue::getSetStates() const
{
    std::lock_guard<std::mutex> lock(_setStatesMutex);

    return _setStates ? _setStates : std::make_shared<const SetStates>();
}

void
SceneDrawStateQueue::forgetIssuedLoads()
{
    _forgetIssued = true;
}

NATRON_NAMESPACE_EXIT
