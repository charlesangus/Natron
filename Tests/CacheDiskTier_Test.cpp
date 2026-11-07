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

#include "Global/Macros.h"

#include <signal.h>
#include <sys/resource.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <list>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <QtCore/QByteArray>
#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QStringList>
#include <QtCore/QTemporaryDir>

#include <fcntl.h>
#include <sys/stat.h>

#include <gtest/gtest.h>

#include "Engine/AppManager.h"
#include "Engine/Cache.h"
#include "Engine/CacheDiskTier.h"
#include "Engine/CacheTier.h"
#include "Engine/ContentHasher.h"
#include "Engine/DeepImage.h"
#include "Engine/DeepImageCacheEntry.h"
#include "Engine/DeepImageKey.h"
#include "Engine/DeepImageParams.h"
#include "Engine/Image.h"
#include "Engine/ImageKey.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/ImageParams.h"
#include "Engine/Plugin.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kChildRootEnv = "NATRON_TEST_CACHE_DISK_TIER_ROOT";
const char* const kChildSuite = "CacheDiskTierInChildProcess";
const char* const kBuildId = "test-build";

const int kSide = 64;
const float kExpensiveNs = 1e9f;
// Under the per-file latency alone, so it never earns a write whatever the disk's speed.
const float kCheapNs = 1e6f;
const U64 kChildHash = 4242;
const float kChildSeed = 9.f;

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
        float costNs,
        float seed,
        const RectI& rendered)
{
    image->allocateMemory();
    fillPattern(image, seed);
    image->markForRendered(rendered);
    image->getEntryHeader()->recomputeCostNs.store(costNs);
    image->getEntryHeader()->lastUse.store(CacheEviction::nowNs());
}

template <typename Predicate>
bool
waitFor(Predicate done)
{
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);

    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    return true;
}

class Session {
public:
    explicit Session(const QString& root,
                     const char* name,
                     bool background = false,
                     const char* build = kBuildId,
                     int heartbeatStaleMs = 60000)
        : cache(name, 1, 64ULL * 1024 * 1024, 1.)
    {
        CacheDiskTier::Config config;
        config.root = root.toStdString();
        config.buildId = build;
        config.heartbeatStaleMs = heartbeatStaleMs;
        config.sizeBytes = 256ULL * 1024 * 1024;
        config.idlePublisher = background;
        config.backgroundMaintenance = background;
        config.publishIntervalMs = 10;
        tier = CacheDiskTier::open(config);
        cache.useEvictionScore();
        cache.setDiskTier(tier);
    }

    ImagePtr createEntry(U64 hash,
                         float costNs,
                         float seed)
    {
        ImagePtr image;

        if (cache.getOrCreate(keyOf(hash), paramsOf(kSide), 0, &image) || !image) {
            return ImagePtr();
        }
        prepare(image, costNs, seed, RectI(0, 0, kSide, kSide));

        return image;
    }

    // Everything unheld leaves the table. Against so small a budget the demotion queue admits one
    // entry at a time, so a test wanting several files calls this after each.
    void evictEverything()
    {
        // A write holds its entry and eviction skips a held entry, so one a lookup reclaimed
        // mid-write would otherwise stay in the table past this call.
        EXPECT_TRUE(tier->waitIdle(30000));
        cache.setMaximumCacheSize(1024);
        cache.setMaximumInMemorySize(1.);
        cache.clearExceedingEntries();
        EXPECT_TRUE(tier->waitIdle(30000));
        cache.setMaximumCacheSize(64ULL * 1024 * 1024);
        cache.setMaximumInMemorySize(1.);
    }

    bool fileOf(U64 hash,
                CacheDiskFileInfo* info)
    {
        std::vector<CacheDiskFileInfo> candidates;

        tier->forgetProbes();
        if (!tier->probe(Image::diskIdentityOfKey(keyOf(hash)), NULL, &candidates) || candidates.empty()) {
            return false;
        }
        *info = candidates.front();

        return true;
    }

    ImagePtr lookup(U64 hash)
    {
        std::list<ImagePtr> found;

        tier->forgetProbes();
        if (!cache.get<Image>(keyOf(hash), &found) || found.empty()) {
            return ImagePtr();
        }

        return found.front();
    }

    Cache<Image> cache;
    CacheDiskTierPtr tier;
};

// Writes one entry the way a demotion does, without it ever being in the tier's own cache, so that
// a test can offer the same key twice with different contents.
void
offerDetached(Session& session,
              Cache<Image>& owner,
              U64 hash,
              const RectI& rendered,
              float seed)
{
    ImagePtr image = std::make_shared<Image>(keyOf(hash), paramsOf(kSide), &owner);

    prepare(image, kExpensiveNs, seed, rendered);
    ASSERT_TRUE(session.tier->offerDemotion(image));
    image.reset();
    ASSERT_TRUE(session.tier->waitIdle(30000));
}

// A write hitting RLIMIT_FSIZE raises SIGXFSZ, which ends the process unless it is ignored.
class FileSizeLimit {
public:
    explicit FileSizeLimit(rlim_t bytes)
    {
        _previousHandler = ::signal(SIGXFSZ, SIG_IGN);
        ::getrlimit(RLIMIT_FSIZE, &_previous);
        struct rlimit limited = _previous;
        limited.rlim_cur = bytes;
        ::setrlimit(RLIMIT_FSIZE, &limited);
    }

    ~FileSizeLimit()
    {
        ::setrlimit(RLIMIT_FSIZE, &_previous);
        ::signal(SIGXFSZ, _previousHandler);
    }

private:
    struct rlimit _previous;
    sighandler_t _previousHandler;
};

} // anon namespace

TEST(CacheDiskTierTest, TinyRamBudgetDemotesTheExpensiveEntryAndDropsTheCheapOne)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Session session(dir.path(), "CacheDiskTierTest_Demotes");
    ASSERT_TRUE(session.tier != nullptr);

    ASSERT_TRUE(session.createEntry(1, kExpensiveNs, 1.f) != nullptr);
    ASSERT_TRUE(session.createEntry(2, kCheapNs, 2.f) != nullptr);
    session.evictEverything();

    CacheDiskFileInfo info;
    EXPECT_TRUE(session.fileOf(1, &info));
    EXPECT_FALSE(session.fileOf(2, &info));
    EXPECT_EQ(1U, session.tier->publishedFiles());
    EXPECT_FALSE(session.lookup(2) != nullptr);
}

TEST(CacheDiskTierTest, PromotionReturnsIdenticalPixelsAndStats)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Session session(dir.path(), "CacheDiskTierTest_Promotes");
    ASSERT_TRUE(session.tier != nullptr);

    ASSERT_TRUE(session.createEntry(1, kExpensiveNs, 3.f) != nullptr);
    session.evictEverything();

    const ImagePtr promoted = session.lookup(1);
    ASSERT_TRUE(promoted != nullptr);
    ASSERT_TRUE(promoted->isAllocated());
    EXPECT_TRUE(matchesPattern(promoted, 3.f));
    EXPECT_EQ(kExpensiveNs, promoted->getEntryHeader()->recomputeCostNs.load());
    EXPECT_TRUE(promoted->getEntryHeader()->residency.load() & CacheEntryHeader::eResidencyDisk);
    EXPECT_TRUE(promoted->getMinimalRect(RectI(0, 0, kSide, kSide)).isNull());
}

