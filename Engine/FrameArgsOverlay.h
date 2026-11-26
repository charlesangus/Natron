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

#ifndef Engine_FrameArgsOverlay_h
#define Engine_FrameArgsOverlay_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

class FrameArgsOverlay;
typedef std::shared_ptr<const FrameArgsOverlay> FrameArgsOverlayPtr;

/**
 * @brief A link of the current thread's frame-args chain. A link holds the args of one effect, the args installed on
 * several effects at once, or the frame a scheduler task renders, which is the base of the chain under it. Links never
 * change once made, and each points at the head the thread had when it was pushed, so a thread spawned for a render
 * reads the chain its spawner captured without any lock while the spawner pushes on its own.
 *
 * A lookup walks from the head towards the base: the innermost args of an effect win, and a base answers with its
 * frame's args for the effect. A chain with no base answers past its last link from the thread's
 * AppTLS::currentFrameContext(), and with no frame either there are no args.
 *
 * Effects are keyed by their thread-local data holder, which an effect shares with its render clones.
 **/
class FrameArgsOverlay {
    struct MakeTag { };

public:
    typedef const TLSHolderBase* Key;
    typedef std::pair<Key, ParallelRenderArgsPtr> Entry;
    typedef std::vector<Entry> Entries;

    typedef bool (*Filter)(const ParallelRenderArgsPtr& args);

    /**
     * @brief Counters of the chains of all threads since the last resetStats(): scopes popped out of order and the
     * deepest chain made.
     **/
    struct Stats {
        std::uint64_t outOfOrderPops;
        std::size_t maxDepth;
    };

    FrameArgsOverlay(MakeTag,
                     const FrameArgsOverlayPtr& parent,
                     const FrameRenderContext* base,
                     bool isBase,
                     Entries entries);

    static FrameArgsOverlayPtr makeEntry(const FrameArgsOverlayPtr& parent,
                                         Key key,
                                         const ParallelRenderArgsPtr& args);

    /**
     * @brief Of several entries for one key, the last one wins, as the last of several pushes would.
     **/
    static FrameArgsOverlayPtr makeEntries(const FrameArgsOverlayPtr& parent,
                                           Entries entries);

    /**
     * @brief A link nothing is under: lookups that reach it answer from context's key-0 args alone, with none when
     * context is null.
     **/
    static FrameArgsOverlayPtr makeBase(const FrameRenderContext* context);

    static const FrameArgsOverlayPtr& currentHead();

    /**
     * @brief Makes head the current thread's head and returns the previous one.
     **/
    static FrameArgsOverlayPtr exchangeHead(FrameArgsOverlayPtr head);

    static ParallelRenderArgsPtr lookup(Key key);

    static ParallelRenderArgsPtr lookup(const FrameArgsOverlay* head,
                                        Key key,
                                        const FrameRenderContext* fallback);

    /**
     * @brief As lookup(key), skipping the args accept rejects: the innermost args of the effect accept takes, the
     * frame's last, or none.
     **/
    static ParallelRenderArgsPtr lookup(Key key,
                                        Filter accept);

    static void noteOutOfOrderPop();

    static Stats stats();

    static void resetStats();

    /**
     * @brief Links from this one down to the end of its chain, this one included.
     **/
    std::size_t depth() const
    {
        return _depth;
    }

    const FrameArgsOverlayPtr& parent() const
    {
        return _parent;
    }

private:
    static FrameArgsOverlayPtr make(const FrameArgsOverlayPtr& parent,
                                    const FrameRenderContext* base,
                                    bool isBase,
                                    Entries entries);

    bool find(Key key,
              ParallelRenderArgsPtr* args) const;

    FrameArgsOverlayPtr _parent;
    std::size_t _depth;
    const FrameRenderContext* _base;
    bool _isBase;
    // Sorted by key, one entry per key.
    Entries _entries;
};

/**
 * @brief Pushes a link on the current thread's chain for the scope's lifetime and makes the head it found current
 * again on destruction. Scopes on a thread must end in the reverse order they began; one that ends while another
 * pushed after it is still open is counted in FrameArgsOverlay::Stats::outOfOrderPops.
 **/
class FrameArgsScope {
public:
    FrameArgsScope();

    FrameArgsScope(FrameArgsOverlay::Key key,
                   const ParallelRenderArgsPtr& args);

    explicit FrameArgsScope(FrameArgsOverlay::Entries entries);

    ~FrameArgsScope();

    /**
     * @brief Leaves the link pushed once the scope is gone; the scope that pushed the link under it restores past it.
     **/
    void leaveInstalled();

    FrameArgsScope(FrameArgsScope&& other) noexcept;
    FrameArgsScope& operator=(FrameArgsScope&& other) noexcept;

    FrameArgsScope(const FrameArgsScope&) = delete;
    FrameArgsScope& operator=(const FrameArgsScope&) = delete;

private:
    void push(const FrameArgsOverlayPtr& link);

    void pop();

    FrameArgsOverlayPtr _saved;
    const FrameArgsOverlay* _pushed;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_FrameArgsOverlay_h
