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

#include "CacheDiskTier.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <new>
#include <random>
#include <set>
#include <sstream>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <QtCore/QDebug>

#include "Global/GlobalDefines.h"

#include "Engine/CacheEntry.h"
#include "Engine/CacheRetention.h"
#include "Engine/ContentHasher.h"

#define XXH_INLINE_ALL
#include "xxhash.h"

NATRON_NAMESPACE_ENTER

namespace fs = std::filesystem;

namespace {

const char kMagic[8] = { 'N', 'A', 'T', 'R', 'O', 'N', 'T', 'C' };
const std::uint32_t kFormatVersion = 1;
const std::size_t kHeaderBytes = 4096;
const std::size_t kStatsBytes = 64;
const std::size_t kBitmapOffset = kHeaderBytes + kStatsBytes;

const char kFileSuffix[] = ".ntc";
const std::size_t kHexDigits = 16;
const std::size_t kShardDigits = 3;
const unsigned kShards = 1u << (4 * kShardDigits);
// Odd, so that it visits every shard, and large, so that a partial walk is not one run of neighbours.
const unsigned kShardStride = 1597;
const std::size_t kNameBytes = 3 * kHexDigits + 2 + sizeof(kFileSuffix) - 1;

const double kInitialWriteBytesPerSecond = 500e6;
// A smaller write times fdatasync's latency, which says nothing about throughput.
const std::size_t kMinimumThroughputSample = 1024 * 1024;
const double kThroughputWeight = 0.2;
const double kPerFileLatencyNs = 2e6;
const double kDemotionQueueFraction = 0.1;
const double kFreeSpaceCap = 0.25;

const std::size_t kProbeStripes = 64;
const std::size_t kGateStripes = 64;
const std::size_t kMaxProbesPerStripe = 4096 / kProbeStripes;
const int kNegativeCacheMs = 1000;
const int kNetworkListingCacheMs = 2000;

const int kBudgetRefreshMs = 1000;
const int kSlowScanMs = 60000;
const int kBuildStampIntervalMs = 60000;
const std::uint64_t kDeadBuildAgeNs = 7ULL * 24ULL * 3600ULL * 1000000000ULL;
const char kBuildStampName[] = "lastlive";
const std::size_t kPublishScan = 256;
const std::size_t kPublishBatch = 8;
const int kPublishBackoffMs = 30000;
const std::size_t kMaxKnownFiles = 65536;

const std::uint64_t kLedgerMagic = 0x4e544c4544474552ULL;

struct FixedHeader {
    char magic[8];
    std::uint32_t formatVersion;
    std::uint32_t cacheVersion;
    std::uint32_t kind;
    std::uint32_t allRendered;
    std::uint32_t keyBytes;
    std::uint32_t paramsBytes;
    std::int32_t x1;
    std::int32_t y1;
    std::int32_t x2;
    std::int32_t y2;
    std::uint64_t renderedPixels;
    std::uint64_t totalPixels;
    std::uint64_t bitmapBytes;
    std::uint64_t payloadBytes;
    std::uint64_t payloadHash;
    std::uint64_t headerHash;
};

struct StatsBlock {
    std::uint64_t lastUse;
    std::uint32_t reuseCount;
    float priority;
    float recomputeCostNs;
    char reserved[44];
};

struct LedgerRecord {
    std::uint64_t magic;
    char build[32];
    std::int64_t delta;
    std::uint64_t heartbeatNs;
    std::uint64_t closed;
};

static_assert(std::is_trivially_copyable<FixedHeader>::value, "the header is stored as raw bytes");
static_assert(sizeof(StatsBlock) == kStatsBytes, "the bitmap offset depends on the stats size");
static_assert(sizeof(FixedHeader) < kHeaderBytes, "the key and params follow the fixed fields");

bool
preadFully(int fd,
           void* data,
           std::size_t bytes,
           std::uint64_t offset)
{
    char* out = static_cast<char*>(data);

    while (bytes > 0) {
        const ssize_t got = ::pread(fd, out, bytes, (off_t)offset);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }

            return false;
        }
        if (got == 0) {
            return false;
        }
        out += got;
        bytes -= (std::size_t)got;
        offset += (std::uint64_t)got;
    }

    return true;
}

bool
pwriteFully(int fd,
            const void* data,
            std::size_t bytes,
            std::uint64_t offset)
{
    const char* in = static_cast<const char*>(data);

    while (bytes > 0) {
        const ssize_t put = ::pwrite(fd, in, bytes, (off_t)offset);
        if (put < 0) {
            if (errno == EINTR) {
                continue;
            }

            return false;
        }
        in += put;
        bytes -= (std::size_t)put;
        offset += (std::uint64_t)put;
    }

    return true;
}

class ScopedFd {
public:
    explicit ScopedFd(int fd)
        : _fd(fd)
    {
    }

    ~ScopedFd()
    {
        if (_fd >= 0) {
            ::close(_fd);
        }
    }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    int get() const
    {
        return _fd;
    }

    // NFS can report a write failure only at close, which must count before the rename publishes.
    bool closeNow()
    {
        const int fd = _fd;

        _fd = -1;

        return fd < 0 || ::close(fd) == 0;
    }

private:
    int _fd;
};

std::string
hexOf(std::uint64_t value)
{
    char text[kHexDigits + 1];

    std::snprintf(text, sizeof(text), "%016" PRIx64, value);

    return std::string(text, kHexDigits);
}

bool
parseHex(const char* text,
         std::uint64_t* value)
{
    std::uint64_t result = 0;

    for (std::size_t i = 0; i < kHexDigits; ++i) {
        const char c = text[i];
        unsigned digit;
        if ((c >= '0') && (c <= '9')) {
            digit = (unsigned)(c - '0');
        } else if ((c >= 'a') && (c <= 'f')) {
            digit = 10u + (unsigned)(c - 'a');
        } else {
            return false;
        }
        result = (result << 4) | digit;
    }
    *value = result;

    return true;
}

bool
isEntryName(const std::string& name)
{
    return name.size() == kNameBytes && name[kHexDigits] == '-' && name[2 * kHexDigits + 1] == '-' && name.compare(3 * kHexDigits + 2, std::string::npos, kFileSuffix) == 0;
}

std::string
entryName(const CacheDiskIdentity& identity)
{
    return hexOf(identity.base) + '-' + hexOf(identity.variant) + '-' + hexOf(identity.paramsHash) + kFileSuffix;
}

void
sanitizeStats(const StatsBlock& block,
              CacheDiskStats* stats)
{
    stats->lastUse = block.lastUse;
    stats->reuseCount = block.reuseCount;
    // Any peer rewrites stats in place, and no checksum covers them.
    stats->priority = block.priority > 0.f && block.priority < 1e9f ? block.priority : 0.f;
    stats->recomputeCostNs = block.recomputeCostNs > 0.f && block.recomputeCostNs < 1e18f ? block.recomputeCostNs : 0.f;
}

StatsBlock
blockOf(const CacheDiskStats& stats)
{
    StatsBlock block;

    std::memset(&block, 0, sizeof(block));
    block.lastUse = stats.lastUse;
    block.reuseCount = stats.reuseCount;
    block.priority = stats.priority;
    block.recomputeCostNs = stats.recomputeCostNs;

    return block;
}

bool
parseHeader(const char* bytes,
            CacheDiskFileInfo* info)
{
    FixedHeader fixed;

    std::memcpy(&fixed, bytes, sizeof(fixed));
    if ((std::memcmp(fixed.magic, kMagic, sizeof(kMagic)) != 0) || (fixed.formatVersion != kFormatVersion) || (fixed.cacheVersion != (std::uint32_t)NATRON_CACHE_VERSION)) {
        return false;
    }
    if ((std::uint64_t)sizeof(FixedHeader) + fixed.keyBytes + fixed.paramsBytes > kHeaderBytes) {
        return false;
    }

    char hashed[kHeaderBytes];
    std::memcpy(hashed, bytes, kHeaderBytes);
    std::memset(hashed + offsetof(FixedHeader, headerHash), 0, sizeof(fixed.headerHash));
    if (XXH3_64bits(hashed, kHeaderBytes) != fixed.headerHash) {
        return false;
    }

    info->kind = fixed.kind;
    info->keyBytes.assign(bytes + sizeof(FixedHeader), fixed.keyBytes);
    info->paramsBytes.assign(bytes + sizeof(FixedHeader) + fixed.keyBytes, fixed.paramsBytes);
    info->bounds = RectI(fixed.x1, fixed.y1, fixed.x2, fixed.y2);
    info->allRendered = fixed.allRendered != 0;
    info->renderedPixels = fixed.renderedPixels;
    info->totalPixels = fixed.totalPixels;
    info->bitmapBytes = fixed.bitmapBytes;
    info->payloadBytes = fixed.payloadBytes;
    info->payloadHash = fixed.payloadHash;
    info->fileBytes = kBitmapOffset + fixed.bitmapBytes + fixed.payloadBytes;

    return true;
}

bool
readFileInfo(int fd,
             CacheDiskFileInfo* info)
{
    char bytes[kBitmapOffset];

    if (!preadFully(fd, bytes, sizeof(bytes), 0) || !parseHeader(bytes, info)) {
        return false;
    }
    struct stat st;
    if ((::fstat(fd, &st) != 0) || ((std::uint64_t)st.st_size != info->fileBytes)) {
        return false;
    }
    StatsBlock block;
    std::memcpy(&block, bytes + kHeaderBytes, sizeof(block));
    sanitizeStats(block, &info->stats);

    return true;
}

bool
readFileInfoAt(const std::string& path,
               CacheDiskFileInfo* info)
{
    ScopedFd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));

    if (fd.get() < 0) {
        return false;
    }
    info->path = path;

    return readFileInfo(fd.get(), info);
}

void
writeStatsAt(const std::string& path,
             const CacheDiskStats& stats)
{
    ScopedFd fd(::open(path.c_str(), O_WRONLY | O_CLOEXEC));

    if (fd.get() >= 0) {
        const StatsBlock block = blockOf(stats);
        (void)pwriteFully(fd.get(), &block, sizeof(block), kHeaderBytes);
    }
}