TEST(CacheDiskTierTest, PromotingAPartialEntryRestoresItsBitmap)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Session session(dir.path(), "CacheDiskTierTest_Partial");
    ASSERT_TRUE(session.tier != nullptr);

    const RectI rendered(0, 0, kSide, kSide / 2);
    std::vector<char> expected;
    {
        ImagePtr image;
        ASSERT_FALSE(session.cache.getOrCreate(keyOf(1), paramsOf(kSide), 0, &image));
        prepare(image, kExpensiveNs, 4.f, rendered);
        const char* const map = image->getEntryHeader()->bitmap.getBitmap();
        expected.assign(map, map + kSide * kSide);
    }
    session.evictEverything();

    const ImagePtr promoted = session.lookup(1);
    ASSERT_TRUE(promoted != nullptr);
    EXPECT_TRUE(matchesPattern(promoted, 4.f));
    EXPECT_EQ(0, std::memcmp(expected.data(), promoted->getEntryHeader()->bitmap.getBitmap(), expected.size()));
}

TEST(CacheDiskTierTest, DeepPayloadRoundTrips)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Session session(dir.path(), "CacheDiskTierTest_Deep");
    ASSERT_TRUE(session.tier != nullptr);

    const DeepImageKey key(NULL, 3, 1., ViewIdx(0), RenderScale::identity);
    const RectI bounds(0, 0, 4, 4);
    std::vector<U32> counts;
    std::vector<float> samples;
    {
        DeepImageCacheEntryPtr deep;
        ASSERT_FALSE(session.cache.getOrCreate(key, std::make_shared<DeepImageParams>(bounds), 0, &deep));
        const DeepImagePtr& image = deep->getDeepImage();
        SampleTable& table = image->getSampleTableForWriting();
        for (std::size_t i = 0; i < table.getPixelCount(); ++i) {
            table.setCount(i, (U32)(i % 3));
        }
        table.recomputeOffsets();
        float* const written = image->getChannelForWriting("A").dataForWriting();
        for (U64 i = 0; i < table.getTotalSampleCount(); ++i) {
            written[i] = (float)i * 0.5f;
        }
        image->setTidy(true);
        deep->allocateMemory();
        deep->getEntryHeader()->recomputeCostNs.store(kExpensiveNs);
        counts = table.getCounts();
        samples.assign(written, written + table.getTotalSampleCount());
    }
    session.evictEverything();

    session.tier->forgetProbes();
    std::list<DeepImageCacheEntryPtr> found;
    ASSERT_TRUE(session.cache.get<DeepImageCacheEntry>(key, &found));
    ASSERT_EQ(1U, found.size());
    const DeepImagePtr& promoted = found.front()->getDeepImage();
    EXPECT_TRUE(found.front()->isFullyRendered());
    EXPECT_TRUE(promoted->isTidy());
    EXPECT_EQ(counts, promoted->getSampleTable().getCounts());
    const DeepChannelBuffer& channel = promoted->getChannels().at("A");
    ASSERT_EQ(samples.size(), channel.size());
    EXPECT_EQ(0, std::memcmp(samples.data(), channel.data(), samples.size() * sizeof(float)));
}

TEST(CacheDiskTierTest, SecondProcessHitsAnEntryTheFirstPublished)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    {
        Session session(dir.path(), "CacheDiskTierTest_Parent");
        ASSERT_TRUE(session.tier != nullptr);
        ASSERT_TRUE(session.createEntry(kChildHash, kExpensiveNs, kChildSeed) != nullptr);
        session.evictEverything();
        ASSERT_EQ(1U, session.tier->publishedFiles());
    }

    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QString::fromUtf8(kChildRootEnv), dir.path());

    QProcess child;
    child.setProcessEnvironment(environment);
    child.setProcessChannelMode(QProcess::MergedChannels);
    child.start(QCoreApplication::applicationFilePath(),
                QStringList() << QString::fromUtf8("--gtest_filter=%1.*").arg(QString::fromUtf8(kChildSuite)));
    ASSERT_TRUE(child.waitForStarted(30000)) << child.errorString().toStdString();
    // The child loads every OFX bundle from a cold cache before it runs anything.
    if (!child.waitForFinished(240000)) {
        child.kill();
        child.waitForFinished(10000);
        FAIL() << "the child Tests process did not finish: " << child.readAll().toStdString();
    }
    const std::string output = child.readAll().toStdString();

    EXPECT_EQ(QProcess::NormalExit, child.exitStatus()) << output;
    EXPECT_EQ(0, child.exitCode()) << output;
    const std::string passed = std::string("[       OK ] ") + kChildSuite + ".";
    EXPECT_NE(std::string::npos, output.find(passed)) << "the child ran no test: " << output;
}

TEST(CacheDiskTierInChildProcess, HitsWhatTheParentPublished)
{
    // Every Tests process runs this case, but only the child started above has an entry to find.
    const QByteArray root = qgetenv(kChildRootEnv);
    if (root.isEmpty()) {
        return;
    }

    Session session(QString::fromUtf8(root), "CacheDiskTierTest_Child");
    ASSERT_TRUE(session.tier != nullptr);
    const ImagePtr promoted = session.lookup(kChildHash);
    ASSERT_TRUE(promoted != nullptr);
    EXPECT_TRUE(matchesPattern(promoted, kChildSeed));
}

TEST(CacheDiskTierTest, FileUnderAnotherKeysNameMisses)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Session session(dir.path(), "CacheDiskTierTest_Forged");
    ASSERT_TRUE(session.tier != nullptr);

    ASSERT_TRUE(session.createEntry(1, kExpensiveNs, 5.f) != nullptr);
    session.evictEverything();
    ASSERT_TRUE(session.createEntry(2, kExpensiveNs, 6.f) != nullptr);
    session.evictEverything();

    CacheDiskFileInfo first, second;
    ASSERT_TRUE(session.fileOf(1, &first));
    ASSERT_TRUE(session.fileOf(2, &second));
    ASSERT_TRUE(QFile::remove(QString::fromStdString(second.path)));
    ASSERT_TRUE(QFile::copy(QString::fromStdString(first.path), QString::fromStdString(second.path)));

    EXPECT_FALSE(session.fileOf(2, &second));
    EXPECT_FALSE(session.lookup(2) != nullptr);
    const ImagePtr genuine = session.lookup(1);
    ASSERT_TRUE(genuine != nullptr);
    EXPECT_TRUE(matchesPattern(genuine, 5.f));
}

