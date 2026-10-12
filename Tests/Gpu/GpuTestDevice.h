#pragma once

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <sstream>
#include <string>

#include "GpuDevice.h"

// Device creation shared by every GPU test binary.
//
// Environment:
//   NATRON_TEST_GPU_NO_VALIDATION=1        do not request the Khronos validation layer
//   NATRON_TEST_GPU_NO_PUSH_DESCRIPTOR=1   force the descriptor-pool path in GpuKernel
//   NATRON_TEST_REQUIRE_GPU=1        fail instead of skipping when no Vulkan device can be created
//   NATRON_TEST_REQUIRE_GPU_HARDWARE=1     fail instead of running on a CPU device (lavapipe)
namespace gputest {

inline bool
envSet(const char* name)
{
    const char* v = std::getenv(name);
    return v && std::strcmp(v, "1") == 0;
}

inline const char*
hardwareRequirementMarker()
{
    return "NATRON_TEST_REQUIRE_GPU_HARDWARE";
}

// A missing device skips the GPU tests so a host without Vulkan stays green; a run that exists to
// exercise the GPU sets NATRON_TEST_REQUIRE_GPU=1, and an unmet hardware requirement always fails.
inline bool
deviceRequired(const std::string& createError)
{
    return envSet("NATRON_TEST_REQUIRE_GPU") || createError.find(hardwareRequirementMarker()) != std::string::npos;
}

inline std::atomic<uint64_t>&
validationErrors()
{
    static std::atomic<uint64_t> n { 0 };
    return n;
}

inline gpu::GpuDeviceOptions
deviceOptions()
{
    gpu::GpuDeviceOptions o;
    o.enableValidation = !envSet("NATRON_TEST_GPU_NO_VALIDATION");
    o.disablePushDescriptor = envSet("NATRON_TEST_GPU_NO_PUSH_DESCRIPTOR");
    o.debugMessage = [](VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
                        const char* message) {
        const bool error = severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        // The loader reports ICDs it could not load as general errors; only validation counts.
        const bool validation = (types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) != 0;
        if (error && validation) {
            ++validationErrors();
        }
        std::fprintf(stderr, "[vulkan %s%s] %s\n", validation ? "validation " : "", error ? "error" : "warning",
                     message);
    };
    return o;
}

inline gpu::GpuStatus
createDevice(std::unique_ptr<gpu::GpuDevice>& out, gpu::GpuDeviceOptions o = deviceOptions())
{
    gpu::GpuStatus s = gpu::GpuDevice::create(o, out);
    if (!s) {
        return s;
    }
    if (envSet("NATRON_TEST_REQUIRE_GPU_HARDWARE") && out->info().type == VK_PHYSICAL_DEVICE_TYPE_CPU) {
        const std::string name = out->info().name;
        out.reset();
        return { VK_ERROR_INITIALIZATION_FAILED,
                 "selected device '" + name + "' is a CPU device but " + hardwareRequirementMarker() + "=1" };
    }
    std::printf("vulkan device: %s | validation=%d debugMessenger=%d pushDescriptor=%d\n", out->info().name.c_str(),
                out->info().validationEnabled, out->info().debugMessenger, out->info().pushDescriptor);
    return s;
}

// Fails the current test, or the suite when called from TearDownTestCase, for every
// validation error since the last call.
class ValidationCheck {
public:
    ValidationCheck()
        : seen_(validationErrors().load())
    {
    }

    void expectClean(const char* where)
    {
        const uint64_t now = validationErrors().load();
        EXPECT_EQ(now - seen_, 0u) << "Vulkan validation errors during " << where << " (see stderr)";
        seen_ = now;
    }

private:
    uint64_t seen_;
};

// The bundled gtest predates GTEST_SKIP: a skipped test prints its reason and passes, like the
// OpenGL skips in GLScheduler_Test.cpp.
class SkipStream {
public:
    template <typename T>
    SkipStream& operator<<(const T& value)
    {
        text_ << value;
        return *this;
    }

    std::string str() const { return text_.str(); }

private:
    std::ostringstream text_;
};

struct SkipReporter {
    bool* skipped = nullptr;

    void operator=(const SkipStream& reason) const
    {
        if (skipped) {
            *skipped = true;
        }
        std::printf("[  SKIPPED ] %s\n", reason.str().c_str());
    }
};

// Returning from SetUp does not stop the test body in this gtest, so a fixture whose SetUp may skip
// derives from SkippableTest and declares its tests with GPU_TEST_F: the body then runs only if SetUp
// neither failed nor called GPUTEST_SKIP_TEST(). TestBody is final, so TEST_F does not compile on such a
// fixture and a test cannot silently lose the guard.
class SkippableTest : public ::testing::Test {
protected:
    SkipReporter skipTest() { return SkipReporter { &skipped_ }; }

private:
    void TestBody() final
    {
        if (!skipped_) {
            runBody();
        }
    }

    virtual void runBody() = 0;

    bool skipped_ = false;
};

} // namespace gputest

#define GPUTEST_SKIP() return ::gputest::SkipReporter() = ::gputest::SkipStream()

// In a SkippableTest's SetUp: reports the skip and keeps the test body from running.
#define GPUTEST_SKIP_TEST() return this->skipTest() = ::gputest::SkipStream()

#define GPUTEST_DEVICE_OR_SKIP_WITH(ok, message, skip)      \
    if (ok) {                                               \
    } else if (gputest::deviceRequired(message)) {          \
        FAIL() << "no usable Vulkan device: " << (message); \
    } else {                                                \
        skip() << "no usable Vulkan device: " << (message); \
    }

#define GPUTEST_DEVICE_OR_SKIP(ok, message) GPUTEST_DEVICE_OR_SKIP_WITH(ok, message, GPUTEST_SKIP)
#define GPUTEST_DEVICE_OR_SKIP_TEST(ok, message) GPUTEST_DEVICE_OR_SKIP_WITH(ok, message, GPUTEST_SKIP_TEST)

// TEST_F for a fixture derived from gputest::SkippableTest. It registers the test as TEST_F does.
#define GPUTEST_CLASS_NAME_(fixture, name) fixture##_##name##_GpuTest
#define GPU_TEST_F(fixture, name)                                                                                            \
    class GPUTEST_CLASS_NAME_(fixture, name)                                                                                 \
        : public fixture {                                                                                                   \
        void runBody() override;                                                                                             \
        [[maybe_unused]] static ::testing::TestInfo* const testInfo_;                                                        \
    };                                                                                                                       \
    ::testing::TestInfo* const GPUTEST_CLASS_NAME_(fixture, name)::testInfo_ = ::testing::internal::MakeAndRegisterTestInfo( \
        #fixture, #name, nullptr, nullptr, ::testing::internal::GetTypeId<fixture>(), fixture::SetUpTestCase,                \
        fixture::TearDownTestCase, new ::testing::internal::TestFactoryImpl<GPUTEST_CLASS_NAME_(fixture, name)>);            \
    void GPUTEST_CLASS_NAME_(fixture, name)::runBody()
