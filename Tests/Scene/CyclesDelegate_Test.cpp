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

#include "Engine/Nodes/Scene/UsdIncludes.h"

#include "Global/Macros.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include <pxr/imaging/hd/pluginRenderDelegateUniqueHandle.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/rendererPlugin.h>
#include <pxr/imaging/hd/rendererPluginHandle.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hf/pluginDesc.h>

namespace {

const char* const kCyclesPluginId = "HdCyclesPlugin";

// The Cycles delegate comes from the build image (tools/ci/local/build-cycles.sh), not from this
// tree, so a build in an older image has none. The bundled gtest has no GTEST_SKIP, so the test
// returns early on false; NATRON_TEST_REQUIRE_CYCLES=1 turns that skip into a failure.
bool
cyclesOrSkip()
{
    if (PXR_NS::PlugRegistry::GetInstance().GetPluginWithName("hdCycles")) {
        return true;
    }
    const char* reason = "OpenUSD has no hdCycles plug-in registered; this build image predates the "
                         "Cycles render delegate (tools/ci/local/build-cycles.sh)";
    const char* require = std::getenv("NATRON_TEST_REQUIRE_CYCLES");
    if (require && (std::strcmp(require, "1") == 0)) {
        ADD_FAILURE() << "NATRON_TEST_REQUIRE_CYCLES=1 but " << reason;
    } else {
        std::printf("[  SKIPPED ] %s\n", reason);
    }

    return false;
}

} // anon namespace

TEST(CyclesDelegate, RegistryListsCycles)
{
    if (!cyclesOrSkip()) {
        return;
    }
    PXR_NS::HfPluginDescVector descs;
    PXR_NS::HdRendererPluginRegistry::GetInstance().GetPluginDescs(&descs);
    const auto cycles = std::find_if(descs.begin(), descs.end(), [](const PXR_NS::HfPluginDesc& desc) {
        return desc.id == PXR_NS::TfToken(kCyclesPluginId);
    });
    ASSERT_TRUE(cycles != descs.end());
    EXPECT_EQ(std::string("Cycles"), cycles->displayName);
}

TEST(CyclesDelegate, CreatesHeadlessRenderDelegate)
{
    if (!cyclesOrSkip()) {
        return;
    }
    const PXR_NS::TfToken pluginId(kCyclesPluginId);
    PXR_NS::HdRendererPluginRegistry& registry = PXR_NS::HdRendererPluginRegistry::GetInstance();

    // Loading the plug-in dlopen()s hdCycles.so, so this is where a missing library surfaces.
    PXR_NS::HdRendererPluginHandle plugin = registry.GetOrCreateRendererPlugin(pluginId);
    ASSERT_TRUE(bool(plugin));

    // No settings, no Hgi: Cycles opens its default CPU device.
    PXR_NS::HdPluginRenderDelegateUniqueHandle delegate = registry.CreateRenderDelegate(pluginId);
    ASSERT_TRUE(bool(delegate));
    EXPECT_EQ(pluginId, delegate.GetPluginId());

    const PXR_NS::TfTokenVector& rprims = delegate->GetSupportedRprimTypes();
    EXPECT_TRUE(std::find(rprims.begin(), rprims.end(), PXR_NS::HdPrimTypeTokens->mesh) != rprims.end());
    EXPECT_FALSE(delegate->GetRenderSettingDescriptors().empty());
}

// USD's default delegate (HdRendererPluginRegistry::GetDefaultPluginId) is the first of these
// descriptors, sorted by ascending plugInfo priority then type name, that loads and is supported.
// Storm has to come before Cycles; at equal priority HdCyclesPlugin sorts first.
TEST(CyclesDelegate, StormIsTheDefaultRendererWithCyclesInstalled)
{
    if (!cyclesOrSkip()) {
        return;
    }
    PXR_NS::HfPluginDescVector descs;
    PXR_NS::HdRendererPluginRegistry::GetInstance().GetPluginDescs(&descs);
    const auto indexOf = [&descs](const char* id) {
        const auto it = std::find_if(descs.begin(), descs.end(), [id](const PXR_NS::HfPluginDesc& desc) {
            return desc.id == PXR_NS::TfToken(id);
        });

        return it - descs.begin();
    };
    const auto storm = indexOf("HdStormRendererPlugin");
    const auto cycles = indexOf(kCyclesPluginId);
    ASSERT_LT(storm, static_cast<decltype(storm)>(descs.size()));
    ASSERT_LT(cycles, static_cast<decltype(cycles)>(descs.size()));
    EXPECT_LT(storm, cycles) << "Storm priority " << descs[storm].priority << ", Cycles priority " << descs[cycles].priority;
    EXPECT_GT(descs[cycles].priority, descs[storm].priority);
}