TEST(CacheDiskTierTest, CorruptPayloadMissesAndLosesItsFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Session session(dir.path(), "CacheDiskTierTest_Corrupt");
    ASSERT_TRUE(session.tier != nullptr);

    ASSERT_TRUE(session.createEntry(1, kExpensiveNs, 7.f) != nullptr);
    session.evictEverything();

    CacheDiskFileInfo info;
    ASSERT_TRUE(session.fileOf(1, &info));
    QFile file(QString::fromStdString(info.path));
    ASSERT_TRUE(file.open(QIODevice::ReadWrite));
    ASSERT_TRUE(file.seek(file.size() - 1));
    const char flipped = (char)~file.peek(1).at(0);
    ASSERT_EQ(1, file.write(&flipped, 1));
    file.close();

    EXPECT_FALSE(session.lookup(1) != nullptr);
    EXPECT_FALSE(QFile::exists(QString::fromStdString(info.path)));
}

TEST(CacheDiskTierTest, ARemovedEntryLeavesNoFileHoweverFarItsWriteGot)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Session session(dir.path(), "CacheDiskTierTest_Retract");
    ASSERT_TRUE(session.tier != nullptr);

    const U64 kEntries = 32;
    for (U64 hash = 1; hash <= kEntries; ++hash) {
        ASSERT_TRUE(session.createEntry(hash, kExpensiveNs, (float)hash) != nullptr);
    }
    session.cache.setMaximumCacheSize(1024);
    session.cache.setMaximumInMemorySize(1.);
    session.cache.clearExceedingEntries();
    for (U64 hash = 1; hash <= kEntries; ++hash) {
        session.cache.removeEntryAndFiles<Image>(keyOf(hash));
    }
    ASSERT_TRUE(session.tier->waitIdle(30000));

    for (U64 hash = 1; hash <= kEntries; ++hash) {
        CacheDiskFileInfo info;
        EXPECT_FALSE(session.fileOf(hash, &info)) << hash;
    }
}

TEST(CacheDiskTierTest, WriteErrorDropsTheEntriesWithOneWarning)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Session session(dir.path(), "CacheDiskTierTest_WriteError");
    ASSERT_TRUE(session.tier != nullptr);

    {
        const FileSizeLimit limit(4096);
        ASSERT_TRUE(session.createEntry(1, kExpensiveNs, 8.f) != nullptr);
        session.evictEverything();
        ASSERT_TRUE(session.createEntry(2, kExpensiveNs, 9.f) != nullptr);
        session.evictEverything();
    }

    EXPECT_EQ(2U, session.tier->writeFailures());
    EXPECT_EQ(1U, session.tier->warningCount());
    EXPECT_FALSE(session.lookup(1) != nullptr);
    EXPECT_FALSE(session.lookup(2) != nullptr);
    EXPECT_EQ(0U, session.tier->publishedFiles());
}

TEST(CacheDiskTierTest, LargerResultReplacesASmallerOneAndNeverTheReverse)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Session session(dir.path(), "CacheDiskTierTest_Replace");
    ASSERT_TRUE(session.tier != nullptr);
    Cache<Image> owner("CacheDiskTierTest_ReplaceOwner", 1, 64ULL * 1024 * 1024, 1.);

    const std::uint64_t rowPixels = kSide;
    CacheDiskFileInfo info;

    offerDetached(session, owner, 1, RectI(0, 0, kSide, 2), 10.f);
    ASSERT_TRUE(session.fileOf(1, &info));
    EXPECT_EQ(2 * rowPixels, info.renderedPixels);

    offerDetached(session, owner, 1, RectI(0, 0, kSide, 8), 11.f);
    ASSERT_TRUE(session.fileOf(1, &info));
    EXPECT_EQ(8 * rowPixels, info.renderedPixels);

    offerDetached(session, owner, 1, RectI(0, 0, kSide, 4), 12.f);
    ASSERT_TRUE(session.fileOf(1, &info));
    EXPECT_EQ(8 * rowPixels, info.renderedPixels);

    const ImagePtr promoted = session.lookup(1);
    ASSERT_TRUE(promoted != nullptr);
    EXPECT_TRUE(matchesPattern(promoted, 11.f));
}

TEST(CacheDiskTierTest, LookupDuringADemotionReturnsTheEntry)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Session session(dir.path(), "CacheDiskTierTest_Reclaim");
    ASSERT_TRUE(session.tier != nullptr);

    for (int round = 0; round < 16; ++round) {
        const U64 hash = 100 + round;
        const float seed = (float)round;
        // Against this budget the queue takes one entry and drops the rest. The previous round's
        // entry is older, so it would be offered first and this one would never be demoted.
        session.evictEverything();
        ASSERT_TRUE(session.createEntry(hash, kExpensiveNs, seed) != nullptr);
        session.cache.setMaximumCacheSize(1024);
        session.cache.setMaximumInMemorySize(1.);
        session.cache.clearExceedingEntries();
        session.cache.setMaximumCacheSize(64ULL * 1024 * 1024);
        session.cache.setMaximumInMemorySize(1.);

        // A refused entry was never demoted, which is a different failure from losing a demotion.
        {
            std::vector<CacheEntryPtr> demoting;
            CacheDiskFileInfo info;
            session.tier->peekDemoting(keyOf(hash).getHash(), &demoting);
            ASSERT_TRUE(!demoting.empty() || session.fileOf(hash, &info)) << round;
        }

        // Whether this finds the entry queued, mid-write or published, the pixels must be the same.
        const ImagePtr found = session.lookup(hash);
        ASSERT_TRUE(found != nullptr) << round;
        EXPECT_TRUE(matchesPattern(found, seed)) << round;
    }
    EXPECT_TRUE(session.tier->waitIdle(30000));
}

TEST(CacheDiskTierTest, PeekReportsAFileAsAnUnallocatedStub)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Session session(dir.path(), "CacheDiskTierTest_Peek");
    ASSERT_TRUE(session.tier != nullptr);

    ASSERT_TRUE(session.createEntry(1, kExpensiveNs, 13.f) != nullptr);
    session.evictEverything();
    session.tier->forgetProbes();

    std::list<ImagePtr> peeked;
    ASSERT_TRUE(session.cache.peek<Image>(keyOf(1), &peeked));
    ASSERT_EQ(1U, peeked.size());
    EXPECT_FALSE(peeked.front()->isAllocated());
    EXPECT_EQ(RectI(0, 0, kSide, kSide), peeked.front()->getBounds());
    peeked.clear();

    std::list<ImagePtr> missing;
    EXPECT_FALSE(session.cache.peek<Image>(keyOf(2), &missing));
}

