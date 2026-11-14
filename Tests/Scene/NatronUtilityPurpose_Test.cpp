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

#include <pxr/base/gf/frustum.h>
#include <pxr/base/gf/rect2i.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/vt/dictionary.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdx/tokens.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>

#include "Global/Macros.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/SceneOps.h"
#include "Engine/Nodes/Scene/Utility/EmbreeTracer.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderDelegate.h"
#include "Engine/Nodes/Scene/Utility/UtilityScene.h"
#include "Engine/Nodes/Scene/Utility/UtilitySubdiv.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_USING

namespace {
const char* const kFixture = NATRON_TESTS_FIXTURES_DIR "/scene-purposes.usda";
const int kRenderSize = 64;

struct Purposes {
    bool render;
    bool proxy;
    bool guide;
};

// The fixture driven directly through the engine, for the delegate's counters.
class PurposeEngine {
public:
    PurposeEngine()
    {
        RenderDelegates::list();
        _stage = UsdStage::Open(kFixture);
        EXPECT_TRUE(_stage);

        UsdImagingGLEngine::Parameters parameters;

        parameters.rendererPluginId = TfToken(kRenderDelegateUtilityId);
        parameters.gpuEnabled = false;
        _engine.reset(new UsdImagingGLEngine(parameters));
        _engine->SetEnablePresentation(false);
        _engine->SetRendererSetting(TfToken(kUtilitySettingSubdivLevel), VtValue(0));
        EXPECT_TRUE(_engine->SetRendererAovs({ HdAovTokens->color, HdAovTokens->depth, HdAovTokens->primId, HdAovTokens->instanceId }));
    }

    void render(const Purposes& purposes)
    {
        GfFrustum camera;

        camera.SetPerspective(45., 1., 0.1, 1000.);
        camera.SetPosition(GfVec3d(0., 0., 20.));
        _engine->SetRenderBufferSize(GfVec2i(kRenderSize, kRenderSize));
        _engine->SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0, 0), kRenderSize, kRenderSize)));
        _engine->SetCameraState(camera.ComputeViewMatrix(), camera.ComputeProjectionMatrix());

        UsdImagingGLRenderParams params;

        params.drawMode = UsdImagingGLDrawMode::DRAW_SHADED_SMOOTH;
        params.clearColor = GfVec4f(0.f, 0.f, 0.f, 0.f);
        params.colorCorrectionMode = HdxColorCorrectionTokens->disabled;
        params.highlight = false;
        HydraRender::setPurposes(_engine.get(), kRenderDelegateUtilityId, purposes.render, purposes.proxy, purposes.guide, &params);
        // A frame draws on the delegate's thread, and Render() only starts it.
        const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + std::chrono::seconds(30);

        _engine->Render(_stage->GetPseudoRoot(), params);
        while (!_engine->IsConverged() && std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            _engine->Render(_stage->GetPseudoRoot(), params);
        }
        EXPECT_TRUE(_engine->IsConverged());
    }

    std::size_t stat(const char* key) const
    {
        return VtDictionaryGet<std::size_t>(_engine->GetRenderStats(), key, VtDefault = std::size_t(0));
    }

private:
    UsdStageRefPtr _stage;
    std::unique_ptr<UsdImagingGLEngine> _engine;
};

std::set<std::string>
shownThroughTheHost(const void* owner,
                    const Purposes& purposes)
{
    UsdStageRefPtr stage = UsdStage::Open(kFixture);
    HydraRender::Request request;
    HydraRender::Result result;
    std::string error;
    std::set<std::string> shown;

    request.owner = owner;
    request.scene = SceneOps::appendLayer(ScenePayload(), stage->GetRootLayer(), 1);
    request.time = UsdTimeCode(1.);
    request.width = kRenderSize;
    request.height = kRenderSize;
    request.renderer = kRenderDelegateUtilityId;
    request.showRender = purposes.render;
    request.showProxy = purposes.proxy;
    request.showGuides = purposes.guide;
    EXPECT_TRUE(HydraRender::cameraFrustum(stage, SdfPath("/World/Cam"), request.time, 1., HydraRender::eFitModeFill, &request.frustum, &error)) << error;
    EXPECT_TRUE(HydraRender::render(request, &result, &error)) << error;
    for (const auto& entry : result.primPaths) {
        shown.insert(entry.second);
    }

    return shown;
}

UtilityScene::GeometryId
addTriangle(UtilityScene& scene,
            const char* path,
            const char* tag)
{
    const UtilityScene::GeometryId id = scene.addGeometry(SdfPath(path), 1);

    scene.setTopology(id, VtVec3fArray { GfVec3f(0.f, 0.f, 0.f), GfVec3f(1.f, 0.f, 0.f), GfVec3f(0.f, 1.f, 0.f) }, VtVec3iArray { GfVec3i(0, 1, 2) });
    scene.setRenderTag(id, TfToken(tag));
    scene.setInstances(id, { GfMatrix4d(1.) });

    return id;
}
} // anon namespace

