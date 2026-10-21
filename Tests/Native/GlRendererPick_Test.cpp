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

#include <array>
#include <cstdint>
#include <list>
#include <string>

#include <gtest/gtest.h>

#include "Engine/GpuDeviceList.h"
#include "Engine/OSGLContext.h"

NATRON_NAMESPACE_USING

namespace {

std::array<uint8_t, 16>
uuidOf(uint8_t seed)
{
    std::array<uint8_t, 16> u {};
    for (size_t i = 0; i < u.size(); ++i) {
        u[i] = static_cast<uint8_t>(seed + i);
    }
    return u;
}

OpenGLRendererInfo
renderer(int id,
         const std::string& name,
         bool accelerated,
         uint32_t vendor = 0,
         uint32_t device = 0)
{
    OpenGLRendererInfo r;
    r.rendererID = GLRendererID(id);
    r.rendererName = name;
    r.accelerated = accelerated;
    r.pciVendorID = vendor;
    r.pciDeviceID = device;
    return r;
}

OpenGLRendererInfo
rendererWithUuid(int id,
                 const std::string& name,
                 uint8_t seed)
{
    OpenGLRendererInfo r = renderer(id, name, true);
    r.deviceUuid = uuidOf(seed);
    r.hasUuid = true;
    return r;
}

GpuDeviceSummary
vulkanDevice(uint8_t uuidSeed,
             uint32_t vendor,
             uint32_t device)
{
    GpuDeviceSummary d;
    d.uuid = uuidOf(uuidSeed);
    d.vendorID = vendor;
    d.deviceID = device;
    return d;
}

} // namespace

TEST(GlRendererPick, MatchesChosenDeviceByUuid)
{
    std::list<OpenGLRendererInfo> renderers;
    renderers.push_back( rendererWithUuid(0, "iris", 0x10) );
    renderers.push_back( rendererWithUuid(1, "radeonsi", 0x20) );
    // Same PCI IDs as the chosen device on another renderer: the UUID must win.
    renderers.push_back( renderer(2, "decoy", true, 0x1002, 0x73bf) );

    const GpuDeviceSummary chosen = vulkanDevice(0x20, 0x1002, 0x73bf);
    const GLRendererPick pick = OSGLContext::pickRenderer(renderers, &chosen, false);

    EXPECT_TRUE(pick.enabled);
    EXPECT_EQ(pick.rendererID.renderID, 1);
    EXPECT_EQ(pick.reason, "radeonsi (matches the chosen Vulkan device)");
}

TEST(GlRendererPick, MatchesChosenDeviceByPciIds)
{
    std::list<OpenGLRendererInfo> renderers;
    renderers.push_back( renderer(0, "iris", true, 0x8086, 0x9a49) );
    renderers.push_back( renderer(1, "radeonsi", true, 0x1002, 0x73bf) );

    const GpuDeviceSummary chosen = vulkanDevice(0x20, 0x1002, 0x73bf);
    const GLRendererPick pick = OSGLContext::pickRenderer(renderers, &chosen, false);

    EXPECT_TRUE(pick.enabled);
    EXPECT_EQ(pick.rendererID.renderID, 1);
    EXPECT_EQ(pick.reason, "radeonsi (matches the chosen Vulkan device)");
}

TEST(GlRendererPick, PciIdsDoNotMatchRendererWithMismatchingUuid)
{
    std::list<OpenGLRendererInfo> renderers;
    OpenGLRendererInfo otherCard = renderer(0, "radeonsi", true, 0x1002, 0x73bf);
    otherCard.deviceUuid = uuidOf(0x10);
    otherCard.hasUuid = true;
    renderers.push_back(otherCard);

    const GpuDeviceSummary chosen = vulkanDevice(0x20, 0x1002, 0x73bf);
    const GLRendererPick pick = OSGLContext::pickRenderer(renderers, &chosen, false);

    EXPECT_TRUE(pick.enabled);
    EXPECT_EQ(pick.rendererID.renderID, 0);
    EXPECT_EQ(pick.reason, "radeonsi (default renderer: no GL renderer matches the chosen Vulkan device)");
}

TEST(GlRendererPick, FallsBackToFirstAllowedRendererWhenNothingMatches)
{
    std::list<OpenGLRendererInfo> renderers;
    renderers.push_back( renderer(0, "llvmpipe", false) );
    renderers.push_back( renderer(1, "iris", true, 0x8086, 0x9a49) );

    const GpuDeviceSummary chosen = vulkanDevice(0x20, 0x1002, 0x73bf);
    const GLRendererPick pick = OSGLContext::pickRenderer(renderers, &chosen, false);

    EXPECT_TRUE(pick.enabled);
    EXPECT_EQ(pick.rendererID.renderID, 1);
    EXPECT_EQ(pick.reason, "iris (default renderer: no GL renderer matches the chosen Vulkan device)");
}

