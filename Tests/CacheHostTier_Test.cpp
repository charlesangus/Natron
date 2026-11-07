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

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "BaseTest.h"
#include "CacheStress.h"

#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/CacheTier.h"
#include "Engine/Format.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Project.h"
#include "Engine/Settings.h"

NATRON_NAMESPACE_USING

namespace {

using namespace CacheStress;

const RectI kWindow(0, 0, 32, 32);

class NoStorage
    : public CacheHostStorage {
public:
    virtual bool dropFromHost(const CacheEntryPtr& /*entry*/) const override
    {
        return false;
    }

    virtual CacheEntryPtr demotionCandidate() const override
    {
        return CacheEntryPtr();
    }
};

struct TierReading {
    std::size_t used;
    std::size_t budget;
};

// used() includes the process-wide idle pool, and a deleter thread still finishing an earlier
// test can return buffers to it between two reads.
TierReading
readTier(const CacheHostTier& tier)
{
    for (;;) {
        TierReading reading;
        reading.used = tier.used();
        reading.budget = tier.budget();
        if (tier.used() == reading.used) {
            return reading;
        }
    }
}

std::size_t
expectedBudget(std::size_t limit,
               std::size_t used,
               std::int64_t headroom)
{
    const std::int64_t room = std::max((std::int64_t)0, (std::int64_t)used + headroom);

    return std::min(limit, (std::size_t)room);
}

// Entries leave the accounting on the cache's deleter thread, with no way to wait for it.
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

class KeepFreePercent {
public:
    explicit KeepFreePercent(int percent)
        : _knob(appPTR->getCurrentSettings()->getKnobByNameAndType<KnobInt>("unreachableRAMPercent"))
        , _previous(_knob ? _knob->getValue() : 0)
    {
        if (_knob) {
            _knob->setValue(percent);
        }
    }

    ~KeepFreePercent()
    {
        if (_knob) {
            _knob->setValue(_previous);
        }
    }

private:
    KnobIntPtr _knob;
    int _previous;
};

class TierLimit {
public:
    TierLimit(CacheHostTier& tier,
              std::size_t limit)
        : _tier(tier)
        , _previous(tier.limit())
    {
        _tier.setLimit(limit);
    }