TEST(CacheDiskTierTest, IdlePublisherWritesBehindAndKeepsTheRamCopy)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Session session(dir.path(), "CacheDiskTierTest_Publisher", true);
    ASSERT_TRUE(session.tier != nullptr);

    ASSERT_TRUE(session.createEntry(1, kExpensiveNs, 14.f) != nullptr);
    const std::size_t resident = session.cache.getMemoryCacheSize();
    ASSERT_GT(resident, 0U);

    const CacheDiskTierPtr tier = session.tier;
    ASSERT_TRUE(waitFor([&tier]() { return tier->publishedFiles() >= 1; }));
    EXPECT_EQ(resident, session.cache.getMemoryCacheSize());

    CacheDiskFileInfo info;
    EXPECT_TRUE(session.fileOf(1, &info));
    session.cache.removeEntryAndFiles<Image>(keyOf(1));
    EXPECT_FALSE(session.fileOf(1, &info));
}

TEST(CacheDiskTierTest, EvictionBringsTheTierUnderItsBudget)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Session session(dir.path(), "CacheDiskTierTest_Eviction");
    ASSERT_TRUE(session.tier != nullptr);

    const U64 kEntries = 8;
    for (U64 hash = 1; hash <= kEntries; ++hash) {
        ASSERT_TRUE(session.createEntry(hash, kExpensiveNs, (float)hash) != nullptr);
        session.evictEverything();
    }
    session.tier->maintainNow(true);
    const std::size_t full = session.tier->used();
    ASSERT_GT(full, 0U);

    session.tier->setSizeBytes(full / 2);
    session.tier->maintainNow(true);
    EXPECT_LE(session.tier->used(), full / 2);

    int survivors = 0;
    for (U64 hash = 1; hash <= kEntries; ++hash) {
        CacheDiskFileInfo info;
        survivors += session.fileOf(hash, &info) ? 1 : 0;
    }
    EXPECT_GT(survivors, 0);
    EXPECT_LT(survivors, (int)kEntries);
}

namespace {

bool
writeFile(const std::string& path,
          std::size_t bytes = 8192)
{
    std::error_code ignored;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ignored);
    std::ofstream out(path, std::ios::binary);
    out << std::string(bytes, 'x');

    return bool(out);
}

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

    times[0].tv_sec = ::time(NULL) - days * 24 * 3600;
    times[0].tv_nsec = 0;
    times[1] = times[0];
    ASSERT_EQ(0, ::utimensat(AT_FDCWD, path.c_str(), times, 0));
}

std::string
entryFileName(U64 index)
{
    char name[64];
    std::snprintf(name, sizeof(name), "%016llx-%016llx-%016llx.ntc", (unsigned long long)index, 0ULL, 0ULL);

    return name;
}

// A build whose session ended days ago, with files as a real one leaves them.
void
plantDeadBuild(const std::string& root,
               const std::string& build,
               int ageDays,
               int autoFiles,
               int pinFiles)
{
    const std::string dir = root + '/' + build;

    for (int i = 0; i < autoFiles; ++i) {
        const std::string name = entryFileName(0x100 + i);
        ASSERT_TRUE(writeFile(dir + "/auto/" + name.substr(0, 3) + '/' + name));
    }
    for (int i = 0; i < pinFiles; ++i) {
        ASSERT_TRUE(writeFile(dir + "/pins/project/" + entryFileName(0x200 + i)));
    }
    ASSERT_TRUE(writeFile(dir + "/lastlive", 0));
    ageByDays(dir + "/lastlive", ageDays);
}

std::vector<std::string>
listNames(const std::string& path)
{
    std::vector<std::string> names;
    std::error_code error;

    for (std::filesystem::directory_iterator it(path, error), end; !error && (it != end); it.increment(error)) {
        names.push_back(it->path().filename().string());
    }

    return names;
}

// This host's session name with a process that cannot exist, above the kernel's highest PID.
std::string
deadSessionNameLike(const std::string& live)
{
    const std::size_t startDash = live.rfind('-');
    const std::size_t pidDash = live.rfind('-', startDash - 1);

    return live.substr(0, pidDash) + "-4194305-1";
}

} // namespace

TEST(CacheDiskTierPersistenceTest, ARestartHitsWhicheverSessionStartedFirst)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Session earlier(dir.path(), "CacheDiskTierPersistence_Earlier");
    ASSERT_TRUE(earlier.tier != nullptr);
    {
        Session writer(dir.path(), "CacheDiskTierPersistence_Writer");
        ASSERT_TRUE(writer.tier != nullptr);
        ASSERT_TRUE(writer.createEntry(1, kExpensiveNs, 3.f) != nullptr);
        writer.evictEverything();
    }
    EXPECT_TRUE(earlier.lookup(1) != nullptr);

    Session later(dir.path(), "CacheDiskTierPersistence_Later");
    ASSERT_TRUE(later.tier != nullptr);
    later.tier->maintainNow(true);
    earlier.tier->maintainNow(true);
    EXPECT_TRUE(later.lookup(1) != nullptr);
}

TEST(CacheDiskTierPersistenceTest, TwoIdentitiesRunTogetherAndNeitherLosesFiles)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Session first(dir.path(), "CacheDiskTierPersistence_BuildA", false, "build-a");
    Session second(dir.path(), "CacheDiskTierPersistence_BuildB", false, "build-b");
    ASSERT_TRUE(first.tier != nullptr);
    ASSERT_TRUE(second.tier != nullptr);

    ASSERT_TRUE(first.createEntry(1, kExpensiveNs, 4.f) != nullptr);
    first.evictEverything();
    ASSERT_TRUE(second.createEntry(2, kExpensiveNs, 5.f) != nullptr);
    second.evictEverything();

    first.tier->maintainNow(true);
    second.tier->maintainNow(true);

    CacheDiskFileInfo info;
    EXPECT_TRUE(first.fileOf(1, &info));
    EXPECT_FALSE(first.fileOf(2, &info));
    EXPECT_TRUE(second.fileOf(2, &info));
    EXPECT_FALSE(second.fileOf(1, &info));
}

TEST(CacheDiskTierPersistenceTest, ABuildNoSessionHasUsedForAWeekIsReclaimedWithItsPins)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string root = dir.path().toStdString();
    plantDeadBuild(root, "old-build", 8, 2, 1);
    plantDeadBuild(root, "recent-build", 6, 2, 1);
    ASSERT_TRUE(writeFile(root + "/not-a-build/keep.txt"));
    ageByDays(root + "/not-a-build", 30);

    Session session(dir.path(), "CacheDiskTierPersistence_Reclaim");
    ASSERT_TRUE(session.tier != nullptr);
    session.tier->maintainNow(true);

    EXPECT_FALSE(exists(root + "/old-build"));
    EXPECT_TRUE(exists(root + "/recent-build/pins/project/" + entryFileName(0x200)));
    EXPECT_TRUE(exists(root + "/not-a-build/keep.txt"));
}