std::vector<std::string>
listDirectory(const std::string& path)
{
    std::vector<std::string> names;
    DIR* dir = ::opendir(path.c_str());

    if (!dir) {
        return names;
    }
    while (const struct dirent* item = ::readdir(dir)) {
        if ((std::strcmp(item->d_name, ".") != 0) && (std::strcmp(item->d_name, "..") != 0)) {
            names.push_back(item->d_name);
        }
    }
    ::closedir(dir);

    return names;
}

bool
isNetworkFilesystem(const std::string& path)
{
    struct statfs info;

    if (::statfs(path.c_str(), &info) != 0) {
        return false;
    }
    switch ((unsigned long)info.f_type) {
    case 0x6969UL: // NFS
    case 0x517BUL: // SMB
    case 0xFF534D42UL: // CIFS
    case 0xFE534D42UL: // SMB2
    case 0x65735546UL: // FUSE
    case 0x00C36400UL: // Ceph
    case 0x0BD00BD0UL: // Lustre
        return true;
    default:
        return false;
    }
}

std::string
hostName()
{
    char host[256] = { 0 };

    if (::gethostname(host, sizeof(host) - 1) != 0) {
        std::strcpy(host, "host");
    }
    for (char* c = host; *c; ++c) {
        if (*c == '/') {
            *c = '_';
        }
    }

    return host;
}

std::string
bootId()
{
    std::string boot;
    std::ifstream in("/proc/sys/kernel/random/boot_id");

    std::getline(in, boot);
    boot.erase(std::remove(boot.begin(), boot.end(), '-'), boot.end());

    return boot;
}

struct ProcessInfo {
    bool readable = false;
    bool zombie = false;
    std::string startTime;
};

ProcessInfo
processInfo(const std::string& pid)
{
    ProcessInfo info;
    std::ifstream in("/proc/" + pid + "/stat");
    std::string line;

    if (!std::getline(in, line)) {
        return info;
    }
    // The process name in field 2 may hold spaces, so the fields count from its closing parenthesis.
    const std::size_t close = line.rfind(')');
    if (close == std::string::npos) {
        return info;
    }
    std::istringstream fields(line.substr(close + 1));
    std::string field;
    for (int i = 3; i <= 22 && (fields >> field); ++i) {
        if (i == 3) {
            info.zombie = (field == "Z");
        } else if (i == 22) {
            info.startTime = field;
            info.readable = true;
        }
    }

    return info;
}

std::string
sessionBaseName()
{
    const ProcessInfo self = processInfo("self");
    std::ostringstream name;

    name << hostName() << '-' << bootId() << '-' << (long)::getpid() << '-' << (self.readable ? self.startTime : std::string("0"));

    return name.str();
}

struct SessionId {
    std::string host;
    std::string boot;
    std::string pid;
    std::string start;
};

// From the right, because a host name may hold dashes.
bool
parseSessionName(const std::string& name,
                 SessionId* id)
{
    const std::size_t startDash = name.rfind('-');
    if ((startDash == std::string::npos) || (startDash == 0)) {
        return false;
    }
    const std::size_t pidDash = name.rfind('-', startDash - 1);
    if ((pidDash == std::string::npos) || (pidDash == 0)) {
        return false;
    }
    const std::size_t bootDash = name.rfind('-', pidDash - 1);
    if (bootDash == std::string::npos) {
        return false;
    }
    id->host = name.substr(0, bootDash);
    id->boot = name.substr(bootDash + 1, pidDash - bootDash - 1);
    id->pid = name.substr(pidDash + 1, startDash - pidDash - 1);
    id->start = name.substr(startDash + 1, name.find('.', startDash) - startDash - 1);

    return !id->pid.empty() && (id->pid.find_first_not_of("0123456789") == std::string::npos);
}

// Stops at shutdown, and returns false then with files left: a tree under a temp directory is the
// next session's to finish, whereas waiting for it would hold up the exit.
bool
removeTree(const std::string& path,
           const std::atomic<bool>& stopping)
{
    for (const std::string& name : listDirectory(path)) {
        if (stopping.load()) {
            return false;
        }
        const std::string child = path + '/' + name;
        struct stat st;
        if ((::lstat(child.c_str(), &st) == 0) && S_ISDIR(st.st_mode)) {
            if (!removeTree(child, stopping)) {
                return false;
            }
        } else {
            (void)::unlink(child.c_str());
        }
    }
    (void)::rmdir(path.c_str());

    return true;
}

// Nothing is synced: the test is about what the filesystem does with a rename, and a crash during it
// leaves only a temp file for a peer to reap.
bool
writeSmallFile(const std::string& path,
               char value)
{
    ScopedFd fd(::open(path.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0666));

    return (fd.get() >= 0) && pwriteFully(fd.get(), &value, 1, 0);
}

bool
renameBehavesAtomically(const std::string& tempDir,
                        const std::string& publishDir,
                        const std::string& session)
{
    const std::string first = tempDir + "/self-test-a";
    const std::string second = tempDir + "/self-test-b";
    const std::string published = publishDir + "/.self-test-" + session;
    char read = 0;
    const bool worked = writeSmallFile(first, 'a')
        && (::rename(first.c_str(), published.c_str()) == 0)
        && writeSmallFile(second, 'b')
        && (::rename(second.c_str(), published.c_str()) == 0)
        && [&published, &read]() {
               ScopedFd fd(::open(published.c_str(), O_RDONLY | O_CLOEXEC));

               return (fd.get() >= 0) && preadFully(fd.get(), &read, 1, 0);
           }()
        && (read == 'b');

    (void)::unlink(first.c_str());
    (void)::unlink(second.c_str());
    (void)::unlink(published.c_str());

    return worked;
}

// Nice 19 and not SCHED_IDLE: these threads hold an entry's lock for a chunk's copy, and under a
// full load a SCHED_IDLE holder can keep a render waiting for seconds.
void
lowerThreadCpuPriority()
{
    (void)::setpriority(PRIO_PROCESS, 0, 19);
}

void
lowerThreadIoPriority()
{
    const int whoProcess = 1;
    const int classIdle = 3;
    const int classShift = 13;

    (void)::syscall(SYS_ioprio_set, whoProcess, 0, classIdle << classShift);
}

struct WriteItem {
    CacheEntryPtr entry; // set for a demotion, which owns the entry until the write ends
    std::weak_ptr<AbstractCacheEntryBase> published;
    const AbstractCacheEntryBase* raw = NULL;
    std::uint64_t hash = 0;
    std::size_t bytes = 0;
    bool reclaimed = false; // guarded by queueMutex
    std::atomic<bool> cancelled { false };
};

typedef std::shared_ptr<WriteItem> WriteItemPtr;

enum WriteOutcomeEnum {
    eWriteOutcomePublished,
    eWriteOutcomeAlreadyThere,
    eWriteOutcomeAbandoned,
    eWriteOutcomeFailed
};

class FileSink final
    : public CacheDiskSink {
public:
    FileSink(int fd,
             std::uint64_t offset,
             const WriteItem& item,
             const std::atomic<bool>& stopping)
        : _fd(fd)
        , _offset(offset)
        , _item(item)
        , _stopping(stopping)
        , _state(XXH3_createState())
        , _errno(0)
    {
        XXH3_64bits_reset(_state);
    }

    virtual ~FileSink()
    {
        XXH3_freeState(_state);
    }

    virtual bool write(const void* data,
                       std::size_t bytes) override final
    {
        if (_stopping.load() || _item.cancelled.load()) {
            return false;
        }
        if (!pwriteFully(_fd, data, bytes, _offset)) {
            _errno = errno;

            return false;
        }
        XXH3_64bits_update(_state, data, bytes);
        _offset += bytes;

        return true;
    }

    std::uint64_t offset() const
    {
        return _offset;
    }

    std::uint64_t digest() const
    {
        return XXH3_64bits_digest(_state);
    }

    int error() const
    {
        return _errno;
    }

private:
    const int _fd;
    std::uint64_t _offset;
    const WriteItem& _item;
    const std::atomic<bool>& _stopping;
    XXH3_state_t* const _state;
    int _errno;
};

class ScopedUnlock {
public:
    explicit ScopedUnlock(int fd)
        : _fd(fd)
    {
    }

    ~ScopedUnlock()
    {
        if (_fd >= 0) {
            (void)::flock(_fd, LOCK_UN);
        }
    }

    ScopedUnlock(const ScopedUnlock&) = delete;
    ScopedUnlock& operator=(const ScopedUnlock&) = delete;

private:
    const int _fd;
};

} // namespace

namespace {
struct EvictionSample {
    std::string path;
    std::string name;
    std::uint64_t base;
    std::uint64_t bytes;
    CacheDiskStats stats;
    int deadRank; // lower goes first; every live build shares the highest
    double score;
};
}

struct CacheDiskTier::Imp {
    struct ProbeEntry {
        std::chrono::steady_clock::time_point expiry;
        std::vector<std::string> names;
    };

    struct ProbeStripe {
        std::mutex mutex;
        std::unordered_map<std::uint64_t, ProbeEntry> entries;
    };

    struct GateStripe {
        std::mutex mutex;
        std::condition_variable released;
        std::unordered_set<std::uint64_t> held;
    };

    explicit Imp(const Config& c)
        : config(c)
        , sizeBytes(c.sizeBytes)
        , networkMount(false)
        , ledgerFd(-1)
        , evictFd(-1)
        , storage(NULL)
        , host(NULL)
        , stopping(false)
        , shutDown(false)
        , queuedBytes(0)
        , demoting(0)
        , publishing(0)
        , budget(c.sizeBytes)
        , used(0)
        , ledgerDelta(0)
        , scanBytes(0)
        , scanRequested(false)
        , clearCounter(0)
        , tempCounter(0)
        , throughput(kInitialWriteBytesPerSecond)
        , failures(0)
        , warnings(0)
        , published(0)
        , warned(false)
        , closing(false)
    {
    }

