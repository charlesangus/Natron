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

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/GpuContext.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuRuntime.h"
#endif

NATRON_NAMESPACE_USING

namespace {
class ScopedEnv {
public:
    ScopedEnv(const char* name,
              const char* value)
        : _name(name)
    {
        const char* old = std::getenv(name);
        _hadOld = old != nullptr;
        if (old) {
            _old = old;
        }
        ::setenv(name, value, 1);
    }

    ~ScopedEnv()
    {
        if (_hadOld) {
            ::setenv(_name.c_str(), _old.c_str(), 1);
        } else {
            ::unsetenv(_name.c_str());
        }
    }

private:
    std::string _name;
    std::string _old;
    bool _hadOld;
};

struct LogCapture {
    std::mutex mutex;
    std::vector<std::string> lines;

    std::function<void(const std::string&)> sink()
    {
        return [this](const std::string& line) {
            std::lock_guard<std::mutex> k(mutex);
            lines.push_back(line);
        };
    }
};

GpuContextOptions
makeOptions(GpuResolvedMode mode,
            LogCapture& log)
{
    GpuContextOptions o;
    o.mode = mode;
    o.infoLog = log.sink();

    return o;
}
}

TEST(GpuContextModeTest, EnvOverridesSetting)
{
    for (GpuComputeModeEnum setting : { eGpuComputeModeOff, eGpuComputeModeAuto }) {
        EXPECT_EQ(GpuContext::resolveMode(setting, "off"), GpuResolvedMode::Off);
        EXPECT_EQ(GpuContext::resolveMode(setting, "auto"), GpuResolvedMode::Auto);
        EXPECT_EQ(GpuContext::resolveMode(setting, "force"), GpuResolvedMode::Force);
        EXPECT_EQ(GpuContext::resolveMode(setting, "FORCE"), GpuResolvedMode::Force);
    }
}

TEST(GpuContextModeTest, SettingDecidesWithoutEnv)
{
    EXPECT_EQ(GpuContext::resolveMode(eGpuComputeModeOff, nullptr), GpuResolvedMode::Off);
    EXPECT_EQ(GpuContext::resolveMode(eGpuComputeModeAuto, nullptr), GpuResolvedMode::Auto);
    EXPECT_EQ(GpuContext::resolveMode(eGpuComputeModeOff, "bogus"), GpuResolvedMode::Off);
    EXPECT_EQ(GpuContext::resolveMode(eGpuComputeModeAuto, ""), GpuResolvedMode::Auto);
}

TEST(GpuContextModeTest, DeviceChoice)
{
    EXPECT_FALSE(GpuContext::isDeviceAllowed(GpuResolvedMode::Off, false));
    EXPECT_FALSE(GpuContext::isDeviceAllowed(GpuResolvedMode::Off, true));
    EXPECT_TRUE(GpuContext::isDeviceAllowed(GpuResolvedMode::Auto, false));
    EXPECT_FALSE(GpuContext::isDeviceAllowed(GpuResolvedMode::Auto, true));
    EXPECT_TRUE(GpuContext::isDeviceAllowed(GpuResolvedMode::Force, false));
    EXPECT_TRUE(GpuContext::isDeviceAllowed(GpuResolvedMode::Force, true));
}

TEST(GpuContextTest, OffIsUnavailableAndSilent)
{
    LogCapture log;
    GpuContext ctx(makeOptions(GpuResolvedMode::Off, log));

    EXPECT_FALSE(ctx.isInitializing());
    EXPECT_FALSE(ctx.isAvailable());
    EXPECT_EQ(ctx.runtime(), nullptr);
    EXPECT_TRUE(log.lines.empty());
}

#ifdef NATRON_HAVE_GPU

TEST(GpuContextTest, NoIcdIsUnavailableWithOneInfoLine)
{
    ScopedEnv icd("VK_ICD_FILENAMES", "/nonexistent/none.json");
    ScopedEnv drivers("VK_DRIVER_FILES", "/nonexistent/none.json");
    LogCapture log;
    GpuContext ctx(makeOptions(GpuResolvedMode::Force, log));

    ASSERT_TRUE(ctx.waitForInit(std::chrono::seconds(60)));
    EXPECT_FALSE(ctx.isAvailable());
    EXPECT_EQ(ctx.runtime(), nullptr);
    EXPECT_LE(log.lines.size(), 1u);
}

TEST(GpuContextTest, AutoNeverPicksSoftwareDevice)
{
    LogCapture log;
    GpuContext ctx(makeOptions(GpuResolvedMode::Auto, log));

    ASSERT_TRUE(ctx.waitForInit(std::chrono::seconds(60)));
    EXPECT_LE(log.lines.size(), 1u);
    if (ctx.isAvailable()) {
        EXPECT_EQ(ctx.deviceName().find("llvmpipe"), std::string::npos);
    } else {
        EXPECT_EQ(ctx.runtime(), nullptr);
    }
}

