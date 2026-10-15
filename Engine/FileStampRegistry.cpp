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

NATRON_NAMESPACE_ENTER

namespace {
const std::chrono::milliseconds kStampTTL(1000);

U64
computeStamp(const std::string& path)
{
    ContentHasher h;

    h.appendString(path);
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
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> k(_mutex);
        std::map<std::string, Entry>::const_iterator it = _entries.find(path);
        if ((it != _entries.end()) && (now - it->second.computedAt < kStampTTL)) {
            return it->second.stamp;
        }
    }

    // The stat runs unlocked so a slow filesystem does not serialize every other path.
    Entry entry;
    entry.stamp = computeStamp(path);
    entry.computedAt = now;
    {
        std::lock_guard<std::mutex> k(_mutex);
        _entries[path] = entry;
    }

    return entry.stamp;
}

void
FileStampRegistry::invalidate(const std::string& path)
{
    std::lock_guard<std::mutex> k(_mutex);

    _entries.erase(path);
}

void
FileStampRegistry::invalidateAll()
{
    std::lock_guard<std::mutex> k(_mutex);

    _entries.clear();
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