    const Config config;
    std::atomic<std::uint64_t> sizeBytes;
    std::string session;
    std::string buildDir;
    std::string autoDir;
    std::string sessionsDir;
    std::string tmpDir;
    std::string ledgerPath;
    bool networkMount;
    int ledgerFd;
    int evictFd;

    const CacheHostStorage* storage;
    const CacheTier* host;
    std::atomic<bool> stopping;
    std::atomic<bool> shutDown;

    // Taken under the cache's _lock, so nothing here does I/O or frees an entry while holding it.
    mutable std::mutex queueMutex;
    std::condition_variable queueCond;
    std::deque<WriteItemPtr> demotionQueue;
    std::vector<WriteItemPtr> demotionsInFlight;
    std::size_t queuedBytes;
    std::atomic<std::size_t> demoting;

    // Never taken under the cache's _lock: its holder can be a thread at the lowest priority.
    std::mutex publishMutex;
    std::condition_variable publishCond;
    std::deque<WriteItemPtr> publishQueue;
    std::vector<WriteItemPtr> publishesInFlight;
    std::atomic<std::size_t> publishing;
    std::chrono::steady_clock::time_point publishBackoffUntil;

    std::array<ProbeStripe, kProbeStripes> probes;
    std::array<GateStripe, kGateStripes> gates;

    std::atomic<std::uint64_t> budget;
    std::atomic<std::uint64_t> used;
    std::atomic<std::int64_t> ledgerDelta;

    std::mutex budgetMutex; // guards scanBytes and scanBaselines
    std::uint64_t scanBytes;
    std::map<std::string, std::int64_t> scanBaselines;

    std::mutex maintenanceMutex; // one tick at a time, between the thread and maintainNow

    std::mutex scanMutex; // guards scanRequested and clearedPaths
    std::condition_variable scanCond;
    bool scanRequested;
    std::vector<std::string> clearedPaths;
    std::atomic<std::uint64_t> clearCounter;
    std::atomic<std::uint64_t> tempCounter;

    std::mutex stopMutex;
    std::condition_variable stopCond;

    std::mutex knownMutex;
    std::unordered_map<std::string, std::weak_ptr<CacheRetentionSignals>> known;

    std::atomic<double> throughput;
    std::atomic<std::uint64_t> failures;
    std::atomic<std::uint64_t> warnings;
    std::atomic<std::uint64_t> published;
    std::atomic<bool> warned;
    bool closing; // set once every thread has been joined

    std::mutex livenessMutex;
    struct PeerBeat {
        std::uint64_t value;
        std::chrono::steady_clock::time_point changedAt;
    };
    std::map<std::string, PeerBeat> peerBeats;

    std::chrono::steady_clock::time_point buildStampedAt;
    std::atomic<std::uint64_t> reclaimCounter { 0 };

    std::vector<std::thread> threads;

    std::string shardDirOf(std::uint64_t base) const
    {
        return autoDir + '/' + hexOf(base).substr(0, kShardDigits);
    }

    std::string pathOf(const CacheDiskIdentity& identity) const
    {
        return shardDirOf(identity.base) + '/' + entryName(identity);
    }

    void warnOnce(const std::string& what)
    {
        failures.fetch_add(1);
        if (!warned.exchange(true)) {
            warnings.fetch_add(1);
            qWarning("Disk tier: %s. Entries that fail to write are dropped; this warning is shown once per session.", what.c_str());
        }
    }

    double costOf(const CacheEntryHeader& header) const
    {
        double cost = header.recomputeCostNs.load();

        if (cost <= 0.) {
            const CacheRetentionSignalsPtr retention = header.retention.load();
            if (retention) {
                cost = retention->rememberedCostNs();
            }
        }

        return cost > 0. ? cost : (double)CacheEviction::kUnmeasuredCostNs;
    }

    bool worthWriting(const AbstractCacheEntryBase& entry) const
    {
        const CacheEntryHeader* const header = entry.evictionHeader();
        const std::size_t bytes = entry.size();

        if (!header || (bytes == 0)) {
            return false;
        }
        // No later session could ever hit it.
        if (ContentHasher::isSalted(entry.getKeyTreeVersion())) {
            return false;
        }

        return costOf(*header) >= kPerFileLatencyNs + (double)bytes / throughput.load() * 1e9;
    }

    // The latency term is charged separately by the test, so it must not also slow the rate.
    void noteThroughput(std::size_t bytes,
                        double seconds)
    {
        if ((bytes < kMinimumThroughputSample) || (seconds <= 0.)) {
            return;
        }
        const double latencySeconds = kPerFileLatencyNs * 1e-9;
        const double measured = (double)bytes / std::max(seconds - latencySeconds, seconds * 0.5);
        double current = throughput.load();

        while (!throughput.compare_exchange_weak(current, current + kThroughputWeight * (measured - current))) {
        }
    }

    void forgetBase(std::uint64_t base)
    {
        ProbeStripe& stripe = probes[base % kProbeStripes];
        std::lock_guard<std::mutex> lock(stripe.mutex);

        stripe.entries.erase(base);
    }

    static std::vector<std::string> withVariant(const std::vector<std::string>& names,
                                                std::uint64_t variant)
    {
        const std::string wanted = hexOf(variant);
        std::vector<std::string> matches;

        for (const std::string& name : names) {
            if (name.compare(kHexDigits + 1, kHexDigits, wanted) == 0) {
                matches.push_back(name);
            }
        }

        return matches;
    }

    // One frame's misses probe every level, layer and depth of an output, so an absent variant is
    // answered from the cache. A local hit is listed again to see a larger result; on a network
    // mount the listing itself is what is expensive.
    std::vector<std::string> namesFor(std::uint64_t base,
                                      std::uint64_t variant)
    {
        ProbeStripe& stripe = probes[base % kProbeStripes];
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(stripe.mutex);
            const auto found = stripe.entries.find(base);
            if ((found != stripe.entries.end()) && (now < found->second.expiry)) {
                std::vector<std::string> matches = withVariant(found->second.names, variant);
                if (matches.empty() || networkMount) {
                    return matches;
                }
            }
        }

        const std::string prefix = hexOf(base) + '-';
        std::vector<std::string> names;
        for (std::string& name : listDirectory(shardDirOf(base))) {
            if (isEntryName(name) && (name.compare(0, prefix.size(), prefix) == 0)) {
                names.push_back(std::move(name));
            }
        }
        std::vector<std::string> matches = withVariant(names, variant);

        std::lock_guard<std::mutex> lock(stripe.mutex);
        if (stripe.entries.size() >= kMaxProbesPerStripe) {
            stripe.entries.clear();
        }
        ProbeEntry& entry = stripe.entries[base];
        entry.expiry = now + std::chrono::milliseconds(networkMount ? kNetworkListingCacheMs : kNegativeCacheMs);
        entry.names = std::move(names);

        return matches;
    }

    void removeFile(const std::string& path,
                    std::uint64_t base)
    {
        struct stat st;
        const bool sized = ::stat(path.c_str(), &st) == 0;

        if ((::unlink(path.c_str()) == 0) && sized) {
            ledgerDelta.fetch_sub((std::int64_t)st.st_size);
        }
        forgetBase(base);
    }

    void cancelQueued(const AbstractCacheEntryBase* raw)
    {
        std::vector<WriteItemPtr> dropped;

        if (demoting.load() > 0) {
            std::lock_guard<std::mutex> lock(queueMutex);
            for (auto it = demotionQueue.begin(); it != demotionQueue.end();) {
                if ((*it)->raw == raw) {
                    queuedBytes -= (*it)->bytes;
                    demoting.fetch_sub(1);
                    dropped.push_back(*it);
                    it = demotionQueue.erase(it);
                } else {
                    ++it;
                }
            }
            for (const WriteItemPtr& item : demotionsInFlight) {
                if (item->raw == raw) {
                    item->cancelled.store(true);
                }
            }
        }
        if (publishing.load() > 0) {
            std::lock_guard<std::mutex> lock(publishMutex);
            for (auto it = publishQueue.begin(); it != publishQueue.end();) {
                if ((*it)->raw == raw) {
                    publishing.fetch_sub(1);
                    it = publishQueue.erase(it);
                } else {
                    ++it;
                }
            }
            for (const WriteItemPtr& item : publishesInFlight) {
                if (item->raw == raw) {
                    item->cancelled.store(true);
                }
            }
        }
    }

    void cancelEverything()
    {
        std::vector<WriteItemPtr> dropped;
        {
            std::lock_guard<std::mutex> lock(queueMutex);
            for (const WriteItemPtr& item : demotionQueue) {
                queuedBytes -= item->bytes;
                demoting.fetch_sub(1);
                dropped.push_back(item);
            }
            demotionQueue.clear();
            for (const WriteItemPtr& item : demotionsInFlight) {
                item->cancelled.store(true);
            }
        }
        std::lock_guard<std::mutex> lock(publishMutex);
        publishing.fetch_sub(publishQueue.size());
        publishQueue.clear();
        for (const WriteItemPtr& item : publishesInFlight) {
            item->cancelled.store(true);
        }
    }

    void rememberFile(const std::string& name,
                      const CacheEntryHeader& header)
    {
        const CacheRetentionSignalsPtr retention = header.retention.load();

        if (!retention) {
            return;
        }
        std::lock_guard<std::mutex> lock(knownMutex);
        if (known.size() >= kMaxKnownFiles) {
            for (auto it = known.begin(); it != known.end();) {
                it = it->second.expired() ? known.erase(it) : std::next(it);
            }
            if (known.size() >= kMaxKnownFiles) {
                return;
            }
        }
        known[name] = retention;
    }

    static CacheDiskStats statsOf(const CacheEntryHeader& header)
    {
        CacheDiskStats stats;

        stats.lastUse = header.lastUse.load();
        stats.reuseCount = header.reuseCount.load(std::memory_order_relaxed);
        stats.priority = header.priority.load(std::memory_order_relaxed);
        stats.recomputeCostNs = header.recomputeCostNs.load();

        return stats;
    }

    WriteOutcomeEnum writeEntry(const AbstractCacheEntryBase& entry,
                                const WriteItem& item);

    // An exception leaving a writer thread would end the process, for want of a scratch buffer.
    WriteOutcomeEnum guardedWrite(const AbstractCacheEntryBase& entry,
                                  const WriteItem& item)
    {
        try {
            return writeEntry(entry, item);
        } catch (const std::bad_alloc&) {
            return eWriteOutcomeAbandoned;
        }
    }

    void demotionLoop();
    void publisherLoop();
    void maintenanceLoop();
    void scanLoop();

    void finishDemotion(const WriteItemPtr& item);

    void tick();
    void writeLedger();
    std::map<std::string, LedgerRecord> readLedgers() const;
    void ensureRegistered();
    void touchBuildStamp(bool force);
    bool sessionIsDead(const std::string& name, const LedgerRecord& record);
    std::set<std::string> liveBuildsOf(const std::map<std::string, LedgerRecord>& ledgers);
    std::vector<std::string> buildNames() const;
    std::uint64_t buildLastLiveNs(const std::string& build) const;
    void reapDeadSessions();
    void reclaimDeadBuilds();
    void sampleFile(const std::string& path,
                    const std::string& name,
                    std::uint64_t base,
                    int deadRank,
                    std::vector<EvictionSample>* sample) const;
    void refreshBudget(const std::map<std::string, LedgerRecord>& ledgers);
    void evictPass(const std::map<std::string, LedgerRecord>& ledgers);
    void offerPublishCandidates();
    void removeCleared();
    void scanOnce();
    bool waitStop(int ms);
};

