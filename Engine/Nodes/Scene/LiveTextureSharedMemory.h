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

#ifndef Engine_Nodes_Scene_LiveTextureSharedMemory_h
#define Engine_Nodes_Scene_LiveTextureSharedMemory_h

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/GlobalDefines.h"
#include "Global/Macros.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

NATRON_NAMESPACE_ENTER

/**
 * @brief The files live textures are served to render delegates from, in tmpfs so nothing reaches a
 * disk: /dev/shm/natron-<session>-<hash of the URI>.natron. A session holds an flock on
 * /dev/shm/natron-<session>.lock for as long as its process lives. The kernel drops that lock however
 * the process ends and whatever its PID becomes, which is how a later session tells a crashed one from
 * a running one.
 **/
namespace LiveTextureSharedMemory {
struct Export {
    // Read-only pixels, shared with every reader of path: header, then every level. Dropping the
    // last reference unmaps them and removes the file.
    std::shared_ptr<const char> bytes;
    std::size_t byteSize = 0;
    std::string path;
};

/**
 * @brief Writes a file holding pixels and their mip chain, and maps it. False, leaving no file, with
 * *errorNumber set to the errno and *what to what failed. Does the work on the calling thread and
 * TBB's, taking no lock another export would wait on. The colour is not premultiplied, so each lower
 * level is the alpha-weighted mean of the texels above it.
 *
 * aborted, when set, is called from those threads between blocks of rows and levels: once it returns
 * true the export stops, failing with ECANCELED.
 **/
bool exportImage(const std::string& uri,
                 int width,
                 int height,
                 const float* pixels,
                 Export* result,
                 int* errorNumber,
                 std::string* what,
                 const std::function<bool()>& aborted = std::function<bool()>());

/**
 * @brief Removes the files of sessions whose lock nobody holds. Returns how many it removed. Files
 * another user owns, and names not of this form, are left alone.
 **/
int reclaimStaleSessions();

/**
 * @brief Removes every file of this process's session and drops its lock; exporting afterwards
 * starts another session. Mappings already made stay readable. Static destruction calls this at
 * normal exit.
 **/
void endSession();

/**
 * @brief The directory the files are in, and the current session's name, empty before its first
 * export.
 **/
std::string directory();
std::string sessionName();

/**
 * @brief The bytes the directory can still take, which an export larger than this cannot be
 * written into.
 **/
std::size_t availableBytes();

/**
 * @brief Makes availableBytes() report bytes, and every export larger than that fail with ENOSPC,
 * in place of what the directory has; limitAvailableBytes(-1) goes back to the directory's. For
 * tests, which cannot fill a real mount.
 **/
void limitAvailableBytes(long long bytes);
} // namespace LiveTextureSharedMemory

NATRON_NAMESPACE_EXIT

#endif // Engine_Nodes_Scene_LiveTextureSharedMemory_h
