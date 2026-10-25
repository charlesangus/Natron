#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "GpuHarness.h"
#include "GpuTestDevice.h"
#include "probe_fused_spirv.h"
#include "probe_mul_spirv.h"

using namespace gpu;

namespace {

enum class Category { Unit,
                      Million,
                      NearSplitLimit,
                      BelowTiny,
                      Unsplittable,
                      Filler };

struct Operands {
    float a;
    float b;
    Category category;
    // The split of an operand overflows, so the product error is dropped rather than computed.
    bool unsplittable;
};

constexpr uint32_t kGroup = 64;
constexpr float kSplitLimit = 8.0e34f;
constexpr double kFloatMin = 1.17549435e-38;

// Operand pairs whose float product has a nonzero error: random 24-bit mantissas, at the scales of
// a curve's coefficients, of a table position, of the largest splittable value (with partners that
// keep the product finite) and of products below 1e-30.
std::vector<Operands>
makePairs()
{
    std::vector<Operands> pairs;
    uint32_t state = 12345;
    auto next = [&state]() {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    auto operand = [&](float scale) {
        const float m = 1.0f + float(next() & 0xffffffu) / float(1 << 24);
        return scale * m;
    };
    auto add = [&](Category category, float scaleA, float scaleB, int count) {
        for (int i = 0; i < count; ++i) {
            pairs.push_back({ operand(scaleA) * 0.5f, operand(scaleB) * 0.5f, category, false });
        }
    };
    for (float scaleA : { 1.0f, 1.0e6f }) {
        for (float scaleB : { 1.0f, 1.0e6f, 1.0e-3f }) {
            add(scaleA == 1.0f ? Category::Unit : Category::Million, scaleA, scaleB, 40);
        }
    }
    for (float scaleB : { 1.0e-3f, 1.0f, 1.0e2f }) {
        add(Category::NearSplitLimit, kSplitLimit * 0.5f, scaleB, 40);
    }
    pairs.push_back({ 9.0e-31f, 1.1f, Category::BelowTiny, false });
    add(Category::BelowTiny, 8.0e-31f, 1.0f, 40);
    add(Category::BelowTiny, 1.0e-16f, 5.0e-15f, 40);
    for (int i = 0; i < 8; ++i) {
        pairs.push_back({ operand(1.0e36f), operand(1.0e-3f), Category::Unsplittable, true });
    }
    while (pairs.size() % kGroup != 0) {
        pairs.push_back({ 1.0f, 1.0f, Category::Filler, false });
    }
    return pairs;
}

class CurveSnapshotPrecisionTest : public gputest::GpuSuite {
protected:
    // Four floats per pair in and out: a and b in, then hi, lo, the product error and the product out.
    static std::vector<float> run(const std::vector<Operands>& pairs, const uint32_t* spirv, size_t words,
                                  const uint32_t (&groupSize)[3], uint32_t buffers, uint32_t pushBytes)
    {
        std::vector<float> in(pairs.size() * 4, 0.0f);
        for (size_t i = 0; i < pairs.size(); ++i) {
            in[4 * i] = pairs[i].a;
            in[4 * i + 1] = pairs[i].b;
        }
        std::vector<float> out(in.size(), -1.0f);
        auto kernel = makeKernel(spirv, words, "main", buffers, pushBytes, groupSize);
        auto transfer = makeTransfer();
        if (!kernel || !transfer) {
            return out;
        }
        Buffer unused = makeStorage(16);
        const std::vector<std::byte> push(pushBytes);
        const TransferFrame frame { in.data(), out.data(), in.size() * sizeof(float) };
        process(*transfer, *kernel, std::span(&frame, 1), 1,
                [&](VkCommandBuffer cmd, const ComputeBinding& b, size_t) -> GpuStatus {
                    std::vector<VkBuffer> bufs(buffers, unused.buffer);
                    bufs[0] = b.input;
                    bufs[1] = b.output;
                    return kernel->record(cmd, bufs, push, { uint32_t(pairs.size()), 1, 1 });
                });
        return out;
    }

    static bool isLavapipe()
    {
        return dev_->info().type == VK_PHYSICAL_DEVICE_TYPE_CPU || dev_->info().name.find("llvmpipe") != std::string::npos;
    }
};

} // namespace

GPU_TEST_F(CurveSnapshotPrecisionTest, CurveMulKeepsTheProductError)
{
    const std::vector<Operands> pairs = makePairs();
    const std::vector<float> out = run(pairs, probe_mul_spirv, probe_mul_spirv_words, probe_mul_group_size,
                                       probe_mul_storage_buffers, probe_mul_push_bytes);
    size_t exactChecked = 0;
    size_t tested[6] = {};
    size_t belowTinyExact = 0;
    for (size_t i = 0; i < pairs.size(); ++i) {
        const float a = pairs[i].a;
        const float b = pairs[i].b;
        const float hi = out[4 * i];
        const float lo = out[4 * i + 1];
        const float err = out[4 * i + 2];
        const float p = out[4 * i + 3];
        const double exact = double(a) * double(b);
        ASSERT_TRUE(std::isfinite(hi) && std::isfinite(lo)) << i << ": " << a << " * " << b;
        EXPECT_EQ(p, float(exact)) << i;
        if (pairs[i].unsplittable) {
            EXPECT_EQ(err, 0.0f) << i << ": " << a << " * " << b;
            EXPECT_EQ(lo, 0.0f) << i;
            continue;
        }
        ++tested[size_t(pairs[i].category)];
        const double exactError = exact - double(p);
        // A device that flushes denormals may lose a residual below FLT_MIN, and only that.
        const bool representable = std::fabs(exactError) >= kFloatMin || exactError == 0.0;
        if (representable) {
            EXPECT_EQ(double(err), exactError) << i << ": " << a << " * " << b;
            if (hi == p) {
                EXPECT_EQ(double(lo), exactError) << i << ": " << a << " * " << b;
            }
            EXPECT_LE(std::fabs((double(hi) + double(lo)) - exact), std::fabs(exact) * std::ldexp(1.0, -44))
                << i << ": " << a << " * " << b;
            exactChecked += exactError != 0.0;
            if (pairs[i].category == Category::BelowTiny && exactError != 0.0) {
                ++belowTinyExact;
            }
        } else {
            EXPECT_TRUE(double(err) == exactError || err == 0.0f) << i << ": " << a << " * " << b;
        }
    }
    for (Category category : { Category::Unit, Category::Million, Category::NearSplitLimit, Category::BelowTiny }) {
        EXPECT_GT(tested[size_t(category)], 0u) << int(category);
    }
    EXPECT_GT(belowTinyExact, 0u);
    EXPECT_GT(exactChecked, pairs.size() / 2);
}

GPU_TEST_F(CurveSnapshotPrecisionTest, CurveMulDropsAnErrorTooSmallToKeep)
{
    std::vector<Operands> pairs = { { 1.0e-39f, 3.0f, Category::Filler, false }, { 1.0e-20f, 1.0e-20f, Category::Filler, false }, { 0.0f, 5.0f, Category::Filler, false } };
    while (pairs.size() % kGroup != 0) {
        pairs.push_back({ 1.0f, 1.0f, Category::Filler, false });
    }
    const std::vector<float> out = run(pairs, probe_mul_spirv, probe_mul_spirv_words, probe_mul_group_size,
                                       probe_mul_storage_buffers, probe_mul_push_bytes);
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_TRUE(std::isfinite(out[4 * i]) && std::isfinite(out[4 * i + 1])) << i;
        EXPECT_EQ(out[4 * i + 2], 0.0f) << i;
    }
}

GPU_TEST_F(CurveSnapshotPrecisionTest, FusedErrorTermCanary)
{
    const std::vector<Operands> pairs = makePairs();
    const std::vector<float> out = run(pairs, probe_fused_spirv, probe_fused_spirv_words, probe_fused_group_size,
                                       probe_fused_storage_buffers, probe_fused_push_bytes);
    size_t lost = 0;
    for (size_t i = 0; i < pairs.size(); ++i) {
        if (pairs[i].unsplittable) {
            continue;
        }
        const double exact = double(pairs[i].a) * double(pairs[i].b);
        const double exactError = exact - double(out[4 * i]);
        lost += double(out[4 * i + 1]) != exactError;
    }
    std::printf("fused fma lost the product error for %zu of %zu pairs on %s\n", lost, pairs.size(),
                dev_->info().name.c_str());
    if (isLavapipe()) {
        EXPECT_GT(lost, 0u) << "the fused error term was exact here, so this device would not have caught the bug";
    }
}