WriteOutcomeEnum
CacheDiskTier::Imp::writeEntry(const AbstractCacheEntryBase& entry,
                               const WriteItem& item)
{
    CacheEntryHeader* const header = entry.evictionHeader();
    CacheDiskSnapshot snapshot;

    if (!header || !entry.diskSnapshot(&snapshot) || (snapshot.renderedPixels == 0)) {
        return eWriteOutcomeAbandoned;
    }
    if (!fitsHeader(snapshot.identity.keyBytes.size(), snapshot.paramsBytes.size())) {
        return eWriteOutcomeAbandoned;
    }
    if (snapshot.renderedPixels == snapshot.totalPixels) {
        snapshot.bitmapRuns.clear();
    }

    const std::string name = entryName(snapshot.identity);
    const std::string shardDir = shardDirOf(snapshot.identity.base);
    const std::string finalPath = shardDir + '/' + name;
    const CacheDiskStats stats = statsOf(*header);

    CacheDiskFileInfo existing;
    if (readFileInfoAt(finalPath, &existing) && (existing.renderedPixels >= snapshot.renderedPixels)) {
        header->residency.fetch_or(CacheEntryHeader::eResidencyDisk);
        writeStatsAt(finalPath, stats);

        return eWriteOutcomeAlreadyThere;
    }

    const std::string tempPath = tmpDir + '/' + name + '.' + std::to_string(tempCounter.fetch_add(1));
    struct RemoveTemp {
        const std::string& path;
        bool armed;
        ~RemoveTemp()
        {
            if (armed) {
                ::unlink(path.c_str());
            }
        }
    } removeTemp { tempPath, true };

    const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    ScopedFd fd(::open(tempPath.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0666));
    if (fd.get() < 0) {
        warnOnce(std::string("cannot create a temporary file: ") + std::strerror(errno));

        return eWriteOutcomeFailed;
    }

    const std::uint64_t bitmapBytes = snapshot.bitmapRuns.size() * sizeof(std::uint32_t);
    const std::uint64_t fileBytes = kBitmapOffset + bitmapBytes + snapshot.payloadBytes;

    // A sparse file would hit ENOSPC in the middle of the pixels, after the copying was paid for.
    const int allocated = ::posix_fallocate(fd.get(), 0, (off_t)fileBytes);
    if (allocated != 0) {
        warnOnce(std::string("cannot allocate a file: ") + std::strerror(allocated));

        return eWriteOutcomeFailed;
    }

    FileSink sink(fd.get(), kBitmapOffset, item, stopping);
    const bool wrote = (bitmapBytes == 0 || sink.write(snapshot.bitmapRuns.data(), (std::size_t)bitmapBytes)) && entry.diskWritePayload(snapshot, sink);
    if (sink.error() != 0) {
        warnOnce(std::string("cannot write a file: ") + std::strerror(sink.error()));

        return eWriteOutcomeFailed;
    }
    if (!wrote || (sink.offset() != fileBytes)) {
        return eWriteOutcomeAbandoned;
    }

    char headerBytes[kHeaderBytes];
    std::memset(headerBytes, 0, sizeof(headerBytes));
    FixedHeader fixed;
    std::memset(&fixed, 0, sizeof(fixed));
    std::memcpy(fixed.magic, kMagic, sizeof(kMagic));
    fixed.formatVersion = kFormatVersion;
    fixed.cacheVersion = (std::uint32_t)NATRON_CACHE_VERSION;
    fixed.kind = snapshot.identity.kind;
    fixed.allRendered = snapshot.bitmapRuns.empty() ? 1 : 0;
    fixed.keyBytes = (std::uint32_t)snapshot.identity.keyBytes.size();
    fixed.paramsBytes = (std::uint32_t)snapshot.paramsBytes.size();
    fixed.x1 = snapshot.bounds.x1;
    fixed.y1 = snapshot.bounds.y1;
    fixed.x2 = snapshot.bounds.x2;
    fixed.y2 = snapshot.bounds.y2;
    fixed.renderedPixels = snapshot.renderedPixels;
    fixed.totalPixels = snapshot.totalPixels;
    fixed.bitmapBytes = bitmapBytes;
    fixed.payloadBytes = snapshot.payloadBytes;
    fixed.payloadHash = sink.digest();
    std::memcpy(headerBytes, &fixed, sizeof(fixed));
    std::memcpy(headerBytes + sizeof(fixed), snapshot.identity.keyBytes.data(), snapshot.identity.keyBytes.size());
    std::memcpy(headerBytes + sizeof(fixed) + snapshot.identity.keyBytes.size(), snapshot.paramsBytes.data(), snapshot.paramsBytes.size());
    fixed.headerHash = XXH3_64bits(headerBytes, sizeof(headerBytes));
    std::memcpy(headerBytes, &fixed, sizeof(fixed));

    const StatsBlock statsBlock = blockOf(stats);
    if (!pwriteFully(fd.get(), headerBytes, sizeof(headerBytes), 0) || !pwriteFully(fd.get(), &statsBlock, sizeof(statsBlock), kHeaderBytes) || (::fdatasync(fd.get()) != 0) || !fd.closeNow()) {
        warnOnce(std::string("cannot finish a file: ") + std::strerror(errno));

        return eWriteOutcomeFailed;
    }
    noteThroughput((std::size_t)fileBytes, std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());

    if (item.cancelled.load() || stopping.load()) {
        return eWriteOutcomeAbandoned;
    }

    // A peer may have published while this file was being written.
    CacheDiskFileInfo latest;
    const bool hasLatest = readFileInfoAt(finalPath, &latest);
    if (hasLatest && (latest.renderedPixels >= snapshot.renderedPixels)) {
        header->residency.fetch_or(CacheEntryHeader::eResidencyDisk);

        return eWriteOutcomeAlreadyThere;
    }

    std::error_code ignored;
    fs::create_directories(shardDir, ignored);
    if (::rename(tempPath.c_str(), finalPath.c_str()) != 0) {
        warnOnce(std::string("cannot publish a file: ") + std::strerror(errno));

        return eWriteOutcomeFailed;
    }
    removeTemp.armed = false;
    ledgerDelta.fetch_add((std::int64_t)fileBytes - (hasLatest ? (std::int64_t)latest.fileBytes : (std::int64_t)0));
    published.fetch_add(1);
    forgetBase(snapshot.identity.base);
    rememberFile(name, *header);

    // In this order, and retract in the opposite: whichever thread comes second sees the other's
    // store, so a retraction racing the rename cannot leave the file behind.
    header->residency.fetch_or(CacheEntryHeader::eResidencyDisk);
    if (item.cancelled.load()) {
        removeFile(finalPath, snapshot.identity.base);

        return eWriteOutcomeAbandoned;
    }

    return eWriteOutcomePublished;
} // CacheDiskTier::Imp::writeEntry

void
CacheDiskTier::Imp::finishDemotion(const WriteItemPtr& item)
{
    CacheEntryPtr entry;
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        demotionsInFlight.erase(std::remove(demotionsInFlight.begin(), demotionsInFlight.end(), item), demotionsInFlight.end());
        queuedBytes -= item->bytes;
        demoting.fetch_sub(1);
        entry = std::move(item->entry);
    }
}

void
CacheDiskTier::Imp::demotionLoop()
{
    for (;;) {
        WriteItemPtr item;
        {
            std::unique_lock<std::mutex> lock(queueMutex);
            queueCond.wait(lock, [this] { return stopping.load() || !demotionQueue.empty(); });
            if (stopping.load()) {
                return;
            }
            item = demotionQueue.front();
            demotionQueue.pop_front();
            demotionsInFlight.push_back(item);
        }
        (void)guardedWrite(*item->entry, *item);
        finishDemotion(item);
    }
}

