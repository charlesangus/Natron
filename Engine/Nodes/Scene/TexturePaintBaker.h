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
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

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
 * stop() also waits for the thread to exit. A stop that allows restarting takes bakes again at
 * once: those enqueued while the thread exits wait, in order, and start once it has.
 *
 * Every baker of the process, retired ones included, is known, so that shutdownAll() can stop and
 * join them all without the event loop that deletes retired ones.
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
     * False once the baker stopped for good.
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

    /**
     * @brief Stops every baker for good and waits for its thread, drops every completion not yet
     * run, whose bakes put their files back, deletes the retired bakers, then shuts the
     * TexturePaintFileQueue down. Main thread, once, while the application shuts down, before
     * the plug-ins and statics the bakes use go.
     **/
    static void shutdownAll();

    // Lets bakers made afterwards, and the file queue, work again after shutdownAll(): see TexturePaintFileQueue::restartForTesting().
    static void restartForTesting();

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

    void onThreadFinished();

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
    // Set under _mutex while a stop lets the thread exit, until onThreadFinished() has seen it gone.
    std::atomic<bool> _quitting;
    // Main thread, under the registry's lock.
    bool _retired;
    bool _shutDown;

    mutable std::mutex _mutex;
    struct Finished {
        Bake bake;
        std::vector<std::shared_ptr<void>> keepAlive;
        int epoch = 0;
        bool ok = false;
    };
    // Bakes whose work returned, oldest first, awaiting their completion on the main thread.
    std::deque<Finished> _finished;
    // Bakes enqueued while _quitting, oldest first, started once the thread has exited.
    std::deque<std::shared_ptr<Task>> _deferred;
    std::chrono::steady_clock::time_point _lastProgress;
    double _lastFraction;
    QString _lastLabel;
};

struct TexturePaintFileStep;
typedef std::shared_ptr<TexturePaintFileStep> TexturePaintFileTicket;

/**
 * @brief The one thread of the process on which every Bake Output file step of every node runs,
 * strictly in the order the steps were submitted: backing up and writing targets, sealing,
 * undo and redo swaps, the rollbacks of failed and dropped bakes, and discarding backups. With
 * one ordered executor, a step sees every file as the steps submitted before it left it.
 *
 * A bake claims each file before backing it up and settles its claims once its undo step owns the
 * files or once they are put back. A claim a bake of another node holds is refused, naming that
 * node; one an earlier bake of the same node holds is reported so that the caller can wait for it
 * to land or to be put back.
 **/
class TexturePaintFileQueue {
public:
    // shuttingDown: shutdown() has begun, and what outlives the application must not be touched.
    typedef std::function<void(bool shuttingDown)> Work;

    enum ClaimResultEnum {
        eClaimResultTaken,
        eClaimResultHeldBySameOwner,
        eClaimResultHeldByAnotherOwner,
    };

    // A step that runs work once submitted.
    static TexturePaintFileTicket makeStep(const Work& work);

    /**
     * @brief Queues step behind every step submitted before it; returns at once. Once shutdown()
     * has finished, runs it on the caller, with shuttingDown.
     **/
    static void submit(const TexturePaintFileTicket& step);

    static TexturePaintFileTicket submit(const Work& work);

    /**
     * @brief Waits until step has run. False when cancelled() turned true first: the step still
     * runs, in its turn. Never on the queue's own thread.
     **/
    static bool wait(const TexturePaintFileTicket& step,
                     const std::function<bool()>& cancelled = std::function<bool()>());

    // A number no call returned before, never 0: the identity of a bake or of a node.
    static std::uint64_t newId();

    /**
     * @brief The key a claim on path is held under: its directory resolved through symbolic links
     * and "..", then its file name, so that every spelling of one file claims it alike.
     **/
    static std::string claimKey(const std::string& path);

    /**
     * @brief Records that bake, of owner, is about to back up the file of key, a claimKey(). Owner
     * 0 is no node, held by no other bake. Otherwise *holder receives the owner whose bake holds it.
     **/
    static ClaimResultEnum claim(std::uint64_t bake,
                                 std::uint64_t owner,
                                 const std::string& key,
                                 std::uint64_t* holder);

    // Whether a bake other than bake holds the file of key, *holder receiving its owner.
    static bool isHeldByAnotherBake(std::uint64_t bake,
                                    const std::string& key,
                                    std::uint64_t* holder);

    /**
     * @brief The name messages give owner by, kept current by the owner as it is renamed; read on
     * any thread, so that a refusal names the node as it is called when it is refused.
     **/
    static void setOwnerName(std::uint64_t owner,
                             const std::string& name);

    // The name stays until the last claim of owner settles.
    static void forgetOwner(std::uint64_t owner);

    // Empty for an owner with no name.
    static std::string getOwnerName(std::uint64_t owner);

    // Drops every claim of bake. Safe to call more than once.
    static void settle(std::uint64_t bake);

    // Returns once some bake settled, or after timeout.
    static void waitForSettlement(std::chrono::milliseconds timeout);

    /**
     * @brief Runs every queued step, then stops the thread and waits for it. Steps submitted
     * afterwards run on their callers. Called once, while the application shuts down.
     **/
    static void shutdown();

    // Runs on the queue's thread before each step; an empty hook removes it.
    static void setBeforeWorkHookForTesting(const std::function<void()>& hook);

    // Whether no step is queued or running.
    static bool isIdleForTesting();

    // How many steps wait behind the one running, if any.
    static std::size_t getQueuedCountForTesting();

    // Whether the queue's thread exists.
    static bool isThreadRunningForTesting();

    // Takes steps on a thread again after shutdown(), with no claim held, no owner named and no hook.
    static void restartForTesting();
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_TexturePaintBaker_h
