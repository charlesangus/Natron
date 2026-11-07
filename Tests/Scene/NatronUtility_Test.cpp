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
#include <pxr/base/gf/matrix4d.h>
#include <pxr/base/gf/rect2i.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/vt/dictionary.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/pluginRenderDelegateUniqueHandle.h>
#include <pxr/imaging/hd/rendererCreateArgsSchema.h>
#include <pxr/imaging/hd/rendererPlugin.h>
#include <pxr/imaging/hd/rendererPluginHandle.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdx/tokens.h>
#include <pxr/imaging/hf/pluginDesc.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>

#include "Global/Macros.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderBuffer.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderDelegate.h"
#include "Engine/Nodes/Scene/Utility/UtilityScene.h"

PXR_NAMESPACE_USING_DIRECTIVE

NATRON_NAMESPACE_USING

namespace {
const char* const kCubeGeometry = R"(
        int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
        int[] faceVertexIndices = [0, 1, 3, 2, 2, 3, 5, 4, 4, 5, 7, 6, 6, 7, 1, 0, 1, 7, 5, 3, 6, 0, 2, 4]
        point3f[] points = [(-1, -1, 1), (1, -1, 1), (-1, 1, 1), (1, 1, 1), (-1, 1, -1), (1, 1, -1), (-1, -1, -1), (1, -1, -1)]
)";

std::string
twoCubes()
{
    return std::string("#usda 1.0\n")
        + "def Mesh \"A\" {" + kCubeGeometry + "}\n"
        + "def Mesh \"B\" {" + kCubeGeometry + "}\n";
}

// Renders a stage with the delegate through the same engine the node drives it with, and reads back
// what the delegate's scene holds.
class UtilityRender {
public:
    explicit UtilityRender(const std::string& usda)
    {
        RenderDelegates::list();
        _layer = SdfLayer::CreateAnonymous(".usda");
        EXPECT_TRUE(_layer->ImportFromString(usda));
        stage = UsdStage::Open(_layer);

        UsdImagingGLEngine::Parameters parameters;

        parameters.rendererPluginId = TfToken(kRenderDelegateUtilityId);
        parameters.gpuEnabled = false;
        _engine.reset(new UsdImagingGLEngine(parameters));
        EXPECT_EQ(TfToken(kRenderDelegateUtilityId), _engine->GetCurrentRendererId());
        _engine->SetEnablePresentation(false);
        EXPECT_TRUE(_engine->SetRendererAovs({ HdAovTokens->color, HdAovTokens->depth, HdAovTokens->primId, HdAovTokens->instanceId }));
    }

    void render()
    {
        const int size = 16;
        GfFrustum frustum;

        frustum.SetPerspective(45., 1., 0.1, 1000.);
        frustum.SetPosition(GfVec3d(0., 0., 30.));
        _engine->SetRenderBufferSize(GfVec2i(size, size));
        _engine->SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0, 0), size, size)));
        _engine->SetCameraState(frustum.ComputeViewMatrix(), frustum.ComputeProjectionMatrix());

        UsdImagingGLRenderParams params;

        params.drawMode = UsdImagingGLDrawMode::DRAW_SHADED_SMOOTH;
        params.clearColor = GfVec4f(0.f, 0.f, 0.f, 0.f);
        params.colorCorrectionMode = HdxColorCorrectionTokens->disabled;
        params.highlight = false;
        _engine->Render(stage->GetPseudoRoot(), params);
        EXPECT_TRUE(_engine->IsConverged());
    }

    std::size_t stat(const char* key) const
    {
        return VtDictionaryGet<std::size_t>(_engine->GetRenderStats(), key, VtDefault = std::size_t(0));
    }

    void movePoint(const char* path,
                   int point,
                   const GfVec3f& position)
    {
        const UsdGeomMesh mesh(stage->GetPrimAtPath(SdfPath(path)));
        VtVec3fArray points;

        ASSERT_TRUE(mesh.GetPointsAttr().Get(&points));
        points[point] = position;
        ASSERT_TRUE(mesh.GetPointsAttr().Set(points));
    }

    UsdGeomImageable imageable(const char* path) const
    {
        return UsdGeomImageable(stage->GetPrimAtPath(SdfPath(path)));
    }

    UsdStageRefPtr stage;

