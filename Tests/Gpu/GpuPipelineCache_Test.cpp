#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "GpuPipelineCache.h"
#include "GpuTestDevice.h"

using namespace gpu;

namespace {

namespace fs = std::filesystem;

// The comment makes each test's source unique so neither the process-wide compile memo nor an
// earlier run's disk entries satisfy another test.
std::string computeSource(const std::string& tag)
{
    return "#version 460\n// " + tag + "\n"
           "layout(local_size_x = 8, local_size_y = 8) in;\n"
           "layout(std430, set = 0, binding = 0) buffer Out { float data[]; } outBuf;\n"
           "void main() { outBuf.data[gl_GlobalInvocationID.x] = 1.0; }\n";
}

class GpuPipelineCacheTest : public gputest::SkippableTest {
protected:
    void SetUp() override
    {
        GpuStatus s = gputest::createDevice(dev);
        GPUTEST_DEVICE_OR_SKIP_TEST(s.ok(), s.message);
        dir = (fs::temp_directory_path() / ("natron-gpu-pipeline-cache-test-" + std::to_string(::getpid()) + "-"
                                            + ::testing::UnitTest::GetInstance()->current_test_info()->name()))
                  .string();
        fs::remove_all(dir);
    }

    void TearDown() override
    {
        if (dev) {
            dev->waitIdle();
        }
        dev.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
        check.expectClean("the test");
    }

    std::unique_ptr<GpuPipelineCache> makeCache(size_t maxMemory = 64, uint64_t maxDisk = 64ull << 20)
    {
        GpuPipelineCacheOptions o;
        o.directory = dir;
        o.maxMemoryEntries = maxMemory;
        o.maxDiskBytes = maxDisk;
        return std::make_unique<GpuPipelineCache>(*dev, o);
    }

    std::vector<fs::path> entryFiles() const
    {
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(dir)) {
            if (e.path().extension() == ".bin") {
                files.push_back(e.path());
            }
        }
        return files;
    }

    std::unique_ptr<GpuDevice> dev;
    gputest::ValidationCheck check;
    std::string dir;
};

} // namespace

GPU_TEST_F(GpuPipelineCacheTest, SecondRequestHitsMemory)
{
    auto cache = makeCache();
    const std::string src = computeSource("memory");
    std::shared_ptr<GpuKernel> a, b;
    ASSERT_TRUE(cache->get(src, ShaderStage::Compute, "main", a).ok());
    ASSERT_TRUE(cache->get(src, ShaderStage::Compute, "main", b).ok());
    EXPECT_EQ(a.get(), b.get());
    const auto st = cache->stats();
    EXPECT_EQ(st.compiles, 1u);
    EXPECT_EQ(st.memoryHits, 1u);
    EXPECT_EQ(st.diskWrites, 1u);
    EXPECT_EQ(entryFiles().size(), 1u);
}

GPU_TEST_F(GpuPipelineCacheTest, EntryPointIsPartOfTheKey)
{
    auto cache = makeCache();
    const std::string src = computeSource("entry");
    std::shared_ptr<GpuKernel> a, b;
    ASSERT_TRUE(cache->get(src, ShaderStage::Compute, "main", a).ok());
    ASSERT_TRUE(cache->get(src, ShaderStage::Compute, "other", b).ok());
    EXPECT_NE(a.get(), b.get());
    EXPECT_EQ(cache->stats().compiles, 2u);
}

GPU_TEST_F(GpuPipelineCacheTest, RestartHitsDiskWithoutCompiling)
{
    const std::string src = computeSource("restart");
    {
        auto first = makeCache();
        std::shared_ptr<GpuKernel> k;
        ASSERT_TRUE(first->get(src, ShaderStage::Compute, "main", k).ok());
        EXPECT_EQ(first->stats().compiles, 1u);
    }
    auto second = makeCache();
    std::shared_ptr<GpuKernel> k;
    ASSERT_TRUE(second->get(src, ShaderStage::Compute, "main", k).ok());
    ASSERT_NE(k, nullptr);
    EXPECT_EQ(k->bindings().size(), 1u);
    EXPECT_EQ(k->desc().groupSize[0], 8u);
    const auto st = second->stats();
    EXPECT_EQ(st.compiles, 0u);
    EXPECT_EQ(st.diskHits, 1u);
    EXPECT_EQ(st.invalidEntries, 0u);
}

GPU_TEST_F(GpuPipelineCacheTest, CorruptEntriesAreRecompiledAndOverwritten)
{
    enum Damage { FlipPayloadByte, FlipMagic, Truncate, Empty, Trailing };
    std::set<fs::path> known;
    for (Damage damage : { FlipPayloadByte, FlipMagic, Truncate, Empty, Trailing }) {
        const std::string src = computeSource("corrupt" + std::to_string(damage));
        {
            auto first = makeCache();
            std::shared_ptr<GpuKernel> k;
            ASSERT_TRUE(first->get(src, ShaderStage::Compute, "main", k).ok());
        }
        // Filesystem timestamps can tie across iterations, so the new entry is found by name.
        fs::path victim;
        for (const auto& f : entryFiles()) {
            if (known.insert(f).second) {
                victim = f;
            }
        }
        ASSERT_FALSE(victim.empty());
        std::string bytes;
        {
            std::ifstream in(victim, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), {});
        }
        ASSERT_GT(bytes.size(), 64u);
        switch (damage) {
        case FlipPayloadByte:
            bytes[bytes.size() / 2 + 20] ^= 0x5A;
            break;
        case FlipMagic:
            bytes[0] ^= 0xFF;
            break;
        case Truncate:
            bytes.resize(bytes.size() / 2);
            break;
        case Empty:
            bytes.clear();
            break;
        case Trailing:
            bytes += "junk";
            break;
        }
        {
            std::ofstream out(victim, std::ios::binary | std::ios::trunc);
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        }

        auto second = makeCache();
        std::shared_ptr<GpuKernel> k;
        ASSERT_TRUE(second->get(src, ShaderStage::Compute, "main", k).ok()) << "damage " << damage;
        auto st = second->stats();
        EXPECT_EQ(st.compiles, 1u) << "damage " << damage;
        EXPECT_EQ(st.invalidEntries, 1u) << "damage " << damage;
        EXPECT_EQ(st.diskHits, 0u) << "damage " << damage;
        EXPECT_EQ(st.diskWrites, 1u) << "damage " << damage;

        auto third = makeCache();
        ASSERT_TRUE(third->get(src, ShaderStage::Compute, "main", k).ok());
        st = third->stats();
        EXPECT_EQ(st.compiles, 0u) << "damage " << damage;
        EXPECT_EQ(st.diskHits, 1u) << "damage " << damage;
    }
}

