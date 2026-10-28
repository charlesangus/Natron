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

#ifndef Engine_Nodes_Loop_LoopFrameCache_h
#define Engine_Nodes_Loop_LoopFrameCache_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <map>
#include <vector>

#include <QtCore/QMutex>

#include "Engine/EngineFwd.h"
#include "Global/GlobalDefines.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief One simulation a cross-frame loop runs: the loop's hash (its generation), the context key
 * it renders under, the view and the mipmap level. Its frames follow from one another.
 **/
struct LoopFrameSeries {
    U64 hash = 0;
    U64 contextKey = 0;
    int view = 0;
    unsigned int mipmapLevel = 0;

    bool operator<(const LoopFrameSeries& other) const
    {
        if (hash != other.hash) {
            return hash < other.hash;
        }
        if (contextKey != other.contextKey) {
            return contextKey < other.contextKey;
        }
        if (view != other.view) {
            return view < other.view;
        }

        return mipmapLevel < other.mipmapLevel;
    }

    bool operator==(const LoopFrameSeries& other) const
    {
        return !(*this < other) && !(other < *this);
    }
};

/**
 * @brief The frames a cross-frame loop has rendered, in RAM under a bytes budget. Over budget, the
 * least recently used frames go first, checkpoint frames last: those at startFrame + k * the
 * checkpoint interval, a power of two chosen so the checkpoints of every series fit in a quarter
 * of the budget. The frame inserted last is never evicted by its own insertion, so a frame larger
 * than the budget still lets a simulation advance. Thread-safe.
 **/
class LoopFrameCache {
public:
    explicit LoopFrameCache(std::size_t budget);

    LoopFrameCache(const LoopFrameCache&) = delete;
    LoopFrameCache& operator=(const LoopFrameCache&) = delete;

    /**
     * @brief The budget a loop's cache gets by default: a quarter of the RAM the image cache may use.
     **/
    static std::size_t defaultBudget() WARN_UNUSED_RETURN;

    void setBudget(std::size_t budget);

    std::size_t getBudget() const WARN_UNUSED_RETURN;

    /**
     * @brief Stores image as frame of series, whose first frame is startFrame, accounting bytes for
     * it (the image's own size when 0), then evicts down to the budget.
     **/
    void insert(const LoopFrameSeries& series,
                int startFrame,
                int frame,
                const ImagePtr& image,
                std::size_t bytes = 0);

    /**
     * @brief frame of series, null when it is not stored. Counts as a use.
     **/
    ImagePtr lookup(const LoopFrameSeries& series,
                    int frame) WARN_UNUSED_RETURN;

    /**
     * @brief The latest stored frame of series before frame, and its image; false when none is.
     * Counts as a use.
     **/
    bool nearestBefore(const LoopFrameSeries& series,
                       int frame,
                       int* found,
                       ImagePtr* image) WARN_UNUSED_RETURN;

    /**
     * @brief Drops every series whose hash is not hash.
     **/
    void purgeOtherGenerations(U64 hash);

    void clear();

    std::size_t bytes() const WARN_UNUSED_RETURN;

    /**
     * @brief The stored frames of series, in increasing order.
     **/
    std::vector<int> frames(const LoopFrameSeries& series) const WARN_UNUSED_RETURN;

    /**
     * @brief The checkpoint interval of series as its stored frames stand, 1 when it has none.
     **/
    int checkpointInterval(const LoopFrameSeries& series) const WARN_UNUSED_RETURN;

private:
    struct Entry {
        ImagePtr image;
        std::size_t bytes = 0;
        U64 lastUse = 0;
    };

    struct Frames {
        int startFrame = 0;
        std::size_t bytes = 0;
        std::map<int, Entry> entries;
    };

    typedef std::map<LoopFrameSeries, Frames> SeriesMap;

    int intervalOf(const Frames& frames) const WARN_UNUSED_RETURN;

    // Evicts down to the budget, keeping keptFrame of kept.
    void evict(const LoopFrameSeries& kept,
               int keptFrame);

    mutable QMutex _mutex;
    std::size_t _budget;
    std::size_t _bytes;
    U64 _clock;
    SeriesMap _series;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Loop_LoopFrameCache_h
