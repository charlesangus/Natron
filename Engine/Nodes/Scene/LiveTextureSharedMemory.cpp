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

#include "LiveTextureSharedMemory.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <tbb/task_arena.h>

#include "Engine/ContentHasher.h"
#include "Engine/Nodes/Scene/LiveTextureOiio/LiveTextureFile.h"

NATRON_NAMESPACE_ENTER

namespace LiveTextureSharedMemory {
namespace {
    const char* const kDirectory = "/dev/shm";
    const char* const kPrefix = "natron-";
    const char* const kLockSuffix = ".lock";
    const char* const kTemporarySuffix = ".tmp";
    const std::size_t kHexDigits = 16;

    std::string
    hex64(U64 value)
    {
        char text[kHexDigits + 1];

        std::snprintf(text, sizeof(text), "%016llx", (unsigned long long)value);

        return text;
    }

    std::string
    inDirectory(const std::string& name)
    {
        return std::string(kDirectory) + "/" + name;
    }

    bool
    isHex(const std::string& text,
          std::size_t start)
    {
        if (text.size() < start + kHexDigits) {
            return false;
        }

        return std::all_of(text.begin() + start, text.begin() + start + kHexDigits, [](unsigned char c) { return std::isdigit(c) || ((c >= 'a') && (c <= 'f')); });
    }

    /*
     * Splits natron-<session>.lock or natron-<session>-<hash>.natron[.tmp]. Anything else in a directory
     * every program on the machine shares is not ours to touch, whatever its prefix.
     */
    bool
    parseName(const std::string& name,
              std::string* session,
              bool* isLock)
    {
        const std::size_t start = std::strlen(kPrefix);

        if ((name.compare(0, start, kPrefix) != 0) || !isHex(name, start)) {
            return false;
        }
        *session = name.substr(start, kHexDigits);
        const std::string rest = name.substr(start + kHexDigits);
        *isLock = (rest == kLockSuffix);
        if (*isLock) {
            return true;
        }
        const std::string extension = std::string(".") + NatronLiveTexture::kFileExtension;
        if ((rest.size() < 1 + kHexDigits) || (rest[0] != '-') || !isHex(rest, 1)) {
            return false;
        }
        const std::string tail = rest.substr(1 + kHexDigits);

        return (tail == extension) || (tail == extension + kTemporarySuffix);
    }

    bool
    removeIfOurs(const std::string& name)
    {
        const std::string path = inDirectory(name);
        struct stat info;

        if ((::lstat(path.c_str(), &info) != 0) || (info.st_uid != ::geteuid()) || !S_ISREG(info.st_mode)) {
            return false;
        }

        return ::unlink(path.c_str()) == 0;
    }

    class Session {
    public:
        Session()
            : _owner(::getpid())
        {
        }

        ~Session()
        {
            end();
        }

        bool ensure(std::string* name,
                    int* errorNumber)
        {
            std::lock_guard<std::mutex> lock(_mutex);

            if (_owner != ::getpid()) {
                // A forked child shares the parent's open file description, so it would hold the parent's
                // lock and could only release it by closing a descriptor it does not own.
                _owner = ::getpid();
                _name.clear();
                _lockFd = -1;
            }
            if (_name.empty() && !create(errorNumber)) {
                return false;
            }
            *name = _name;

            return true;
        }

        std::string name()
        {
            std::lock_guard<std::mutex> lock(_mutex);

            return _owner == ::getpid() ? _name : std::string();
        }

        void end()
        {
            std::lock_guard<std::mutex> lock(_mutex);

            // A forked child leaving through exit() must not remove what its parent still serves.
            if (_name.empty() || (_owner != ::getpid())) {
                return;
            }
            removeSessionFiles(_name);
            removeIfOurs(kPrefix + _name + kLockSuffix);
            ::close(_lockFd);
            _lockFd = -1;
            _name.clear();
        }

    private:
        static void removeSessionFiles(const std::string& session)
        {
            DIR* dir = ::opendir(kDirectory);

            if (!dir) {
                return;
            }
            std::vector<std::string> names;
            while (const dirent* entry = ::readdir(dir)) {
                std::string owner;
                bool isLock = false;
                if (parseName(entry->d_name, &owner, &isLock) && !isLock && (owner == session)) {
                    names.push_back(entry->d_name);
                }
            }
            ::closedir(dir);
            for (const std::string& name : names) {
                removeIfOurs(name);
            }
        }