GPU_TEST_F(GpuPipelineCacheTest, ConcurrentRequestsBuildOnce)
{
    auto cache = makeCache();
    const std::string src = computeSource("concurrent");
    constexpr int kThreads = 8;
    std::atomic<bool> go { false };
    std::vector<std::shared_ptr<GpuKernel>> kernels(kThreads);
    std::vector<std::thread> threads;
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&, i] {
            while (!go.load()) {
                std::this_thread::yield();
            }
            EXPECT_TRUE(cache->get(src, ShaderStage::Compute, "main", kernels[i]).ok());
        });
    }
    go = true;
    for (auto& t : threads) {
        t.join();
    }
    EXPECT_EQ(cache->stats().compiles, 1u);
    EXPECT_EQ(cache->stats().diskWrites, 1u);
    for (int i = 0; i < kThreads; ++i) {
        EXPECT_EQ(kernels[i].get(), kernels[0].get());
    }
}

GPU_TEST_F(GpuPipelineCacheTest, MemoryLruDropsLeastRecentlyRequested)
{
    auto cache = makeCache(2);
    std::shared_ptr<GpuKernel> a, b, c, again;
    const std::string sa = computeSource("lru-a"), sb = computeSource("lru-b"), sc = computeSource("lru-c");
    ASSERT_TRUE(cache->get(sa, ShaderStage::Compute, "main", a).ok());
    ASSERT_TRUE(cache->get(sb, ShaderStage::Compute, "main", b).ok());
    ASSERT_TRUE(cache->get(sa, ShaderStage::Compute, "main", again).ok());
    ASSERT_TRUE(cache->get(sc, ShaderStage::Compute, "main", c).ok());
    EXPECT_EQ(cache->stats().memoryHits, 1u);

    ASSERT_TRUE(cache->get(sa, ShaderStage::Compute, "main", again).ok());
    EXPECT_EQ(again.get(), a.get());
    EXPECT_EQ(cache->stats().memoryHits, 2u);

    // b was the least recently requested, so it returns from disk as a new kernel.
    ASSERT_TRUE(cache->get(sb, ShaderStage::Compute, "main", again).ok());
    EXPECT_NE(again.get(), b.get());
    EXPECT_EQ(cache->stats().diskHits, 1u);
    EXPECT_EQ(cache->stats().compiles, 3u);
}

GPU_TEST_F(GpuPipelineCacheTest, DiskSizeIsCappedByEvictingTheOldest)
{
    uint64_t one = 0;
    {
        auto probe = makeCache();
        std::shared_ptr<GpuKernel> k;
        ASSERT_TRUE(probe->get(computeSource("cap-probe"), ShaderStage::Compute, "main", k).ok());
        ASSERT_EQ(entryFiles().size(), 1u);
        one = fs::file_size(entryFiles()[0]);
        fs::remove_all(dir);
    }
    auto cache = makeCache(64, one * 2 + one / 2);
    std::shared_ptr<GpuKernel> k;
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(cache->get(computeSource("cap-" + std::to_string(i)), ShaderStage::Compute, "main", k).ok());
        fs::path newest;
        for (const auto& f : entryFiles()) {
            if (newest.empty() || fs::last_write_time(f) >= fs::last_write_time(newest)) {
                newest = f;
            }
        }
        // Distinct mtimes so "oldest" is well defined on coarse-grained filesystems.
        std::error_code ec;
        fs::last_write_time(newest, fs::file_time_type::clock::now() + std::chrono::seconds(i + 1), ec);
    }
    uint64_t total = 0;
    for (const auto& f : entryFiles()) {
        total += fs::file_size(f);
    }
    EXPECT_LE(total, one * 2 + one / 2);
    EXPECT_GE(entryFiles().size(), 1u);

    auto restart = makeCache();
    ASSERT_TRUE(restart->get(computeSource("cap-4"), ShaderStage::Compute, "main", k).ok());
    EXPECT_EQ(restart->stats().diskHits, 1u);
}

GPU_TEST_F(GpuPipelineCacheTest, CompileFailureIsReportedAndNotStored)
{
    auto cache = makeCache();
    std::shared_ptr<GpuKernel> k;
    const GpuStatus s = cache->get(computeSource("bad") + "float x = ;\n", ShaderStage::Compute,
                                   "main", k);
    EXPECT_FALSE(s.ok());
    EXPECT_NE(s.message.find("ERROR"), std::string::npos) << s.message;
    EXPECT_EQ(k, nullptr);
    EXPECT_TRUE(entryFiles().empty());
}