    ~TierLimit()
    {
        _tier.setLimit(_previous);
    }

private:
    CacheHostTier& _tier;
    std::size_t _previous;
};

class CacheHostTierTest
    : public BaseTest {
protected:
    virtual void TearDown() override
    {
        getApp()->getProject()->reset(false, true);
        appPTR->clearAllCaches();
        BaseTest::TearDown();
    }

    NodePtr cachedConstant()
    {
        const AppInstancePtr app = getApp();
        const NodeCollectionPtr root = app->getProject();
        const NodePtr constant = createConstantIn(app, root, kWindow, { 0.25, 0.5, 0.75, 1. });
        if (!constant) {
            return constant;
        }
        // A node with no output is otherwise not cached.
        KnobBoolPtr forceCaching = std::dynamic_pointer_cast<KnobBool>(constant->getKnobByName("forceCaching"));
        if (!forceCaching) {
            return NodePtr();
        }
        forceCaching->setValue(true);

        return constant;
    }
};

TEST_F(CacheHostTierTest, BudgetIsTheLimitOrTheRoomTheSystemLeaves)
{
    const std::size_t limit = 1000;
    NoStorage storage;
    CacheHostTier tier(&storage, limit);

    tier.addBytes(400);
    EXPECT_EQ((std::size_t)400, tier.liveBytes());

    EXPECT_EQ(limit, readTier(tier).budget) << "with no reading of the system, only the limit binds";

    const struct
    {
        std::size_t available;
        std::size_t keepFree;
    } cases[] = {
        { 900, 200 },
        { 500, 200 },
        { 100, 200 },
        { 0, 1000 },
        { 200, 200 },
    };
    for (const auto& c : cases) {
        tier.setSystemMemory(c.available, c.keepFree);
        const TierReading reading = readTier(tier);
        EXPECT_EQ(expectedBudget(limit, reading.used, (std::int64_t)c.available - (std::int64_t)c.keepFree), reading.budget)
            << "available " << c.available << ", keep free " << c.keepFree << ", used " << reading.used;
    }

    tier.setSystemMemory(1u << 20, 0);
    EXPECT_EQ(limit, readTier(tier).budget);
    tier.setLimit(100);
    EXPECT_EQ((std::size_t)100, readTier(tier).budget);
}

TEST_F(CacheHostTierTest, LiveBytesSaturateAtZero)
{
    NoStorage storage;
    CacheHostTier tier(&storage, 1000);

    tier.addBytes(10);
    tier.removeBytes(4);
    EXPECT_EQ((std::size_t)6, tier.liveBytes());
    tier.removeBytes(100);
    EXPECT_EQ((std::size_t)0, tier.liveBytes());
}

TEST_F(CacheHostTierTest, DemotingNothingAndPromotingBelowTheLastTierRefuse)
{
    NoStorage storage;
    CacheHostTier tier(&storage, 1000);

    EXPECT_FALSE(tier.demote(CacheEntryPtr()));
    EXPECT_FALSE(tier.promote(CacheEntryPtr()));
    EXPECT_EQ((CacheTier*)NULL, tier.next());
    EXPECT_FALSE(bool(tier.demotionCandidate()));
}

TEST_F(CacheHostTierTest, FreeRamCheckFeedsTheNodeCacheBudget)
{
    CacheHostTier& tier = appPTR->getNodeCacheHostTier();
    const std::size_t noLimit = std::numeric_limits<std::size_t>::max();
    TierLimit limit(tier, noLimit);
    KeepFreePercent keepFree(90);

    appPTR->checkCacheFreeMemoryIsGoodEnough();

    // Keeping 90% of the machine free leaves far less than an unbounded limit, whatever is free.
    EXPECT_LT(tier.budget(), noLimit);
}

TEST_F(CacheHostTierTest, DemotingDropsAnUnheldEntryAndARenderRestoresIdenticalPixels)
{
    const AppInstancePtr app = getApp();
    clearEverything(app);

    CacheHostTier& tier = appPTR->getNodeCacheHostTier();
    ASSERT_TRUE(waitFor([&tier]() { return tier.liveBytes() == 0; }));

    app->getProject()->setOrAddProjectFormat(Format(0, 0, kWindow.x2, kWindow.y2, "cacheHostTier", 1.));
    const NodePtr constant = cachedConstant();
    ASSERT_TRUE(bool(constant));

    RenderedPlane reference;
    std::string error;
    ASSERT_TRUE(renderProbe(constant, 1., 0, kWindow, eRenderSchedulerModeLegacy, false, &reference, &error)) << error;

    const std::size_t rendered = tier.liveBytes();
    ASSERT_GT(rendered, (std::size_t)0);
    EXPECT_GE(tier.used(), rendered);

    CacheEntryPtr entry = tier.demotionCandidate();
    ASSERT_TRUE(bool(entry));

    {
        const CacheEntryPtr held = entry;
        EXPECT_FALSE(tier.demote(entry)) << "an entry someone holds stays";
        EXPECT_EQ(rendered, tier.liveBytes());
    }

    EXPECT_TRUE(tier.demote(entry));
    EXPECT_FALSE(tier.demote(entry)) << "the entry already left the tier";

    entry.reset();
    EXPECT_TRUE(waitFor([&tier, rendered]() { return tier.liveBytes() < rendered; }));

    RenderedPlane again;
    ASSERT_TRUE(renderProbe(constant, 1., 0, kWindow, eRenderSchedulerModeLegacy, false, &again, &error)) << error;
    EXPECT_EQ(std::string(), compareExact(reference, again));
    EXPECT_TRUE(bool(tier.demotionCandidate())) << "the render caches the entry again";
}

} // anon namespace
