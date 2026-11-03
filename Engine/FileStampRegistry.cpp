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
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>

#include "Engine/ContentHasher.h"
#include "Engine/FileSystemModel.h"

NATRON_NAMESPACE_ENTER

namespace {
FileIdentity
statIdentity(const std::string& path)
{
    FileIdentity id;
    struct statx st;

    if (statx(AT_FDCWD, path.c_str(), 0, STATX_MTIME | STATX_SIZE | STATX_INO, &st) != 0) {
        return id;
    }
    id.exists = true;
    id.mtimeSec = (std::int64_t)st.stx_mtime.tv_sec;
    id.mtimeNsec = (std::int64_t)st.stx_mtime.tv_nsec;
    id.size = st.stx_size;
    id.inode = st.stx_ino;

    return id;
}

U64
computeStamp(const std::string& path,
             const FileIdentity& id)
{
    ContentHasher h;

    h.appendString(path);
    if (!id.exists) {
        h.appendBool(false);

        return h.finalize();
    }
    h.appendBool(true);
    h.appendI64(id.mtimeSec);
    h.appendI64(id.mtimeNsec);
    h.appendU64(id.size);
    h.appendU64(id.inode);

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

struct SequenceMember {
    int frame;
    int view;
    std::string path;
    FileIdentity identity;
};

std::vector<SequenceMember>
listSequence(const std::string& pattern,
             std::atomic<std::uint64_t>* diskAccesses)
{
    std::map<int, std::map<int, std::string>> sequence;
    std::vector<SequenceMember> members;

    diskAccesses->fetch_add(1, std::memory_order_relaxed);
    FileSystemModel::filesListFromPattern(pattern, &sequence);
    for (std::map<int, std::map<int, std::string>>::const_iterator frame = sequence.begin(); frame != sequence.end(); ++frame) {
        for (std::map<int, std::string>::const_iterator view = frame->second.begin(); view != frame->second.end(); ++view) {
            diskAccesses->fetch_add(1, std::memory_order_relaxed);
            SequenceMember member;
            member.frame = frame->first;
            member.view = view->first;
            member.path = view->second;
            member.identity = statIdentity(view->second);
            members.push_back(member);
        }
    }

    return members;
}

// Members are in frame then view order; each one's stamp is that of its recorded identity.
U64
computeSequenceStamp(const std::string& pattern,
                     const std::vector<SequenceMember>& members,
                     const std::vector<U64>& memberStamps)
{
    ContentHasher h;
    std::size_t frames = 0;

    for (std::size_t i = 0; i < members.size(); ++i) {
        if ((i == 0) || (members[i].frame != members[i - 1].frame)) {
            ++frames;
        }
    }
    h.appendString(pattern);
    h.appendU64(frames);
    for (std::size_t i = 0; i < members.size();) {
        std::size_t end = i;
        while ((end < members.size()) && (members[end].frame == members[i].frame)) {
            ++end;
        }
        h.appendI64(members[i].frame);
        h.appendU64(end - i);
        for (; i < end; ++i) {
            h.appendI64(members[i].view);
            h.appendU64(memberStamps[i]);
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

FileStampRegistry::Entry
FileStampRegistry::entry(const std::string& path)
{
    std::uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> k(_mutex);
        std::map<std::string, Entry>::const_iterator it = _entries.find(path);
        if (it != _entries.end()) {
            return it->second;
        }
        generation = _generation;
    }

    // The stat runs unlocked so a slow filesystem does not serialize every other path.
    _diskAccesses.fetch_add(1, std::memory_order_relaxed);
    Entry entry;
    entry.identity = statIdentity(path);
    entry.stamp = computeStamp(path, entry.identity);
    {
        std::lock_guard<std::mutex> k(_mutex);
        if (generation == _generation) {
            // Another thread may have recorded it meanwhile; the first record is the one kept.
            return _entries.emplace(path, entry).first->second;
        }
    }

    return entry;
}

U64
FileStampRegistry::stamp(const std::string& path)
{
    return entry(path).stamp;
}

FileIdentity
FileStampRegistry::identity(const std::string& path)
{
    return entry(path).identity;
}

bool
FileStampRegistry::matchesDisk(const std::string& path,
                               FileIdentity* recorded)
{
    const FileIdentity pinned = identity(path);

    if (recorded) {
        *recorded = pinned;
    }

    return statIdentity(path) == pinned;
}

FileIdentity
FileStampRegistry::statNow(const std::string& path)
{
    return statIdentity(path);
}

std::string
FileStampRegistry::changedOnDiskMessage(const std::string& path)
{
    return path + " changed on disk since it was loaded, Reload to read it";
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

    const std::vector<SequenceMember> members = listSequence(pattern, &_diskAccesses);
    std::vector<U64> memberStamps(members.size());
    SequenceEntry entry;
    entry.directory = parentDirectory(pattern);
    {
        std::lock_guard<std::mutex> k(_mutex);
        const bool keep = (generation == _generation);
        for (std::size_t i = 0; i < members.size(); ++i) {
            Entry member;
            member.identity = members[i].identity;
            member.stamp = computeStamp(members[i].path, member.identity);
            // A member stamped on its own before keeps that record, so both stamps agree on it.
            memberStamps[i] = keep ? _entries.emplace(members[i].path, member).first->second.stamp : member.stamp;
        }
        entry.stamp = computeSequenceStamp(pattern, members, memberStamps);
        if (keep) {
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
    for (std::map<std::string, Entry>::iterator it = _entries.begin(); it != _entries.end();) {
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
FileStampRegistry::coversSameFiles(const std::string& a,
                                   const std::string& b)
{
    if (a == b) {
        return true;
    }

    return (hasFramePattern(a) || hasFramePattern(b)) && (parentDirectory(a) == parentDirectory(b));
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