        bool create(int* errorNumber)
        {
            std::random_device random;

            for (int attempt = 0; attempt < 8; ++attempt) {
                const std::string name = hex64((U64(random()) << 32) ^ U64(random()));
                const std::string path = inDirectory(kPrefix + name + kLockSuffix);
                const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
                if (fd < 0) {
                    if (errno == EEXIST) {
                        continue;
                    }
                    *errorNumber = errno;

                    return false;
                }
                // Another start-up scanning the directory can take the lock in the moment before this
                // does and remove the file as a dead session's: only a lock on a linked file counts.
                struct stat info;
                if ((::flock(fd, LOCK_EX | LOCK_NB) != 0) || (::fstat(fd, &info) != 0) || (info.st_nlink == 0)) {
                    ::close(fd);
                    continue;
                }
                _name = name;
                _lockFd = fd;

                return true;
            }
            *errorNumber = EEXIST;

            return false;
        }

        std::mutex _mutex;
        std::string _name;
        int _lockFd = -1;
        pid_t _owner;
    };

    Session&
    session()
    {
        static Session instance;

        return instance;
    }

    struct Footprint {
        int first;
        int count;
        float weights[3];
    };

    /*
     * The source texels under each destination texel, weighted by overlap: a level whose size is odd
     * halves to more than two texels' worth, and the plain two-by-two average would shift the image by up
     * to half a texel at each such level.
     */
    std::vector<Footprint>
    makeFootprints(int from,
                   int to)
    {
        std::vector<Footprint> footprints((std::size_t)to);

        for (int i = 0; i < to; ++i) {
            const double begin = double(i) * from / to;
            const double end = double(i + 1) * from / to;
            Footprint& footprint = footprints[(std::size_t)i];
            footprint.first = (int)std::floor(begin);
            footprint.count = 0;
            for (int source = footprint.first; (source < from) && (double(source) < end) && (footprint.count < 3); ++source) {
                const double overlap = std::min(end, double(source + 1)) - std::max(begin, double(source));
                footprint.weights[footprint.count++] = float(overlap / (end - begin));
            }
        }

        return footprints;
    }

    // Shared by the threads of one export: once the caller's predicate says so, every one stops.
    class Abort {
    public:
        explicit Abort(const std::function<bool()>& aborted)
            : _aborted(aborted)
        {
        }

        bool check()
        {
            if (_stopped.load(std::memory_order_relaxed)) {
                return true;
            }
            if (_aborted && _aborted()) {
                _stopped.store(true, std::memory_order_relaxed);

                return true;
            }

            return false;
        }

        bool stopped() const
        {
            return _stopped.load(std::memory_order_relaxed);
        }

    private:
        const std::function<bool()>& _aborted;
        std::atomic<bool> _stopped { false };
    };

    /*
     * The colour is straight, so averaging it as it is would give a half-transparent texel's colour as
     * much weight as an opaque one's, and an edge would take on the colour of what is barely there.
     * Where nothing above has an alpha the colour is the straight average, which keeps the colour the
     * store spread into transparent texels for filtering across edges.
     */
    void
    downsample(const float* source,
               int sourceWidth,
               int sourceHeight,
               float* destination,
               int width,
               int height,
               Abort& abort)
    {
        const std::vector<Footprint> columns = makeFootprints(sourceWidth, width);
        const std::vector<Footprint> rows = makeFootprints(sourceHeight, height);

        tbb::parallel_for(tbb::blocked_range<int>(0, height), [&](const tbb::blocked_range<int>& range) {
            if (abort.check()) {
                return;
            }
            for (int y = range.begin(); y != range.end(); ++y) {
                const Footprint& row = rows[(std::size_t)y];
                float* out = destination + std::size_t(y) * std::size_t(width) * 4;
                for (int x = 0; x < width; ++x, out += 4) {
                    const Footprint& column = columns[(std::size_t)x];
                    float weighted[3] = { 0.f, 0.f, 0.f };
                    float straight[3] = { 0.f, 0.f, 0.f };
                    float alpha = 0.f;
                    for (int j = 0; j < row.count; ++j) {
                        const float* texel = source + (std::size_t(row.first + j) * std::size_t(sourceWidth) + std::size_t(column.first)) * 4;
                        for (int i = 0; i < column.count; ++i, texel += 4) {
                            const float weight = row.weights[j] * column.weights[i];
                            const float covered = texel[3] * weight;
                            for (int c = 0; c < 3; ++c) {
                                weighted[c] += texel[c] * covered;
                                straight[c] += texel[c] * weight;
                            }
                            alpha += covered;
                        }
                    }
                    // Below FLT_MIN the weighted sums can have flushed to zero while the alpha has not.
                    for (int c = 0; c < 3; ++c) {
                        out[c] = (alpha > FLT_MIN) ? weighted[c] / alpha : straight[c];
                    }
                    out[3] = alpha;
                }
            }
        });
    }

