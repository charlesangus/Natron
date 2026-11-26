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

#include "FrameArgsOverlay.h"

#include <algorithm>
#include <atomic>

#include "Engine/FrameRenderContext.h"
#include "Engine/TLSHolder.h"

NATRON_NAMESPACE_ENTER

NATRON_NAMESPACE_ANONYMOUS_ENTER

thread_local FrameArgsOverlayPtr tHead;

// Written only when a counter moves, so the threads pushing and looking up share these lines read-only.
std::atomic<std::uint64_t> gOutOfOrderPops(0);
std::atomic<std::size_t> gMaxDepth(0);

bool
entryKeyLess(const FrameArgsOverlay::Entry& a,
             const FrameArgsOverlay::Entry& b)
{
    return a.first < b.first;
}

void
noteDepth(std::size_t depth)
{
    std::size_t seen = gMaxDepth.load(std::memory_order_relaxed);

    while ((depth > seen) && !gMaxDepth.compare_exchange_weak(seen, depth, std::memory_order_relaxed)) {
    }
}

NATRON_NAMESPACE_ANONYMOUS_EXIT

FrameArgsOverlay::FrameArgsOverlay(MakeTag,
                                   const FrameArgsOverlayPtr& parent,
                                   const FrameRenderContext* base,
                                   bool isBase,
                                   Entries entries)
    : _parent(isBase ? FrameArgsOverlayPtr() : parent)
    , _depth(_parent ? _parent->_depth + 1 : 1)
    , _base(base)
    , _isBase(isBase)
    , _entries()
{
    std::stable_sort(entries.begin(), entries.end(), entryKeyLess);
    _entries.reserve(entries.size());
    for (Entries::iterator it = entries.begin(); it != entries.end(); ++it) {
        if (!_entries.empty() && (_entries.back().first == it->first)) {
            _entries.back().second = it->second;
        } else {
            _entries.push_back(*it);
        }
    }
}

FrameArgsOverlayPtr
FrameArgsOverlay::make(const FrameArgsOverlayPtr& parent,
                       const FrameRenderContext* base,
                       bool isBase,
                       Entries entries)
{
    FrameArgsOverlayPtr ret = std::make_shared<const FrameArgsOverlay>(MakeTag(), parent, base, isBase, std::move(entries));

    noteDepth(ret->_depth);

    return ret;
}

FrameArgsOverlayPtr
FrameArgsOverlay::makeEntry(const FrameArgsOverlayPtr& parent,
                            Key key,
                            const ParallelRenderArgsPtr& args)
{
    return make(parent, 0, false, Entries(1, Entry(key, args)));
}

FrameArgsOverlayPtr
FrameArgsOverlay::makeEntries(const FrameArgsOverlayPtr& parent,
                              Entries entries)
{
    return make(parent, 0, false, std::move(entries));
}

FrameArgsOverlayPtr
FrameArgsOverlay::makeBase(const FrameRenderContext* context)
{
    return make(FrameArgsOverlayPtr(), context, true, Entries());
}

const FrameArgsOverlayPtr&
FrameArgsOverlay::currentHead()
{
    return tHead;
}

FrameArgsOverlayPtr
FrameArgsOverlay::exchangeHead(FrameArgsOverlayPtr head)
{
    tHead.swap(head);

    return head;
}

bool
FrameArgsOverlay::find(Key key,
                       ParallelRenderArgsPtr* args) const
{
    if (_entries.size() == 1) {
        if (_entries.front().first != key) {
            return false;
        }
        *args = _entries.front().second;

        return true;
    }
    Entries::const_iterator found = std::lower_bound(_entries.begin(), _entries.end(), Entry(key, ParallelRenderArgsPtr()), entryKeyLess);
    if ((found == _entries.end()) || (found->first != key)) {
        return false;
    }
    *args = found->second;

    return true;
}

ParallelRenderArgsPtr
FrameArgsOverlay::lookup(Key key)
{
    return lookup(tHead.get(), key, AppTLS::currentFrameContext());
}

