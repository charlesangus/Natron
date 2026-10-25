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

#ifndef Engine_Nodes_Scene_StageCache_h
#define Engine_Nodes_Scene_StageCache_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <list>
#include <mutex>

#include "Engine/Nodes/Scene/UsdIncludes.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Where consumers get composed stages: payloads are only composed here, never by the
 * nodes that author them. Entries are keyed by stack hash, so every consumer of one layer stack
 * shares one stage, and the least recently used entry goes once there are more than the capacity.
 *
 * The stages are shared and read-only: a consumer must not edit one, nor hold it past the
 * evaluation it was fetched for. MT-safe; composition runs outside the lock.
 **/
class StageCache {
public:
    explicit StageCache(std::size_t capacity);

    ~StageCache();

    // The process-wide cache consumers use.
    static StageCache& instance();

    /**
     * @brief The composed stage of payload. An empty payload composes to an empty stage. Null
     * only if USD failed to compose it.
     **/
    PXR_NS::UsdStageRefPtr getStage(const ScenePayload& payload);

    void clear();

    std::size_t getNumEntries() const;

    U64 getHitCount() const;

    U64 getMissCount() const;

private:
    struct Entry {
        U64 stackHash;
        // Keeps the layers alive for as long as the stage composed from them.
        ScenePayload payload;
        PXR_NS::UsdStageRefPtr stage;
    };

    mutable std::mutex _mutex;
    std::size_t _capacity;
    std::list<Entry> _entries; //< most recently used first
    U64 _hits;
    U64 _misses;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_StageCache_h
