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

#include <cstdint>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Scene/GpuTextureBudget.h"

NATRON_NAMESPACE_USING

namespace {
const std::uint64_t kMB = std::uint64_t(1) << 20;

std::vector<GpuTextureBudget::Request>
requests(const std::vector<std::pair<std::string, std::uint64_t>>& textures)
{
    std::vector<GpuTextureBudget::Request> out;
    for (const std::pair<std::string, std::uint64_t>& texture : textures) {
        out.push_back(GpuTextureBudget::Request { texture.first, texture.second });
    }

    return out;
}
} // anonymous namespace

TEST(GpuTextureBudget, AdmitsUntilTheLimitThenRefuses)
{
    GpuTextureBudget budget(100 * kMB);
    const std::vector<bool> admitted = budget.claim(1, "/m", requests({ { "a", 60 * kMB }, { "b", 50 * kMB }, { "c", 40 * kMB } }));

    ASSERT_EQ(3u, admitted.size());
    EXPECT_TRUE(admitted[0]);
    EXPECT_FALSE(admitted[1]);
    EXPECT_TRUE(admitted[2]);
    EXPECT_EQ(100 * kMB, budget.getUsedBytes());
    EXPECT_EQ(1u, budget.getRefusedCount());
}

TEST(GpuTextureBudget, TextureLargerThanTheLimitIsRefused)
{
    GpuTextureBudget budget(10 * kMB);

    EXPECT_FALSE(budget.claim(1, "/m", requests({ { "a", 11 * kMB } }))[0]);
    EXPECT_EQ(0u, budget.getUsedBytes());
}

TEST(GpuTextureBudget, DecisionHoldsWhileClaimedAndIsSharedAcrossOwners)
{
    GpuTextureBudget budget(100 * kMB);
    EXPECT_TRUE(budget.claim(1, "/a", requests({ { "big", 80 * kMB } }))[0]);
    EXPECT_FALSE(budget.claim(1, "/b", requests({ { "other", 30 * kMB } }))[0]);

    budget.release(1, "/a");
    EXPECT_EQ(0u, budget.getUsedBytes());
    // Still refused: /b claims it, and the decision is not revisited while it does.
    EXPECT_FALSE(budget.claim(1, "/c", requests({ { "other", 30 * kMB } }))[0]);

    budget.release(1, "/b");
    budget.release(1, "/c");
    EXPECT_EQ(0u, budget.getRefusedCount());
    EXPECT_TRUE(budget.claim(1, "/d", requests({ { "other", 30 * kMB } }))[0]);
}

TEST(GpuTextureBudget, SharedTextureCountsOnce)
{
    GpuTextureBudget budget(100 * kMB);
    budget.claim(1, "/a", requests({ { "t", 40 * kMB } }));
    budget.claim(2, "/a", requests({ { "t", 40 * kMB } }));
    budget.claim(1, "/a", requests({ { "t", 40 * kMB } }));
    EXPECT_EQ(40 * kMB, budget.getUsedBytes());

    std::uint64_t bytes = 0;
    EXPECT_TRUE(budget.findBytes("t", &bytes));
    EXPECT_EQ(40 * kMB, bytes);
    EXPECT_FALSE(budget.findBytes("u", &bytes));

    budget.release(1, "/a");
    EXPECT_EQ(40 * kMB, budget.getUsedBytes());
    budget.release(2, "/a");
    EXPECT_EQ(0u, budget.getUsedBytes());
    EXPECT_FALSE(budget.findBytes("t", &bytes));
}

TEST(GpuTextureBudget, ClaimsAddUntilStaleThenReplace)
{
    GpuTextureBudget budget(100 * kMB);
    budget.claim(1, "/m", requests({ { "a", 10 * kMB } }));
    budget.claim(1, "/m", requests({ { "b", 20 * kMB } }));
    EXPECT_EQ(30 * kMB, budget.getUsedBytes());

    budget.markStale(1, "/m");
    budget.claim(1, "/m", requests({ { "b", 20 * kMB }, { "c", 5 * kMB } }));
    EXPECT_EQ(25 * kMB, budget.getUsedBytes());

    budget.claim(1, "/m", requests({ { "a", 10 * kMB } }));
    EXPECT_EQ(35 * kMB, budget.getUsedBytes());

    budget.markStale(1, "/m");
    budget.claim(1, "/m", requests({}));
    EXPECT_EQ(0u, budget.getUsedBytes());
}

