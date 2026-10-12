#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "smoke_spirv.h"

#include "slang-cpp-prelude.h"

extern "C" void smoke(ComputeVaryingInput*, void* entryPointParams, void* globalParams);

namespace {

// Mirrors the layout slangc's C++ target generates for the globals in
// smoke.slang; see the emitted GlobalParams_0.
struct SmokeParams {
    float scale;
    uint32_t count;
};

struct SmokeGlobals {
    RWStructuredBuffer<float> outBuf;
    SmokeParams* params;
};

} // namespace

TEST(Smoke, SpirvHasMagic)
{
    ASSERT_GT(smoke_spirv_words, 5u);
    EXPECT_EQ(smoke_spirv[0], 0x07230203u);
}

TEST(Smoke, CppTargetWritesExpectedValues)
{
    constexpr uint32_t kCount = 200;
    constexpr uint32_t kGroups = (kCount + 63) / 64;
    std::vector<float> out(kGroups * 64, -1.0f);
    SmokeParams params { 0.5f, kCount };
    SmokeGlobals globals { { out.data(), out.size() }, &params };

    ComputeVaryingInput vi = {};
    vi.startGroupID = { 0, 0, 0 };
    vi.endGroupID = { kGroups, 1, 1 };
    smoke(&vi, nullptr, &globals);

    for (uint32_t i = 0; i < kCount; ++i)
        EXPECT_FLOAT_EQ(out[i], static_cast<float>(i) * 0.5f) << i;
    for (uint32_t i = kCount; i < out.size(); ++i)
        EXPECT_EQ(out[i], -1.0f) << i;
}
