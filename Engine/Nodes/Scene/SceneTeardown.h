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

#ifndef Engine_Nodes_Scene_SceneTeardown_h
#define Engine_Nodes_Scene_SceneTeardown_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <cstddef>
#include <memory>
#include <utility>

NATRON_NAMESPACE_ENTER

/**
 * @brief Where the last references to large scene state go when the main thread lets them go:
 * closing a stage of a million prims, or the layer it read, takes seconds, which the user would
 * otherwise wait out on a frozen window after closing a project or deleting a node.
 *
 * Deferred, what is released is destroyed on one teardown thread, in release order, each under a
 * SceneWriteBarrier::ReadScope so that no in-place layer reload runs into a stage closing. Not
 * deferred, or once shut down, release() destroys at once on the calling thread, which is what a
 * background process and the tests get: there nothing waits on the main thread, and a project
 * reset must have let go of the files it read before the next one opens them.
 *
 * Whatever is released must hold no reference anything else still uses on another thread, and
 * nothing may reach it through a raw pointer afterwards.
 **/
namespace SceneTeardown {
// Off until the GUI turns it on. MT-safe.
void setDeferred(bool deferred);

bool isDeferred();

void releaseHeld(std::shared_ptr<void> held);

template <typename T>
void
release(T value)
{
    releaseHeld(std::make_shared<T>(std::move(value)));
}

// What was released and is not destroyed yet, the one being destroyed included.
std::size_t getPendingCount();

/**
 * @brief Waits until everything released so far is destroyed, or timeoutMs passed (never when
 * negative). Returns whether none is pending. Not from the teardown thread.
 **/
bool waitIdle(int timeoutMs);

/**
 * @brief Destroys everything released so far, then stops the teardown thread: later releases
 * destroy at once. Before OpenUSD's statics go, as what is pending still uses them.
 **/
void shutdown();
} // namespace SceneTeardown

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_SceneTeardown_h