TEST(GpuTextureBudget, StaleReplacementKeepsASharedTextureInPlace)
{
    GpuTextureBudget budget(50 * kMB);
    EXPECT_TRUE(budget.claim(1, "/m", requests({ { "a", 40 * kMB } }))[0]);
    budget.markStale(1, "/m");
    // Were "a" let go first, "b" would take its place and "a" would no longer fit.
    const std::vector<bool> admitted = budget.claim(1, "/m", requests({ { "a", 40 * kMB }, { "b", 20 * kMB } }));
    EXPECT_TRUE(admitted[0]);
    EXPECT_FALSE(admitted[1]);
}

TEST(GpuTextureBudget, ReleaseTakesThePathAndWhatIsUnderIt)
{
    GpuTextureBudget budget(1000 * kMB);
    budget.claim(1, "/a", requests({ { "1", kMB } }));
    budget.claim(1, "/a/b", requests({ { "2", kMB } }));
    budget.claim(1, "/a.b", requests({ { "3", kMB } }));
    budget.claim(1, "/ab", requests({ { "4", kMB } }));
    budget.claim(2, "/a", requests({ { "5", kMB } }));

    budget.release(1, "/a");
    EXPECT_EQ(3 * kMB, budget.getUsedBytes());
    std::uint64_t bytes = 0;
    EXPECT_FALSE(budget.findBytes("1", &bytes));
    EXPECT_FALSE(budget.findBytes("2", &bytes));
    EXPECT_TRUE(budget.findBytes("3", &bytes));
    EXPECT_TRUE(budget.findBytes("4", &bytes));
    EXPECT_TRUE(budget.findBytes("5", &bytes));

    budget.release(1, "/");
    EXPECT_EQ(kMB, budget.getUsedBytes());
    budget.releaseScope(2);
    EXPECT_EQ(0u, budget.getUsedBytes());
}

TEST(GpuTextureBudget, MarkStaleCoversDescendants)
{
    GpuTextureBudget budget(1000 * kMB);
    budget.claim(1, "/a/m", requests({ { "1", kMB } }));
    budget.claim(1, "/a.m", requests({ { "2", kMB } }));
    budget.markStale(1, "/a");
    budget.claim(1, "/a/m", requests({}));
    budget.claim(1, "/a.m", requests({}));
    EXPECT_EQ(kMB, budget.getUsedBytes());
}

TEST(GpuTextureBudget, ReportsOnceUntilMoreAreRefusedAndAgainAfterAllAreForgotten)
{
    GpuTextureBudget budget(10 * kMB);
    std::size_t refused = 0;
    EXPECT_FALSE(budget.takeReport(&refused));

    budget.claim(1, "/a", requests({ { "a", 20 * kMB }, { "b", 20 * kMB } }));
    EXPECT_TRUE(budget.takeReport(&refused));
    EXPECT_EQ(2u, refused);
    EXPECT_FALSE(budget.takeReport(&refused));

    budget.claim(1, "/b", requests({ { "c", 20 * kMB } }));
    EXPECT_TRUE(budget.takeReport(&refused));
    EXPECT_EQ(3u, refused);

    budget.release(1, "/b");
    EXPECT_FALSE(budget.takeReport(&refused));

    budget.releaseScope(1);
    EXPECT_EQ(0u, budget.getRefusedCount());
    budget.claim(2, "/a", requests({ { "a", 20 * kMB } }));
    EXPECT_TRUE(budget.takeReport(&refused));
    EXPECT_EQ(1u, refused);
}

TEST(GpuTextureBudget, NewLimitAppliesToLaterClaims)
{
    GpuTextureBudget budget(100 * kMB);
    EXPECT_TRUE(budget.claim(1, "/a", requests({ { "a", 80 * kMB } }))[0]);
    budget.setLimit(50 * kMB);
    EXPECT_EQ(50 * kMB, budget.getLimit());
    EXPECT_TRUE(budget.claim(1, "/b", requests({ { "a", 80 * kMB } }))[0]);
    EXPECT_FALSE(budget.claim(1, "/c", requests({ { "b", kMB } }))[0]);
}