void
CacheDiskTier::Imp::publisherLoop()
{
    lowerThreadCpuPriority();
    lowerThreadIoPriority();

    for (;;) {
        WriteItemPtr item;
        {
            std::unique_lock<std::mutex> lock(publishMutex);
            publishCond.wait(lock, [this] { return stopping.load() || !publishQueue.empty(); });
            if (stopping.load()) {
                return;
            }
            item = publishQueue.front();
            publishQueue.pop_front();
            publishesInFlight.push_back(item);
        }

        // Demotions hold RAM until they finish, so an idle write yields the disk to them.
        if (demoting.load() == 0) {
            const CacheEntryPtr entry = item->published.lock();
            if (entry && (guardedWrite(*entry, *item) == eWriteOutcomeFailed)) {
                std::lock_guard<std::mutex> lock(publishMutex);
                publishBackoffUntil = std::chrono::steady_clock::now() + std::chrono::milliseconds(kPublishBackoffMs);
            }
        }

        std::lock_guard<std::mutex> lock(publishMutex);
        publishesInFlight.erase(std::remove(publishesInFlight.begin(), publishesInFlight.end(), item), publishesInFlight.end());
        publishing.fetch_sub(1);
    }
}

bool
CacheDiskTier::Imp::waitStop(int ms)
{
    std::unique_lock<std::mutex> lock(stopMutex);

    return stopCond.wait_for(lock, std::chrono::milliseconds(ms), [this] { return stopping.load(); });
}

void
CacheDiskTier::Imp::maintenanceLoop()
{
    // CPU priority stays normal: this thread takes the cache's _lock to sample publish candidates,
    // and a preempted holder would stall every lookup of the cache.
    lowerThreadIoPriority();

    const int interval = std::max(1, std::min(kBudgetRefreshMs, config.publishIntervalMs));

    while (!waitStop(interval)) {
        std::lock_guard<std::mutex> lock(maintenanceMutex);
        tick();
    }
}

void
CacheDiskTier::Imp::scanLoop()
{
    lowerThreadCpuPriority();
    lowerThreadIoPriority();

    for (;;) {
        reapDeadSessions();
        reclaimDeadBuilds();
        removeCleared();
        scanOnce();

        std::unique_lock<std::mutex> lock(scanMutex);
        scanCond.wait_for(lock, std::chrono::milliseconds(kSlowScanMs), [this] { return stopping.load() || scanRequested; });
        scanRequested = false;
        if (stopping.load()) {
            return;
        }
    }
}

void
CacheDiskTier::Imp::removeCleared()
{
    std::vector<std::string> paths;
    {
        std::lock_guard<std::mutex> lock(scanMutex);
        paths.swap(clearedPaths);
    }
    for (const std::string& path : paths) {
        removeTree(path, stopping);
    }
}

void
CacheDiskTier::Imp::scanOnce()
{
    const std::map<std::string, LedgerRecord> ledgers = readLedgers();
    std::uint64_t total = 0;

    for (const std::string& build : buildNames()) {
        for (const char* section : { "auto", "pins" }) {
            std::error_code error;
            fs::recursive_directory_iterator it(config.root + '/' + build + '/' + section, fs::directory_options::skip_permission_denied, error);
            const fs::recursive_directory_iterator end;
            while (!error && (it != end)) {
                if (stopping.load()) {
                    return;
                }
                // A peer's unlink between listing and stat is normal, and costs one file of accuracy.
                std::error_code statError;
                if (it->is_regular_file(statError) && !statError) {
                    const std::uintmax_t bytes = it->file_size(statError);
                    if (!statError) {
                        total += bytes;
                    }
                }
                it.increment(error);
            }
        }
    }

    std::lock_guard<std::mutex> lock(budgetMutex);
    scanBytes = total;
    scanBaselines.clear();
    for (const auto& ledger : ledgers) {
        scanBaselines[ledger.first] = ledger.second.delta;
    }
}

void
CacheDiskTier::Imp::writeLedger()
{
    if (ledgerFd < 0) {
        return;
    }
    LedgerRecord record;
    std::memset(&record, 0, sizeof(record));
    record.magic = kLedgerMagic;
    std::strncpy(record.build, config.buildId.c_str(), sizeof(record.build) - 1);
    record.delta = ledgerDelta.load();
    record.heartbeatNs = CacheEviction::nowNs();
    record.closed = closing ? 1 : 0;
    (void)pwriteFully(ledgerFd, &record, sizeof(record), 0);
}

std::map<std::string, LedgerRecord>
CacheDiskTier::Imp::readLedgers() const
{
    std::map<std::string, LedgerRecord> ledgers;

    for (const std::string& name : listDirectory(sessionsDir)) {
        ScopedFd fd(::open((sessionsDir + '/' + name).c_str(), O_RDONLY | O_CLOEXEC));
        LedgerRecord record;
        // A ledger that holds nothing yet, or never will, still names a session to judge.
        if ((fd.get() < 0) || !preadFully(fd.get(), &record, sizeof(record), 0) || (record.magic != kLedgerMagic)) {
            std::memset(&record, 0, sizeof(record));
        }
        record.build[sizeof(record.build) - 1] = '\0';
        ledgers[name] = record;
    }

    return ledgers;
}

void
CacheDiskTier::Imp::ensureRegistered()
{
    struct stat st;
    if ((::stat(ledgerPath.c_str(), &st) == 0) || (errno != ENOENT)) {
        return;
    }
    // A peer judged this session dead, which a stalled mount or a shared host name in another PID
    // namespace can make it do. It took the ledger's bytes and the temp files, so this session starts
    // over with nothing to its name.
    if (ledgerFd >= 0) {
        ::close(ledgerFd);
    }
    ledgerFd = ::open(ledgerPath.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0666);
    ledgerDelta.store(0);
    std::error_code ignored;
    fs::create_directories(tmpDir, ignored);
}

void
CacheDiskTier::Imp::touchBuildStamp(bool force)
{
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (!force && (now - buildStampedAt < std::chrono::milliseconds(kBuildStampIntervalMs))) {
        return;
    }
    buildStampedAt = now;
    const std::string stamp = buildDir + '/' + kBuildStampName;
    int fd = ::open(stamp.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0666);
    if ((fd < 0) && (errno == ENOENT)) {
        // Reclaimed in the moment this session started, which costs only the files.
        std::error_code ignored;
        fs::create_directories(autoDir, ignored);
        fd = ::open(stamp.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0666);
    }
    ScopedFd closer(fd);
    if (fd >= 0) {
        (void)::futimens(fd, NULL);
    }
}

bool
CacheDiskTier::Imp::sessionIsDead(const std::string& name,
                                  const LedgerRecord& record)
{
    if (name == session) {
        return false;
    }
    if (record.closed != 0) {
        return true;
    }
    SessionId id;
    const bool parsed = parseSessionName(name, &id);
    if (parsed && (id.host == hostName()) && (id.start != "0")) {
        if (id.boot != bootId()) {
            return true;
        }
        const ProcessInfo process = processInfo(id.pid);
        if (process.readable) {
            return process.zombie || (process.startTime != id.start);
        }
        // /proc may hide other users' processes.
        if ((::kill((pid_t)std::atol(id.pid.c_str()), 0) != 0) && (errno == ESRCH)) {
            return true;
        }
    }

    // Measured against this host's clock, because clocks of two hosts need not agree: a beat is
    // stale once its value has stayed the same for long enough.
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(livenessMutex);
    const auto found = peerBeats.find(name);
    if ((found == peerBeats.end()) || (found->second.value != record.heartbeatNs)) {
        peerBeats[name] = PeerBeat { record.heartbeatNs, now };

        return false;
    }

    return now - found->second.changedAt >= std::chrono::milliseconds(config.heartbeatStaleMs);
}

std::set<std::string>
CacheDiskTier::Imp::liveBuildsOf(const std::map<std::string, LedgerRecord>& ledgers)
{
    std::set<std::string> live;

    live.insert(config.buildId);
    for (const auto& ledger : ledgers) {
        if (!sessionIsDead(ledger.first, ledger.second)) {
            live.insert(ledger.second.build);
        }
    }

    return live;
}

std::vector<std::string>
CacheDiskTier::Imp::buildNames() const
{
    std::vector<std::string> builds;

    // The stamp is what proves a directory a build's: the location is the user's choice, and it may
    // hold directories that are nothing to do with the tier and must survive its reclaiming.
    for (const std::string& name : listDirectory(config.root)) {
        struct stat st;
        if ((name != "sessions") && (name != "tmp") && (::stat((config.root + '/' + name + '/' + kBuildStampName).c_str(), &st) == 0)) {
            builds.push_back(name);
        }
    }

    return builds;
}

std::uint64_t
CacheDiskTier::Imp::buildLastLiveNs(const std::string& build) const
{
    struct stat st;

    if (::stat((config.root + '/' + build + '/' + kBuildStampName).c_str(), &st) != 0) {
        return 0;
    }

    return (std::uint64_t)st.st_mtim.tv_sec * 1000000000ULL + (std::uint64_t)st.st_mtim.tv_nsec;
}

void
CacheDiskTier::Imp::reapDeadSessions()
{
    // Temp directories first: a session makes its ledger before its directory, so one listed here
    // with no ledger below truly has none.
    const std::vector<std::string> tempDirs = listDirectory(config.root + "/tmp");
    const std::map<std::string, LedgerRecord> ledgers = readLedgers();

    for (const auto& ledger : ledgers) {
        if (stopping.load()) {
            return;
        }
        if (!sessionIsDead(ledger.first, ledger.second)) {
            continue;
        }
        // Whoever renames it owns the session's remains. Failing means a peer got there first.
        const std::string claim = tmpDir + "/claimed-" + ledger.first;
        if (::rename((sessionsDir + '/' + ledger.first).c_str(), claim.c_str()) != 0) {
            continue;
        }
        // Read again, since only the claim guarantees the session wrote its last.
        LedgerRecord record;
        std::int64_t delta = 0;
        {
            ScopedFd fd(::open(claim.c_str(), O_RDONLY | O_CLOEXEC));
            if ((fd.get() >= 0) && preadFully(fd.get(), &record, sizeof(record), 0) && (record.magic == kLedgerMagic)) {
                delta = record.delta;
            }
        }
        {
            std::lock_guard<std::mutex> lock(budgetMutex);
            const auto baseline = scanBaselines.find(ledger.first);
            if (baseline != scanBaselines.end()) {
                delta -= baseline->second;
                scanBaselines.erase(baseline);
            }
        }
        ledgerDelta.fetch_add(delta);
        if (removeTree(config.root + "/tmp/" + ledger.first, stopping)) {
            (void)::unlink(claim.c_str());
        }
        {
            std::lock_guard<std::mutex> lock(livenessMutex);
            peerBeats.erase(ledger.first);
        }
    }

    // A reaper that died mid-way leaves a directory no ledger names.
    for (const std::string& name : tempDirs) {
        if ((name != session) && (ledgers.find(name) == ledgers.end())) {
            if (!removeTree(config.root + "/tmp/" + name, stopping)) {
                return;
            }
        }
    }
}