TEST(CacheDiskTierPersistenceTest, ALiveBuildIsNeverReclaimedHoweverOldItsStamp)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string root = dir.path().toStdString();
    Session peer(dir.path(), "CacheDiskTierPersistence_LivePeer", false, "live-build");
    ASSERT_TRUE(peer.tier != nullptr);
    ASSERT_TRUE(peer.createEntry(1, kExpensiveNs, 6.f) != nullptr);
    peer.evictEverything();
    ageByDays(root + "/live-build/lastlive", 30);

    Session session(dir.path(), "CacheDiskTierPersistence_LiveWatcher");
    ASSERT_TRUE(session.tier != nullptr);
    session.tier->maintainNow(true);

    CacheDiskFileInfo info;
    EXPECT_TRUE(peer.fileOf(1, &info));
}

TEST(CacheDiskTierPersistenceTest, PressureTakesTheOldestDeadBuildFirstPinsIncluded)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string root = dir.path().toStdString();
    plantDeadBuild(root, "older-build", 3, (int)CacheEviction::kBatch - 1, 1);
    plantDeadBuild(root, "newer-build", 1, 1, 0);

    Session session(dir.path(), "CacheDiskTierPersistence_Pressure");
    ASSERT_TRUE(session.tier != nullptr);
    ASSERT_TRUE(session.createEntry(1, kExpensiveNs, 7.f) != nullptr);
    session.evictEverything();

    session.tier->setSizeBytes(1);
    session.tier->maintainNow(true);

    EXPECT_FALSE(exists(root + "/older-build/pins/project/" + entryFileName(0x200)));
    const std::string survivor = entryFileName(0x100);
    EXPECT_FALSE(exists(root + "/older-build/auto/" + survivor.substr(0, 3) + '/' + survivor));
    EXPECT_TRUE(exists(root + "/newer-build/auto/" + survivor.substr(0, 3) + '/' + survivor));
    CacheDiskFileInfo info;
    EXPECT_TRUE(session.fileOf(1, &info));
}

TEST(CacheDiskTierPersistenceTest, ADeadSessionIsReapedOnceAndALiveOneIsLeftAlone)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string root = dir.path().toStdString();
    Session reaper(dir.path(), "CacheDiskTierPersistence_Reaper");
    Session peer(dir.path(), "CacheDiskTierPersistence_ReapPeer");
    ASSERT_TRUE(reaper.tier != nullptr);
    ASSERT_TRUE(peer.tier != nullptr);

    const std::string dead = deadSessionNameLike(peer.tier->sessionName());
    ASSERT_TRUE(writeFile(root + "/sessions/" + dead, 0));
    ASSERT_TRUE(writeFile(root + "/tmp/" + dead + "/partial"));

    reaper.tier->maintainNow(true);
    EXPECT_FALSE(exists(root + "/sessions/" + dead));
    EXPECT_FALSE(exists(root + "/tmp/" + dead));
    EXPECT_FALSE(exists(root + "/tmp/" + reaper.tier->sessionName() + "/claimed-" + dead));
    EXPECT_TRUE(exists(root + "/sessions/" + peer.tier->sessionName()));
    EXPECT_TRUE(exists(root + "/tmp/" + peer.tier->sessionName()));
}

TEST(CacheDiskTierPersistenceTest, ACleanExitLeavesItsLedgerForAPeerToClaim)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string root = dir.path().toStdString();
    std::string name;
    {
        Session ended(dir.path(), "CacheDiskTierPersistence_Ended");
        ASSERT_TRUE(ended.tier != nullptr);
        name = ended.tier->sessionName();
    }
    EXPECT_TRUE(exists(root + "/sessions/" + name));

    Session session(dir.path(), "CacheDiskTierPersistence_Claimer");
    ASSERT_TRUE(session.tier != nullptr);
    session.tier->maintainNow(true);
    EXPECT_FALSE(exists(root + "/sessions/" + name));
}

TEST(CacheDiskTierPersistenceTest, ASessionOnAnotherHostDiesWhenItsHeartbeatStops)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string root = dir.path().toStdString();
    Session session(dir.path(), "CacheDiskTierPersistence_Heartbeat", false, kBuildId, 10);
    ASSERT_TRUE(session.tier != nullptr);

    const std::string other = "some-other-host-0123456789abcdef-42-1000";
    ASSERT_TRUE(writeFile(root + "/sessions/" + other, 0));
    ASSERT_TRUE(writeFile(root + "/tmp/" + other + "/partial"));

    session.tier->maintainNow(true);
    EXPECT_TRUE(exists(root + "/tmp/" + other + "/partial"));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    session.tier->maintainNow(true);
    EXPECT_FALSE(exists(root + "/sessions/" + other));
    EXPECT_FALSE(exists(root + "/tmp/" + other));
}

TEST(CacheDiskTierPersistenceTest, ASessionReapedByMistakeRegistersAgainAndKeepsWriting)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string root = dir.path().toStdString();
    Session session(dir.path(), "CacheDiskTierPersistence_Mistaken");
    ASSERT_TRUE(session.tier != nullptr);
    const std::string name = session.tier->sessionName();

    ASSERT_EQ(0, ::unlink((root + "/sessions/" + name).c_str()));
    std::error_code ignored;
    std::filesystem::remove_all(root + "/tmp/" + name, ignored);

    session.tier->maintainNow(false);
    EXPECT_TRUE(exists(root + "/sessions/" + name));
    EXPECT_TRUE(exists(root + "/tmp/" + name));

    ASSERT_TRUE(session.createEntry(1, kExpensiveNs, 8.f) != nullptr);
    session.evictEverything();
    CacheDiskFileInfo info;
    EXPECT_TRUE(session.fileOf(1, &info));
    EXPECT_EQ(0U, session.tier->writeFailures());
}

TEST(CacheDiskTierPersistenceTest, ASaltedEntryIsNeverPublishedOrLookedFor)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Session session(dir.path(), "CacheDiskTierPersistence_Salted", true);
    ASSERT_TRUE(session.tier != nullptr);

    const U64 salted = ContentHasher::withSaltFlag(1, true);
    const ImagePtr saltedImage = session.createEntry(salted, kExpensiveNs, 9.f);
    ASSERT_TRUE(saltedImage != nullptr);
    EXPECT_FALSE(session.tier->offerDemotion(saltedImage));
    ASSERT_TRUE(session.createEntry(2, kExpensiveNs, 10.f) != nullptr);

    const CacheDiskTierPtr tier = session.tier;
    ASSERT_TRUE(waitFor([&tier]() { return tier->publishedFiles() >= 1; }));
    EXPECT_TRUE(tier->waitIdle(30000));
    EXPECT_EQ(1U, tier->publishedFiles());
    CacheDiskFileInfo info;
    EXPECT_TRUE(session.fileOf(2, &info));
    EXPECT_FALSE(session.fileOf(salted, &info));
    EXPECT_TRUE(Image::diskIdentityOfKey(keyOf(salted)).salted);
    EXPECT_FALSE(Image::diskIdentityOfKey(keyOf(2)).salted);
}

