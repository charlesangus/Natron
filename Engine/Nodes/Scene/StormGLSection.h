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

#ifndef Engine_Nodes_Scene_StormGLSection_h
#define Engine_Nodes_Scene_StormGLSection_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstdint>

#include "Engine/Nodes/Scene/UsdIncludes.h"

PXR_NAMESPACE_OPEN_SCOPE
class Hgi;
PXR_NAMESPACE_CLOSE_SCOPE

NATRON_NAMESPACE_ENTER

/**
 * @brief The process's one section of Storm work on HgiGL: what a thread holds while it builds,
 * renders with, reads from or destroys a UsdImagingGLEngine drawing through HgiGL.
 *
 * Every HgiGL of a process shares one garbage collector (hgiGL/garbageCollector.h). The handles
 * any of them destroys wait in per-thread lists that are static, and whichever HgiGL collects next,
 * at the end of a frame or after a submit outside one, deletes what all those lists hold, on its
 * own thread, with its own context current and without a lock. Natron draws through HgiGL on two
 * threads, the scene view's engines on the main thread and USDRender's on its render thread. Left
 * alone, one thread collects while the other destroys: the lists race, and one thread deletes the
 * other's buffers, textures and vertex arrays in its own context, before the copies and draws the
 * other already recorded from them have run.
 *
 * Sections are serialized across threads and leave no garbage behind them: the outermost section
 * of a thread collects with the Hgi given to collectWith(), while the thread's context is still
 * current, before another thread may enter. A collection then only ever finds the handles of the
 * thread running it. Sections nest on one thread. Never wait on another thread inside one.
 **/
class StormGLSection {
public:
    StormGLSection();

    ~StormGLSection();

    /**
     * @brief The Hgi whose garbage the thread's outermost section collects when it ends, with the
     * context that Hgi draws in current. Null, the default, collects nothing, for a section that
     * destroys the Hgi itself (its destructor collects) or makes no Storm object go.
     **/
    void collectWith(PXR_NS::Hgi* hgi);

    // Whether the calling thread is inside a section.
    static bool isHeldOnThisThread() WARN_UNUSED_RETURN;

    // How many outermost sections have been entered in this process.
    static std::uint64_t getSectionCount() WARN_UNUSED_RETURN;

    // How many outermost sections ended collecting with an Hgi.
    static std::uint64_t getCollectionCount() WARN_UNUSED_RETURN;

private:
    StormGLSection(const StormGLSection&) = delete;
    StormGLSection& operator=(const StormGLSection&) = delete;
};

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_StormGLSection_h
