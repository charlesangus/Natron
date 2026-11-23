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

#ifndef Engine_Nodes_Loop_GeoSimCache_h
#define Engine_Nodes_Loop_GeoSimCache_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <map>
#include <mutex>
#include <string>

#include "Engine/EngineFwd.h"
#include "Engine/GeoDetail.h"
#include "Global/GlobalDefines.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief One simulation a geometry solver runs: the solver's hash over its inputs, body and
 * settings, the key of the context around it, and the view. Its frames follow from one another.
 **/
struct GeoSimSeries {
    U64 hash = 0;
    U64 contextKey = 0;
    int view = 0;

    bool operator<(const GeoSimSeries& other) const
    {
        if (hash != other.hash) {
            return hash < other.hash;
        }
        if (contextKey != other.contextKey) {
            return contextKey < other.contextKey;
        }

        return view < other.view;
    }

    bool operator==(const GeoSimSeries& other) const
    {
        return (hash == other.hash) && (contextKey == other.contextKey) && (view == other.view);
    }
};

/**
 * @brief The frames a geometry solver has solved. They stay in RAM up to a budget; over it, the
 * least recently used go to files in the native geometry format under the spill directory, or
 * are dropped when there is none, and solve again when asked for. The frame inserted last is
 * never evicted by its own insertion, so a frame larger than the budget still lets a simulation
 * advance. Thread-safe.
 **/
class GeoSimCache {
public:
    explicit GeoSimCache(std::size_t budget);

    /** @brief Removes the files this cache spilled and its directory. */
    ~GeoSimCache();

    GeoSimCache(const GeoSimCache&) = delete;
    GeoSimCache& operator=(const GeoSimCache&) = delete;

    /** @brief The RAM the frames may hold, in bytes; evicts down to it. */
    void setBudget(std::size_t budget);
    std::size_t getBudget() const WARN_UNUSED_RETURN;

    /**
     * @brief Spills into a directory of this cache's own under parent; empty stops spilling and
     * removes what was spilled.
     **/
    void setSpillParent(const std::string& parent);

    /** @brief The directory frames spill to, empty when they do not. */
    std::string spillDirectory() const WARN_UNUSED_RETURN;

    void insert(const GeoSimSeries& series,
                int frame,
                const GeoDetailConstPtr& detail);

    /**
     * @brief The frame of series, read back into RAM when it was spilled; null when it is not
     * kept, or its file can no longer be read.
     **/
    GeoDetailConstPtr find(const GeoSimSeries& series,
                           int frame) WARN_UNUSED_RETURN;

    /** @brief The last frame of series kept in [first, last], in RAM or spilled; false for none. */
    bool latest(const GeoSimSeries& series,
                int first,
                int last,
                int* frame) const WARN_UNUSED_RETURN;

    /** @brief Forgets every frame of series but frame. */
    void retainOnly(const GeoSimSeries& series,
                    int frame);

    /** @brief Forgets every series whose hash is not hash. */
    void retainHash(U64 hash);

    void clear();

    std::size_t ramBytes() const WARN_UNUSED_RETURN;
    std::size_t ramFrameCount() const WARN_UNUSED_RETURN;
    std::size_t spilledFrameCount() const WARN_UNUSED_RETURN;

private:
    struct Entry {
        GeoDetailConstPtr detail;
        std::size_t bytes = 0;
        U64 lastUse = 0;
    };

    struct Frames {
        std::map<int, Entry> ram;
        std::map<int, std::string> spilled;
    };

    void insertLocked(const GeoSimSeries& series,
                      int frame,
                      const GeoDetailConstPtr& detail);
    void evictLocked(const GeoSimSeries& kept,
                     int keptFrame);
    void forgetLocked(Frames* frames,
                      std::map<int, Entry>::iterator ram);
    void removeSpilledLocked(Frames* frames);
    std::string spillPathLocked(const GeoSimSeries& series,
                                int frame) const;

    mutable std::mutex _mutex;
    std::size_t _budget;
    std::size_t _bytes = 0;
    U64 _clock = 0;
    std::string _spillDirectory;
    std::map<GeoSimSeries, Frames> _series;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Loop_GeoSimCache_h