TEST(GlRendererPick, NoChosenDeviceUsesTheDefaultRenderer)
{
    std::list<OpenGLRendererInfo> renderers;
    renderers.push_back( renderer(-1, "radeonsi", true) );

    const GLRendererPick pick = OSGLContext::pickRenderer(renderers, nullptr, false);

    EXPECT_TRUE(pick.enabled);
    EXPECT_EQ(pick.rendererID.renderID, -1);
    EXPECT_EQ(pick.reason, "radeonsi (default renderer)");
}

TEST(GlRendererPick, SoftwareRendererRefusedWithoutAllowSoftware)
{
    std::list<OpenGLRendererInfo> renderers;
    renderers.push_back( renderer(0, "llvmpipe", false) );

    const GLRendererPick pick = OSGLContext::pickRenderer(renderers, nullptr, false);

    EXPECT_FALSE(pick.enabled);
    EXPECT_EQ(pick.reason, "disabled: only a software GL renderer is available");
}

TEST(GlRendererPick, SoftwareUuidMatchRefusedWithoutAllowSoftware)
{
    std::list<OpenGLRendererInfo> renderers;
    OpenGLRendererInfo llvmpipe = renderer(0, "llvmpipe", false);
    llvmpipe.deviceUuid = uuidOf(0x30);
    llvmpipe.hasUuid = true;
    renderers.push_back(llvmpipe);
    renderers.push_back( renderer(1, "iris", true, 0x8086, 0x9a49) );

    const GpuDeviceSummary chosen = vulkanDevice(0x30, 0x10005, 0);
    const GLRendererPick pick = OSGLContext::pickRenderer(renderers, &chosen, false);

    EXPECT_TRUE(pick.enabled);
    EXPECT_EQ(pick.rendererID.renderID, 1);
    EXPECT_EQ(pick.reason, "iris (default renderer: no GL renderer matches the chosen Vulkan device)");
}

TEST(GlRendererPick, SoftwareRendererAcceptedWithAllowSoftware)
{
    std::list<OpenGLRendererInfo> renderers;
    renderers.push_back( renderer(0, "llvmpipe", false) );

    const GLRendererPick pick = OSGLContext::pickRenderer(renderers, nullptr, true);

    EXPECT_TRUE(pick.enabled);
    EXPECT_EQ(pick.rendererID.renderID, 0);
    EXPECT_EQ(pick.reason, "llvmpipe, software (default renderer)");
}

TEST(GlRendererPick, PciIdsOfZeroNeverMatch)
{
    std::list<OpenGLRendererInfo> renderers;
    renderers.push_back( renderer(0, "iris", true, 0x8086, 0x9a49) );
    renderers.push_back( renderer(1, "llvmpipe", false) );

    const GpuDeviceSummary chosen = vulkanDevice(0x40, 0, 0);
    const GLRendererPick pick = OSGLContext::pickRenderer(renderers, &chosen, true);

    EXPECT_TRUE(pick.enabled);
    EXPECT_EQ(pick.rendererID.renderID, 0);
    EXPECT_EQ(pick.reason, "iris (default renderer: no GL renderer matches the chosen Vulkan device)");
}

TEST(GlRendererPick, EmptyListIsDisabled)
{
    const std::list<OpenGLRendererInfo> renderers;
    const GpuDeviceSummary chosen = vulkanDevice(0x20, 0x1002, 0x73bf);

    const GLRendererPick withDevice = OSGLContext::pickRenderer(renderers, &chosen, true);
    EXPECT_FALSE(withDevice.enabled);
    EXPECT_EQ(withDevice.reason, "disabled: no GL renderer is available");

    const GLRendererPick withoutDevice = OSGLContext::pickRenderer(renderers, nullptr, false);
    EXPECT_FALSE(withoutDevice.enabled);
}

TEST(GlRendererPick, SoftwareRendererNames)
{
    EXPECT_TRUE( OSGLContext::isSoftwareRendererName("llvmpipe (LLVM 17.0.6, 256 bits)") );
    EXPECT_TRUE( OSGLContext::isSoftwareRendererName("softpipe") );
    EXPECT_FALSE( OSGLContext::isSoftwareRendererName("AMD Radeon RX 6900 XT (radeonsi, navi21, LLVM 17.0.6, DRM 3.54)") );
    EXPECT_FALSE( OSGLContext::isSoftwareRendererName("NVIDIA GeForce RTX 4090/PCIe/SSE2") );
}