private:
    SdfLayerRefPtr _layer;
    std::unique_ptr<UsdImagingGLEngine> _engine;
};
} // anon namespace

TEST(NatronUtility, IsListedAsAvailable)
{
    bool found = false;

    for (const RenderDelegates::Delegate& delegate : RenderDelegates::list()) {
        if (delegate.id == kRenderDelegateUtilityId) {
            found = true;
            EXPECT_EQ("Natron Utility", delegate.displayName);
            EXPECT_TRUE(delegate.available) << delegate.error;
        }
    }
    EXPECT_TRUE(found);
}

// Hydra picks the default by walking its renderers in priority order to the first supported one.
// Storm also needs an Hgi this test has no GL context to make, so its place in the order is the
// proof, and the default itself is checked where Storm reports itself supported.
TEST(NatronUtility, StormStaysTheDefault)
{
    RenderDelegates::list();

    HdRendererPluginRegistry& registry = HdRendererPluginRegistry::GetInstance();
    HfPluginDescVector descs;

    registry.GetPluginDescs(&descs);

    int stormPosition = -1;
    int utilityPosition = -1;

    for (std::size_t i = 0; i < descs.size(); ++i) {
        if (descs[i].id == TfToken(kRenderDelegateStormId)) {
            stormPosition = static_cast<int>(i);
        } else if (descs[i].id == TfToken(kRenderDelegateUtilityId)) {
            utilityPosition = static_cast<int>(i);
            EXPECT_EQ(50, descs[i].priority);
        }
    }
    ASSERT_GE(stormPosition, 0);
    ASSERT_GE(utilityPosition, 0);
    EXPECT_LT(stormPosition, utilityPosition);

    const HdRendererCreateArgsSchema args(HdRendererCreateArgsSchema::Builder()
                                              .SetGpuEnabled(HdRetainedTypedSampledDataSource<bool>::New(true))
                                              .Build());
    const HdRendererPluginHandle storm = registry.GetOrCreateRendererPlugin(TfToken(kRenderDelegateStormId));

    ASSERT_TRUE(static_cast<bool>(storm));
    if (storm->IsSupported(args)) {
        EXPECT_EQ(TfToken(kRenderDelegateStormId), registry.GetDefaultPluginId(args));
    }
}

TEST(NatronUtility, CreatingAndDestroyingTheDelegateLeavesNothingBehind)
{
    RenderDelegates::list();

    const int delegates = UtilityRenderDelegate::liveCount();
    const int buffers = UtilityRenderBuffer::liveCount();

    {
        const HdPluginRenderDelegateUniqueHandle delegate = HdRendererPluginRegistry::GetInstance().CreateRenderDelegate(TfToken(kRenderDelegateUtilityId));

        ASSERT_TRUE(static_cast<bool>(delegate));
        EXPECT_EQ(delegates + 1, UtilityRenderDelegate::liveCount());
        EXPECT_NE(nullptr, delegate->GetRenderParam());
        EXPECT_TRUE(static_cast<bool>(delegate->GetResourceRegistry()));

        HdBprim* bprim = delegate->CreateBprim(HdPrimTypeTokens->renderBuffer, SdfPath("/buffer"));
        HdBprim* fallback = delegate->CreateFallbackBprim(HdPrimTypeTokens->renderBuffer);

        EXPECT_EQ(buffers + 2, UtilityRenderBuffer::liveCount());
        delegate->DestroyBprim(bprim);
        delegate->DestroyBprim(fallback);

        HdSprim* camera = delegate->CreateFallbackSprim(HdPrimTypeTokens->camera);

        EXPECT_NE(nullptr, camera);
        delegate->DestroySprim(camera);
    }
    EXPECT_EQ(delegates, UtilityRenderDelegate::liveCount());
    EXPECT_EQ(buffers, UtilityRenderBuffer::liveCount());
}