TEST(NatronUtilityPurpose, TheHostDrawsDefaultAndRenderUntilAskedOtherwise)
{
    const int owner = 0;
    const std::string defaultPrim = "/World/Default";
    const std::string renderPrim = "/World/Render";
    const std::string proxyPrim = "/World/Proxy";
    const std::string guidePrim = "/World/Guide";
    const HydraRender::Request defaults;

    EXPECT_EQ(std::set<std::string>({ defaultPrim, renderPrim }), shownThroughTheHost(&owner, { defaults.showRender, defaults.showProxy, defaults.showGuides }));
    EXPECT_EQ(std::set<std::string>({ defaultPrim, renderPrim, proxyPrim }), shownThroughTheHost(&owner, { true, true, false }));
    EXPECT_EQ(std::set<std::string>({ defaultPrim, proxyPrim }), shownThroughTheHost(&owner, { false, true, false }));
    EXPECT_EQ(std::set<std::string>({ defaultPrim, guidePrim }), shownThroughTheHost(&owner, { false, false, true }));
    EXPECT_EQ(std::set<std::string>({ defaultPrim }), shownThroughTheHost(&owner, { false, false, false }));
    EXPECT_EQ(std::set<std::string>({ defaultPrim, renderPrim, proxyPrim, guidePrim }), shownThroughTheHost(&owner, { true, true, true }));
    HydraRender::releaseAll();
}

// Every purpose's prims are built on the first render, hidden ones included, so no toggle builds
// anything, even showing a purpose for the first time.
TEST(NatronUtilityPurpose, TogglingBuildsNothing)
{
    PurposeEngine engine;

    engine.render({ true, false, false });
    EXPECT_EQ(4u, engine.stat(kUtilityStatGeometryCount));
    EXPECT_EQ(4u, engine.stat(kUtilityStatTopologyBuilds));
    EXPECT_EQ(2u, engine.stat(kUtilityStatInstanceCount));

    const std::size_t points = engine.stat(kUtilityStatPointUpdates);
    const std::size_t colors = engine.stat(kUtilityStatColorUpdates);

    engine.render({ true, true, false });
    EXPECT_EQ(3u, engine.stat(kUtilityStatInstanceCount));
    engine.render({ true, true, true });
    EXPECT_EQ(4u, engine.stat(kUtilityStatInstanceCount));
    engine.render({ true, false, false });
    EXPECT_EQ(2u, engine.stat(kUtilityStatInstanceCount));
    engine.render({ false, true, false });
    EXPECT_EQ(2u, engine.stat(kUtilityStatInstanceCount));
    engine.render({ false, false, false });
    EXPECT_EQ(1u, engine.stat(kUtilityStatInstanceCount));

    EXPECT_EQ(4u, engine.stat(kUtilityStatTopologyBuilds));
    EXPECT_EQ(4u, engine.stat(kUtilityStatGeometryCount));
    EXPECT_EQ(points, engine.stat(kUtilityStatPointUpdates));
    EXPECT_EQ(colors, engine.stat(kUtilityStatColorUpdates));
}

TEST(NatronUtilityPurpose, ChangingTheTagsRebuildsOnlyTheInstanceScene)
{
    UtilityScene scene;

    addTriangle(scene, "/render", "render");
    addTriangle(scene, "/proxy", "proxy");
    scene.setRenderTags({ TfToken("render") });

    EmbreeTracer tracer;

    tracer.update(scene.snapshot(), 1);
    EXPECT_EQ(2u, tracer.stats().geometryBuilds);

    const std::size_t instanceBuilds = tracer.stats().instanceSceneBuilds;

    scene.setRenderTags({ TfToken("proxy") });
    UtilityScene::Snapshot snapshot = scene.snapshot();
    ASSERT_EQ(2u, snapshot.geometries.size());
    ASSERT_EQ(1u, snapshot.instances.size());
    tracer.update(snapshot, 1);
    EXPECT_EQ(2u, tracer.stats().geometryBuilds);
    EXPECT_EQ(0u, tracer.stats().geometryRefits);
    EXPECT_EQ(instanceBuilds + 1, tracer.stats().instanceSceneBuilds);
}

TEST(NatronUtilityPurpose, TagsBumpTheVersionOnlyWhenTheyChange)
{
    UtilityScene scene;
    const UtilityScene::GeometryId id = addTriangle(scene, "/a", "render");
    const std::uint64_t tagged = scene.version();

    scene.setRenderTag(id, TfToken("render"));
    EXPECT_EQ(tagged, scene.version());
    scene.setRenderTag(id, TfToken("proxy"));
    EXPECT_LT(tagged, scene.version());

    const std::uint64_t retagged = scene.version();

    scene.setRenderTags({ TfToken("proxy") });
    EXPECT_LT(retagged, scene.version());

    const std::uint64_t filtered = scene.version();

    scene.setRenderTags({ TfToken("proxy") });
    EXPECT_EQ(filtered, scene.version());
    EXPECT_EQ(1u, scene.snapshot().instances.size());
    scene.setRenderTags({ TfToken("render") });
    EXPECT_TRUE(scene.snapshot().instances.empty());
    EXPECT_EQ(0u, scene.stats().instanceCount);
    scene.setRenderTags(TfTokenVector());
    EXPECT_EQ(1u, scene.snapshot().instances.size());
}