void
CacheDiskTier::Imp::reclaimDeadBuilds()
{
    const std::set<std::string> live = liveBuildsOf(readLedgers());
    const std::uint64_t now = CacheEviction::nowNs();

    for (const std::string& build : buildNames()) {
        if (stopping.load()) {
            return;
        }
        if (live.count(build) != 0) {
            continue;
        }
        const std::uint64_t lastLive = buildLastLiveNs(build);
        // 0 is a stamp a peer has just moved away.
        if ((lastLive == 0) || (now <= lastLive) || (now - lastLive < kDeadBuildAgeNs)) {
            continue;
        }
        // Whoever renames it deletes it. A session of this build starting right now loses only
        // cached files, and its next write makes the directories it needs.
        const std::string moved = tmpDir + "/reclaimed-" + std::to_string(reclaimCounter.fetch_add(1));
        if (::rename((config.root + '/' + build).c_str(), moved.c_str()) == 0) {
            removeTree(moved, stopping);
        }
    }
}

void
CacheDiskTier::Imp::refreshBudget(const std::map<std::string, LedgerRecord>& ledgers)
{
    std::int64_t total;
    {
        std::lock_guard<std::mutex> lock(budgetMutex);
        total = (std::int64_t)scanBytes;
        for (const auto& ledger : ledgers) {
            const auto baseline = scanBaselines.find(ledger.first);
            total += ledger.second.delta - (baseline == scanBaselines.end() ? 0 : baseline->second);
        }
    }
    const std::uint64_t usedNow = total > 0 ? (std::uint64_t)total : 0;
    used.store(usedNow);

    std::uint64_t limit = sizeBytes.load();
    struct statvfs volume;
    if (::statvfs(config.root.c_str(), &volume) == 0) {
        // The tier's own files count as free, or the cap would shrink a full tier at every launch.
        const double freeBytes = (double)volume.f_bavail * (double)volume.f_frsize + (double)usedNow;
        limit = std::min(limit, (std::uint64_t)(freeBytes * kFreeSpaceCap));
    }
    budget.store(limit);
}

void
CacheDiskTier::Imp::sampleFile(const std::string& path,
                               const std::string& name,
                               std::uint64_t base,
                               int deadRank,
                               std::vector<EvictionSample>* sample) const
{
    ScopedFd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    struct stat st;
    StatsBlock block;

    if ((fd.get() < 0) || (::fstat(fd.get(), &st) != 0)) {
        return;
    }
    EvictionSample sampled;
    sampled.path = path;
    sampled.name = name;
    sampled.base = base;
    sampled.bytes = (std::uint64_t)st.st_size;
    if (preadFully(fd.get(), &block, sizeof(block), kHeaderBytes)) {
        sanitizeStats(block, &sampled.stats);
    }
    sampled.deadRank = deadRank;
    sampled.score = 0.;
    sample->push_back(std::move(sampled));
}

void
CacheDiskTier::Imp::evictPass(const std::map<std::string, LedgerRecord>& ledgers)
{
    // EWOULDBLOCK means a peer is at it. Any other error means the mount has no flock, which costs
    // only the guard against duplicate work.
    if ((evictFd >= 0) && (::flock(evictFd, LOCK_EX | LOCK_NB) != 0) && (errno == EWOULDBLOCK)) {
        return;
    }
    ScopedUnlock unlock(evictFd);

    const std::uint64_t now = CacheEviction::nowNs();
    const std::set<std::string> liveBuilds = liveBuildsOf(ledgers);

    // Dead builds first, the one longest out of use before the others.
    std::vector<std::pair<std::uint64_t, std::string>> deadBuilds;
    std::vector<std::string> builds;
    for (const std::string& build : buildNames()) {
        if (liveBuilds.count(build) == 0) {
            deadBuilds.push_back(std::make_pair(buildLastLiveNs(build), build));
        } else {
            builds.push_back(build);
        }
    }
    std::sort(deadBuilds.begin(), deadBuilds.end());
    const int deadCount = (int)deadBuilds.size();
    {
        std::vector<std::string> ordered;
        ordered.reserve(deadBuilds.size() + builds.size());
        for (const auto& dead : deadBuilds) {
            ordered.push_back(dead.second);
        }
        ordered.insert(ordered.end(), builds.begin(), builds.end());
        builds.swap(ordered);
    }

    std::random_device entropy;
    std::mt19937 random(entropy());
    std::vector<EvictionSample> sample;
    std::size_t deadSampled = 0;
    for (std::size_t buildIndex = 0; buildIndex < builds.size(); ++buildIndex) {
        const std::string& build = builds[buildIndex];
        const bool deadBuild = (int)buildIndex < deadCount;
        const int rank = deadBuild ? (int)buildIndex : deadCount;
        const std::size_t sampledBefore = sample.size();
        // Files of dead builds go first, so sampling live ones would only delay the pass.
        if (!deadBuild && (deadSampled >= CacheEviction::kBatch)) {
            break;
        }

        const unsigned first = random() % kShards;
        for (unsigned i = 0; (i < kShards) && (sample.size() < CacheEviction::kSample); ++i) {
            if (stopping.load()) {
                return;
            }
            char shard[kShardDigits + 1];
            std::snprintf(shard, sizeof(shard), "%03x", (first + i * kShardStride) % kShards);
            const std::string shardPath = config.root + '/' + build + "/auto/" + shard;
            for (const std::string& name : listDirectory(shardPath)) {
                std::uint64_t base;
                if (!isEntryName(name) || !parseHex(name.c_str(), &base)) {
                    continue;
                }
                sampleFile(shardPath + '/' + name, name, base, rank, &sample);
            }
        }
        bool walkedEverything = sample.size() < CacheEviction::kSample;

        // A pin protects files from its own build's eviction, and no session of a dead build is left
        // to want them.
        if (deadBuild && (sample.size() < CacheEviction::kSample)) {
            std::error_code error;
            fs::recursive_directory_iterator it(config.root + '/' + build + "/pins", fs::directory_options::skip_permission_denied, error);
            const fs::recursive_directory_iterator end;
            while (!error && (it != end) && (sample.size() < CacheEviction::kSample)) {
                if (stopping.load()) {
                    return;
                }
                std::error_code statError;
                if (it->is_regular_file(statError) && !statError) {
                    const std::string name = it->path().filename().string();
                    std::uint64_t base = 0;
                    if (!isEntryName(name) || !parseHex(name.c_str(), &base)) {
                        base = 0;
                    }
                    sampleFile(it->path().string(), name, base, rank, &sample);
                }
                it.increment(error);
            }
            walkedEverything = walkedEverything && (sample.size() < CacheEviction::kSample);
        }
        if (deadBuild) {
            deadSampled += sample.size() - sampledBefore;
            if (walkedEverything && (sample.size() == sampledBefore)) {
                // Nothing left to evict, so nothing is left of this build but its directories.
                removeTree(config.root + '/' + build, stopping);
            }
        }
        if (sample.size() >= CacheEviction::kSample) {
            break;
        }
    }

    {
        const std::uint64_t retentionNow = CacheRetention::nowNs();
        std::lock_guard<std::mutex> lock(knownMutex);
        for (EvictionSample& s : sample) {
            double priority = s.stats.priority > 0.f ? s.stats.priority : CacheEviction::kNeutralPriority;
            const auto found = known.find(s.name);
            if (found != known.end()) {
                const CacheRetentionSignalsPtr retention = found->second.lock();
                if (retention) {
                    priority = retention->priority(retentionNow);
                }
            }
            const std::uint64_t age = now > s.stats.lastUse ? now - s.stats.lastUse : 0;
            s.score = CacheEviction::score(priority, s.stats.recomputeCostNs, s.stats.reuseCount, age, (std::size_t)s.bytes);
        }
    }

    const auto order = [](const EvictionSample& a, const EvictionSample& b) {
        return a.deadRank != b.deadRank ? a.deadRank < b.deadRank : a.score < b.score;
    };
    if (sample.size() > CacheEviction::kBatch) {
        std::nth_element(sample.begin(), sample.begin() + CacheEviction::kBatch, sample.end(), order);
        sample.resize(CacheEviction::kBatch);
    }
    for (const EvictionSample& s : sample) {
        // ENOENT is a peer having evicted it first, which is what this was about to do.
        if (::unlink(s.path.c_str()) == 0) {
            ledgerDelta.fetch_sub((std::int64_t)s.bytes);
        }
        forgetBase(s.base);
    }
} // CacheDiskTier::Imp::evictPass

void
CacheDiskTier::Imp::offerPublishCandidates()
{
    if (!config.publish || !config.idlePublisher || !storage || (demoting.load() > 0) || (budget.load() == 0)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(publishMutex);
        if ((publishing.load() > 0) || (std::chrono::steady_clock::now() < publishBackoffUntil)) {
            return;
        }
    }

    std::vector<std::weak_ptr<AbstractCacheEntryBase>> candidates;
    storage->publishCandidates(kPublishScan, &candidates);

    std::vector<WriteItemPtr> items;
    for (const std::weak_ptr<AbstractCacheEntryBase>& candidate : candidates) {
        const CacheEntryPtr entry = candidate.lock();
        if (!entry || !worthWriting(*entry)) {
            continue;
        }
        WriteItemPtr item = std::make_shared<WriteItem>();
        item->published = entry;
        item->raw = entry.get();
        item->hash = entry->getHashKey();
        items.push_back(std::move(item));
        if (items.size() >= kPublishBatch) {
            break;
        }
    }
    if (items.empty()) {
        return;
    }

    std::lock_guard<std::mutex> lock(publishMutex);
    for (WriteItemPtr& item : items) {
        publishQueue.push_back(std::move(item));
        publishing.fetch_add(1);
    }
    publishCond.notify_all();
}