TEST(NatronUtility, DrivingAStageThroughTheEngineLeavesNothingBehind)
{
    const int delegates = UtilityRenderDelegate::liveCount();
    const int buffers = UtilityRenderBuffer::liveCount();

    {
        UtilityRender render(twoCubes());

        render.render();
        EXPECT_EQ(delegates + 1, UtilityRenderDelegate::liveCount());
        EXPECT_GT(UtilityRenderBuffer::liveCount(), buffers);
    }
    EXPECT_EQ(delegates, UtilityRenderDelegate::liveCount());
    EXPECT_EQ(buffers, UtilityRenderBuffer::liveCount());
}

TEST(NatronUtility, DefaultAovDescriptorsServeTheFourAovs)
{
    const UtilityRenderDelegate delegate;

    const HdAovDescriptor color = delegate.GetDefaultAovDescriptor(HdAovTokens->color);
    EXPECT_EQ(HdFormatFloat32Vec4, color.format);
    EXPECT_EQ(VtValue(GfVec4f(0.f)), color.clearValue);

    const HdAovDescriptor depth = delegate.GetDefaultAovDescriptor(HdAovTokens->depth);
    EXPECT_EQ(HdFormatFloat32, depth.format);
    EXPECT_EQ(VtValue(1.f), depth.clearValue);

    for (const TfToken& name : { HdAovTokens->primId, HdAovTokens->instanceId }) {
        const HdAovDescriptor id = delegate.GetDefaultAovDescriptor(name);
        EXPECT_EQ(HdFormatInt32, id.format);
        EXPECT_EQ(VtValue(-1), id.clearValue);
    }

    EXPECT_EQ(HdFormatInvalid, delegate.GetDefaultAovDescriptor(HdAovTokens->normal).format);
}

TEST(NatronUtility, RenderBufferHoldsFloatAndIntPixelsInHostMemory)
{
    UtilityRenderBuffer buffer((SdfPath("/buffer")));

    EXPECT_FALSE(buffer.Allocate(GfVec3i(2, 2, 2), HdFormatFloat32Vec4, false));

    ASSERT_TRUE(buffer.Allocate(GfVec3i(3, 2, 1), HdFormatFloat32Vec4, false));
    EXPECT_EQ(3u, buffer.GetWidth());
    EXPECT_EQ(2u, buffer.GetHeight());
    EXPECT_EQ(1u, buffer.GetDepth());
    EXPECT_EQ(HdFormatFloat32Vec4, buffer.GetFormat());
    EXPECT_FALSE(buffer.IsMultiSampled());
    EXPECT_FALSE(buffer.IsConverged());
    buffer.clear(VtValue(GfVec4f(1.f, 2.f, 3.f, 4.f)));

    const float* floats = static_cast<const float*>(buffer.Map());
    ASSERT_NE(nullptr, floats);
    EXPECT_TRUE(buffer.IsMapped());
    EXPECT_EQ(3.f, floats[5 * 4 + 2]);
    buffer.Unmap();
    EXPECT_FALSE(buffer.IsMapped());

    ASSERT_TRUE(buffer.Allocate(GfVec3i(2, 2, 1), HdFormatInt32, false));
    buffer.clear(VtValue(-1));

    const int* ints = static_cast<const int*>(buffer.Map());
    ASSERT_NE(nullptr, ints);
    EXPECT_EQ(-1, ints[3]);
    buffer.Unmap();

    buffer.setConverged(true);
    buffer.Resolve();
    EXPECT_TRUE(buffer.IsConverged());
}