TEST(GpuTextureBudget, ConcurrentClaimsNeverExceedTheLimit)
{
    GpuTextureBudget budget(100 * kMB);
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&budget, t]() {
            for (int i = 0; i < 200; ++i) {
                const std::string owner = "/t" + std::to_string(t) + "/m" + std::to_string(i % 10);
                budget.claim((std::uint64_t)(t % 2), owner, requests({ { "k" + std::to_string(i % 37), 7 * kMB } }));
                if ((i % 5) == 4) {
                    budget.release((std::uint64_t)(t % 2), "/t" + std::to_string(t));
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_LE(budget.getUsedBytes(), 100 * kMB);

    budget.releaseScope(0);
    budget.releaseScope(1);
    EXPECT_EQ(0u, budget.getUsedBytes());
    EXPECT_EQ(0u, budget.getRefusedCount());
}

TEST(GpuTextureBudget, FallbackBudgetIsHalfTheCard)
{
    EXPECT_EQ(std::uint64_t(12) << 30, automaticGpuTextureBudget(std::uint64_t(24) << 30));
    EXPECT_EQ(std::uint64_t(4) << 30, automaticGpuTextureBudget(0));
}

TEST(GpuTextureBudget, HeadroomIsASixteenthOfTheCardAndAtLeastOneGiB)
{
    EXPECT_EQ(std::uint64_t(1536) * kMB, automaticGpuTextureHeadroom(std::uint64_t(24) << 30));
    EXPECT_EQ(std::uint64_t(3) << 30, automaticGpuTextureHeadroom(std::uint64_t(48) << 30));
    EXPECT_EQ(std::uint64_t(1) << 30, automaticGpuTextureHeadroom(std::uint64_t(8) << 30));
    EXPECT_EQ(std::uint64_t(1) << 30, automaticGpuTextureHeadroom(0));
}

TEST(GpuTextureBudget, AutomaticUsesTheFallbackUntilFreeMemoryIsKnown)
{
    GpuTextureBudget budget(0);
    budget.setAutomatic(50 * kMB, 10 * kMB);
    EXPECT_EQ(50 * kMB, budget.getLimit());
    EXPECT_FALSE(budget.getBasis().freeMemoryKnown);
    EXPECT_TRUE(budget.claim(1, "/a", requests({ { "a", 40 * kMB } }))[0]);
    EXPECT_FALSE(budget.claim(1, "/b", requests({ { "b", 20 * kMB } }))[0]);
}

TEST(GpuTextureBudget, AutomaticAdmitsWhileTheHeadroomStaysFree)
{
    GpuTextureBudget budget(0);
    budget.setAutomatic(1000 * kMB, 10 * kMB);
    budget.noteFreeMemory(100 * kMB);
    EXPECT_EQ(90 * kMB, budget.getLimit());

    const std::vector<bool> admitted = budget.claim(1, "/m", requests({ { "a", 60 * kMB }, { "b", 40 * kMB }, { "c", 30 * kMB } }));
    EXPECT_TRUE(admitted[0]);
    EXPECT_FALSE(admitted[1]);
    EXPECT_TRUE(admitted[2]);

    const GpuTextureBudget::Basis basis = budget.getBasis();
    EXPECT_TRUE(basis.automatic);
    EXPECT_TRUE(basis.freeMemoryKnown);
    EXPECT_EQ(10 * kMB, basis.headroomBytes);
    EXPECT_EQ(90 * kMB, basis.usedBytes);
    EXPECT_EQ(10 * kMB, basis.freeBytes);
}

TEST(GpuTextureBudget, AutomaticCountsWhatWasAdmittedBeforeASampleAsAlreadyTaken)
{
    GpuTextureBudget budget(0);
    budget.setAutomatic(1000 * kMB, 10 * kMB);
    budget.noteFreeMemory(100 * kMB);
    EXPECT_TRUE(budget.claim(1, "/a", requests({ { "a", 50 * kMB } }))[0]);

    // Uploaded by now, and other buffers took 30 MB more: 100 - 50 - 30.
    budget.noteFreeMemory(20 * kMB);
    EXPECT_EQ(60 * kMB, budget.getLimit());
    EXPECT_FALSE(budget.claim(1, "/b", requests({ { "b", 15 * kMB } }))[0]);
    EXPECT_TRUE(budget.claim(1, "/c", requests({ { "c", 10 * kMB } }))[0]);

    // Forgetting a texture gives its bytes back before the next sample sees them freed.
    budget.release(1, "/a");
    EXPECT_EQ(60 * kMB, budget.getBasis().freeBytes);
    EXPECT_TRUE(budget.claim(1, "/d", requests({ { "d", 40 * kMB } }))[0]);
}

TEST(GpuTextureBudget, AutomaticRefusesEverythingWhenTheCardIsAlreadyFull)
{
    GpuTextureBudget budget(0);
    budget.setAutomatic(1000 * kMB, 10 * kMB);
    budget.noteFreeMemory(5 * kMB);
    EXPECT_EQ(0u, budget.getLimit());
    EXPECT_FALSE(budget.claim(1, "/a", requests({ { "a", kMB } }))[0]);
}

TEST(GpuTextureBudget, FixedLimitIgnoresFreeMemory)
{
    GpuTextureBudget budget(0);
    budget.setAutomatic(1000 * kMB, 10 * kMB);
    budget.noteFreeMemory(20 * kMB);
    budget.setLimit(100 * kMB);
    EXPECT_EQ(100 * kMB, budget.getLimit());
    EXPECT_FALSE(budget.getBasis().automatic);
    EXPECT_TRUE(budget.claim(1, "/a", requests({ { "a", 80 * kMB } }))[0]);

    budget.setAutomatic(1000 * kMB, 10 * kMB);
    EXPECT_EQ(10 * kMB, budget.getLimit());
    EXPECT_FALSE(budget.claim(1, "/b", requests({ { "b", kMB } }))[0]);
}

TEST(GpuTextureBudget, ConcurrentAutomaticClaimsKeepTheHeadroom)
{
    GpuTextureBudget budget(0);
    budget.setAutomatic(1000 * kMB, 20 * kMB);
    budget.noteFreeMemory(120 * kMB);
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&budget, t]() {
            for (int i = 0; i < 200; ++i) {
                const std::string owner = "/t" + std::to_string(t) + "/m" + std::to_string(i % 10);
                budget.claim((std::uint64_t)(t % 2), owner, requests({ { "k" + std::to_string(i % 37), 7 * kMB } }));
                if ((i % 5) == 4) {
                    budget.release((std::uint64_t)(t % 2), "/t" + std::to_string(t));
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    EXPECT_LE(budget.getUsedBytes(), 100 * kMB);

    budget.releaseScope(0);
    budget.releaseScope(1);
    EXPECT_EQ(0u, budget.getUsedBytes());
}

TEST(GpuTextureBudget, PtexSingleFaceTakesOneMinimumPage)
{
    const std::uint64_t layoutBytes = 5461 * 3 * 4;
    EXPECT_EQ(512u * 512u * 4u + layoutBytes, stormPtexUploadBytes({ { 8, 8 } }, 4, 4, 0));
    EXPECT_EQ(512u * 512u * 8u + layoutBytes, stormPtexUploadBytes({ { 8, 7 } }, 4, 8, 0));
    EXPECT_EQ(0u, stormPtexUploadBytes({}, 4, 4, 0));
}

TEST(GpuTextureBudget, PtexTargetMemoryShrinksTheUpload)
{
    const std::vector<std::pair<int, int>> faces(4096, std::make_pair(8, 8));
    const std::uint64_t full = stormPtexUploadBytes(faces, 4, 4, 0);
    const std::uint64_t reduced = stormPtexUploadBytes(faces, 4, 4, 64 * kMB);

    EXPECT_GE(full, 4096u * 388u * 258u * 4u);
    EXPECT_LT(reduced, full);
    EXPECT_GT(reduced, 0u);
}

TEST(GpuTextureBudget, UriAssetPathsAreNotFiles)
{
    EXPECT_FALSE(gpuTextureIsFile("natron://Constant1/rgba?v=0123456789abcdef"));
    EXPECT_FALSE(gpuTextureIsFile("natron:Constant1/rgba"));
    EXPECT_FALSE(gpuTextureIsFile("s3+tex.v-2://bucket/key.exr"));
}

TEST(GpuTextureBudget, PlainPathsAreFiles)
{
    EXPECT_TRUE(gpuTextureIsFile("/textures/brick.exr"));
    EXPECT_TRUE(gpuTextureIsFile("textures/brick.<UDIM>.exr"));
    EXPECT_TRUE(gpuTextureIsFile("./a:b.png"));
    EXPECT_TRUE(gpuTextureIsFile("1abc:x.png"));
    EXPECT_TRUE(gpuTextureIsFile(":x.png"));
    EXPECT_TRUE(gpuTextureIsFile("/scene.usdz[textures/brick.png]"));
    EXPECT_TRUE(gpuTextureIsFile(""));
}
