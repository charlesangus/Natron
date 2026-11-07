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
#include <cstring>
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

#include <gtest/gtest.h>

#include "Engine/AppManager.h"
#include "Engine/Cache.h"
#include "Engine/CacheDiskTier.h"
#include "Engine/CacheTier.h"
#include "Engine/DeepImage.h"
#include "Engine/DeepImageCacheEntry.h"
#include "Engine/DeepImageKey.h"
#include "Engine/DeepImageParams.h"
#include "Engine/Image.h"
#include "Engine/ImageKey.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/ImageParams.h"
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
                     bool background = false)
        : cache(name, 1, 64ULL * 1024 * 1024, 1.)
    {
        CacheDiskTier::Config config;
        config.root = root.toStdString();
        config.buildId = kBuildId;
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
        ASSERT_TRUE(session.createEntry(hash, kExpensiveNs, seed) != nullptr);
        session.cache.setMaximumCacheSize(1024);
        session.cache.setMaximumInMemorySize(1.);
        session.cache.clearExceedingEntries();
        session.cache.setMaximumCacheSize(64ULL * 1024 * 1024);
        session.cache.setMaximumInMemorySize(1.);

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