TEST(NatronUtility, SceneVersionBumpsOnChangesAndSnapshotListsEveryInstance)
{
    UtilityScene scene;
    const UtilityScene::GeometryId id = scene.addGeometry(SdfPath("/mesh"), 7);
    std::uint64_t version = scene.version();

    scene.setInstances(id, { GfMatrix4d(1.), GfMatrix4d(2.) });
    EXPECT_GT(scene.version(), version);

    version = scene.version();
    scene.setInstances(id, { GfMatrix4d(1.), GfMatrix4d(2.) });
    EXPECT_EQ(version, scene.version());

    VtVec3fArray points(3);
    VtVec3iArray triangles(1, GfVec3i(0, 1, 2));
    scene.setTopology(id, points, triangles);
    scene.setPoints(id, points);

    const UtilityScene::Snapshot snapshot = scene.snapshot();
    ASSERT_EQ(1u, snapshot.geometries.size());
    const UtilityScene::Geometry& geometry = snapshot.geometries.at(id);
    EXPECT_EQ(7, geometry.primId);
    EXPECT_EQ(1u, geometry.topologyVersion);
    EXPECT_EQ(2u, geometry.pointsVersion);
    ASSERT_EQ(2u, snapshot.instances.size());
    EXPECT_EQ(id, snapshot.instances[1].geometry);
    EXPECT_EQ(1, snapshot.instances[1].instanceIndex);
    EXPECT_EQ(GfMatrix4d(2.), snapshot.instances[1].transform);

    scene.removeGeometry(id);
    EXPECT_EQ(0u, scene.stats().geometryCount);
    EXPECT_EQ(0u, scene.stats().instanceCount);
}

TEST(NatronUtility, PointInstancerOfThreeCubesIsOneGeometryPlacedThreeTimes)
{
    UtilityRender render(std::string("#usda 1.0\n")
                         + "def PointInstancer \"Scatter\" {\n"
                           "    point3f[] positions = [(0, 0, 0), (3, 0, 0), (6, 0, 0)]\n"
                           "    int[] protoIndices = [0, 0, 0]\n"
                           "    rel prototypes = </Scatter/Protos/Cube>\n"
                           "    def Scope \"Protos\" {\n"
                           "        def Mesh \"Cube\" {"
                         + kCubeGeometry + "}\n"
                                           "    }\n"
                                           "}\n");

    render.render();
    EXPECT_EQ(1u, render.stat(kUtilityStatGeometryCount));
    EXPECT_EQ(3u, render.stat(kUtilityStatInstanceCount));
    EXPECT_EQ(12u, render.stat(kUtilityStatTriangleCount));
    EXPECT_EQ(1u, render.stat(kUtilityStatTopologyBuilds));
}

TEST(NatronUtility, EditingOnePointUpdatesOnlyThatMesh)
{
    UtilityRender render(twoCubes());

    render.render();
    EXPECT_EQ(2u, render.stat(kUtilityStatGeometryCount));
    const std::size_t builds = render.stat(kUtilityStatTopologyBuilds);
    const std::size_t updates = render.stat(kUtilityStatPointUpdates);
    const std::size_t colors = render.stat(kUtilityStatColorUpdates);
    const std::size_t version = render.stat(kUtilityStatSceneVersion);

    render.movePoint("/A", 0, GfVec3f(-1.5f, -1.f, 1.f));
    render.render();
    EXPECT_EQ(builds, render.stat(kUtilityStatTopologyBuilds));
    EXPECT_EQ(updates + 1, render.stat(kUtilityStatPointUpdates));
    EXPECT_EQ(colors, render.stat(kUtilityStatColorUpdates));
    EXPECT_GT(render.stat(kUtilityStatSceneVersion), version);
}