TEST(GpuContextTest, ForceCalibratesAnyDevice)
{
    LogCapture log;
    GpuContext ctx(makeOptions(GpuResolvedMode::Force, log));

    ASSERT_TRUE(ctx.waitForInit(std::chrono::seconds(60)));
    if (!ctx.isAvailable() && !std::getenv("NATRON_TEST_REQUIRE_GPU")) {
        std::fprintf(stderr, "no Vulkan device, skipping\n");
        return;
    }
    ASSERT_TRUE(ctx.isAvailable());
    ASSERT_NE(ctx.runtime(), nullptr);
    const GpuCalibration c = ctx.calibration();
    EXPECT_GT(c.uploadBytesPerSecond, 0.);
    EXPECT_GT(c.downloadBytesPerSecond, 0.);
    if (c.hostImport) {
        EXPECT_GT(c.hostImportUploadBytesPerSecond, 0.);
        EXPECT_GT(c.hostImportDownloadBytesPerSecond, 0.);
        EXPECT_EQ(c.stagingUploadBytesPerSecond, 0.);
        EXPECT_EQ(c.stagingDownloadBytesPerSecond, 0.);
        EXPECT_EQ(c.uploadBytesPerSecond, c.hostImportUploadBytesPerSecond);
    } else {
        EXPECT_GT(c.stagingUploadBytesPerSecond, 0.);
        EXPECT_GT(c.stagingDownloadBytesPerSecond, 0.);
        EXPECT_EQ(c.uploadBytesPerSecond, c.stagingUploadBytesPerSecond);
    }
    EXPECT_FALSE(ctx.deviceName().empty());
    EXPECT_TRUE(log.lines.empty());
}

TEST(GpuContextTest, MemoryBudgetIsApplied)
{
    LogCapture log;
    GpuContextOptions o = makeOptions(GpuResolvedMode::Force, log);
    o.memoryBudgetMB = 1024;
    GpuContext ctx(o);

    ASSERT_TRUE(ctx.waitForInit(std::chrono::seconds(60)));
    if (!ctx.isAvailable()) {
        std::fprintf(stderr, "no Vulkan device, skipping\n");
        return;
    }
    EXPECT_LE(ctx.runtime()->pool().budget(), std::uint64_t(1024) << 20);
}

TEST(GpuContextTest, LostRuntimeIsRecreatedOnceNothingHoldsIt)
{
    LogCapture log;
    GpuContext ctx(makeOptions(GpuResolvedMode::Force, log));

    ASSERT_TRUE(ctx.waitForInit(std::chrono::seconds(60)));
    if (!ctx.isAvailable()) {
        if (std::getenv("NATRON_TEST_REQUIRE_GPU")) {
            ADD_FAILURE() << "NATRON_TEST_REQUIRE_GPU is set but no Vulkan device is available";
        } else {
            std::fprintf(stderr, "no Vulkan device, skipping\n");
        }
        return;
    }
    EXPECT_TRUE(ctx.recoverIfLost());
    gpu::GpuRuntime* rt = ctx.runtime();
    rt->device().injectFault(gpu::GpuFaultPlan::Kind::DeviceLost, 1);
    gpu::GpuBufferHandle handle;
    EXPECT_EQ(rt->acquire(1 << 20, handle).result, VK_ERROR_DEVICE_LOST);
    EXPECT_FALSE(ctx.isAvailable());
    EXPECT_EQ(ctx.runtime(), rt);

    std::shared_ptr<void> hold = ctx.holdRuntime();
    ASSERT_TRUE(bool(hold));
    EXPECT_FALSE(ctx.recoverIfLost());
    EXPECT_FALSE(ctx.isAvailable());

    hold.reset();
    EXPECT_TRUE(ctx.recoverIfLost());
    EXPECT_TRUE(ctx.isAvailable());
    EXPECT_EQ(rt->state(), gpu::GpuRuntimeState::Ready);
    EXPECT_TRUE(rt->acquire(1 << 20, handle).ok());
    EXPECT_EQ(log.lines.size(), 1u);
}

TEST(GpuContextTest, InjectedFaultAtStartupDegradesSilently)
{
    ScopedEnv fault("NATRON_GPU_FAULT", "lost@1");
    LogCapture log;
    GpuContext ctx(makeOptions(GpuResolvedMode::Force, log));

    ASSERT_TRUE(ctx.waitForInit(std::chrono::seconds(60)));
    EXPECT_FALSE(ctx.isAvailable());
    EXPECT_EQ(ctx.runtime(), nullptr);
    EXPECT_LE(log.lines.size(), 1u);
}

#else

TEST(GpuContextTest, WithoutGpuBuildNeverAvailable)
{
    LogCapture log;
    GpuContext ctx(makeOptions(GpuResolvedMode::Force, log));

    EXPECT_TRUE(ctx.waitForInit(std::chrono::seconds(1)));
    EXPECT_FALSE(ctx.isAvailable());
    EXPECT_EQ(ctx.runtime(), nullptr);
}

#endif
