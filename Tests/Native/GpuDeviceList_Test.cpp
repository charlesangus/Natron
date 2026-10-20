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
#include <array>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Tests/Native/NativeGpuParity.h"

#include "Engine/GpuDeviceList.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuDevice.h"
#endif

NATRON_NAMESPACE_USING

TEST(GpuDeviceList, UuidHexIsLowercaseAndFixedWidth)
{
    std::array<uint8_t, 16> uuid {};
    for (size_t i = 0; i < uuid.size(); ++i) {
        uuid[i] = static_cast<uint8_t>(0xa0 + i);
    }
    EXPECT_EQ(uuidHex(uuid), "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf");
    EXPECT_EQ(uuidHex({}), std::string(32, '0'));
}

TEST(GpuDeviceList, SelectorForUnknownOrAutoIsEmpty)
{
    GpuDeviceSummary d;
    d.index = 3;
    d.uuid[0] = 0x12;
    const std::vector<GpuDeviceSummary> list = { d };
    EXPECT_EQ(gpuSelectorForDeviceId(list, uuidHex(d.uuid)), "3");
    EXPECT_EQ(gpuSelectorForDeviceId(list, "auto"), "");
    EXPECT_EQ(gpuSelectorForDeviceId(list, ""), "");
    EXPECT_EQ(gpuSelectorForDeviceId(list, std::string(32, 'f')), "");
    EXPECT_EQ(gpuSelectorForDeviceId({}, uuidHex(d.uuid)), "");
}

#ifdef NATRON_HAVE_GPU

TEST(GpuDeviceList, EnumeratesDevicesAndRoundTripsSelectors)
{
    const std::vector<GpuDeviceSummary> list = enumerateGpuDevices();
    if (list.empty()) {
        reportNoGpu("no Vulkan device enumerated");
        return;
    }
    EXPECT_TRUE(std::any_of(list.begin(), list.end(),
                            [](const GpuDeviceSummary& d) { return d.type == GpuDeviceType::Cpu; }));
    for (const GpuDeviceSummary& d : list) {
        const std::string hex = uuidHex(d.uuid);
        EXPECT_EQ(hex.size(), 32u);
        EXPECT_EQ(hex.find_first_not_of("0123456789abcdef"), std::string::npos);
        EXPECT_FALSE(d.name.empty());
        EXPECT_EQ(gpuSelectorForDeviceId(list, hex), std::to_string(d.index));
    }
}

TEST(GpuDeviceList, SelectorOpensTheEnumeratedDevice)
{
    const std::vector<GpuDeviceSummary> list = enumerateGpuDevices();
    if (list.empty()) {
        reportNoGpu("no Vulkan device enumerated");
        return;
    }
    for (const GpuDeviceSummary& d : list) {
        gpu::GpuDeviceOptions options;
        options.deviceSelector = gpuSelectorForDeviceId(list, uuidHex(d.uuid));
        ASSERT_FALSE(options.deviceSelector.empty());
        std::unique_ptr<gpu::GpuDevice> device;
        const gpu::GpuStatus status = gpu::GpuDevice::create(options, device);
        if (!status) {
            // A device that lacks the required features is refused by create, not mis-indexed.
            continue;
        }
        EXPECT_EQ(device->info().name, d.name);
        EXPECT_EQ(device->info().physicalDeviceIndex, static_cast<uint32_t>(d.index));
    }
}

#endif // NATRON_HAVE_GPU
