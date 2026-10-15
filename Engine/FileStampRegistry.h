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

#ifndef NATRON_ENGINE_FILESTAMPREGISTRY_H
#define NATRON_ENGINE_FILESTAMPREGISTRY_H

// ***** BEGIN PYTHON BLOCK *****
// from <https://docs.python.org/3/c-api/intro.html#include-files>:
// "Since Python may define some pre-processor definitions which affect the standard headers on some systems, you must include Python.h before any standard headers are included."
#include <Python.h>
// ***** END PYTHON BLOCK *****

#include "Global/Macros.h"

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>

#include "Global/GlobalDefines.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief Process-wide, thread-safe source of file identity stamps. A stamp hashes the path with
 * the file's modification time (ns), size and inode, so a file changed on disk gets a new stamp.
 * Stamps are memoized per path for a short time to keep repeated hashing from stat'ing again.
 * Sequence stamps are memoized until invalidated, either explicitly or by a file system watcher
 * on the sequence's directory that lives on the application's main thread.
 **/
class FileStampRegistry {
public:
    static FileStampRegistry& instance();

    /**
     * @brief Identity stamp of the file at path. A missing file yields a stamp that is stable
     * and distinct per path. Never 0.
     **/
    U64 stamp(const std::string& path);

    /**
     * @brief Stamp of the frame sequence named by pattern: the stamps of every file on disk that
     * matches it, in frame then view order. Never 0.
     **/
    U64 sequenceStamp(const std::string& pattern);

    /**
     * @brief Drops the memo of the file or sequence pattern at path, and of every sequence in
     * the directory holding it.
     **/
    void invalidate(const std::string& path);

    /**
     * @brief Drops the memo of every file and sequence directly inside directory.
     **/
    void invalidateDirectory(const std::string& directory);

    void invalidateAll();

    /**
     * @brief True if path contains a frame-number or view pattern (#, %04d, %v, %V) and so
     * names a sequence rather than a single file.
     **/
    static bool hasFramePattern(const std::string& path);

private:
    FileStampRegistry() { }

    struct Entry {
        U64 stamp;
        std::chrono::steady_clock::time_point computedAt;
    };

    struct SequenceEntry {
        U64 stamp;
        std::string directory;
    };

    void watchDirectory(const std::string& directory);
    void addWatchOnMainThread(const std::string& directory);

    std::mutex _mutex;
    std::map<std::string, Entry> _entries;
    std::map<std::string, SequenceEntry> _sequences;
    std::set<std::string> _watchedDirectories;
    // Bumped by every invalidation, so a scan that raced one is not memoized.
    std::uint64_t _generation = 0;
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_FILESTAMPREGISTRY_H
