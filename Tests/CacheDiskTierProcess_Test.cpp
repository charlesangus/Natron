/* ***** BEGIN LICENSE BLOCK *****
 * This file is part of Natron <https://natrongithub.github.io/>,
 * (C) 2018-2021 The Natron developers
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

#include "Global/Macros.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryDir>

#include "BaseTest.h"
#include "CacheDiskTierWorker.h"

#include "Engine/CacheDiskTier.h"
#include "Engine/CacheEntry.h"
#include "Engine/CacheTier.h"
#include "Engine/Image.h"
#include "Engine/ImageKey.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/ImageParams.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const int kSide = 64;
const float kExpensiveNs = 1e9f;
const std::uint64_t kAmpleBudget = 256ULL * 1024 * 1024;
const std::uint64_t kHostLimit = 64ULL * 1024 * 1024;

ImageKey
keyOf(U64 hash)
{
    return Image::makeKey(NULL, hash, false, 1., ViewIdx(0), false, false).forEntry(0, ImageLayerDesc::getRGBAComponents(), eImageBitDepthFloat);
}

ImageParamsPtr
paramsOf(int side)
{
    return Image::makeParams(RectD(0, 0, side, side), 1., 0, false,
                             ImageLayerDesc::getRGBAComponents(),
                             eImageBitDepthFloat,
                             eImageFieldingOrderNone);
}

float
seedOf(U64 hash)
{
    return (float)(hash % 97);
}

float
patternAt(float seed,
          int x,
          int y,
          int component)
{
    return seed + (float)x * 0.001f + (float)y * 0.25f + (float)component * 16.f;
}

void
fillPattern(const ImagePtr& image,
            float seed)
{
    const RectI bounds = image->getBounds();
    Image::WriteAccess access = image->getWriteRights();

    for (int y = bounds.y1; y < bounds.y2; ++y) {
        for (int x = bounds.x1; x < bounds.x2; ++x) {
            float* const pixel = reinterpret_cast<float*>(access.pixelAt(x, y));
            for (int c = 0; c < 4; ++c) {
                pixel[c] = patternAt(seed, x, y, c);
            }
        }
    }
}

bool
matchesPattern(const ImagePtr& image,
               float seed)
{
    const RectI bounds = image->getBounds();
    Image::ReadAccess access = image->getReadRights();

    for (int y = bounds.y1; y < bounds.y2; ++y) {
        for (int x = bounds.x1; x < bounds.x2; ++x) {
            const float* const pixel = reinterpret_cast<const float*>(access.pixelAt(x, y));
            for (int c = 0; c < 4; ++c) {
                if (pixel[c] != patternAt(seed, x, y, c)) {
                    return false;
                }
            }
        }
    }

    return true;
}

void
prepare(const ImagePtr& image,
        float seed)
{
    image->allocateMemory();
    fillPattern(image, seed);
    image->markForRendered(RectI(0, 0, kSide, kSide));
    image->getEntryHeader()->recomputeCostNs.store(kExpensiveNs);
    image->getEntryHeader()->lastUse.store(CacheEviction::nowNs());
}

class PinHolder final
    : public CacheEntryHolder {
public:
    explicit PinHolder(const std::string& id)
        : _id(id)
    {
    }

    virtual std::string getCacheID() const override final
    {
        return _id;
    }

private:
    std::string _id;
};

ImageKey
pinKeyOf(const std::string& holderID,
         U64 hash)
{
    const PinHolder holder(holderID);

    return Image::makeKey(&holder, hash, false, 1., ViewIdx(0), false, false).forEntry(0, ImageLayerDesc::getRGBAComponents(), eImageBitDepthFloat);
}

// Images need a cache to be identifiable on disk; only their accounting calls reach it here.
class FakeCacheAPI final
    : public CacheAPI {
public:
    virtual QString getCachePath() const override final { return QString(); }
    virtual bool isTileCache() const override final { return false; }
    virtual std::size_t getTileSizeBytes() const override final { return 0; }
    virtual void notifyEntrySizeChanged(std::size_t, std::size_t) const override final { }
    virtual void notifyEntryAllocated(double, std::size_t, StorageModeEnum) const override final { }
    virtual void notifyEntryDestroyed(double, std::size_t, StorageModeEnum) const override final { }
    virtual void notifyMemoryDeallocated() const override final { }
    virtual void backingFileClosed() const override final { }
    virtual void notifyEntryStorageChanged(StorageModeEnum, StorageModeEnum, double, std::size_t) const override final { }
    virtual void removeAllEntriesWithDifferentNodeHashForHolderPrivate(const std::string&, U64, bool) override final { }
    virtual TileCacheFilePtr allocTile(std::size_t*) override final { return TileCacheFilePtr(); }
    virtual TileCacheFilePtr getTileCacheFile(const std::string&, std::size_t) override final { return TileCacheFilePtr(); }
    virtual void freeTile(const TileCacheFilePtr&, std::size_t) override final { }
};

class NullStorage final
    : public CacheHostStorage {
public:
    virtual bool dropFromHost(const CacheEntryPtr&) const override final { return false; }
    virtual CacheEntryPtr demotionCandidate() const override final { return CacheEntryPtr(); }
};

struct HeldRead {
    explicit HeldRead(const CacheDiskFileInfo& fileInfo)
        : info(fileInfo)
        , reader(info)
    {
    }

    CacheDiskFileInfo info;
    CacheDiskTier::Reader reader;
    std::vector<char> payload;
    std::size_t done = 0;
};

class WorkerSession {
public:
    WorkerSession(const std::string& root,
                  const std::string& build,
                  std::uint64_t sizeBytes,
                  const std::string& fallback,
                  const std::string& renameFailsAt)
        : _host(&_storage, kHostLimit)
    {
        CacheDiskTier::Config config;
        config.root = root;
        config.buildId = build;
        config.sizeBytes = sizeBytes;
        config.idlePublisher = false;
        config.backgroundMaintenance = false;
        config.publishIntervalMs = 10;
        config.renameSelfTestFailsAt = renameFailsAt;

        std::string warning;
        _tier = fallback.empty() ? CacheDiskTier::open(config) : CacheDiskTier::openWithFallback(config, fallback, &warning);
        if (!warning.empty()) {
            reply("warning " + warning);
        }
        _root = root;
        if (_tier) {
            _tier->attach(&_storage, &_host);
            _tier->setPinBudget(kAmpleBudget);
        }
    }

    bool hasTier() const
    {
        return (bool)_tier;
    }

    std::string sessionName() const
    {
        return _tier->sessionName();
    }

    void shutdown()
    {
        _mapped.clear();
        _held.clear();
        if (_tier) {
            _tier->shutdown();
        }
    }

    static void reply(const std::string& text)
    {
        std::printf("@ %s\n", text.c_str());
        std::fflush(stdout);
    }

    bool write(U64 hash)
    {
        ImagePtr image = std::make_shared<Image>(keyOf(hash), paramsOf(kSide), &_api);

        prepare(image, seedOf(hash));
        const bool offered = _tier->offerDemotion(image);
        image.reset();

        return _tier->waitIdle(60000) && offered;
    }

    std::string read(U64 hash)
    {
        CacheDiskFileInfo info;
        if (!fileOf(hash, &info)) {
            return "miss";
        }
        ImagePtr image = std::make_shared<Image>(keyOf(hash), paramsOf(kSide), &_api);
        CacheDiskTier::Reader reader(info);
        if (!reader.open()) {
            return "miss";
        }
        if (!image->diskRestore(info, reader, false) || !reader.finish()) {
            return "bad";
        }

        return matchesPattern(image, seedOf(hash)) ? "hit" : "bad";
    }

    std::string holdRead(U64 hash)
    {
        CacheDiskFileInfo info;
        if (!fileOf(hash, &info)) {
            return "miss";
        }
        std::unique_ptr<HeldRead> held(new HeldRead(info));
        if (!held->reader.open()) {
            return "miss";
        }
        std::vector<char> bitmap((std::size_t)info.bitmapBytes);
        if (!bitmap.empty() && !held->reader.read(bitmap.data(), bitmap.size())) {
            return "bad";
        }
        held->payload.resize((std::size_t)info.payloadBytes);
        held->done = held->payload.size() / 2;
        if (!held->reader.read(held->payload.data(), held->done)) {
            return "bad";
        }
        _held[hash] = std::move(held);

        return info.path;
    }

    std::string holdFinish(U64 hash)
    {
        const auto found = _held.find(hash);
        if (found == _held.end()) {
            return "bad";
        }
        HeldRead& held = *found->second;
        if (!held.reader.read(held.payload.data() + held.done, held.payload.size() - held.done) || !held.reader.finish()) {
            return "bad";
        }
        ImagePtr image = std::make_shared<Image>(keyOf(hash), paramsOf(kSide), &_api);
        prepare(image, seedOf(hash));
        Image::ReadAccess access = image->getReadRights();
        const bool same = std::memcmp(access.pixelAt(0, 0), held.payload.data(), held.payload.size()) == 0;
        _held.erase(found);

        return same ? "good" : "bad";
    }

    // The outcome, then where the file is.
    std::string pin(const std::string& holder,
                    U64 hash)
    {
        ImagePtr image = std::make_shared<Image>(pinKeyOf(holder, hash), paramsOf(kSide), &_api);

        prepare(image, seedOf(hash));
        const CacheDiskTier::PinOutcomeEnum outcome = _tier->publishPin(holder, image);
        CacheDiskFileInfo info;
        const std::string path = pinOf(holder, hash, &info) ? info.path : std::string("none");

        return std::to_string((int)outcome) + " " + path;
    }

    bool hasPin(const std::string& holder,
                U64 hash)
    {
        CacheDiskFileInfo info;

        return pinOf(holder, hash, &info);
    }

    std::string map(const std::string& holder,
                    U64 hash)
    {
        _tier->holdPins(holder);
        CacheDiskFileInfo info;
        if (!pinOf(holder, hash, &info)) {
            return "miss";
        }
        ImagePtr image = std::make_shared<Image>(pinKeyOf(holder, hash), paramsOf(kSide), &_api);
        CacheDiskTier::Reader reader(info);
        if (!reader.open()) {
            return "miss";
        }
        if (!image->diskRestore(info, reader, !_tier->isNetworkMount()) || !reader.finish()) {
            return "bad";
        }
        const bool mapped = image->isFileMapped();
        _mapped[holder + "/" + std::to_string(hash)] = image;

        return mapped ? "mapped" : "copied";
    }

    std::string checkMap(const std::string& holder,
                         U64 hash)
    {
        const auto found = _mapped.find(holder + "/" + std::to_string(hash));
        if (found == _mapped.end()) {
            return "bad";
        }

        return matchesPattern(found->second, seedOf(hash)) ? "good" : "bad";
    }

    std::string maintain(bool scan)
    {
        _tier->maintainNow(scan);

        return "ok " + std::to_string(_tier->used()) + " " + std::to_string(_tier->budget());
    }

    std::string stats() const
    {
        return std::to_string(_tier->writeFailures()) + " " + std::to_string(_tier->warningCount()) + " " + std::to_string(_tier->publishedFiles());
    }

    void plantPartial()
    {
        const std::string dir = _root + "/tmp/" + _tier->sessionName();
        std::filesystem::create_directories(dir);
        std::ofstream(dir + "/planted-partial") << "torn";
    }

    long long benchMiss(U64 first,
                        int count,
                        bool forget)
    {
        std::vector<CacheDiskIdentity> identities;

        identities.reserve((std::size_t)count);
        for (int i = 0; i < count; ++i) {
            identities.push_back(Image::diskIdentityOfKey(keyOf(first + (U64)i)));
        }
        std::vector<CacheDiskFileInfo> candidates;
        const auto start = std::chrono::steady_clock::now();
        for (const CacheDiskIdentity& identity : identities) {
            if (forget) {
                _tier->forgetProbes();
            }
            candidates.clear();
            (void)_tier->probe(identity, NULL, &candidates);
        }

        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    }

    long long timedMaintain(bool scan)
    {
        const auto start = std::chrono::steady_clock::now();

        _tier->maintainNow(scan);

        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
    }

    CacheDiskTier& tier()
    {
        return *_tier;
    }

private:
    bool fileOf(U64 hash,
                CacheDiskFileInfo* info)
    {
        std::vector<CacheDiskFileInfo> candidates;

        _tier->forgetProbes();
        if (!_tier->probe(Image::diskIdentityOfKey(keyOf(hash)), NULL, &candidates) || candidates.empty()) {
            return false;
        }
        *info = candidates.front();

        return true;
    }

    bool pinOf(const std::string& holder,
               U64 hash,
               CacheDiskFileInfo* info)
    {
        std::vector<CacheDiskFileInfo> candidates;

        _tier->forgetProbes();
        if (!_tier->probePin(holder, Image::diskIdentityOfKey(pinKeyOf(holder, hash)), NULL, &candidates) || candidates.empty()) {
            return false;
        }
        *info = candidates.front();

        return true;
    }

    FakeCacheAPI _api;
    NullStorage _storage;
    CacheHostTier _host;
    CacheDiskTierPtr _tier;
    std::string _root;
    std::map<U64, std::unique_ptr<HeldRead>> _held;
    std::map<std::string, ImagePtr> _mapped;
};

std::string
optionOf(int argc,
         char** argv,
         const std::string& name)
{
    const std::string prefix = name + "=";

    for (int i = 0; i < argc; ++i) {
        const std::string argument(argv[i]);
        if (argument.compare(0, prefix.size(), prefix) == 0) {
            return argument.substr(prefix.size());
        }
    }

    return std::string();
}

} // anon namespace

int
runCacheDiskTierWorker(int argc,
                       char** argv)
{
    const std::string size = optionOf(argc, argv, "size");
    WorkerSession session(optionOf(argc, argv, "root"),
                          optionOf(argc, argv, "build"),
                          size.empty() ? kAmpleBudget : std::strtoull(size.c_str(), NULL, 10),
                          optionOf(argc, argv, "fallback"),
                          optionOf(argc, argv, "renamefails"));

    if (!session.hasTier()) {
        WorkerSession::reply("ready none");

        return 0;
    }
    WorkerSession::reply("ready " + session.sessionName());

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream in(line);
        std::string command;
        in >> command;
        if (command == "write") {
            U64 hash = 0;
            in >> hash;
            WorkerSession::reply(session.write(hash) ? "ok" : "refused");
        } else if (command == "read") {
            U64 hash = 0;
            in >> hash;
            WorkerSession::reply(session.read(hash));
        } else if (command == "scan") {
            U64 first = 0;
            int count = 0;
            in >> first >> count;
            int hits = 0;
            int bad = 0;
            for (int i = 0; i < count; ++i) {
                const std::string result = session.read(first + (U64)i);
                hits += (result == "hit") ? 1 : 0;
                bad += (result == "bad") ? 1 : 0;
            }
            WorkerSession::reply("hits " + std::to_string(hits) + " bad " + std::to_string(bad));
        } else if (command == "fill") {
            U64 first = 0;
            int count = 0;
            in >> first >> count;
            for (int i = 0; i < count; ++i) {
                (void)session.write(first + (U64)i);
                session.tier().maintainNow(false);
            }
            WorkerSession::reply("ok");
        } else if (command == "churn") {
            U64 first = 0;
            int span = 1;
            in >> first >> span;
            for (U64 i = 0;; ++i) {
                (void)session.write(first + i % (U64)span);
                WorkerSession::reply("w " + std::to_string(i));
            }
        } else if (command == "plant") {
            session.plantPartial();
            WorkerSession::reply("ok");
        } else if (command == "maintain") {
            int scan = 0;
            in >> scan;
            WorkerSession::reply(session.maintain(scan != 0));
        } else if (command == "hold-read") {
            U64 hash = 0;
            in >> hash;
            WorkerSession::reply(session.holdRead(hash));
        } else if (command == "hold-finish") {
            U64 hash = 0;
            in >> hash;
            WorkerSession::reply(session.holdFinish(hash));
        } else if (command == "clearauto") {
            session.tier().clearAuto();
            WorkerSession::reply("ok");
        } else if (command == "pin") {
            std::string holder;
            U64 hash = 0;
            in >> holder >> hash;
            WorkerSession::reply(session.pin(holder, hash));
        } else if (command == "haspin") {
            std::string holder;
            U64 hash = 0;
            in >> holder >> hash;
            WorkerSession::reply(session.hasPin(holder, hash) ? "yes" : "no");
        } else if (command == "hold") {
            std::string holder;
            in >> holder;
            session.tier().holdPins(holder);
            WorkerSession::reply("ok");
        } else if (command == "release") {
            std::string holder;
            in >> holder;
            session.tier().releaseHolder(holder);
            WorkerSession::reply("ok");
        } else if (command == "clearpins") {
            session.tier().clearPins();
            WorkerSession::reply("ok");
        } else if (command == "map") {
            std::string holder;
            U64 hash = 0;
            in >> holder >> hash;
            WorkerSession::reply(session.map(holder, hash));
        } else if (command == "checkmap") {
            std::string holder;
            U64 hash = 0;
            in >> holder >> hash;
            WorkerSession::reply(session.checkMap(holder, hash));
        } else if (command == "rlimit") {
            unsigned long long bytes = 0;
            in >> bytes;
            // A write over the limit raises SIGXFSZ, which ends the process unless it is ignored.
            (void)::signal(SIGXFSZ, SIG_IGN);
            struct rlimit limited;
            (void)::getrlimit(RLIMIT_FSIZE, &limited);
            limited.rlim_cur = (rlim_t)bytes;
            (void)::setrlimit(RLIMIT_FSIZE, &limited);
            WorkerSession::reply("ok");
        } else if (command == "stats") {
            WorkerSession::reply(session.stats());
        } else if (command == "bench-miss") {
            U64 first = 0;
            int count = 0;
            int forget = 0;
            in >> first >> count >> forget;
            WorkerSession::reply(std::to_string(session.benchMiss(first, count, forget != 0)));
        } else if (command == "timed-maintain") {
            int scan = 0;
            in >> scan;
            WorkerSession::reply(std::to_string(session.timedMaintain(scan != 0)));
        } else if (command == "quit") {
            break;
        } else {
            WorkerSession::reply("unknown " + command);
        }
    }
    session.shutdown();
    WorkerSession::reply("bye");

    return 0;
} // runCacheDiskTierWorker

namespace {

// A generous bound on one command, so a hung child fails the test rather than the whole run.
const int kReplyTimeoutMs = 120000;

class Worker {
public:
    Worker(const QString& root,
           const QString& build,
           const QStringList& options = QStringList())
    {
        QStringList arguments;
        arguments << QString::fromUtf8(cacheDiskTierWorkerFlag())
                  << QString::fromUtf8("root=") + root
                  << QString::fromUtf8("build=") + build;
        arguments << options;
        _process.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        _process.start(QCoreApplication::applicationFilePath(), arguments);
        if (!_process.waitForStarted(30000)) {
            return;
        }
        std::string first = reply();
        while (first.compare(0, 8, "warning ") == 0) {
            _warning += first.substr(8);
            first = reply();
        }
        if ((first.compare(0, 6, "ready ") == 0) && (first != "ready none")) {
            _session = first.substr(6);
        }
    }

    ~Worker()
    {
        if (_process.state() != QProcess::NotRunning) {
            _process.closeWriteChannel();
            if (!_process.waitForFinished(30000)) {
                _process.kill();
                _process.waitForFinished(30000);
            }
        }
    }

    bool started() const
    {
        return !_session.empty();
    }

    const std::string& session() const
    {
        return _session;
    }

    const std::string& warning() const
    {
        return _warning;
    }

    void send(const std::string& line)
    {
        const std::string withEnd = line + "\n";

        _process.write(withEnd.data(), (qint64)withEnd.size());
        _process.waitForBytesWritten(30000);
    }

    // The next reply of the session, or "dead" or "timeout".
    std::string reply()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kReplyTimeoutMs);

        for (;;) {
            while (_process.canReadLine()) {
                const std::string line = QString::fromUtf8(_process.readLine()).trimmed().toStdString();
                if (line.compare(0, 2, "@ ") == 0) {
                    return line.substr(2);
                }
            }
            if (_process.state() == QProcess::NotRunning) {
                return "dead";
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return "timeout";
            }
            _process.waitForReadyRead(500);
        }
    }

    std::string command(const std::string& line)
    {
        send(line);

        return reply();
    }

    void killHard()
    {
        // SIGKILL on Unix; waiting reaps it, so a peer sees it as dead rather than as a zombie.
        _process.kill();
        _process.waitForFinished(30000);
    }

    void quit()
    {
        send("quit");
        (void)reply();
        _process.closeWriteChannel();
        _process.waitForFinished(30000);
    }

private:
    QProcess _process;
    std::string _session;
    std::string _warning;
};

bool
exists(const std::string& path)
{
    struct stat st;

    return ::lstat(path.c_str(), &st) == 0;
}

void
ageByDays(const std::string& path,
          int days)
{
    struct timespec times[2];

    times[0].tv_sec = ::time(NULL) - (time_t)days * 24 * 3600;
    times[0].tv_nsec = 0;
    times[1] = times[0];
    ASSERT_EQ(0, ::utimensat(AT_FDCWD, path.c_str(), times, 0));
}

std::vector<std::string>
regularFilesUnder(const std::string& dir)
{
    std::vector<std::string> files;
    std::error_code ec;

    for (std::filesystem::recursive_directory_iterator it(dir, ec), end; !ec && (it != end); it.increment(ec)) {
        if (it->is_regular_file(ec)) {
            files.push_back(it->path().string());
        }
    }

    return files;
}

int
entryFilesUnder(const std::string& dir)
{
    int count = 0;

    for (const std::string& file : regularFilesUnder(dir)) {
        count += (std::filesystem::path(file).extension() == ".ntc") ? 1 : 0;
    }

    return count;
}

std::uint64_t
entryBytesUnder(const std::string& dir)
{
    std::uint64_t total = 0;

    for (const std::string& file : regularFilesUnder(dir)) {
        std::error_code ec;
        const std::uint64_t bytes = std::filesystem::file_size(file, ec);
        if (!ec && (std::filesystem::path(file).extension() == ".ntc")) {
            total += bytes;
        }
    }

    return total;
}

int
hitsOf(const std::string& scanReply)
{
    int hits = -1;
    int bad = -1;

    return (std::sscanf(scanReply.c_str(), "hits %d bad %d", &hits, &bad) == 2) ? hits : -1;
}

int
badOf(const std::string& scanReply)
{
    int hits = -1;
    int bad = -1;

    return (std::sscanf(scanReply.c_str(), "hits %d bad %d", &hits, &bad) == 2) ? bad : -1;
}

std::uint64_t
budgetOf(const std::string& maintainReply)
{
    unsigned long long used = 0;
    unsigned long long budget = 0;

    return (std::sscanf(maintainReply.c_str(), "ok %llu %llu", &used, &budget) == 2) ? budget : 0;
}

std::string
pinPathOf(const std::string& pinReply)
{
    const std::size_t space = pinReply.find(' ');

    return (space == std::string::npos) ? std::string() : pinReply.substr(space + 1);
}

std::string
directoryOf(const std::string& path)
{
    return std::filesystem::path(path).parent_path().string();
}

std::string
utf8(const QString& text)
{
    return text.toStdString();
}

} // anon namespace

TEST(CacheDiskTierProcessTest, PeersSeeEachOthersFilesWhileBothRun)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString root = dir.path();
    const QString build = QString::fromUtf8("build");

    Worker a(root, build);
    Worker b(root, build);
    ASSERT_TRUE(a.started());
    ASSERT_TRUE(b.started());

    EXPECT_EQ("ok", a.command("write 1"));
    EXPECT_EQ("hit", b.command("read 1"));
    EXPECT_EQ("ok", b.command("write 2"));
    EXPECT_EQ("hit", a.command("read 2"));
    EXPECT_EQ("miss", a.command("read 3"));

    a.quit();
    EXPECT_EQ("hit", b.command("read 1"));

    Worker c(root, build);
    ASSERT_TRUE(c.started());
    EXPECT_EQ("hit", c.command("read 2"));
}

// A kill can land anywhere in a write, so the planted temporary file is what makes the reaping
// checkable in every round rather than only when the kill happens to tear a write.
TEST(CacheDiskTierProcessTest, KilledWritersLeaveNoInvalidReadAndTheirTemporaryFilesAreReaped)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string root = utf8(dir.path());
    const QString build = QString::fromUtf8("build");
    const int kFirst = 1000;
    const int kSpan = 64;

    Worker survivor(dir.path(), build);
    ASSERT_TRUE(survivor.started());

    int hits = 0;
    for (int round = 0; round < 8; ++round) {
        Worker writer(dir.path(), build);
        ASSERT_TRUE(writer.started());
        const std::string name = writer.session();
        ASSERT_EQ("ok", writer.command("plant"));
        writer.send("churn " + std::to_string(kFirst) + " " + std::to_string(kSpan));
        for (int w = 0; w <= (round % 4); ++w) {
            ASSERT_EQ(0U, writer.reply().find("w ")) << "round " << round;
        }

        survivor.send("scan " + std::to_string(kFirst) + " " + std::to_string(kSpan));
        writer.killHard();
        const std::string during = survivor.reply();
        EXPECT_EQ(0, badOf(during)) << "round " << round << ": " << during;

        EXPECT_EQ(0U, survivor.command("maintain 1").find("ok")) << "round " << round;
        EXPECT_FALSE(exists(root + "/tmp/" + name)) << "round " << round;
        EXPECT_FALSE(exists(root + "/sessions/" + name)) << "round " << round;

        const std::string after = survivor.command("scan " + std::to_string(kFirst) + " " + std::to_string(kSpan));
        EXPECT_EQ(0, badOf(after)) << "round " << round << ": " << after;
        hits = std::max(hits, hitsOf(after));
    }
    EXPECT_GT(hits, 0);

    for (const std::string& file : regularFilesUnder(root + "/build/auto")) {
        EXPECT_EQ(".ntc", std::filesystem::path(file).extension().string()) << file;
    }
}

TEST(CacheDiskTierProcessTest, UnlinkUnderAnActiveReadLeavesTheReadWhole)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string root = utf8(dir.path());
    const QString build = QString::fromUtf8("build");

    Worker a(dir.path(), build);
    Worker b(dir.path(), build);
    ASSERT_TRUE(a.started());
    ASSERT_TRUE(b.started());

    ASSERT_EQ("ok", a.command("write 5"));
    const std::string path = b.command("hold-read 5");
    ASSERT_EQ(0U, path.find(root)) << path;
    ASSERT_TRUE(exists(path));

    EXPECT_EQ("ok", a.command("clearauto"));
    EXPECT_EQ(0U, a.command("maintain 1").find("ok"));
    EXPECT_FALSE(exists(path));
    EXPECT_EQ(0, (int)regularFilesUnder(root + "/tmp/" + a.session()).size());

    EXPECT_EQ("good", b.command("hold-finish 5"));
    EXPECT_EQ("miss", b.command("read 5"));
}

TEST(CacheDiskTierProcessTest, UnlinkUnderAMappedPinLeavesTheMappingWhole)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString build = QString::fromUtf8("build");

    Worker a(dir.path(), build);
    Worker b(dir.path(), build);
    ASSERT_TRUE(a.started());
    ASSERT_TRUE(b.started());

    const std::string path = pinPathOf(a.command("pin held 6"));
    ASSERT_TRUE(exists(path)) << path;
    const std::string how = b.command("map held 6");
    ASSERT_TRUE((how == "mapped") || (how == "copied")) << how;
    if (how == "copied") {
        std::printf("The temporary directory is a network mount, so the pin was read, not mapped.\n");
    }

    EXPECT_EQ("ok", a.command("release held"));
    EXPECT_TRUE(exists(path));
    EXPECT_EQ("good", b.command("checkmap held 6"));
    EXPECT_EQ("ok", b.command("release held"));
    EXPECT_FALSE(exists(path));
    EXPECT_FALSE(exists(directoryOf(path)));
    EXPECT_EQ("good", b.command("checkmap held 6"));
}

// Between refreshes the ledgers are stale, so free-running writers may pass the budget for a
// moment by design. The budget is a promise at the points where every ledger has been read, which
// sequential maintenance of all four reaches.
TEST(CacheDiskTierProcessTest, FourWritersStayWithinTheBudget)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string autoDir = utf8(dir.path()) + "/build/auto";
    const std::uint64_t kBudget = 4ULL * 1024 * 1024;
    const QString build = QString::fromUtf8("build");
    const QStringList options = { QString::fromUtf8("size=") + QString::number((qulonglong)kBudget) };

    std::vector<std::unique_ptr<Worker>> writers;
    for (int w = 0; w < 4; ++w) {
        writers.emplace_back(new Worker(dir.path(), build, options));
        ASSERT_TRUE(writers.back()->started());
    }

    const auto maintainAll = [&writers](const std::string& line) {
        std::uint64_t budget = 0;
        for (const std::unique_ptr<Worker>& writer : writers) {
            budget = budgetOf(writer->command(line));
        }

        return budget;
    };

    std::uint64_t peak = 0;
    for (int round = 0; round < 48; ++round) {
        for (int w = 0; w < 4; ++w) {
            writers[w]->send("write " + std::to_string(1000 * (w + 1) + round));
        }
        for (const std::unique_ptr<Worker>& writer : writers) {
            ASSERT_EQ("ok", writer->reply()) << "round " << round;
        }
        const std::uint64_t budget = maintainAll("maintain 0");
        ASSERT_GT(budget, 0U);
        ASSERT_LE(budget, kBudget);
        const std::uint64_t onDisk = entryBytesUnder(autoDir);
        EXPECT_LE(onDisk, budget) << "round " << round;
        peak = std::max(peak, onDisk);
    }
    EXPECT_GT(peak, kBudget / 2);

    std::uint64_t freePeak = 0;
    for (int w = 0; w < 4; ++w) {
        writers[w]->send("fill " + std::to_string(5000 + w * 60) + " 120");
    }
    for (const std::unique_ptr<Worker>& writer : writers) {
        freePeak = std::max(freePeak, entryBytesUnder(autoDir));
        ASSERT_EQ("ok", writer->reply());
    }
    std::printf("Four free-running writers reached %llu bytes against a budget of %llu.\n",
                (unsigned long long)freePeak, (unsigned long long)kBudget);

    std::uint64_t budget = 0;
    for (int pass = 0; pass < 64; ++pass) {
        budget = maintainAll("maintain 1");
        if (entryBytesUnder(autoDir) <= budget) {
            break;
        }
    }
    EXPECT_LE(entryBytesUnder(autoDir), budget);

    for (int w = 0; w < 4; ++w) {
        const std::string scan = writers[w]->command("scan " + std::to_string(5000 + w * 60) + " 120");
        EXPECT_EQ(0, badOf(scan)) << scan;
    }
}

TEST(CacheDiskTierProcessTest, TwoBuildsCoexistAndAnAbandonedOneIsReclaimed)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string root = utf8(dir.path());
    const QString buildA = QString::fromUtf8("build-a");
    const QString buildB = QString::fromUtf8("build-b");
    const QString buildC = QString::fromUtf8("build-c");

    std::unique_ptr<Worker> a(new Worker(dir.path(), buildA));
    std::unique_ptr<Worker> b(new Worker(dir.path(), buildB));
    Worker c(dir.path(), buildC);
    ASSERT_TRUE(a->started());
    ASSERT_TRUE(b->started());
    ASSERT_TRUE(c.started());

    ASSERT_EQ("ok", a->command("write 1"));
    ASSERT_EQ(0U, a->command("pin kept 2").find("0 "));
    ASSERT_EQ("ok", b->command("write 3"));

    EXPECT_EQ("hit", a->command("read 1"));
    EXPECT_EQ("hit", b->command("read 3"));
    EXPECT_EQ("miss", a->command("read 3"));
    EXPECT_EQ("miss", b->command("read 1"));
    EXPECT_EQ("miss", c.command("read 1"));
    EXPECT_EQ("no", b->command("haspin kept 2"));

    EXPECT_EQ(0U, a->command("maintain 0").find("ok"));
    EXPECT_EQ(0U, b->command("maintain 0").find("ok"));
    EXPECT_EQ(0U, c.command("maintain 0").find("ok"));

    // A recently live build outlasts all its sessions.
    a->killHard();
    a.reset();
    EXPECT_EQ(0U, b->command("maintain 1").find("ok"));
    EXPECT_TRUE(exists(root + "/build-a"));
    {
        Worker again(dir.path(), buildA);
        ASSERT_TRUE(again.started());
        EXPECT_EQ("hit", again.command("read 1"));
        EXPECT_EQ("yes", again.command("haspin kept 2"));
        again.killHard();
    }

    // Age alone reclaims nothing while a session of that build lives.
    ageByDays(root + "/build-a/lastlive", 8);
    ageByDays(root + "/build-c/lastlive", 8);
    EXPECT_EQ(0U, b->command("maintain 1").find("ok"));
    EXPECT_FALSE(exists(root + "/build-a"));
    EXPECT_TRUE(exists(root + "/build-b"));
    EXPECT_TRUE(exists(root + "/build-c"));

    b->quit();
    b.reset();
    Worker restarted(dir.path(), buildB);
    ASSERT_TRUE(restarted.started());
    EXPECT_EQ("hit", restarted.command("read 3"));
}

TEST(CacheDiskTierProcessTest, DeletingANodeKeepsItsPinsOnlyWhileALivePeerHoldsThem)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString build = QString::fromUtf8("build");

    Worker a(dir.path(), build);
    ASSERT_TRUE(a.started());
    {
        Worker b(dir.path(), build);
        ASSERT_TRUE(b.started());

        // The writer deletes its node first.
        std::string path = pinPathOf(a.command("pin first 1"));
        ASSERT_TRUE(exists(path)) << path;
        EXPECT_EQ("ok", b.command("hold first"));
        EXPECT_EQ("ok", a.command("release first"));
        EXPECT_TRUE(exists(path));
        EXPECT_EQ("ok", b.command("release first"));
        EXPECT_FALSE(exists(path));
        EXPECT_FALSE(exists(directoryOf(path)));

        // The peer deletes its node first.
        path = pinPathOf(a.command("pin second 2"));
        ASSERT_TRUE(exists(path)) << path;
        EXPECT_EQ("ok", b.command("hold second"));
        EXPECT_EQ("ok", b.command("release second"));
        EXPECT_TRUE(exists(path));
        EXPECT_EQ("ok", a.command("release second"));
        EXPECT_FALSE(exists(path));

        // Nobody else holds it.
        path = pinPathOf(a.command("pin sole 3"));
        ASSERT_TRUE(exists(path)) << path;
        EXPECT_EQ("ok", a.command("release sole"));
        EXPECT_FALSE(exists(path));

        // A peer that exited cleanly keeps nothing.
        path = pinPathOf(a.command("pin left 4"));
        ASSERT_TRUE(exists(path)) << path;
        EXPECT_EQ("ok", b.command("hold left"));
        b.quit();
        EXPECT_EQ("ok", a.command("release left"));
        EXPECT_FALSE(exists(path));
    }
    {
        Worker killed(dir.path(), build);
        ASSERT_TRUE(killed.started());
        const std::string path = pinPathOf(a.command("pin gone 5"));
        ASSERT_TRUE(exists(path)) << path;
        EXPECT_EQ("ok", killed.command("hold gone"));
        killed.killHard();
        EXPECT_EQ("ok", a.command("release gone"));
        EXPECT_FALSE(exists(path));
    }
    {
        Worker writer(dir.path(), build);
        ASSERT_TRUE(writer.started());
        const std::string path = pinPathOf(writer.command("pin orphan 6"));
        ASSERT_TRUE(exists(path)) << path;
        writer.killHard();
        EXPECT_TRUE(exists(path));
        EXPECT_EQ("yes", a.command("haspin orphan 6"));
        const std::string how = a.command("map orphan 6");
        EXPECT_TRUE((how == "mapped") || (how == "copied")) << how;
        EXPECT_EQ("good", a.command("checkmap orphan 6"));
        EXPECT_EQ("ok", a.command("release orphan"));
        EXPECT_FALSE(exists(path));
    }
}

TEST(CacheDiskTierProcessTest, ClearingPinsTakesOnlyTheFilesThisSessionHolds)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString build = QString::fromUtf8("build");

    Worker a(dir.path(), build);
    Worker b(dir.path(), build);
    ASSERT_TRUE(a.started());
    ASSERT_TRUE(b.started());

    const std::string shared = pinPathOf(a.command("pin shared 1"));
    ASSERT_TRUE(exists(shared)) << shared;
    const std::string how = b.command("map shared 1");
    ASSERT_TRUE((how == "mapped") || (how == "copied")) << how;
    const std::string peers = pinPathOf(b.command("pin peers 2"));
    ASSERT_TRUE(exists(peers)) << peers;

    EXPECT_EQ("ok", a.command("clearpins"));
    EXPECT_EQ(0, entryFilesUnder(directoryOf(shared)));
    EXPECT_EQ(1, entryFilesUnder(directoryOf(peers)));
    EXPECT_EQ("good", b.command("checkmap shared 1"));

    EXPECT_EQ("ok", b.command("clearpins"));
    EXPECT_EQ(0, entryFilesUnder(directoryOf(peers)));
}

TEST(CacheDiskTierProcessTest, AFileSizeLimitGivesOneWarningAndNoFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string root = utf8(dir.path());
    const QString build = QString::fromUtf8("build");

    Worker writer(dir.path(), build);
    Worker peer(dir.path(), build);
    ASSERT_TRUE(writer.started());
    ASSERT_TRUE(peer.started());

    ASSERT_EQ("ok", writer.command("rlimit 4096"));
    (void)writer.command("write 1");
    (void)writer.command("write 2");
    EXPECT_EQ("2 1 0", writer.command("stats"));

    EXPECT_EQ("miss", peer.command("read 1"));
    EXPECT_EQ("miss", peer.command("read 2"));
    EXPECT_EQ(0, entryFilesUnder(root + "/build/auto"));
    EXPECT_EQ(0, (int)regularFilesUnder(root + "/tmp/" + writer.session()).size());
}

TEST(CacheDiskTierProcessTest, ARenameFailureTakesTheFallback)
{
    QTemporaryDir dir;
    QTemporaryDir fallbackDir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(fallbackDir.isValid());
    const QString build = QString::fromUtf8("build");
    const QString renameFails = QString::fromUtf8("renamefails=") + dir.path();

    Worker writer(dir.path(), build, { renameFails, QString::fromUtf8("fallback=") + fallbackDir.path() });
    ASSERT_TRUE(writer.started());
    EXPECT_NE(std::string::npos, writer.warning().find(utf8(fallbackDir.path()))) << writer.warning();
    EXPECT_EQ("ok", writer.command("write 1"));
    EXPECT_EQ(0, entryFilesUnder(utf8(dir.path())));

    Worker peer(fallbackDir.path(), build);
    ASSERT_TRUE(peer.started());
    EXPECT_EQ("hit", peer.command("read 1"));

    Worker ramOnly(dir.path(), build, { renameFails, QString::fromUtf8("fallback=") + dir.path() });
    EXPECT_FALSE(ramOnly.started());
    EXPECT_NE(std::string::npos, ramOnly.warning().find("RAM only")) << ramOnly.warning();
}

namespace {

const char* const kHashFileEnv = "NATRON_TEST_CACHE_TIER_HASH_FILE";

// Each process gets its own settings and caches, so nothing but the executables can tell them apart.
QProcessEnvironment
hashEnvironment(const QString& scratch,
                const QString& name,
                const QString& hashFile)
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString home = scratch + QLatin1Char('/') + name;

    (void)QDir().mkpath(home);
    env.insert(QString::fromUtf8("HOME"), home);
    env.insert(QString::fromUtf8("XDG_CONFIG_HOME"), home + QString::fromUtf8("/config"));
    env.insert(QString::fromUtf8("XDG_DATA_HOME"), home + QString::fromUtf8("/data"));
    env.insert(QString::fromUtf8("NATRON_DISK_CACHE_PATH"), home + QString::fromUtf8("/cache"));
    env.insert(QString::fromUtf8(kHashFileEnv), hashFile);

    return env;
}

std::string
fileText(const QString& path)
{
    QFile file(path);

    if (!file.open(QIODevice::ReadOnly)) {
        return std::string();
    }

    return QString::fromUtf8(file.readAll()).trimmed().toStdString();
}

const char* const kHashScript = "import os\n"
                                "source = app.createNode(\"Constant\")\n"
                                "blur = app.createNode(\"Blur\")\n"
                                "blur.connectInput(0, source)\n"
                                "blur.getParam(\"size\").set(7.0, 7.0)\n"
                                "with open(os.environ[\"NATRON_TEST_CACHE_TIER_HASH_FILE\"], \"w\") as f:\n"
                                "    f.write(\"%x\" % blur.getContentHash())\n";

// Runs each executable on the script at once and returns what they wrote. The exit code says
// nothing: a script without a writer ends in an error after it has done its work.
std::vector<std::string>
batchHashes(const std::vector<QString>& executables,
            const QString& scratch)
{
    const QString script = scratch + QString::fromUtf8("/hash.py");
    {
        QFile file(script);
        EXPECT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(kHashScript);
    }

    std::vector<std::unique_ptr<QProcess>> processes;
    std::vector<QString> files;
    for (std::size_t i = 0; i < executables.size(); ++i) {
        const QString name = QString::fromUtf8("batch-") + QString::number((qulonglong)i);
        files.push_back(scratch + QLatin1Char('/') + name + QString::fromUtf8(".hash"));
        processes.emplace_back(new QProcess);
        processes.back()->setProcessChannelMode(QProcess::ForwardedChannels);
        processes.back()->setProcessEnvironment(hashEnvironment(scratch, name, files.back()));
        processes.back()->start(executables[i], { QString::fromUtf8("-b"), script });
    }
    std::vector<std::string> hashes;
    for (std::size_t i = 0; i < processes.size(); ++i) {
        if (!processes[i]->waitForFinished(120000)) {
            processes[i]->kill();
            processes[i]->waitForFinished(30000);
        }
        hashes.push_back(fileText(files[i]));
    }

    return hashes;
}

std::string
testsProcessHash(const QString& scratch,
                 const QString& name)
{
    const QString file = scratch + QLatin1Char('/') + name + QString::fromUtf8(".hash");
    QProcess process;

    process.setProcessChannelMode(QProcess::ForwardedChannels);
    process.setProcessEnvironment(hashEnvironment(scratch, name, file));
    process.start(QCoreApplication::applicationFilePath(), { QString::fromUtf8("--gtest_filter=CacheDiskTierProcessHashChild.*") });
    EXPECT_TRUE(process.waitForStarted(30000));
    EXPECT_TRUE(process.waitForFinished(120000));
    EXPECT_EQ(0, process.exitCode());

    return fileText(file);
}

class CacheDiskTierProcessHashChild
    : public BaseTest {
};

} // anon namespace

TEST_F(CacheDiskTierProcessHashChild, WriteTheContentHash)
{
    const char* const hashFile = std::getenv(kHashFileEnv);

    if (!hashFile || !hashFile[0]) {
        std::printf("Only runs as the child of the hash test.\n");

        return;
    }
    const NodePtr source = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
    const NodePtr blur = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR), PLUGIN_MAJOR_NATRON_BLUR);
    ASSERT_TRUE(source && blur);
    ASSERT_TRUE(blur->connectInput(source, 0));
    const std::shared_ptr<KnobDouble> size = std::dynamic_pointer_cast<KnobDouble>(blur->getKnobByName(kBlurParamSize));
    ASSERT_TRUE(size != nullptr);
    size->setValue(7., ViewSpec::all(), 0);
    size->setValue(7., ViewSpec::all(), 1);

    std::ofstream(hashFile) << std::hex << blur->getContentHash();
}

// Peers only find each other's files if they compute the same hashes. A plug-in's hash holds the
// stamp of the executables beside the running one, so the pair is hard-linked into one directory the
// way a release bundle ships it; the build tree keeps them apart and they would differ by design.
TEST(CacheDiskTierProcessTest, ProcessesOfOneBuildAgreeOnContentHashes)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const std::string first = testsProcessHash(dir.path(), QString::fromUtf8("tests-1"));
    const std::string second = testsProcessHash(dir.path(), QString::fromUtf8("tests-2"));
    ASSERT_FALSE(first.empty());
    EXPECT_EQ(first, second);

    const QString buildDir = QCoreApplication::applicationDirPath() + QString::fromUtf8("/..");
    const QString renderer = buildDir + QString::fromUtf8("/Renderer/NatronRenderer");
    const QString natron = buildDir + QString::fromUtf8("/App/Natron");
    if (!QFileInfo::exists(renderer)) {
        std::printf("No NatronRenderer beside the tests, so only the test executable was compared.\n");

        return;
    }

    // Inside the build tree, where a hard link stays on the same filesystem.
    QTemporaryDir pairDir(QCoreApplication::applicationDirPath() + QString::fromUtf8("/hash-pair-XXXXXX"));
    ASSERT_TRUE(pairDir.isValid());
    const QString linkedRenderer = pairDir.path() + QString::fromUtf8("/NatronRenderer");
    const QString linkedNatron = pairDir.path() + QString::fromUtf8("/Natron");
    const bool linked = QFileInfo::exists(natron) && (::link(QFile::encodeName(renderer).constData(), QFile::encodeName(linkedRenderer).constData()) == 0) && (::link(QFile::encodeName(natron).constData(), QFile::encodeName(linkedNatron).constData()) == 0);

    std::vector<QString> executables;
    if (linked) {
        executables = { linkedRenderer, linkedNatron };
    } else {
        std::printf("Natron could not be linked beside NatronRenderer, so the renderer was compared with itself.\n");
        executables = { renderer, renderer };
    }
    const std::vector<std::string> hashes = batchHashes(executables, dir.path());
    ASSERT_EQ(2U, hashes.size());
    ASSERT_FALSE(hashes[0].empty()) << "the first batch process wrote no hash";
    ASSERT_FALSE(hashes[1].empty()) << "the second batch process wrote no hash";
    EXPECT_EQ(hashes[0], hashes[1]);
}