TEST(NatronUtility, APointEditThatCollapsesATriangleRebuildsTheTopology)
{
    UtilityRender render("#usda 1.0\n"
                         "def Mesh \"Triangle\" {\n"
                         "    int[] faceVertexCounts = [3]\n"
                         "    int[] faceVertexIndices = [0, 1, 2]\n"
                         "    point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n"
                         "}\n");

    render.render();
    EXPECT_EQ(1u, render.stat(kUtilityStatTriangleCount));
    const std::size_t builds = render.stat(kUtilityStatTopologyBuilds);

    render.movePoint("/Triangle", 1, GfVec3f(0.f, 0.f, 0.f));
    render.render();
    EXPECT_EQ(0u, render.stat(kUtilityStatTriangleCount));
    EXPECT_EQ(builds + 1, render.stat(kUtilityStatTopologyBuilds));
}

TEST(NatronUtility, HidingAPrimDropsItsInstanceAndRebuildsNothing)
{
    UtilityRender render(twoCubes());

    render.render();
    EXPECT_EQ(2u, render.stat(kUtilityStatInstanceCount));
    const std::size_t builds = render.stat(kUtilityStatTopologyBuilds);
    const std::size_t updates = render.stat(kUtilityStatPointUpdates);

    render.imageable("/A").MakeInvisible();
    render.render();
    EXPECT_EQ(1u, render.stat(kUtilityStatInstanceCount));
    EXPECT_EQ(builds, render.stat(kUtilityStatTopologyBuilds));
    EXPECT_EQ(updates, render.stat(kUtilityStatPointUpdates));

    render.imageable("/A").MakeVisible();
    render.render();
    EXPECT_EQ(2u, render.stat(kUtilityStatInstanceCount));
    EXPECT_EQ(builds, render.stat(kUtilityStatTopologyBuilds));
    EXPECT_EQ(updates, render.stat(kUtilityStatPointUpdates));
}

TEST(NatronUtility, AMeshHiddenFromTheStartIsNotBuiltUntilShown)
{
    UtilityRender render(std::string("#usda 1.0\n")
                         + "def Mesh \"A\" {\n"
                           "    token visibility = \"invisible\"\n"
                         + kCubeGeometry + "}\n");

    render.render();
    EXPECT_EQ(0u, render.stat(kUtilityStatInstanceCount));
    EXPECT_EQ(0u, render.stat(kUtilityStatTopologyBuilds));

    render.imageable("/A").MakeVisible();
    render.render();
    EXPECT_EQ(1u, render.stat(kUtilityStatInstanceCount));
    EXPECT_EQ(1u, render.stat(kUtilityStatTopologyBuilds));
    EXPECT_EQ(12u, render.stat(kUtilityStatTriangleCount));
}

TEST(NatronUtility, TriangulationAppliesNGonsAndHolesAndDropsDegenerates)
{
    UtilityRender render("#usda 1.0\n"
                         "def Mesh \"Faces\" {\n"
                         "    int[] faceVertexCounts = [5, 4, 3, 3]\n"
                         "    int[] faceVertexIndices = [0, 1, 2, 3, 4, 0, 1, 2, 3, 0, 0, 1, 5, 6, 7]\n"
                         "    int[] holeIndices = [1]\n"
                         "    point3f[] points = [(0, 0, 0), (2, 0, 0), (3, 2, 0), (1, 3, 0), (-1, 2, 0), (0, 0, 1), (1, 0, 1), (2, 0, 1)]\n"
                         "}\n");

    render.render();
    // The pentagon's three triangles. The quad is a hole, the next face repeats a point, and the
    // last one's points are collinear.
    EXPECT_EQ(3u, render.stat(kUtilityStatTriangleCount));
}

TEST(NatronUtility, ImplicitPrimsArriveAsMeshes)
{
    UtilityRender render("#usda 1.0\n"
                         "def Cube \"Box\" {\n"
                         "}\n");

    render.render();
    EXPECT_EQ(1u, render.stat(kUtilityStatGeometryCount));
    EXPECT_EQ(12u, render.stat(kUtilityStatTriangleCount));
}
