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

#ifndef Engine_Nodes_Scene_SceneQueryPool_h
#define Engine_Nodes_Scene_SceneQueryPool_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <list>
#include <memory>
#include <mutex>
#include <string>

#include "Engine/Nodes/Scene/SceneStage.h"
#include "Engine/Nodes/Scene/SceneWriteBarrier.h"
#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Long-lived stages for code that queries a scene it does not display: a node reading its
 * upstream, a test checking a payload. A caller borrows a stage synced to the payload it needs,
 * reads it, and gives it back; nobody else touches the stage meanwhile. The pool hands out the
 * idle stage closest to the payload (the same stack, else the one sharing the longest base), so
 * consecutive queries along a chain, or of a stack one edit away, move a few sublayers instead
 * of composing a new stage.
 *
 * Bounded: past the capacity, the least recently returned idle stages are dropped. Idle stages
 * are dropped too on a cache purge and when a project closes (SceneCaches). MT-safe.
 **/
class SceneQueryPool {
public:
    /**
     * @brief Exclusive use of one stage, given back to the pool when the lease is destroyed or
     * released. A lease holds a SceneWriteBarrier::ReadScope, so it is used and ended on the
     * thread that borrowed it, and a layer reload waits for it to end.
     **/
    class Lease {
    public:
        Lease();

        Lease(Lease&& other);

        Lease& operator=(Lease&& other);

        ~Lease();

        bool isValid() const;

        // The stage, composing the borrowed payload. Null for an invalid lease.
        PXR_NS::UsdStageRefPtr getStage() const;

        // Gives the stage back now.
        void release();

    private:
        friend class SceneQueryPool;

        SceneQueryPool* _pool;
        std::unique_ptr<SceneStage> _stage;
        std::unique_ptr<SceneWriteBarrier::ReadScope> _read;
    };

    // The process-wide pool, sized to the machine's hardware threads.
    static SceneQueryPool& instance();

    explicit SceneQueryPool(std::size_t capacity);

    ~SceneQueryPool();

    /**
     * @brief A stage composing payload, borrowed. Returns an invalid lease with *errorMessage set
     * when OpenUSD is unusable.
     **/
    Lease borrow(const ScenePayload& payload, std::string* errorMessage);

    // Drops every idle stage, and the layers only they kept open.
    void releaseIdle();

    std::size_t getNumIdle() const;

    std::size_t getCapacity() const;

private:
    void giveBack(std::unique_ptr<SceneStage> stage);

    const std::size_t _capacity;
    mutable std::mutex _mutex;
    // Most recently returned first.
    std::list<std::unique_ptr<SceneStage>> _idle;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_SceneQueryPool_h
