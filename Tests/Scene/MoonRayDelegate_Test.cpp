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
#include <fstream>
#include <iostream>
#include <iterator>
#include <set>
#include <string>

#include <gtest/gtest.h>

#include <pxr/imaging/hd/pluginRenderDelegateUniqueHandle.h>
#include <pxr/imaging/hd/renderDelegate.h>
#include <pxr/imaging/hd/rendererPlugin.h>
#include <pxr/imaging/hd/rendererPluginHandle.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hf/pluginDesc.h>

#include "Engine/Nodes/Scene/MoonRayRuntime.h"
#include "Tests/Scene/DelegateDrawFilterProbe.h"

NATRON_NAMESPACE_USING

namespace {

const char* const kMoonRayPluginId = "HdMoonrayRendererPlugin";

// The MoonRay delegate comes from the build image (tools/ci/local/build-moonray.sh), and
// AppManager registers it only on a CPU that runs it, so a build in an older image or on an older
// CPU has none. The bundled gtest has no GTEST_SKIP, so the test returns early on false;
// NATRON_TEST_REQUIRE_MOONRAY=1 turns that skip into a failure on a CPU that runs MoonRay.
bool
moonRayOrSkip()
{
    if (PXR_NS::PlugRegistry::GetInstance().GetPluginWithName("hd_moonray")) {
        return true;
    }
    const char* reason = "OpenUSD has no hd_moonray plug-in registered; this build image predates the "
                         "MoonRay render delegate (tools/ci/local/build-moonray.sh), or the CPU lacks an instruction set MoonRay needs (MoonRayRuntime::cpuRunsMoonRay)";
    // A CPU the runtime gate refuses leaves the delegate unregistered by design, so it is only a
    // failure where MoonRay can run.
    const char* require = std::getenv("NATRON_TEST_REQUIRE_MOONRAY");
    if (require && (std::strcmp(require, "1") == 0) && MoonRayRuntime::cpuRunsMoonRay()) {
        ADD_FAILURE() << "NATRON_TEST_REQUIRE_MOONRAY=1 but " << reason;
    } else {
        std::printf("[  SKIPPED ] %s\n", reason);
    }

    return false;
}

bool
envStartsWith(const char* name,
              const std::string& dir)
{
    const char* value = std::getenv(name);

    if (!value) {
        return false;
    }
    const std::string current(value);

    return (current == dir) || (current.compare(0, dir.size() + 1, dir + ":") == 0);
}

} // anon namespace

TEST(MoonRayDelegate, StartupPointsSearchPathsAtInstall)
{
    if (!moonRayOrSkip()) {
        return;
    }
    // AppManager already ran it; a second call returns the install it set up.
    const std::string prefix = MoonRayRuntime::setUp(std::string());
    ASSERT_FALSE(prefix.empty());

    const PXR_NS::PlugPluginPtr plugin = PXR_NS::PlugRegistry::GetInstance().GetPluginWithName("hd_moonray");
    ASSERT_TRUE(bool(plugin));
    EXPECT_EQ(0u, plugin->GetPath().find(prefix + "/plugin/"));

    EXPECT_TRUE(envStartsWith("RDL2_DSO_PATH", prefix + "/rdl2dso"));
    EXPECT_TRUE(envStartsWith("ARRAS_SESSION_PATH", prefix + "/sessions"));
    EXPECT_TRUE(envStartsWith("PATH", prefix + "/bin"));
}

// The abort this guards against needs a thread to win a race of a few instructions, so no run of a
// session shows it reliably. The library's dynamic symbols show whether the racing call is there.
TEST(MoonRayDelegate, ArrasClientStartsNoSessionNotificationThread)
{
    if (!moonRayOrSkip()) {
        return;
    }
    const std::string prefix = MoonRayRuntime::setUp(std::string());
    ASSERT_FALSE(prefix.empty());

    const std::string library = prefix + "/lib64/libclient_api.so";
    std::ifstream file(library, std::ios::binary);
    ASSERT_TRUE(file.good()) << library;
    const std::string bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    ASSERT_FALSE(bytes.empty()) << library;
    EXPECT_EQ(std::string::npos, bytes.find("SessionNotification"))
        << library << " still calls SessionNotification, whose thread can abort the process as a session starts";
}

