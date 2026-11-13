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

#ifndef Engine_Nodes_Scene_TexturePaintBaker_h
#define Engine_Nodes_Scene_TexturePaintBaker_h

#include "Global/Macros.h"

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>

#include <QString>

#include "Engine/EngineFwd.h"
#include "Engine/GenericSchedulerThread.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Runs the bakes of a TexturePaint node one after the other on a thread of its own, so the
 * main thread only ever enqueues them. A bake is a worker function and a completion function:
 * the worker runs on the scheduler thread and polls Context for cancellation between tiles and
 * sessions; the completion runs on the main thread, queued, once the worker returned, and is
 * where a bake changes knobs, freezes sessions or pushes its undo step. Completions run in
 * enqueue order. Nothing here takes the viewer lock.
 *
 * Stopping (the node is deleted, the project closes) cancels the running worker and drops the
 * queued bakes and every completion not yet run. requestStop() and retire() return at once;
 * stop() also waits for the thread to exit.
 **/
class TexturePaintBaker
    : public GenericSchedulerThread {
    GCC_DIAG_SUGGEST_OVERRIDE_OFF
    Q_OBJECT
    GCC_DIAG_SUGGEST_OVERRIDE_ON

    class Task;

public:
    class Context {
    public:
        explicit Context(TexturePaintBaker* baker,
                         Task* task = nullptr)
            : _baker(baker)
            , _task(task)
        {
        }

        bool cancelled() const
        {
            return _baker->_cancel.load();
        }

        // fraction is 0 to 1 over the whole bake; the signal is rate-limited, so call as often as is cheap.
        void progress(double fraction,
                      const QString& label);

        /**
         * @brief Keeps object alive until the bake's result reaches the main thread, so that the
         * worker never drops the last reference to it: what it destroys is then destroyed on the
         * main thread, whether or not the completion runs.
         **/
        void keepAlive(const std::shared_ptr<void>& object);

    private:
        TexturePaintBaker* _baker;
        Task* _task;
    };

    struct Bake {
        QString label;
        // Scheduler thread. False when the bake failed or was cancelled.
        std::function<bool(Context&)> work;
        // Main thread, after work() returned, with its result. Never run for a bake that stopping dropped.
        std::function<void(bool ok)> completion;
    };

    TexturePaintBaker();

    virtual ~TexturePaintBaker();

    /**
     * @brief Queues bake behind those already queued or running. Does not wait. Main thread.
     * False once stop() was called.
     **/
    bool enqueue(const Bake& bake);

    /**
     * @brief Cancels the running bake and drops the queued ones and their completions; does not
     * wait. With allowRestart the baker takes bakes again afterwards, as a node does when its
     * deletion is undone.
     **/
    void requestStop(bool allowRestart);

    // requestStop(), then waits for the thread to exit: bounded by one tile, the worker polling between tiles.
    void stop(bool allowRestart = false);

    /**
     * @brief Stops baker for good without waiting and deletes it on the main thread once its thread
     * has exited. Safe to call from inside one of its own signals or completions.
     **/
    static void retire(std::unique_ptr<TexturePaintBaker> baker);

    // Whether a bake is queued or running, or finished but its completion has not run yet.
    bool isBaking() const WARN_UNUSED_RETURN
    {
        return _pending.load() > 0;
    }

    // Bakes queued, running or awaiting their completion.
    int getPendingCount() const WARN_UNUSED_RETURN
    {
        return _pending.load();
    }

    // How many bakes have succeeded and run their completion; each one bumps it.
    quint64 getGeneration() const WARN_UNUSED_RETURN
    {
        return _generation.load();
    }

    // The most recent progress reported, for a GUI that connects after the bake began.
    void getLastProgress(double* fraction,
                         QString* label) const;

Q_SIGNALS:

    // Queued to the main thread, at most every 50 ms.
    void progress(double fraction,
                  const QString& label);

    // After a bake's completion ran. generation is getGeneration() including this bake when ok.
    void bakeFinished(bool ok,
                      quint64 generation);

    // After the last queued bake finished.
    void idle();

    // Emitted by the scheduler thread when a bake's work returned; queued to onWorkDone().
    void workDone();

private Q_SLOTS:

    void onWorkDone();

private:
    virtual TaskQueueBehaviorEnum tasksQueueBehaviour() const OVERRIDE FINAL WARN_UNUSED_RETURN
    {
        return eTaskQueueBehaviorProcessInOrder;
    }

    virtual ThreadStateEnum threadLoopOnce(const GenericThreadStartArgsPtr& inArgs) OVERRIDE FINAL WARN_UNUSED_RETURN;
    virtual void onAbortRequested(bool keepOldestRender) OVERRIDE FINAL;
    virtual void onQuitRequested(bool allowRestarts) OVERRIDE FINAL;

    void reportProgress(double fraction,
                        const QString& label);

    std::atomic<bool> _cancel;
    std::atomic<int> _pending;
    std::atomic<quint64> _generation;
    std::atomic<bool> _stopped;
    // Counts the stops, so that a bake enqueued before one never runs its completion after it.
    std::atomic<int> _epoch;

    mutable std::mutex _mutex;
    // Bakes whose work returned, oldest first, awaiting their completion on the main thread.
    std::deque<std::pair<std::shared_ptr<Task>, bool>> _finished;
    std::chrono::steady_clock::time_point _lastProgress;
    double _lastFraction;
    QString _lastLabel;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_TexturePaintBaker_h