void
CacheDiskTier::Imp::tick()
{
    ensureRegistered();
    writeLedger();
    touchBuildStamp(false);

    const std::map<std::string, LedgerRecord> ledgers = readLedgers();

    refreshBudget(ledgers);
    if ((double)used.load() > (double)budget.load() * NATRON_CACHE_LIMIT_PERCENT) {
        evictPass(ledgers);
    }
    offerPublishCandidates();
}

CacheDiskTier::CacheDiskTier(const Config& config)
    : CacheTier()
    , _imp(new Imp(config))
{
}

CacheDiskTierPtr
CacheDiskTier::open(const Config& config)
{
    static std::atomic<unsigned> instances(0);

    std::shared_ptr<CacheDiskTier> tier(new CacheDiskTier(config));
    Imp& imp = *tier->_imp;
    const unsigned instance = instances.fetch_add(1);

    imp.session = sessionBaseName();
    if (instance > 0) {
        imp.session += '.' + std::to_string(instance);
    }
    imp.buildDir = config.root + '/' + config.buildId;
    imp.autoDir = imp.buildDir + "/auto";
    imp.sessionsDir = config.root + "/sessions";
    imp.tmpDir = config.root + "/tmp/" + imp.session;
    imp.ledgerPath = imp.sessionsDir + '/' + imp.session;

    // The ledger comes before the temp directory: a directory no ledger names is a reaper's leftover.
    std::error_code error;
    fs::create_directories(imp.sessionsDir, error);
    if (error) {
        return CacheDiskTierPtr();
    }
    imp.ledgerFd = ::open(imp.ledgerPath.c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0666);
    if (imp.ledgerFd < 0) {
        return CacheDiskTierPtr();
    }
    const auto abandon = [&imp]() {
        ::close(imp.ledgerFd);
        imp.ledgerFd = -1;
        (void)::unlink(imp.ledgerPath.c_str());
        std::error_code ignored;
        fs::remove_all(imp.tmpDir, ignored);

        return CacheDiskTierPtr();
    };
    for (const std::string& dir : { imp.autoDir, imp.tmpDir }) {
        fs::create_directories(dir, error);
        if (error) {
            return abandon();
        }
    }
    if ((config.root == config.renameSelfTestFailsAt) || !renameBehavesAtomically(imp.tmpDir, imp.autoDir, imp.session)) {
        return abandon();
    }
    imp.evictFd = ::open((config.root + "/evict.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0666);
    imp.networkMount = isNetworkFilesystem(config.root);

    // A write past RLIMIT_FSIZE has to come back as EFBIG, to be dropped like any other write
    // failure. This signal's default action would kill the process.
    struct sigaction current;
    if ((::sigaction(SIGXFSZ, NULL, &current) == 0) && (current.sa_handler == SIG_DFL)) {
        ::signal(SIGXFSZ, SIG_IGN);
    }

    imp.writeLedger();
    imp.touchBuildStamp(true);
    imp.refreshBudget(imp.readLedgers());

    return tier;
}

CacheDiskTierPtr
CacheDiskTier::openWithFallback(const Config& config,
                                const std::string& fallbackRoot,
                                std::string* warning)
{
    CacheDiskTierPtr tier = open(config);

    if (tier) {
        return tier;
    }
    if (fallbackRoot != config.root) {
        Config fallback = config;
        fallback.root = fallbackRoot;
        tier = open(fallback);
    }
    *warning = tier ? "Disk tier: cannot use " + config.root + ", using " + fallbackRoot
                    : "Disk tier: cannot use " + config.root + ", caching in RAM only";

    return tier;
}

CacheDiskTier::~CacheDiskTier()
{
    shutdown();
}

void
CacheDiskTier::attach(const CacheHostStorage* storage,
                      const CacheTier* host)
{
    Imp& imp = *_imp;

    imp.storage = storage;
    imp.host = host;

    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    const unsigned writers = std::max(1u, std::min(4u, cores / 8));

    if (imp.config.publish) {
        for (unsigned i = 0; i < writers; ++i) {
            imp.threads.emplace_back([&imp] { imp.demotionLoop(); });
        }
        if (imp.config.idlePublisher) {
            imp.threads.emplace_back([&imp] { imp.publisherLoop(); });
        }
    }
    if (imp.config.backgroundMaintenance) {
        imp.threads.emplace_back([&imp] { imp.maintenanceLoop(); });
        imp.threads.emplace_back([&imp] { imp.scanLoop(); });
    }
}

void
CacheDiskTier::shutdown()
{
    Imp& imp = *_imp;

    if (imp.shutDown.exchange(true)) {
        return;
    }
    imp.stopping.store(true);
    {
        std::lock_guard<std::mutex> lock(imp.queueMutex);
    }
    imp.queueCond.notify_all();
    {
        std::lock_guard<std::mutex> lock(imp.publishMutex);
    }
    imp.publishCond.notify_all();
    {
        std::lock_guard<std::mutex> lock(imp.stopMutex);
    }
    imp.stopCond.notify_all();
    {
        std::lock_guard<std::mutex> lock(imp.scanMutex);
    }
    imp.scanCond.notify_all();
    for (std::thread& thread : imp.threads) {
        thread.join();
    }
    imp.threads.clear();

    {
        std::deque<WriteItemPtr> queued;
        {
            std::lock_guard<std::mutex> lock(imp.queueMutex);
            queued.swap(imp.demotionQueue);
            imp.queuedBytes = 0;
            imp.demoting.store(0);
        }
    }
    {
        std::lock_guard<std::mutex> lock(imp.publishMutex);
        imp.publishQueue.clear();
        imp.publishing.store(0);
    }
    // The ledger stays for a peer to claim, as after a crash, so only one path has to work.
    imp.closing = true;
    imp.writeLedger();
    imp.storage = NULL;
    imp.host = NULL;
    if (imp.ledgerFd >= 0) {
        ::close(imp.ledgerFd);
        imp.ledgerFd = -1;
    }
    if (imp.evictFd >= 0) {
        ::close(imp.evictFd);
        imp.evictFd = -1;
    }

    // A cleared or reclaimed tree can take minutes to unlink, so it waits for a peer rather than
    // holding up the exit.
    for (const std::string& name : listDirectory(imp.tmpDir)) {
        (void)::unlink((imp.tmpDir + '/' + name).c_str());
    }
    (void)::rmdir(imp.tmpDir.c_str());
} // CacheDiskTier::shutdown

std::size_t
CacheDiskTier::budget() const
{
    return (std::size_t)_imp->budget.load();
}

std::size_t
CacheDiskTier::used() const
{
    return (std::size_t)_imp->used.load();
}

bool
CacheDiskTier::demote(const CacheEntryPtr& entry)
{
    if (!entry) {
        return false;
    }
    retract(entry, true);

    return true;
}

bool
CacheDiskTier::promote(const CacheEntryPtr& /*entry*/)
{
    return false;
}

void
CacheDiskTier::setSizeBytes(std::uint64_t bytes)
{
    _imp->sizeBytes.store(bytes);
    _imp->budget.store(std::min((std::uint64_t)_imp->budget.load(), bytes));
}

bool
CacheDiskTier::offerDemotion(const CacheEntryPtr& entry)
{
    Imp& imp = *_imp;

    if (!entry || !imp.config.publish || imp.stopping.load() || !imp.host || (imp.budget.load() == 0)) {
        return false;
    }
    if (!imp.worthWriting(*entry)) {
        return false;
    }

    WriteItemPtr item = std::make_shared<WriteItem>();
    item->entry = entry;
    item->raw = entry.get();
    item->hash = entry->getHashKey();
    item->bytes = entry->size();

    const double limit = (double)imp.host->budget() * kDemotionQueueFraction;
    {
        std::lock_guard<std::mutex> lock(imp.queueMutex);
        // Over the limit a slow disk costs recomputes, which beats it costing all the RAM.
        if ((double)imp.queuedBytes > limit) {
            return false;
        }
        imp.demotionQueue.push_back(item);
        imp.queuedBytes += item->bytes;
        imp.demoting.fetch_add(1);
    }
    imp.queueCond.notify_one();

    return true;
}

bool
CacheDiskTier::hasDemoting() const
{
    return _imp->demoting.load() > 0;
}

void
CacheDiskTier::reclaimDemoting(std::uint64_t hash,
                               std::vector<CacheEntryPtr>* entries)
{
    Imp& imp = *_imp;
    std::lock_guard<std::mutex> lock(imp.queueMutex);

    for (auto it = imp.demotionQueue.begin(); it != imp.demotionQueue.end();) {
        if ((*it)->hash == hash) {
            entries->push_back((*it)->entry);
            imp.queuedBytes -= (*it)->bytes;
            imp.demoting.fetch_sub(1);
            it = imp.demotionQueue.erase(it);
        } else {
            ++it;
        }
    }
    // A write in progress runs to its end and publishes, which does no harm: the caller's
    // reference keeps the entry alive past it.
    for (const WriteItemPtr& item : imp.demotionsInFlight) {
        if ((item->hash == hash) && !item->reclaimed) {
            item->reclaimed = true;
            entries->push_back(item->entry);
        }
    }
}

void
CacheDiskTier::peekDemoting(std::uint64_t hash,
                            std::vector<CacheEntryPtr>* entries) const
{
    Imp& imp = *_imp;
    std::lock_guard<std::mutex> lock(imp.queueMutex);

    for (const WriteItemPtr& item : imp.demotionQueue) {
        if (item->hash == hash) {
            entries->push_back(item->entry);
        }
    }
    for (const WriteItemPtr& item : imp.demotionsInFlight) {
        if ((item->hash == hash) && !item->reclaimed) {
            entries->push_back(item->entry);
        }
    }
}

void
CacheDiskTier::retract(const CacheEntryPtr& entry,
                       bool removeFile)
{
    if (!entry) {
        return;
    }
    Imp& imp = *_imp;

    imp.cancelQueued(entry.get());
    if (!removeFile) {
        return;
    }
    const CacheEntryHeader* const header = entry->evictionHeader();
    CacheDiskIdentity identity;
    if (header && (header->residency.load() & CacheEntryHeader::eResidencyDisk) && entry->diskIdentity(&identity)) {
        imp.removeFile(imp.pathOf(identity), identity.base);
    }
}

void
CacheDiskTier::retractFiles(const CacheDiskIdentity& identity)
{
    Imp& imp = *_imp;
    const std::string shardDir = imp.shardDirOf(identity.base);

    imp.forgetBase(identity.base);
    for (const std::string& name : imp.namesFor(identity.base, identity.variant)) {
        imp.removeFile(shardDir + '/' + name, identity.base);
    }
}

void
CacheDiskTier::cancelWrites()
{
    _imp->cancelEverything();
}

void
CacheDiskTier::clearAuto()
{
    Imp& imp = *_imp;

    imp.cancelEverything();

    // A clear comes from the main thread and unlinking a full tier takes a long while, so the
    // directory moves aside at once and the scanner deletes it.
    const std::string cleared = imp.tmpDir + "/cleared-" + std::to_string(imp.clearCounter.fetch_add(1));
    const bool moved = ::rename(imp.autoDir.c_str(), cleared.c_str()) == 0;
    std::error_code ignored;

    fs::create_directories(imp.autoDir, ignored);
    forgetProbes();
    {
        std::lock_guard<std::mutex> lock(imp.knownMutex);
        imp.known.clear();
    }

    std::lock_guard<std::mutex> lock(imp.scanMutex);
    if (moved) {
        imp.clearedPaths.push_back(cleared);
    }
    imp.scanRequested = true;
    imp.scanCond.notify_all();
}

bool
CacheDiskTier::probe(const CacheDiskIdentity& identity,
                     const std::string* wantedParamsBytes,
                     std::vector<CacheDiskFileInfo>* candidates)
{
    Imp& imp = *_imp;

    if (identity.salted) {
        return false;
    }

    const std::vector<std::string> names = imp.namesFor(identity.base, identity.variant);

    if (names.empty()) {
        return false;
    }

    const std::string shardDir = imp.shardDirOf(identity.base);
    for (const std::string& name : names) {
        std::uint64_t paramsHash;
        if (!parseHex(name.c_str() + 2 * kHexDigits + 2, &paramsHash)) {
            continue;
        }
        if (wantedParamsBytes && (paramsHash != hashBytes(wantedParamsBytes->data(), wantedParamsBytes->size()))) {
            continue;
        }
        CacheDiskFileInfo info;
        if (!readFileInfoAt(shardDir + '/' + name, &info)) {
            continue;
        }
        // The name alone proves nothing: 64-bit hashes collide and a file can be renamed.
        if ((info.kind != identity.kind) || (info.keyBytes != identity.keyBytes) || (hashBytes(info.paramsBytes.data(), info.paramsBytes.size()) != paramsHash)) {
            continue;
        }
        if (wantedParamsBytes && (info.paramsBytes != *wantedParamsBytes)) {
            continue;
        }
        candidates->push_back(std::move(info));
    }
    std::sort(candidates->begin(), candidates->end(), [](const CacheDiskFileInfo& a, const CacheDiskFileInfo& b) {
        return a.renderedPixels > b.renderedPixels;
    });

    return !candidates->empty();
}

void
CacheDiskTier::forgetProbes()
{
    for (Imp::ProbeStripe& stripe : _imp->probes) {
        std::lock_guard<std::mutex> lock(stripe.mutex);
        stripe.entries.clear();
    }
}

CacheDiskTier::PromotionGate::PromotionGate(CacheDiskTier& tier,
                                            std::uint64_t hash)
    : _tier(tier)
    , _hash(hash)
{
    Imp::GateStripe& stripe = _tier._imp->gates[hash % kGateStripes];
    std::unique_lock<std::mutex> lock(stripe.mutex);

    stripe.released.wait(lock, [&stripe, hash] { return stripe.held.count(hash) == 0; });
    stripe.held.insert(hash);
}

CacheDiskTier::PromotionGate::~PromotionGate()
{
    Imp::GateStripe& stripe = _tier._imp->gates[_hash % kGateStripes];
    {
        std::lock_guard<std::mutex> lock(stripe.mutex);
        stripe.held.erase(_hash);
    }
    stripe.released.notify_all();
}

CacheDiskTier::Reader::Reader(const CacheDiskFileInfo& info)
    : _info(info)
    , _fd(-1)
    , _offset(kBitmapOffset)
    , _hashState(XXH3_createState())
{
    XXH3_64bits_reset(static_cast<XXH3_state_t*>(_hashState));
}

CacheDiskTier::Reader::~Reader()
{
    if (_fd >= 0) {
        ::close(_fd);
    }
    XXH3_freeState(static_cast<XXH3_state_t*>(_hashState));
}

bool
CacheDiskTier::Reader::open()
{
    _fd = ::open(_info.path.c_str(), O_RDONLY | O_CLOEXEC);
    if (_fd < 0) {
        return false;
    }
    CacheDiskFileInfo current;
    if (!readFileInfo(_fd, &current)) {
        return false;
    }

    return current.payloadHash == _info.payloadHash && current.fileBytes == _info.fileBytes && current.bounds == _info.bounds;
}

bool
CacheDiskTier::Reader::read(void* data,
                            std::size_t bytes)
{
    if ((_fd < 0) || (_offset + bytes > _info.fileBytes) || !preadFully(_fd, data, bytes, _offset)) {
        return false;
    }
    XXH3_64bits_update(static_cast<XXH3_state_t*>(_hashState), data, bytes);
    _offset += bytes;

    return true;
}

bool
CacheDiskTier::Reader::finish()
{
    return _offset == _info.fileBytes && XXH3_64bits_digest(static_cast<XXH3_state_t*>(_hashState)) == _info.payloadHash;
}

void
CacheDiskTier::discardCorrupt(const CacheDiskFileInfo& info)
{
    std::uint64_t base = 0;
    const std::size_t slash = info.path.rfind('/');

    if ((slash != std::string::npos) && (info.path.size() - slash - 1 == kNameBytes)) {
        (void)parseHex(info.path.c_str() + slash + 1, &base);
    }
    _imp->removeFile(info.path, base);
}

void
CacheDiskTier::notePromoted(const CacheDiskFileInfo& info)
{
    CacheDiskStats stats = info.stats;

    stats.lastUse = CacheEviction::nowNs();
    ++stats.reuseCount;
    writeStatsAt(info.path, stats);
}

bool
CacheDiskTier::waitIdle(int timeoutMs)
{
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);

    for (;;) {
        if ((_imp->demoting.load() == 0) && (_imp->publishing.load() == 0)) {
            return true;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void
CacheDiskTier::maintainNow(bool scan)
{
    Imp& imp = *_imp;
    std::lock_guard<std::mutex> lock(imp.maintenanceMutex);

    if (scan) {
        imp.ensureRegistered();
        imp.writeLedger();
        imp.reapDeadSessions();
        imp.reclaimDeadBuilds();
        imp.removeCleared();
        imp.scanOnce();
    }
    imp.tick();
}

bool
CacheDiskTier::isNetworkMount() const
{
    return _imp->networkMount;
}

std::uint64_t
CacheDiskTier::writeFailures() const
{
    return _imp->failures.load();
}

std::uint64_t
CacheDiskTier::warningCount() const
{
    return _imp->warnings.load();
}

std::uint64_t
CacheDiskTier::publishedFiles() const
{
    return _imp->published.load();
}

double
CacheDiskTier::writeBytesPerSecond() const
{
    return _imp->throughput.load();
}

std::string
CacheDiskTier::sessionName() const
{
    return _imp->session;
}

CacheDiskIdentity
CacheDiskTier::makeIdentity(std::uint32_t kind,
                            const std::string& baseBytes,
                            const std::string& variantBytes)
{
    CacheDiskIdentity identity;

    identity.kind = kind;
    identity.keyBytes = baseBytes + variantBytes;
    identity.base = XXH3_64bits_withSeed(baseBytes.data(), baseBytes.size(), kind);
    identity.variant = XXH3_64bits(variantBytes.data(), variantBytes.size());

    return identity;
}

std::uint64_t
CacheDiskTier::hashBytes(const void* data,
                         std::size_t bytes)
{
    return XXH3_64bits(data, bytes);
}

void
CacheDiskTier::encodeBitmap(const char* map,
                            std::size_t count,
                            std::vector<std::uint32_t>* runs,
                            std::uint64_t* renderedPixels)
{
    runs->clear();
    *renderedPixels = 0;

    // Value 2, being rendered elsewhere, is not rendered: that render may never finish.
    bool rendered = false;
    std::size_t i = 0;
    while (i < count) {
        std::size_t end = i;
        while ((end < count) && ((map[end] == 1) == rendered)) {
            ++end;
        }
        runs->push_back((std::uint32_t)(end - i));
        if (rendered) {
            *renderedPixels += end - i;
        }
        i = end;
        rendered = !rendered;
    }
}

bool
CacheDiskTier::decodeBitmap(const std::vector<std::uint32_t>& runs,
                            char* map,
                            std::size_t count)
{
    bool rendered = false;
    std::size_t at = 0;

    for (const std::uint32_t run : runs) {
        if (run > count - at) {
            return false;
        }
        std::memset(map + at, rendered ? 1 : 0, run);
        at += run;
        rendered = !rendered;
    }

    return at == count;
}

bool
CacheDiskTier::fitsHeader(std::size_t keyBytes,
                          std::size_t paramsBytes)
{
    return sizeof(FixedHeader) + keyBytes + paramsBytes <= kHeaderBytes;
}

NATRON_NAMESPACE_EXIT
