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

#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include "Global/GlobalDefines.h"

NATRON_NAMESPACE_ENTER

/**
 * @brief What a stat of a file says about it: whether it exists, and its modification time (ns),
 * size and inode. Two identities are equal when the file was not rewritten or replaced between
 * the two stats.
 **/
struct FileIdentity {
    bool exists = false;
    std::int64_t mtimeSec = 0;
    std::int64_t mtimeNsec = 0;
    std::uint64_t size = 0;
    std::uint64_t inode = 0;

    bool operator==(const FileIdentity& other) const
    {
        return (exists == other.exists) && (mtimeSec == other.mtimeSec) && (mtimeNsec == other.mtimeNsec) && (size == other.size) && (inode == other.inode);
    }

    bool operator!=(const FileIdentity& other) const
    {
        return !(*this == other);
    }
};

/**
 * @brief Which version of a file a reader reads. eStamp pins it to the identity recorded with the
 * file's stamp, the one its node hash was taken under, and fails once the file on disk is another
 * one; only invalidating the stamp (Reload) lets the new file in. eDisk reads the file as it is on
 * disk now, for readers whose results are not keyed by the stamp.
 **/
enum class FilePin {
    eStamp,
    eDisk
};

/**
 * @brief Process-wide, thread-safe source of file identity stamps. A stamp hashes the path with
 * the file's modification time (ns), size and inode, so a file changed on disk gets a new stamp.
 * A file or sequence is looked at on disk once, the first time its stamp is asked for, and the
 * stamp is kept until invalidate() is called for it: nothing watches or stats the files again.
 * Stamping on first use rather than keeping stamps across sessions is what keys a file changed
 * while the application was closed differently in the disk cache.
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
     * @brief Identity of the file at path as recorded with its stamp: looked at on disk the first
     * time either is asked for, then kept until invalidate(). stamp() hashes this same record.
     **/
    FileIdentity identity(const std::string& path);

    /**
     * @brief True if the file at path is, on disk now, the one its recorded identity describes.
     * Records the identity first if none is. A reader pinned to the stamp fails rather than read a
     * file this rejects. The stat made here is not counted by diskAccessCount().
     **/
    bool matchesDisk(const std::string& path, FileIdentity* recorded = nullptr);

    /**
     * @brief Stats path now, recording nothing.
     **/
    static FileIdentity statNow(const std::string& path);

    /**
     * @brief The error a reader pinned to the stamp reports for a file that changed on disk.
     **/
    static std::string changedOnDiskMessage(const std::string& path);

    /**
     * @brief Stamp of the frame sequence named by pattern: the stamps of every file on disk that
     * matches it, in frame then view order. Records the identity of each of those files as
     * identity() would. Never 0.
     **/
    U64 sequenceStamp(const std::string& pattern);

    /**
     * @brief Drops the stamp and identity of the file or sequence pattern at path, so that the
     * next request looks at the disk again. Also drops every sequence in the directory holding path and, for a
     * pattern, every single file in that directory, since those stamps cover the same files.
     **/
    void invalidate(const std::string& path);

    /**
     * @brief True if path contains a frame-number or view pattern (#, %04d, %v, %V) and so
     * names a sequence rather than a single file.
     **/
    static bool hasFramePattern(const std::string& path);

    /**
     * @brief True if invalidating either path drops the stamp of the other: the same path, or a
     * sequence pattern and any path in the same directory.
     **/
    static bool coversSameFiles(const std::string& a, const std::string& b);

    /**
     * @brief Number of stat calls and directory scans made so far to stamp files, for tests
     * counting disk accesses.
     **/
    std::uint64_t diskAccessCount() const;

private:
    FileStampRegistry() { }

    struct Entry {
        FileIdentity identity;
        U64 stamp;
    };

    struct SequenceEntry {
        U64 stamp;
        std::string directory;
    };

    Entry entry(const std::string& path);

    std::mutex _mutex;
    std::map<std::string, Entry> _entries;
    std::map<std::string, SequenceEntry> _sequences;
    // Bumped by every invalidation, so a stat or scan that raced one is not kept.
    std::uint64_t _generation = 0;
    std::atomic<std::uint64_t> _diskAccesses { 0 };
};

NATRON_NAMESPACE_EXIT

#endif // NATRON_ENGINE_FILESTAMPREGISTRY_H
