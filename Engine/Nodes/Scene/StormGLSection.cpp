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

#include "StormGLSection.h"

#include <atomic>
#include <mutex>

#include <pxr/imaging/hgi/hgi.h>

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_ENTER

namespace {
std::mutex gSectionMutex;
std::atomic<std::uint64_t> gSectionCount(0);
std::atomic<std::uint64_t> gCollectionCount(0);

thread_local int tDepth = 0;
thread_local Hgi* tCollectWith = nullptr;
} // namespace

StormGLSection::StormGLSection()
{
    if (tDepth == 0) {
        gSectionMutex.lock();
        tCollectWith = nullptr;
        ++gSectionCount;
    }
    ++tDepth;
}

StormGLSection::~StormGLSection()
{
    if (--tDepth > 0) {
        return;
    }
    Hgi* const hgi = tCollectWith;
    tCollectWith = nullptr;
    if (hgi) {
        // Everything this thread destroyed since it entered goes now, in its own context.
        hgi->GarbageCollect();
        ++gCollectionCount;
    }
    gSectionMutex.unlock();
}

void
StormGLSection::collectWith(Hgi* hgi)
{
    tCollectWith = hgi;
}

bool
StormGLSection::isHeldOnThisThread()
{
    return tDepth > 0;
}

std::uint64_t
StormGLSection::getSectionCount()
{
    return gSectionCount.load();
}

std::uint64_t
StormGLSection::getCollectionCount()
{
    return gCollectionCount.load();
}

NATRON_NAMESPACE_EXIT
