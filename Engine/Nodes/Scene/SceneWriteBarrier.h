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

#ifndef Engine_Nodes_Scene_SceneWriteBarrier_h
#define Engine_Nodes_Scene_SceneWriteBarrier_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <functional>
#include <string>

#include "Global/GlobalDefines.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief The process-wide barrier between reading USD stages and editing the layers they share
 * in place. Free of pxr types, so code compiled under Py_LIMITED_API can take a ReadScope.
 *
 * OpenUSD allows many readers or one writer, and a layer two stages share puts both stages in one
 * write domain (UsdStage::Reload in stage.h: call it only when no other thread reads any stage).
 * Node layers are never edited, so ordinary scene work only reads; the exceptions are layers read
 * from files, which SdfLayer::ReloadLayers edits in place so that every stage composing them sees
 * the new content through its change notices. A SceneStage's private root layer is edited by its
 * owner alone and needs no barrier.
 *
 * Readers: every thread that evaluates a scene or reads a stage shared with others holds a
 * ReadScope while it does (the scene action, the layer memo, query stage leases, the viewer's
 * sync). Scopes nest on one thread; only the outermost waits. Take the outermost scope before any
 * lock another reader may wait on, as a writer waiting to start blocks new outermost scopes.
 *
 * Writers: runExclusive() aborts the viewers' renders (on the main thread), parks the participants
 * that can park and waits for them while readers still run, then waits until no thread holds a
 * ReadScope, quiesces the registered participants (owners of stages that read outside a
 * ReadScope, like a viewer painting or a Hydra render thread), runs the write, then resumes and
 * unparks them. Parking first keeps new readers, the GUI's among them, from queueing behind a
 * participant that is inside a long delegate call: the quiesce then finds it already stopped.
 * runExclusiveAsync() does the same without blocking the main thread on the parking.
 **/
class SceneWriteBarrier {
public:
    class ReadScope {
    public:
        ReadScope();

        ~ReadScope();

    private:
        ReadScope(const ReadScope&) = delete;
        ReadScope& operator=(const ReadScope&) = delete;

        bool _outermost;
    };

    /**
     * @brief Counts the calling thread as reading under a ReadScope another thread holds for
     * longer than this one lives, without waiting for a writer. For a helper that holder waits
     * for: a writer pending would hold the helper's own outermost scope off, while waiting for
     * the holder's scope to end. Does nothing when held is false.
     **/
    class SharedReadScope {
    public:
        explicit SharedReadScope(bool held);

        ~SharedReadScope();

    private:
        SharedReadScope(const SharedReadScope&) = delete;
        SharedReadScope& operator=(const SharedReadScope&) = delete;

        bool _held;
    };

    typedef U64 ParticipantId;

    // When a participant is quiesced relative to the others; resume runs in the reverse order.
    enum QuiesceOrderEnum {
        eQuiesceOrderNormal = 0,
        // After every normal participant: for a quiesce that takes a lock the GUI thread waits on,
        // so the lock is not held while a normal participant waits out a long call.
        eQuiesceOrderLast
    };

    // Asks a participant to park; it calls parked, once, from any thread, when it is parked.
    typedef std::function<void(const std::function<void()>& parked)> ParkFunction;
    typedef std::function<void(bool ok, const std::string& errorMessage)> WriteDoneFunction;

    /**
     * @brief Registers the owner of stages read outside any ReadScope. quiesce is called before a
     * write, from the writing thread, once no ReadScope is held; it returns once the owner no
     * longer reads any stage and will not until resume is called after the write. Both may be
     * empty. MT-safe; unregistering waits for a write in progress to end.
     *
     * park, if given, must not block: it asks the owner to stop at its next point where it holds
     * nothing quiesce waits on and starts no call that would, and to call parked from there (or
     * at once, from inside park, if it is already at one). It stays parked until unpark is
     * called. A write waits for every parkable participant to report parked before it keeps
     * readers out, so its quiesce takes no time. Unregistering counts as parked. Neither is
     * called after unregisterParticipant() returns.
     **/
    static ParticipantId registerParticipant(const std::function<void()>& quiesce,
                                             const std::function<void()>& resume,
                                             QuiesceOrderEnum order = eQuiesceOrderNormal,
                                             const ParkFunction& park = ParkFunction(),
                                             const std::function<void()>& unpark = std::function<void()>());

    static void unregisterParticipant(ParticipantId id);

    /**
     * @brief Runs write while nothing reads a stage. Fails, with *errorMessage set, when the
     * calling thread holds a ReadScope: it would wait for itself. Writers run one at a time.
     * Blocks while the parkable participants park, but lets readers run meanwhile.
     **/
    static bool runExclusive(const std::function<void()>& write,
                             std::string* errorMessage);

    /**
     * @brief runExclusive() for the main thread, which it never blocks on the parking: aborts the
     * viewers' renders at once, asks the participants to park, and returns. Once the last has
     * parked, the write runs on the main thread from its event loop, then the participants are
     * resumed and unparked and done is called on the main thread with the outcome. Requests made
     * while one is waiting run in the same exclusive section, each with its own done. Runs at
     * once, before returning, when every participant is already parked or none can park (no
     * viewer is rendering), and blocking, as runExclusive(), off the main thread.
     **/
    static void runExclusiveAsync(const std::function<void()>& write,
                                  const WriteDoneFunction& done);

    // How many runExclusiveAsync() writes have not called their done yet.
    static int getPendingAsyncWriteCount();

    /**
     * @brief Main thread only: processes events until every runExclusiveAsync() write has called
     * its done, or timeoutMs passed (never when negative). Returns whether none is pending.
     **/
    static bool waitForAsyncWrites(int timeoutMs);

    // Whether the calling thread holds a ReadScope.
    static bool isReadingOnThisThread();

    // Whether the calling thread is running the write of runExclusive().
    static bool isWritingOnThisThread();

    // How many writes have run in this process.
    static U64 getWriteCount();

    // Called, if set, on the writing thread of runExclusive() once the readers have drained: it
    // holds writerMutex there and has not yet taken the GIL back. Lets a test hold the writer at
    // the point where a lock-order mistake would show. Pass nullptr to clear.
    typedef void (*TestHookFunction)(void* context);
    static void setTestHook(TestHookFunction hook, void* context);
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_SceneWriteBarrier_h
