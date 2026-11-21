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

#include "SceneTeardown.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <QCoreApplication>

#include "Engine/Nodes/Scene/SceneWriteBarrier.h"

NATRON_NAMESPACE_ENTER

namespace {
void
postToMainThread(const std::function<void()>& callback)
{
    QCoreApplication* app = QCoreApplication::instance();

    if (!app) {
        callback();

        return;
    }
    QMetaObject::invokeMethod(app, callback, Qt::QueuedConnection);
}

void
postAll(std::vector<std::function<void()>>* callbacks)
{
    for (const std::function<void()>& callback : *callbacks) {
        postToMainThread(callback);
    }
    callbacks->clear();
}

struct TeardownState {
    std::atomic<bool> deferred { false };
    std::mutex mutex;
    std::condition_variable changed;
    struct Held {
        std::shared_ptr<void> held;
        bool readScope;
    };
    std::deque<Held> queue;
    std::vector<std::function<void()>> idleCallbacks;
    std::vector<std::shared_ptr<void>> mainThreadHeld;
    bool busy = false;
    bool stopping = false;
    bool stopped = false;
    std::thread thread;

    void run()
    {
        std::unique_lock<std::mutex> lock(mutex);
        for (;;) {
            changed.wait(lock, [this]() { return !queue.empty() || stopping; });
            if (queue.empty()) {
                return;
            }
            Held held = std::move(queue.front());
            queue.pop_front();
            busy = true;
            lock.unlock();
            if (held.readScope) {
                SceneWriteBarrier::ReadScope readScope;
                held.held.reset();
            } else {
                held.held.reset();
            }
            lock.lock();
            // Posted while still busy, so that a waitIdle() that returns finds them posted.
            while (queue.empty() && !idleCallbacks.empty()) {
                std::vector<std::function<void()>> idle;
                idle.swap(idleCallbacks);
                lock.unlock();
                postAll(&idle);
                lock.lock();
            }
            busy = false;
            changed.notify_all();
        }
    }
};

// Never destroyed: a release may come while statics are torn down, and then destroys at once.
TeardownState&
state()
{
    static TeardownState* instance = new TeardownState;

    return *instance;
}
} // anonymous namespace

namespace SceneTeardown {
void
setDeferred(bool deferred)
{
    state().deferred = deferred;
}

bool
isDeferred()
{
    return state().deferred;
}

static void
enqueue(std::shared_ptr<void> held,
        bool readScope)
{
    if (!held) {
        return;
    }
    TeardownState& s = state();
    if (!s.deferred) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.stopped || s.stopping) {
            return;
        }
        s.queue.push_back(TeardownState::Held { std::move(held), readScope });
        if (!s.thread.joinable()) {
            s.thread = std::thread([&s]() { s.run(); });
        }
    }
    s.changed.notify_all();
}

void
releaseHeld(std::shared_ptr<void> held)
{
    enqueue(std::move(held), true);
}

void
releaseHeldUnscoped(std::shared_ptr<void> held)
{
    enqueue(std::move(held), false);
}

static void
destroyMainThreadHeld()
{
    TeardownState& s = state();
    std::vector<std::shared_ptr<void>> held;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        held.swap(s.mainThreadHeld);
    }
    held.clear();
}

void
releaseOnMainThread(std::shared_ptr<void> held)
{
    if (!held) {
        return;
    }
    TeardownState& s = state();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.mainThreadHeld.push_back(std::move(held));
    }
    postToMainThread(&destroyMainThreadHeld);
}

std::size_t
getPendingCount()
{
    TeardownState& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);

    return s.queue.size() + (s.busy ? 1 : 0);
}

bool
waitIdle(int timeoutMs)
{
    TeardownState& s = state();
    std::unique_lock<std::mutex> lock(s.mutex);
    const auto idle = [&s]() { return s.queue.empty() && !s.busy; };

    if (timeoutMs < 0) {
        s.changed.wait(lock, idle);

        return true;
    }

    return s.changed.wait_for(lock, std::chrono::milliseconds(timeoutMs), idle);
}

void
notifyWhenIdle(const std::function<void()>& callback)
{
    if (!callback) {
        return;
    }
    TeardownState& s = state();
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.queue.empty() || s.busy) {
            s.idleCallbacks.push_back(callback);

            return;
        }
    }
    postToMainThread(callback);
}

void
shutdown()
{
    TeardownState& s = state();
    std::thread thread;
    {
        std::unique_lock<std::mutex> lock(s.mutex);
        if (s.stopped || s.stopping) {
            return;
        }
        s.stopping = true;
        thread.swap(s.thread);
    }
    s.changed.notify_all();
    if (thread.joinable()) {
        thread.join();
    }
    std::vector<std::function<void()>> idle;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        s.stopped = true;
        idle.swap(s.idleCallbacks);
    }
    s.changed.notify_all();
    // The event loop that would have destroyed them may not run again.
    destroyMainThreadHeld();
    postAll(&idle);
}
} // namespace SceneTeardown

NATRON_NAMESPACE_EXIT
