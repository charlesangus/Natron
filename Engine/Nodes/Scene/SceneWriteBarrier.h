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
 * Writers: runExclusive() aborts the viewers' renders (on the main thread), waits until no thread
 * holds a ReadScope, quiesces the registered participants (owners of stages that read outside a
 * ReadScope, like a viewer painting or a Hydra render thread), runs the write, then resumes them.
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

    typedef U64 ParticipantId;

    /**
     * @brief Registers the owner of stages read outside any ReadScope. quiesce is called before a
     * write, from the writing thread, once no ReadScope is held; it returns once the owner no
     * longer reads any stage and will not until resume is called after the write. Both may be
     * empty. MT-safe; unregistering waits for a write in progress to end.
     **/
    static ParticipantId registerParticipant(const std::function<void()>& quiesce,
                                             const std::function<void()>& resume);

    static void unregisterParticipant(ParticipantId id);

    /**
     * @brief Runs write while nothing reads a stage. Fails, with *errorMessage set, when the
     * calling thread holds a ReadScope: it would wait for itself. Writers run one at a time.
     **/
    static bool runExclusive(const std::function<void()>& write,
                             std::string* errorMessage);

    // Whether the calling thread holds a ReadScope.
    static bool isReadingOnThisThread();

    // How many writes have run in this process.
    static U64 getWriteCount();
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_SceneWriteBarrier_h