    bool
    writeImage(char* bytes,
               int width,
               int height,
               const float* pixels,
               Abort& abort)
    {
        NatronLiveTexture::writeHeader(bytes, width, height);
        const std::size_t rowBytes = std::size_t(width) * NatronLiveTexture::kTexelBytes;
        char* first = bytes + NatronLiveTexture::kHeaderSize;

        // The caller can hold a once-per-texture wait that another Hydra task on this thread's arena
        // would then wait on too, if TBB let this thread steal it while waiting for these tasks.
        tbb::this_task_arena::isolate([&]() {
            tbb::parallel_for(tbb::blocked_range<int>(0, height), [&](const tbb::blocked_range<int>& range) {
                if (abort.check()) {
                    return;
                }
                std::memcpy(first + std::size_t(range.begin()) * rowBytes,
                            reinterpret_cast<const char*>(pixels) + std::size_t(range.begin()) * rowBytes,
                            std::size_t(range.end() - range.begin()) * rowBytes);
            });
            const int count = NatronLiveTexture::levelCount(width, height);
            for (int index = 1; (index < count) && !abort.check(); ++index) {
                const NatronLiveTexture::Level above = NatronLiveTexture::levelAt(width, height, index - 1);
                const NatronLiveTexture::Level level = NatronLiveTexture::levelAt(width, height, index);
                downsample(reinterpret_cast<const float*>(bytes + above.offset), above.width, above.height,
                           reinterpret_cast<float*>(bytes + level.offset), level.width, level.height, abort);
            }
        });

        return !abort.stopped();
    }

    // The file an export writes before it is renamed into place: removed, and unmapped, on every way
    // out but publish().
    class TemporaryFile {
    public:
        explicit TemporaryFile(const std::string& path)
            : _path(path)
            , _fd(::open(path.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600))
        {
        }

        ~TemporaryFile()
        {
            const int saved = errno;

            if (_bytes) {
                ::munmap(_bytes, _size);
            }
            if (_fd >= 0) {
                ::close(_fd);
                if (!_published) {
                    ::unlink(_path.c_str());
                }
            }
            errno = saved;
        }

        TemporaryFile(const TemporaryFile&) = delete;
        TemporaryFile& operator=(const TemporaryFile&) = delete;

        int fd() const
        {
            return _fd;
        }

        char* bytes() const
        {
            return _bytes;
        }

        bool map(std::size_t size)
        {
            void* mapped = ::mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, _fd, 0);

            if (mapped == MAP_FAILED) {
                return false;
            }
            _bytes = static_cast<char*>(mapped);
            _size = size;

            return true;
        }

        // Once renamed into place: the mapping is the caller's from then on.
        char* publish()
        {
            char* bytes = _bytes;

            _bytes = nullptr;
            _published = true;

            return bytes;
        }

    private:
        const std::string _path;
        const int _fd;
        char* _bytes = nullptr;
        std::size_t _size = 0;
        bool _published = false;
    };

    std::atomic<long long> limitedBytes(-1);

    int
    allocate(int fd,
             std::size_t size)
    {
        int result;

        do {
            result = ::posix_fallocate(fd, 0, (off_t)size);
        } while (result == EINTR);

        return result;
    }

    // The URI can be exported again under the same name once its entry is evicted, while a delegate
    // still holds this one: a later export's file must survive this one's release.
    void
    releaseExport(const char* bytes,
                  std::size_t size,
                  const std::string& path,
                  ino_t inode,
                  pid_t owner)
    {
        ::munmap(const_cast<char*>(bytes), size);
        if (owner != ::getpid()) {
            return;
        }
        struct stat info;
        if ((::stat(path.c_str(), &info) == 0) && (info.st_ino == inode)) {
            ::unlink(path.c_str());
        }
    }
} // anonymous namespace