TEST(CacheDiskTierPersistenceTest, ASessionWithPublishOffReadsAndFillsNothing)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    {
        Session writer(dir.path(), "CacheDiskTierPersistence_PublishWriter");
        ASSERT_TRUE(writer.tier != nullptr);
        ASSERT_TRUE(writer.createEntry(1, kExpensiveNs, 11.f) != nullptr);
        writer.evictEverything();
    }
    CacheDiskTier::Config config;
    config.root = dir.path().toStdString();
    config.buildId = kBuildId;
    config.sizeBytes = 256ULL * 1024 * 1024;
    config.publish = false;
    config.backgroundMaintenance = false;
    const CacheDiskTierPtr reader = CacheDiskTier::open(config);
    ASSERT_TRUE(reader != nullptr);
    std::vector<CacheDiskFileInfo> candidates;
    EXPECT_TRUE(reader->probe(Image::diskIdentityOfKey(keyOf(1)), NULL, &candidates));

    Cache<Image> owner("CacheDiskTierPersistence_PublishOwner", 1, 64ULL * 1024 * 1024, 1.);
    const ImagePtr image = std::make_shared<Image>(keyOf(2), paramsOf(kSide), &owner);
    EXPECT_FALSE(reader->offerDemotion(image));
}

TEST(CacheDiskTierPersistenceTest, AFailedSelfTestFallsBackThenToRamOnlyWithOneWarning)
{
    QTemporaryDir dir;
    QTemporaryDir fallback;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(fallback.isValid());
    const std::string root = dir.path().toStdString();

    CacheDiskTier::Config config;
    config.root = root;
    config.buildId = kBuildId;
    config.sizeBytes = 256ULL * 1024 * 1024;
    config.backgroundMaintenance = false;
    config.renameSelfTestFailsAt = root;

    EXPECT_TRUE(CacheDiskTier::open(config) == nullptr);
    EXPECT_TRUE(listNames(root + "/sessions").empty());

    std::string warning;
    const CacheDiskTierPtr moved = CacheDiskTier::openWithFallback(config, fallback.path().toStdString(), &warning);
    ASSERT_TRUE(moved != nullptr);
    EXPECT_NE(std::string::npos, warning.find(fallback.path().toStdString()));

    warning.clear();
    EXPECT_TRUE(CacheDiskTier::openWithFallback(config, root, &warning) == nullptr);
    EXPECT_NE(std::string::npos, warning.find("RAM only"));

    config.renameSelfTestFailsAt.clear();
    warning.clear();
    const CacheDiskTierPtr direct = CacheDiskTier::openWithFallback(config, fallback.path().toStdString(), &warning);
    EXPECT_TRUE(direct != nullptr);
    EXPECT_TRUE(warning.empty());
}

TEST(CacheDiskTierPersistenceTest, ClearingInRamLeavesEveryFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Session session(dir.path(), "CacheDiskTierPersistence_ClearInRam");
    ASSERT_TRUE(session.tier != nullptr);
    ASSERT_TRUE(session.createEntry(1, kExpensiveNs, 12.f) != nullptr);
    session.evictEverything();
    ASSERT_TRUE(session.createEntry(2, kExpensiveNs, 13.f) != nullptr);

    session.cache.clearInRam();
    EXPECT_EQ(0U, session.cache.getMemoryCacheSize());
    CacheDiskFileInfo info;
    EXPECT_TRUE(session.fileOf(1, &info));
    EXPECT_TRUE(session.lookup(1) != nullptr);
}

TEST(CacheDiskTierPersistenceTest, BothExecutablesStampAlikeAndAShimDoesNotCount)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string bin = dir.path().toStdString();
    ASSERT_TRUE(writeFile(bin + "/Natron", 10));
    ASSERT_TRUE(writeFile(bin + "/Natron.bin", 1000));
    ASSERT_TRUE(writeFile(bin + "/NatronRenderer", 10));
    ASSERT_TRUE(writeFile(bin + "/NatronRenderer.bin", 2000));

    bool stamped = false;
    const U64 fromNatron = Plugin::executablesStamp(bin + "/Natron.bin", bin + "/Natron.bin", &stamped);
    EXPECT_TRUE(stamped);
    const U64 fromRenderer = Plugin::executablesStamp(bin + "/NatronRenderer.bin", bin + "/NatronRenderer.bin", &stamped);
    EXPECT_TRUE(stamped);
    EXPECT_EQ(fromNatron, fromRenderer);

    ASSERT_TRUE(writeFile(bin + "/NatronRenderer", 20));
    EXPECT_EQ(fromNatron, Plugin::executablesStamp(bin + "/Natron.bin", bin + "/Natron.bin", &stamped));

    ASSERT_TRUE(writeFile(bin + "/NatronRenderer.bin", 3000));
    EXPECT_NE(fromNatron, Plugin::executablesStamp(bin + "/Natron.bin", bin + "/Natron.bin", &stamped));
}

TEST(CacheDiskTierPersistenceTest, AnExecutableWithoutItsSiblingStampsItselfAndAMissingOneIsUnstamped)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string bin = dir.path().toStdString();
    ASSERT_TRUE(writeFile(bin + "/Natron", 1000));

    bool stamped = false;
    Plugin::executablesStamp(bin + "/Natron", bin + "/Natron", &stamped);
    EXPECT_TRUE(stamped);

    Plugin::executablesStamp(bin + "/gone", bin + "/gone", &stamped);
    EXPECT_FALSE(stamped);
}

namespace {

const std::uint64_t kAmplePinBudget = 1024ULL * 1024 * 1024;
const RectI kWholeImage(0, 0, kSide, kSide);

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
pinKeyOf(const PinHolder& holder,
         U64 hash)
{
    return Image::makeKey(&holder, hash, false, 1., ViewIdx(0), false, false).forEntry(0, ImageLayerDesc::getRGBAComponents(), eImageBitDepthFloat);
}

ImagePtr
renderPinned(Session& session,
             const PinHolder& holder,
             U64 hash,
             float seed,
             CacheDiskTier::PinOutcomeEnum* outcome,
             const RectI& rendered = kWholeImage)
{
    ImagePtr image;

    if (session.cache.getOrCreatePinned(pinKeyOf(holder, hash), paramsOf(kSide), &image) || !image) {
        return ImagePtr();
    }
    prepare(image, kExpensiveNs, seed, rendered);
    *outcome = session.tier->publishPin(holder.getCacheID(), image);

    return image;
}

ImagePtr
lookupPinned(Session& session,
             const PinHolder& holder,
             U64 hash)
{
    std::list<ImagePtr> found;

    session.tier->forgetProbes();
    if (!session.cache.getPinned<Image>(pinKeyOf(holder, hash), &found) || found.empty()) {
        return ImagePtr();
    }

    return found.front();
}

bool
hasPin(Session& session,
       const PinHolder& holder,
       U64 hash)
{
    std::vector<CacheDiskFileInfo> candidates;

    return session.tier->probePin(holder.getCacheID(), Image::diskIdentityOfKey(pinKeyOf(holder, hash)), NULL, &candidates) && !candidates.empty();
}

std::string
pinDirOf(const QString& root,
         const PinHolder& holder)
{
    return root.toStdString() + '/' + kBuildId + "/pins/" + holder.getCacheID();
}

std::string
onlyPinFileIn(const std::string& pinDir)
{
    std::string file;

    for (const std::string& name : listNames(pinDir)) {
        if ((name.size() > 4) && (name.compare(name.size() - 4, 4, ".ntc") == 0)) {
            EXPECT_TRUE(file.empty());
            file = pinDir + '/' + name;
        }
    }

    return file;
}

} // anon namespace