ParallelRenderArgsPtr
FrameArgsOverlay::lookup(const FrameArgsOverlay* head,
                         Key key,
                         const FrameRenderContext* fallback)
{
    for (const FrameArgsOverlay* link = head; link; link = link->_parent.get()) {
        if (link->_isBase) {
            return link->_base ? link->_base->getArgsForHolder(key) : ParallelRenderArgsPtr();
        }
        ParallelRenderArgsPtr args;
        if (link->find(key, &args)) {
            return args;
        }
    }

    return fallback ? fallback->getArgsForHolder(key) : ParallelRenderArgsPtr();
}

ParallelRenderArgsPtr
FrameArgsOverlay::lookup(Key key,
                         Filter accept)
{
    for (const FrameArgsOverlay* link = tHead.get(); link; link = link->_parent.get()) {
        if (link->_isBase) {
            const ParallelRenderArgsPtr args = link->_base ? link->_base->getArgsForHolder(key) : ParallelRenderArgsPtr();

            return (args && accept(args)) ? args : ParallelRenderArgsPtr();
        }
        ParallelRenderArgsPtr args;
        if (link->find(key, &args) && args && accept(args)) {
            return args;
        }
    }
    const FrameRenderContext* fallback = AppTLS::currentFrameContext();
    const ParallelRenderArgsPtr args = fallback ? fallback->getArgsForHolder(key) : ParallelRenderArgsPtr();

    return (args && accept(args)) ? args : ParallelRenderArgsPtr();
}

void
FrameArgsOverlay::noteOutOfOrderPop()
{
    gOutOfOrderPops.fetch_add(1, std::memory_order_relaxed);
}

FrameArgsOverlay::Stats
FrameArgsOverlay::stats()
{
    Stats ret;

    ret.outOfOrderPops = gOutOfOrderPops.load(std::memory_order_relaxed);
    ret.maxDepth = gMaxDepth.load(std::memory_order_relaxed);

    return ret;
}

void
FrameArgsOverlay::resetStats()
{
    gOutOfOrderPops.store(0, std::memory_order_relaxed);
    gMaxDepth.store(0, std::memory_order_relaxed);
}

FrameArgsScope::FrameArgsScope()
    : _saved()
    , _pushed(0)
{
}

FrameArgsScope::FrameArgsScope(FrameArgsOverlay::Key key,
                               const ParallelRenderArgsPtr& args)
    : _saved()
    , _pushed(0)
{
    push(FrameArgsOverlay::makeEntry(tHead, key, args));
}

FrameArgsScope::FrameArgsScope(FrameArgsOverlay::Entries entries)
    : _saved()
    , _pushed(0)
{
    if (!entries.empty()) {
        push(FrameArgsOverlay::makeEntries(tHead, std::move(entries)));
    }
}

FrameArgsScope::~FrameArgsScope()
{
    pop();
}

void
FrameArgsScope::leaveInstalled()
{
    _saved.reset();
    _pushed = 0;
}

FrameArgsScope::FrameArgsScope(FrameArgsScope&& other) noexcept
    : _saved(std::move(other._saved))
    , _pushed(other._pushed)
{
    other._pushed = 0;
}

FrameArgsScope&
FrameArgsScope::operator=(FrameArgsScope&& other) noexcept
{
    if (this != &other) {
        pop();
        _saved = std::move(other._saved);
        _pushed = other._pushed;
        other._pushed = 0;
    }

    return *this;
}

void
FrameArgsScope::push(const FrameArgsOverlayPtr& link)
{
    _saved = tHead;
    _pushed = link.get();
    tHead = link;
}

void
FrameArgsScope::pop()
{
    if (!_pushed) {
        return;
    }
    if (tHead.get() != _pushed) {
        FrameArgsOverlay::noteOutOfOrderPop();
    }
    tHead = std::move(_saved);
    _saved.reset();
    _pushed = 0;
}

NATRON_NAMESPACE_EXIT