bool
exportImage(const std::string& uri,
            int width,
            int height,
            const float* pixels,
            Export* result,
            int* errorNumber,
            std::string* what,
            const std::function<bool()>& aborted)
{
    std::string name;

    if (!session().ensure(&name, errorNumber)) {
        *what = std::string("cannot start a session in ") + kDirectory;

        return false;
    }
    ContentHasher hasher;
    hasher.appendString(uri);
    const std::string path = inDirectory(kPrefix + name + "-" + hex64(hasher.finalize()) + "." + NatronLiveTexture::kFileExtension);
    const std::string temporary = path + kTemporarySuffix;
    const auto fail = [&](const char* doing) {
        *errorNumber = errno;
        *what = std::string(doing) + " " + temporary;

        return false;
    };

    TemporaryFile file(temporary);
    if (file.fd() < 0) {
        return fail("cannot create");
    }
    const std::size_t size = NatronLiveTexture::fileSize(width, height);
    if ((limitedBytes.load() >= 0) && (size > std::size_t(limitedBytes.load()))) {
        errno = ENOSPC;

        return fail("cannot allocate");
    }
    // ftruncate would give a file that raises SIGBUS at the first page a full tmpfs cannot back.
    const int allocated = allocate(file.fd(), size);
    if (allocated != 0) {
        errno = allocated;

        return fail("cannot allocate");
    }
    if (!file.map(size)) {
        return fail("cannot map");
    }
    Abort abort(aborted);
    if (!writeImage(file.bytes(), width, height, pixels, abort)) {
        errno = ECANCELED;

        return fail("stopped writing");
    }
    struct stat info;
    // Both renderers check that the final name exists before opening it, so it must never name a
    // partly written file.
    if ((::mprotect(file.bytes(), size, PROT_READ) != 0) || (::fchmod(file.fd(), 0400) != 0) || (::fstat(file.fd(), &info) != 0) || (::rename(temporary.c_str(), path.c_str()) != 0)) {
        return fail("cannot publish");
    }
    const char* const bytes = file.publish();

    const ino_t inode = info.st_ino;
    const pid_t owner = ::getpid();
    result->bytes = std::shared_ptr<const char>(bytes, [size, path, inode, owner](const char* p) { releaseExport(p, size, path, inode, owner); });
    result->byteSize = size;
    result->path = path;

    return true;
} // exportImage

int
reclaimStaleSessions()
{
    DIR* dir = ::opendir(kDirectory);

    if (!dir) {
        return 0;
    }
    std::map<std::string, std::vector<std::string>> sessions;
    while (const dirent* entry = ::readdir(dir)) {
        std::string owner;
        bool isLock = false;
        if (!parseName(entry->d_name, &owner, &isLock)) {
            continue;
        }
        std::vector<std::string>& files = sessions[owner];
        if (!isLock) {
            files.push_back(entry->d_name);
        }
    }
    ::closedir(dir);

    const std::string mine = session().name();
    int removed = 0;
    for (const std::pair<const std::string, std::vector<std::string>>& found : sessions) {
        if (found.first == mine) {
            continue;
        }
        const std::string lockName = kPrefix + found.first + kLockSuffix;
        // Opened rather than taken from the scan, which can miss a lock made while it ran. A session
        // makes its lock before its first file, so files with no lock now are a dead session's.
        const int lockFd = ::open(inDirectory(lockName).c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
        if ((lockFd < 0) && (errno != ENOENT)) {
            continue;
        }
        if ((lockFd >= 0) && (::flock(lockFd, LOCK_EX | LOCK_NB) != 0)) {
            ::close(lockFd);
            continue;
        }
        for (const std::string& name : found.second) {
            removed += removeIfOurs(name) ? 1 : 0;
        }
        if (lockFd >= 0) {
            removed += removeIfOurs(lockName) ? 1 : 0;
            ::close(lockFd);
        }
    }

    return removed;
} // reclaimStaleSessions

void
endSession()
{
    session().end();
}

std::string
directory()
{
    return kDirectory;
}

std::string
sessionName()
{
    return session().name();
}

std::size_t
availableBytes()
{
    const long long limit = limitedBytes.load();
    if (limit >= 0) {
        return std::size_t(limit);
    }
    struct statvfs info;
    if (::statvfs(kDirectory, &info) != 0) {
        return 0;
    }

    return std::size_t(info.f_bavail) * std::size_t(info.f_frsize);
}

void
limitAvailableBytes(long long bytes)
{
    limitedBytes.store(bytes);
}
} // namespace LiveTextureSharedMemory

NATRON_NAMESPACE_EXIT