TEST(CacheDiskTierPinTest, APinnedOutputHitsWithItsPixelsAndBitmapInANewSession)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const PinHolder holder("holder-a");
    const RectI rendered(0, 0, kSide, kSide / 2);
    std::vector<char> expected;
    {
        Session first(dir.path(), "CacheDiskTierPin_RestoreFirst");
        ASSERT_TRUE(first.tier != nullptr);
        first.tier->setPinBudget(kAmplePinBudget);
        CacheDiskTier::PinOutcomeEnum outcome = CacheDiskTier::ePinOutcomeFailed;
        const ImagePtr image = renderPinned(first, holder, 1, 5.f, &outcome, rendered);
        ASSERT_TRUE(image != nullptr);
        EXPECT_EQ(CacheDiskTier::ePinOutcomePublished, outcome);
        const char* const map = image->getEntryHeader()->bitmap.getBitmap();
        expected.assign(map, map + kSide * kSide);
    }

    Session second(dir.path(), "CacheDiskTierPin_RestoreSecond");
    ASSERT_TRUE(second.tier != nullptr);
    const ImagePtr restored = lookupPinned(second, holder, 1);
    ASSERT_TRUE(restored != nullptr);
    EXPECT_TRUE(matchesPattern(restored, 5.f));
    EXPECT_EQ(0, std::memcmp(expected.data(), restored->getEntryHeader()->bitmap.getBitmap(), expected.size()));
    EXPECT_TRUE(restored->isPinnedBy(holder.getCacheID()));
    EXPECT_EQ(!second.tier->isNetworkMount(), restored->isFileMapped());
}

TEST(CacheDiskTierPinTest, OverfillingTheAutomaticTierLeavesPinsAndARestartRestoresThem)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const PinHolder holder("holder-a");
    const U64 kPinnedHash = 1000;
    std::uint64_t pinned = 0;
    {
        Session session(dir.path(), "CacheDiskTierPin_OverfillFirst");
        ASSERT_TRUE(session.tier != nullptr);
        session.tier->setPinBudget(kAmplePinBudget);
        {
            CacheDiskTier::PinOutcomeEnum outcome = CacheDiskTier::ePinOutcomeFailed;
            ASSERT_TRUE(renderPinned(session, holder, kPinnedHash, 11.f, &outcome) != nullptr);
            ASSERT_EQ(CacheDiskTier::ePinOutcomePublished, outcome);
        }
        session.evictEverything();
        CacheDiskFileInfo info;
        EXPECT_FALSE(session.fileOf(kPinnedHash, &info));
        pinned = session.tier->pinnedBytes();
        ASSERT_GT(pinned, 0U);
        session.tier->maintainNow(true);
        EXPECT_EQ(0U, session.tier->used());

        const U64 kEntries = 8;
        for (U64 hash = 1; hash <= kEntries; ++hash) {
            ASSERT_TRUE(session.createEntry(hash, kExpensiveNs, (float)hash) != nullptr);
            session.evictEverything();
        }
        session.tier->maintainNow(true);
        const std::size_t full = session.tier->used();
        ASSERT_GT(full, 0U);
        session.tier->setSizeBytes(full / 2);
        session.tier->maintainNow(true);
        EXPECT_LE(session.tier->used(), full / 2);
        EXPECT_EQ(pinned, session.tier->pinnedBytes());

        const ImagePtr image = lookupPinned(session, holder, kPinnedHash);
        ASSERT_TRUE(image != nullptr);
        EXPECT_TRUE(matchesPattern(image, 11.f));
    }

    Session restarted(dir.path(), "CacheDiskTierPin_OverfillRestarted");
    ASSERT_TRUE(restarted.tier != nullptr);
    EXPECT_EQ(pinned, restarted.tier->pinnedBytes());
    const ImagePtr image = lookupPinned(restarted, holder, kPinnedHash);
    ASSERT_TRUE(image != nullptr);
    EXPECT_TRUE(matchesPattern(image, 11.f));
}

TEST(CacheDiskTierPinTest, DeletingANodeKeepsAPeersPinsAndDeletingItInBothRemovesThem)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const PinHolder holder("holder-a");
    Session a(dir.path(), "CacheDiskTierPin_DeleteA");
    Session b(dir.path(), "CacheDiskTierPin_DeleteB");
    ASSERT_TRUE(a.tier != nullptr);
    ASSERT_TRUE(b.tier != nullptr);
    a.tier->setPinBudget(kAmplePinBudget);
    CacheDiskTier::PinOutcomeEnum outcome = CacheDiskTier::ePinOutcomeFailed;
    ASSERT_TRUE(renderPinned(a, holder, 1, 3.f, &outcome) != nullptr);
    ASSERT_EQ(CacheDiskTier::ePinOutcomePublished, outcome);

    const ImagePtr mapped = lookupPinned(b, holder, 1);
    ASSERT_TRUE(mapped != nullptr);

    a.tier->releaseHolder(holder.getCacheID());
    EXPECT_GT(b.tier->pinnedBytesOf(holder.getCacheID()), 0U);
    EXPECT_TRUE(hasPin(b, holder, 1));

    b.tier->releaseHolder(holder.getCacheID());
    EXPECT_EQ(0U, b.tier->pinnedBytesOf(holder.getCacheID()));
    EXPECT_FALSE(exists(pinDirOf(dir.path(), holder)));
    EXPECT_TRUE(matchesPattern(mapped, 3.f));
}

TEST(CacheDiskTierPinTest, ASessionThatExitedHoldsNoPins)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const PinHolder holder("holder-a");
    Session survivor(dir.path(), "CacheDiskTierPin_ExitedSurvivor");
    ASSERT_TRUE(survivor.tier != nullptr);
    survivor.tier->setPinBudget(kAmplePinBudget);
    CacheDiskTier::PinOutcomeEnum outcome = CacheDiskTier::ePinOutcomeFailed;
    ASSERT_TRUE(renderPinned(survivor, holder, 1, 3.f, &outcome) != nullptr);
    ASSERT_EQ(CacheDiskTier::ePinOutcomePublished, outcome);
    {
        Session exited(dir.path(), "CacheDiskTierPin_ExitedPeer");
        ASSERT_TRUE(exited.tier != nullptr);
        exited.tier->holdPins(holder.getCacheID());
    }

    survivor.tier->releaseHolder(holder.getCacheID());
    EXPECT_FALSE(exists(pinDirOf(dir.path(), holder)));
}

