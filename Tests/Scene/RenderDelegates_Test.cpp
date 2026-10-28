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

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/ScenePayload.h"

NATRON_NAMESPACE_USING

// The test mains register Tests/fixtures/renderDelegates, a plugInfo.json declaring this renderer
// with a library that does not exist.
namespace {
const char* const kStubId = "NatronTestStubRendererPlugin";
}

TEST(RenderDelegates, StormIsListedFirstAndAvailable)
{
    const std::vector<RenderDelegates::Delegate> delegates = RenderDelegates::list();

    ASSERT_FALSE(delegates.empty());
    EXPECT_EQ(kRenderDelegateStormId, delegates.front().id);
    EXPECT_EQ("Storm", delegates.front().displayName);
    EXPECT_TRUE(delegates.front().available) << delegates.front().error;
    EXPECT_TRUE(RenderDelegates::isStorm(delegates.front().id));
    EXPECT_TRUE(RenderDelegates::unavailableMessage(kRenderDelegateStormId).empty());
}

TEST(RenderDelegates, ARendererWhoseLibraryIsMissingIsUnavailableWithTheLoadersError)
{
    RenderDelegates::Delegate stub;

    ASSERT_TRUE(RenderDelegates::find(kStubId, &stub));
    EXPECT_EQ("Natron Test Stub", stub.displayName);
    EXPECT_FALSE(stub.available);
    EXPECT_NE(std::string::npos, stub.error.find("libNatronTestStubRendererMissing.so")) << stub.error;
    const std::string message = RenderDelegates::unavailableMessage(kStubId);
    EXPECT_NE(std::string::npos, message.find("Natron Test Stub")) << message;
    EXPECT_NE(std::string::npos, message.find(kStubId)) << message;
}

TEST(RenderDelegates, AnUnknownRendererIsNotFoundAndItsMessageNamesIt)
{
    RenderDelegates::Delegate missing;

    EXPECT_FALSE(RenderDelegates::find("NoSuchRendererPlugin", &missing));
    const std::string message = RenderDelegates::unavailableMessage("NoSuchRendererPlugin");
    EXPECT_NE(std::string::npos, message.find("NoSuchRendererPlugin")) << message;
}

// An unavailable renderer fails before any engine or GL work: no fallback to Storm.
TEST(RenderDelegates, RenderingWithAnUnavailableRendererIsAnErrorAndMakesNoEngine)
{
    PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(NATRON_TESTS_FIXTURES_DIR "/scene-reference.usda");
    ASSERT_TRUE(stage);
    const int owner = 0;
    HydraRender::Request request;
    request.owner = &owner;
    request.scene = SceneOps::appendLayer(ScenePayload(), stage->GetRootLayer(), 1);
    request.time = PXR_NS::UsdTimeCode(1.);
    request.width = 16;
    request.height = 16;
    request.renderer = kStubId;
    std::string error;
    ASSERT_TRUE(HydraRender::cameraFrustum(stage, PXR_NS::SdfPath("/World/Cam"), request.time, 1., HydraRender::eFitModeFill, &request.frustum, &error)) << error;

    const std::size_t stormEngines = HydraRender::getEngineCount();
    const std::size_t cpuEngines = HydraRender::getCpuEngineCount();
    const std::uint64_t builds = HydraRender::getEngineBuildCount();
    HydraRender::Result result;
    EXPECT_FALSE(HydraRender::render(request, &result, &error));
    EXPECT_NE(std::string::npos, error.find(kStubId)) << error;
    EXPECT_EQ(stormEngines, HydraRender::getEngineCount());
    EXPECT_EQ(cpuEngines, HydraRender::getCpuEngineCount());
    EXPECT_EQ(builds, HydraRender::getEngineBuildCount());

    std::vector<float> pixels;
    HydraRender::PlaneWindow window;
    window.x2 = 16;
    window.y2 = 16;
    error.clear();
    EXPECT_FALSE(HydraRender::renderWindow(request, window, &pixels, nullptr, &error));
    EXPECT_NE(std::string::npos, error.find(kStubId)) << error;
    EXPECT_EQ(builds, HydraRender::getEngineBuildCount());
}
