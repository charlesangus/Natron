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

#include "FileStampRegistry.h"

#include <cstddef>
#include <cstdint>

#include <fcntl.h>
#include <sys/stat.h>

#include "Engine/ContentHasher.h"
#include "Engine/FileSystemModel.h"

NATRON_NAMESPACE_ENTER

namespace {
U64
computeStamp(const std::string& path,
             std::atomic<std::uint64_t>* diskAccesses)
{
    ContentHasher h;

    h.appendString(path);
    diskAccesses->fetch_add(1, std::memory_order_relaxed);
    struct statx st;
    if (statx(AT_FDCWD, path.c_str(), 0, STATX_MTIME | STATX_SIZE | STATX_INO, &st) != 0) {
        h.appendBool(false);

        return h.finalize();
    }
    h.appendBool(true);
    h.appendI64((std::int64_t)st.stx_mtime.tv_sec);
    h.appendI64((std::int64_t)st.stx_mtime.tv_nsec);
    h.appendU64(st.stx_size);
    h.appendU64(st.stx_ino);

    return h.finalize();
}

std::string
parentDirectory(const std::string& path)
{
    const std::size_t slash = path.find_last_of('/');

    if (slash == std::string::npos) {
        return std::string();
    }
    if (slash == 0) {
        return std::string("/");
    }

    return path.substr(0, slash);
}

U64
computeSequenceStamp(const std::string& pattern,
                     std::atomic<std::uint64_t>* diskAccesses)
{
    ContentHasher h;
    std::map<int, std::map<int, std::string>> sequence;

    diskAccesses->fetch_add(1, std::memory_order_relaxed);
    FileSystemModel::filesListFromPattern(pattern, &sequence);
    h.appendString(pattern);
    h.appendU64(sequence.size());
    for (std::map<int, std::map<int, std::string>>::const_iterator frame = sequence.begin(); frame != sequence.end(); ++frame) {
        h.appendI64(frame->first);
        h.appendU64(frame->second.size());
        for (std::map<int, std::string>::const_iterator view = frame->second.begin(); view != frame->second.end(); ++view) {
            h.appendI64(view->first);
            h.appendU64(computeStamp(view->second, diskAccesses));
        }
    }

    return h.finalize();
}
} // namespace

FileStampRegistry&
FileStampRegistry::instance()
{
    static FileStampRegistry registry;

    return registry;
}

U64
FileStampRegistry::stamp(const std::string& path)
{
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> k(_mutex);
        std::map<std::string, U64>::const_iterator it = _entries.find(path);
        if (it != _entries.end()) {
            return it->second;
        }
        generation = _generation;
    }

    // The stat runs unlocked so a slow filesystem does not serialize every other path.
    const U64 stamp = computeStamp(path, &_diskAccesses);
    {
        std::lock_guard<std::mutex> k(_mutex);
        if (generation == _generation) {
            _entries[path] = stamp;
        }
    }

    return stamp;
}

U64
FileStampRegistry::sequenceStamp(const std::string& pattern)
{
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> k(_mutex);
        std::map<std::string, SequenceEntry>::const_iterator it = _sequences.find(pattern);
        if (it != _sequences.end()) {
            return it->second.stamp;
        }
        generation = _generation;
    }

    SequenceEntry entry;
    entry.stamp = computeSequenceStamp(pattern, &_diskAccesses);
    entry.directory = parentDirectory(pattern);
    {
        std::lock_guard<std::mutex> k(_mutex);
        if (generation == _generation) {
            _sequences[pattern] = entry;
        }
    }

    return entry.stamp;
}

void
FileStampRegistry::invalidate(const std::string& path)
{
    const std::string directory = parentDirectory(path);
    const bool isPattern = hasFramePattern(path);
    std::lock_guard<std::mutex> k(_mutex);

    ++_generation;
    _entries.erase(path);
    _sequences.erase(path);
    for (std::map<std::string, SequenceEntry>::iterator it = _sequences.begin(); it != _sequences.end();) {
        if (it->second.directory == directory) {
            it = _sequences.erase(it);
        } else {
            ++it;
        }
    }
    if (!isPattern) {
        return;
    }
    for (std::map<std::string, U64>::iterator it = _entries.begin(); it != _entries.end();) {
        if (parentDirectory(it->first) == directory) {
            it = _entries.erase(it);
        } else {
            ++it;
        }
    }
}

std::uint64_t
FileStampRegistry::diskAccessCount() const
{
    return _diskAccesses.load(std::memory_order_relaxed);
}

bool
FileStampRegistry::hasFramePattern(const std::string& path)
{
    for (std::size_t i = 0; i < path.size(); ++i) {
        const char c = path[i];
        if (c == '#') {
            return true;
        }
        if (c != '%') {
            continue;
        }
        std::size_t j = i + 1;
        while ((j < path.size()) && (path[j] >= '0') && (path[j] <= '9')) {
            ++j;
        }
        if ((j < path.size()) && ((path[j] == 'd') || (path[j] == 'v') || (path[j] == 'V'))) {
            return true;
        }
    }

    return false;
}

NATRON_NAMESPACE_EXIT