TEST(CacheDiskTierPinTest, ClearingDropsThisSessionsHoldersAndNoOthers)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const PinHolder held("holder-a");
    const PinHolder peers("holder-b");
    Session a(dir.path(), "CacheDiskTierPin_ClearA");
    Session b(dir.path(), "CacheDiskTierPin_ClearB");
    ASSERT_TRUE(a.tier != nullptr);
    ASSERT_TRUE(b.tier != nullptr);
    a.tier->setPinBudget(kAmplePinBudget);
    b.tier->setPinBudget(kAmplePinBudget);
    CacheDiskTier::PinOutcomeEnum outcome = CacheDiskTier::ePinOutcomeFailed;
    ASSERT_TRUE(renderPinned(a, held, 1, 3.f, &outcome) != nullptr);
    ASSERT_EQ(CacheDiskTier::ePinOutcomePublished, outcome);
    ASSERT_TRUE(renderPinned(b, peers, 2, 4.f, &outcome) != nullptr);
    ASSERT_EQ(CacheDiskTier::ePinOutcomePublished, outcome);
    const std::uint64_t peersBytes = b.tier->pinnedBytesOf(peers.getCacheID());

    const ImagePtr mappedByB = lookupPinned(b, held, 1);
    ASSERT_TRUE(mappedByB != nullptr);

    a.cache.clearPins();

    EXPECT_EQ(0U, a.tier->pinnedBytesOf(held.getCacheID()));
    EXPECT_EQ(peersBytes, a.tier->pinnedBytesOf(peers.getCacheID()));
    EXPECT_TRUE(lookupPinned(a, held, 1) == nullptr);
    EXPECT_TRUE(hasPin(b, peers, 2));
    EXPECT_TRUE(matchesPattern(mappedByB, 3.f));
}

TEST(CacheDiskTierPinTest, AFullBudgetDropsTheHoldersOtherHashesThenLeavesTheOutputInRamWithOneWarning)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const PinHolder holder("holder-a");
    const PinHolder other("holder-b");
    Session session(dir.path(), "CacheDiskTierPin_Budget");
    ASSERT_TRUE(session.tier != nullptr);
    session.tier->setPinBudget(kAmplePinBudget);

    CacheDiskTier::PinOutcomeEnum outcome = CacheDiskTier::ePinOutcomeFailed;
    ASSERT_TRUE(renderPinned(session, holder, 1, 1.f, &outcome) != nullptr);
    ASSERT_EQ(CacheDiskTier::ePinOutcomePublished, outcome);
    const std::uint64_t onePin = session.tier->pinnedBytes();
    ASSERT_GT(onePin, 0U);
    session.tier->setPinBudget(onePin + onePin / 2);

    ASSERT_TRUE(renderPinned(session, holder, 2, 2.f, &outcome) != nullptr);
    EXPECT_EQ(CacheDiskTier::ePinOutcomePublished, outcome);
    EXPECT_FALSE(hasPin(session, holder, 1));
    EXPECT_TRUE(hasPin(session, holder, 2));
    EXPECT_EQ(onePin, session.tier->pinnedBytes());
    EXPECT_EQ(0U, session.tier->pinBudgetWarnings());

    const ImagePtr inRam = renderPinned(session, other, 3, 3.f, &outcome);
    ASSERT_TRUE(inRam != nullptr);
    EXPECT_EQ(CacheDiskTier::ePinOutcomeDoesNotFit, outcome);
    EXPECT_FALSE(inRam->isPinnedBy(other.getCacheID()));
    EXPECT_TRUE(matchesPattern(inRam, 3.f));
    EXPECT_TRUE(hasPin(session, holder, 2));
    EXPECT_EQ(1U, session.tier->pinBudgetWarnings());

    ASSERT_TRUE(renderPinned(session, other, 4, 4.f, &outcome) != nullptr);
    EXPECT_EQ(CacheDiskTier::ePinOutcomeDoesNotFit, outcome);
    EXPECT_EQ(1U, session.tier->pinBudgetWarnings());
}

TEST(CacheDiskTierPinTest, ExtendingAPinWritesANewFileAndPeersKeepTheOldPixels)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const PinHolder holder("holder-a");
    Session writer(dir.path(), "CacheDiskTierPin_ExtendWriter");
    Session peer(dir.path(), "CacheDiskTierPin_ExtendPeer");
    Session extender(dir.path(), "CacheDiskTierPin_ExtendExtender");
    ASSERT_TRUE(writer.tier != nullptr);
    ASSERT_TRUE(peer.tier != nullptr);
    ASSERT_TRUE(extender.tier != nullptr);
    writer.tier->setPinBudget(kAmplePinBudget);
    extender.tier->setPinBudget(kAmplePinBudget);

    CacheDiskTier::PinOutcomeEnum outcome = CacheDiskTier::ePinOutcomeFailed;
    ASSERT_TRUE(renderPinned(writer, holder, 1, 6.f, &outcome, RectI(0, 0, kSide, kSide / 2)) != nullptr);
    ASSERT_EQ(CacheDiskTier::ePinOutcomePublished, outcome);

    const std::string file = onlyPinFileIn(pinDirOf(dir.path(), holder));
    ASSERT_FALSE(file.empty());
    const int oldFile = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
    ASSERT_GE(oldFile, 0);
    struct stat before;
    ASSERT_EQ(0, ::fstat(oldFile, &before));

    const ImagePtr mappedByPeer = lookupPinned(peer, holder, 1);
    const ImagePtr extended = lookupPinned(extender, holder, 1);
    ASSERT_TRUE(mappedByPeer != nullptr);
    ASSERT_TRUE(extended != nullptr);

    fillPattern(extended, 7.f);
    extended->markForRendered(kWholeImage);
    EXPECT_EQ(CacheDiskTier::ePinOutcomePublished, extender.tier->publishPin(holder.getCacheID(), extended));

    struct stat oldAfter;
    struct stat current;
    ASSERT_EQ(0, ::fstat(oldFile, &oldAfter));
    ::close(oldFile);
    ASSERT_EQ(0, ::stat(onlyPinFileIn(pinDirOf(dir.path(), holder)).c_str(), &current));
    EXPECT_NE(before.st_ino, current.st_ino);
    EXPECT_EQ(before.st_size, oldAfter.st_size);
    EXPECT_EQ(0U, (unsigned)oldAfter.st_nlink);
    EXPECT_TRUE(matchesPattern(mappedByPeer, 6.f));

    Session later(dir.path(), "CacheDiskTierPin_ExtendLater");
    ASSERT_TRUE(later.tier != nullptr);
    const ImagePtr restored = lookupPinned(later, holder, 1);
    ASSERT_TRUE(restored != nullptr);
    EXPECT_TRUE(matchesPattern(restored, 7.f));
}