TEST(MoonRayDelegate, RegistryListsMoonRay)
{
    if (!moonRayOrSkip()) {
        return;
    }
    PXR_NS::HfPluginDescVector descs;
    PXR_NS::HdRendererPluginRegistry::GetInstance().GetPluginDescs(&descs);
    const auto moonRay = std::find_if(descs.begin(), descs.end(), [](const PXR_NS::HfPluginDesc& desc) {
        return desc.id == PXR_NS::TfToken(kMoonRayPluginId);
    });
    ASSERT_TRUE(moonRay != descs.end());
    EXPECT_EQ(std::string("Moonray"), moonRay->displayName);
    // USD defaults to the lowest priority; above Storm's 0, MoonRay renders only when asked for by id.
    EXPECT_GT(moonRay->priority, 0);
}

TEST(MoonRayDelegate, CreatesHeadlessRenderDelegate)
{
    if (!moonRayOrSkip()) {
        return;
    }
    const PXR_NS::TfToken pluginId(kMoonRayPluginId);
    PXR_NS::HdRendererPluginRegistry& registry = PXR_NS::HdRendererPluginRegistry::GetInstance();

    // Loading the plug-in dlopen()s hd_moonray.so, so this is where a missing library surfaces.
    PXR_NS::HdRendererPluginHandle plugin = registry.GetOrCreateRendererPlugin(pluginId);
    ASSERT_TRUE(bool(plugin));

    // The Arras renderer only starts its render process when a frame is asked for, so creating
    // the delegate needs no execComp and no session.
    PXR_NS::HdPluginRenderDelegateUniqueHandle delegate = registry.CreateRenderDelegate(pluginId);
    ASSERT_TRUE(bool(delegate));
    EXPECT_EQ(pluginId, delegate.GetPluginId());

    const PXR_NS::TfTokenVector& rprims = delegate->GetSupportedRprimTypes();
    EXPECT_TRUE(std::find(rprims.begin(), rprims.end(), PXR_NS::HdPrimTypeTokens->mesh) != rprims.end());
    EXPECT_FALSE(delegate->GetRenderSettingDescriptors().empty());
}

// hdMoonray reads HdVisibilitySchema if a prim held invisible leaves the primId AOV, which this
// test prints; a Hidden row removes the prim, so it leaves either way.
TEST(MoonRayDelegate, DrawFilterHidesPrimsAndLights)
{
    if (!moonRayOrSkip()) {
        return;
    }
    using namespace DelegateDrawFilterProbe;
    const Scenario scenario = runScenario(Probe::eKindMoonRay);
    ASSERT_TRUE(scenario.engineMade);
    ASSERT_EQ(std::string(), scenario.seeded.error);
    ASSERT_EQ(std::string(), scenario.aBoxed.error);
    ASSERT_EQ(std::string(), scenario.bHidden.error);
    ASSERT_EQ(std::string(), scenario.lightHidden.error);

    const std::set<std::string> all { meshA(), meshB(), meshC() };
    EXPECT_EQ(all, scenario.seeded.paths);

    const bool boxedGone = scenario.aBoxed.paths.count(meshA()) == 0;
    std::cout << "MoonRay respects HdVisibilitySchema: " << ((boxedGone && (scenario.heldInvisibleAfterBox > 0)) ? "yes" : "no")
              << " (prim held invisible: " << scenario.heldInvisibleAfterBox << ", BBox prim absent from primId: " << (boxedGone ? "yes" : "no") << ")" << std::endl;
    EXPECT_GT(scenario.heldInvisibleAfterBox, 0u);
    EXPECT_EQ((std::set<std::string> { meshB(), meshC() }), scenario.aBoxed.paths);
    EXPECT_EQ((std::set<std::string> { meshC() }), scenario.bHidden.paths);
    EXPECT_EQ((std::set<std::string> { meshC() }), scenario.lightHidden.paths);

    std::cout << "MoonRay frame red/green means: lit " << scenario.bHidden.red << "/" << scenario.bHidden.green << ", light hidden "
              << scenario.lightHidden.red << "/" << scenario.lightHidden.green << std::endl;
    EXPECT_GT(scenario.bHidden.red, 0.);
    EXPECT_LT(scenario.bHidden.green, 0.1 * scenario.bHidden.red);
    // Not darker: a renderer may light a scene with no light of its own, but never in the key's red.
    EXPECT_LT(scenario.lightHidden.red - scenario.lightHidden.green, 0.1 * (scenario.bHidden.red - scenario.bHidden.green));
}
