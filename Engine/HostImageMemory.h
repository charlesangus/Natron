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

#ifndef NATRON_ENGINE_HOSTIMAGEMEMORY_H
#define NATRON_ENGINE_HOSTIMAGEMEMORY_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <functional>

#include "Engine/EngineFwd.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Allocator for RAM image buffers that can be imported by a GPU as host memory.
 *
 * Buffers of at least minPooledBytes() are page-aligned and padded to the import
 * alignment, and released ones are kept in a bounded pool so repeated same-size frames
 * reuse memory that is already touched and possibly already imported. Smaller buffers
 * come from plain malloc: they are never worth importing.
 *
 * Pooled-but-unused bytes count against a cache's RAM limit: before a Cache evicts
 * anything it calls trimTo() with its remaining headroom, so idle pool memory is given up
 * before live entries are. They are also bounded by maxPoolBytes(), a quarter of the system
 * RAM by default, and dropped by trim() or when an allocation fails.
 *
 * All functions are thread-safe.
 **/
class HostImageMemory
{
public:

    typedef std::function<void (void* ptr, std::size_t paddedBytes)> FreeHook;

    static std::size_t minPooledBytes();

    static void* allocate(std::size_t bytes);

    /**
     * @brief Returns a buffer obtained from allocate(). A large buffer goes back to the
     * pool unless the pool is full, in which case the oldest pooled buffers are freed.
     **/
    static void release(void* ptr, std::size_t bytes);

    /**
     * @brief Size a request for @p bytes occupies once padded.
     **/
    static std::size_t paddedSize(std::size_t bytes);

    static std::size_t pageSize();

    /**
     * @brief Sets the size multiple that buffers are padded to (at least the page size).
     * Takes effect for later allocations; the pool is emptied.
     **/
    static void setImportAlignment(std::size_t alignment);
    static std::size_t importAlignment();

    static void setMaxPoolBytes(std::size_t bytes);
    static std::size_t maxPoolBytes();

    static std::size_t pooledBytes();
    static std::size_t pooledBufferCount();

    /**
     * @brief Frees every pooled buffer.
     **/
    static void trim();

    /**
     * @brief Frees the oldest pooled buffers until at most @p maxBytes remain pooled.
     **/
    static void trimTo(std::size_t maxBytes);

    /**
     * @brief Registers a callback invoked, without internal locks held, just before a
     * pooled-size buffer is really returned to the OS. It is not invoked when a buffer is
     * recycled into the pool. Returns an id for removeFreeHook().
     **/
    static int addFreeHook(const FreeHook& hook);
    static void removeFreeHook(int id);
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_HOSTIMAGEMEMORY_H
