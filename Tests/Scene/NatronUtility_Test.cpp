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
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/rendererCreateArgsSchema.h>
#include <pxr/imaging/hd/rendererPlugin.h>
#include <pxr/imaging/hd/rendererPluginHandle.h>
#include <pxr/imaging/hd/rendererPluginRegistry.h>
#include <pxr/imaging/hd/retainedDataSource.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/imaging/hdx/tokens.h>
#include <pxr/imaging/hf/pluginDesc.h>
#include <pxr/imaging/hio/image.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/imageable.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>

#include "Global/Macros.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <QTemporaryDir>

#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderBuffer.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderDelegate.h"
#include "Engine/Nodes/Scene/Utility/UtilityScene.h"
#include "Engine/Nodes/Scene/Utility/WavefrontGpu.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/AppManager.h"
#include "Engine/Gpu/GpuBufferPool.h"
#include "Engine/Gpu/GpuRuntime.h"
#include "Engine/GpuContext.h"
#include "Gpu/GpuTestDevice.h"
#include "Gpu/GpuTestSelector.h"
#endif

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

const int kRenderSize = 16;
const double kCameraZ = 30.;

// The window holds float depth with the camera's near plane at 0.1, so a distance of about 30 comes back
// only to within about a thousandth.
const double kDistanceTolerance = 5e-3;

// Every stage the harness renders holds /EmitDisplayColor: with no light in the scene, a prim shows its
// display colour only through a material that emits it.
const char* const kEmitDisplayColorMaterial = R"(
def Material "EmitDisplayColor" {
    token outputs:surface.connect = </EmitDisplayColor/Surface.outputs:surface>
    def Shader "Surface" {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (0, 0, 0)
        color3f inputs:emissiveColor.connect = </EmitDisplayColor/Color.outputs:result>
        float inputs:opacity.connect = </EmitDisplayColor/Opacity.outputs:result>
        token outputs:surface
    }
    def Shader "Color" {
        uniform token info:id = "UsdPrimvarReader_float3"
        string inputs:varname = "displayColor"
        float3 outputs:result
    }
    def Shader "Opacity" {
        uniform token info:id = "UsdPrimvarReader_float"
        string inputs:varname = "displayOpacity"
        float outputs:result
    }
}
)";

const std::string kRedColor = "    color3f[] primvars:displayColor = [(1, 0, 0)] (interpolation = \"constant\")\n"
                              "    rel material:binding = </EmitDisplayColor>\n";

std::string
cubeAt(const char* name,
       const std::string& attributes)
{
    return std::string("def Mesh \"") + name + "\" (prepend apiSchemas = [\"MaterialBindingAPI\"]) {" + kCubeGeometry + attributes + "}\n";
}

std::string
boundTo(const char* material)
{
    return std::string("    rel material:binding = <") + material + ">\n";
}

// A UsdPreviewSurface material at /name with the given inputs, one per line.
std::string
previewSurface(const char* name,
               const std::string& inputs)
{
    return std::string("def Material \"") + name + "\" {\n"
        + "    token outputs:surface.connect = </" + name + "/Surface.outputs:surface>\n"
        + "    def Shader \"Surface\" {\n"
          "        uniform token info:id = \"UsdPreviewSurface\"\n"
        + inputs
        + "        token outputs:surface\n"
          "    }\n"
          "}\n";
}

std::string
translated(double x,
           double y)
{
    return "    double3 xformOp:translate = (" + std::to_string(x) + ", " + std::to_string(y) + ", 0)\n"
                                                                                                "    uniform token[] xformOpOrder = [\"xformOp:translate\"]\n";
}

// Renders a stage with the delegate through the same engine the node drives it with, and reads back
// what the delegate's scene holds.
class UtilityRender {
public:
    explicit UtilityRender(const std::string& usda)
    {
        RenderDelegates::list();
        _layer = SdfLayer::CreateAnonymous(".usda");
        EXPECT_TRUE(_layer->ImportFromString(usda + kEmitDisplayColorMaterial));
        stage = UsdStage::Open(_layer);

        UsdImagingGLEngine::Parameters parameters;

        parameters.rendererPluginId = TfToken(kRenderDelegateUtilityId);
        parameters.gpuEnabled = false;
        _engine.reset(new UsdImagingGLEngine(parameters));
        EXPECT_EQ(TfToken(kRenderDelegateUtilityId), _engine->GetCurrentRendererId());
        _engine->SetEnablePresentation(false);
        EXPECT_TRUE(_engine->SetRendererAovs({ HdAovTokens->color, HdAovTokens->depth, HdAovTokens->primId, HdAovTokens->instanceId }));
    }

    // A frame draws on the delegate's thread, and Render() only starts it.
    void render()
    {
        const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + std::chrono::seconds(30);

        renderWithoutChecks();
        while (!_engine->IsConverged() && std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            renderWithoutChecks();
        }
        EXPECT_TRUE(_engine->IsConverged());
    }

    void renderWithoutChecks()
    {
        const GfFrustum camera = frustum();

        _engine->SetRenderBufferSize(GfVec2i(kRenderSize, kRenderSize));
        _engine->SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0, 0), kRenderSize, kRenderSize)));
        _engine->SetCameraState(camera.ComputeViewMatrix(), camera.ComputeProjectionMatrix());

        UsdImagingGLRenderParams params;

        params.drawMode = UsdImagingGLDrawMode::DRAW_SHADED_SMOOTH;
        params.clearColor = GfVec4f(0.f, 0.f, 0.f, 0.f);
        params.colorCorrectionMode = HdxColorCorrectionTokens->disabled;
        params.highlight = false;
        _engine->Render(stage->GetPseudoRoot(), params);
    }

    bool isConverged() const
    {
        return _engine->IsConverged();
    }

    bool stopRenderer()
    {
        return _engine->StopRenderer();
    }

    bool restartRenderer()
    {
        return _engine->RestartRenderer();
    }

    GfVec4f colorAt(int x,
                    int y) const
    {
        return pixelAt<GfVec4f>(HdAovTokens->color, x, y);
    }

    float windowDepthAt(int x,
                        int y) const
    {
        return pixelAt<float>(HdAovTokens->depth, x, y);
    }

    // The window depth at the pixel's centre, carried back through the camera to a distance along its axis.
    double distanceAt(int x,
                      int y) const
    {
        const GfMatrix4d inverse = frustum().ComputeProjectionMatrix().GetInverse();
        const double ndcX = (x + 0.5) / kRenderSize * 2. - 1.;
        const double ndcY = (y + 0.5) / kRenderSize * 2. - 1.;
        const double ndcZ = windowDepthAt(x, y) * 2. - 1.;
        const double z = ndcX * inverse[0][2] + ndcY * inverse[1][2] + ndcZ * inverse[2][2] + inverse[3][2];
        const double w = ndcX * inverse[0][3] + ndcY * inverse[1][3] + ndcZ * inverse[2][3] + inverse[3][3];

        return -z / w;
    }

    int primIdAt(int x,
                 int y) const
    {
        return pixelAt<std::int32_t>(HdAovTokens->primId, x, y);
    }

    int instanceIdAt(int x,
                     int y) const
    {
        return pixelAt<std::int32_t>(HdAovTokens->instanceId, x, y);
    }

    std::size_t stat(const char* key) const
    {
        return VtDictionaryGet<std::size_t>(_engine->GetRenderStats(), key, VtDefault = std::size_t(0));
    }

    std::string textStat(const char* key) const
    {
        return VtDictionaryGet<std::string>(_engine->GetRenderStats(), key, VtDefault = std::string());
    }

    void setDevice(const char* device)
    {
        _engine->SetRendererSetting(TfToken(kUtilitySettingDevice), VtValue(TfToken(device)));
    }

    // A failed frame surfaces as the next Render() throwing. Returns its message, or empty when the
    // frame converged instead.
    std::string renderError()
    {
        const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + std::chrono::seconds(30);

        while (std::chrono::steady_clock::now() < end) {
            try {
                renderWithoutChecks();
            } catch (const std::runtime_error& e) {
                return e.what();
            }
            if (_engine->IsConverged()) {
                return std::string();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ADD_FAILURE() << "the frame neither converged nor failed";

        return std::string();
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
    static GfFrustum frustum()
    {
        GfFrustum camera;

        camera.SetPerspective(45., 1., 0.1, 1000.);
        camera.SetPosition(GfVec3d(0., 0., kCameraZ));

        return camera;
    }

    template <class T>
    T pixelAt(const TfToken& aov,
              int x,
              int y) const
    {
        HdRenderBuffer* buffer = _engine->GetAovRenderBuffer(aov);

        if (!buffer) {
            ADD_FAILURE() << "no buffer for " << aov.GetString();

            return T();
        }
        EXPECT_EQ(sizeof(T), HdDataSizeOfFormat(buffer->GetFormat()));
        EXPECT_EQ(static_cast<unsigned int>(kRenderSize), buffer->GetWidth());
        EXPECT_EQ(static_cast<unsigned int>(kRenderSize), buffer->GetHeight());

        const T* pixels = static_cast<const T*>(buffer->Map());
        const T value = pixels[y * kRenderSize + x];

        buffer->Unmap();

        return value;
    }

    SdfLayerRefPtr _layer;
    std::unique_ptr<UsdImagingGLEngine> _engine;
};
// A 4x4 texture whose texel in column x of row y, counted from the top of the file, is
// (80 x, 80 y, 20 (x + y) + 60, 255).
const int kTextureSize = 4;

GfVec3f
writtenTexel(int x,
             int rowFromTop)
{
    return GfVec3f(80.f * x, 80.f * rowFromTop, 20.f * (x + rowFromTop) + 60.f) / 255.f;
}

bool
writeTexture(const std::string& path)
{
    std::vector<unsigned char> bytes;

    for (int y = 0; y < kTextureSize; ++y) {
        for (int x = 0; x < kTextureSize; ++x) {
            const GfVec3f texel = writtenTexel(x, y) * 255.f;

            bytes.push_back((unsigned char)std::lround(texel[0]));
            bytes.push_back((unsigned char)std::lround(texel[1]));
            bytes.push_back((unsigned char)std::lround(texel[2]));
            bytes.push_back(255);
        }
    }

    HioImage::StorageSpec spec;

    spec.width = kTextureSize;
    spec.height = kTextureSize;
    spec.depth = 1;
    spec.format = HioFormatUNorm8Vec4;
    spec.flipped = false;
    spec.data = bytes.data();

    const HioImageSharedPtr image = HioImage::OpenForWriting(path);

    return image && image->Write(spec);
}

// A quad filling the frame at z = 0 that emits the texture at path. Its texture coordinates put the
// centres of pixels 2, 6, 10 and 14 on the centres of the texels, both ways.
std::string
texturedQuad(const std::string& path)
{
    const double h = kCameraZ * std::tan(22.5 * M_PI / 180.);
    const std::string lo = std::to_string(-h);
    const std::string hi = std::to_string(h);
    const std::string a = std::to_string(-1. / 32.);
    const std::string b = std::to_string(31. / 32.);

    return "def Material \"Tex\" {\n"
           "    token outputs:surface.connect = </Tex/Surface.outputs:surface>\n"
           "    def Shader \"Surface\" {\n"
           "        uniform token info:id = \"UsdPreviewSurface\"\n"
           "        color3f inputs:diffuseColor = (0, 0, 0)\n"
           "        color3f inputs:emissiveColor.connect = </Tex/Image.outputs:rgb>\n"
           "        token outputs:surface\n"
           "    }\n"
           "    def Shader \"Image\" {\n"
           "        uniform token info:id = \"UsdUVTexture\"\n"
           "        asset inputs:file = @"
        + path + "@\n"
                 "        token inputs:sourceColorSpace = \"raw\"\n"
                 "        token inputs:wrapS = \"clamp\"\n"
                 "        token inputs:wrapT = \"clamp\"\n"
                 "        float2 inputs:st.connect = </Tex/St.outputs:result>\n"
                 "        float3 outputs:rgb\n"
                 "    }\n"
                 "    def Shader \"St\" {\n"
                 "        uniform token info:id = \"UsdPrimvarReader_float2\"\n"
                 "        string inputs:varname = \"st\"\n"
                 "        float2 outputs:result\n"
                 "    }\n"
                 "}\n"
                 "def Mesh \"Quad\" (prepend apiSchemas = [\"MaterialBindingAPI\"]) {\n"
                 "    int[] faceVertexCounts = [4]\n"
                 "    int[] faceVertexIndices = [0, 1, 2, 3]\n"
                 "    point3f[] points = [("
        + lo + ", " + lo + ", 0), (" + hi + ", " + lo + ", 0), (" + hi + ", " + hi + ", 0), (" + lo + ", " + hi + ", 0)]\n"
                                                                                                                  "    texCoord2f[] primvars:st = [("
        + a + ", " + a + "), (" + b + ", " + a + "), (" + b + ", " + b + "), (" + a + ", " + b + ")] (interpolation = \"vertex\")\n"
                                                                                                 "    rel material:binding = </Tex>\n"
                                                                                                 "}\n";
}

void
expectTexelCentres(const std::function<GfVec4f(int, int)>& colorAt)
{
    for (int y = 0; y < kTextureSize; ++y) {
        for (int x = 0; x < kTextureSize; ++x) {
            const GfVec4f color = colorAt(4 * x + 2, 4 * y + 2);
            const GfVec3f expected = writtenTexel(x, kTextureSize - 1 - y);

            for (int c = 0; c < 3; ++c) {
                EXPECT_NEAR(expected[c], color[c], 1e-3) << "texel " << x << ", " << y << " channel " << c;
            }
            EXPECT_FLOAT_EQ(1.f, color[3]) << "texel " << x << ", " << y;
        }
    }
}
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

// The cube is two units wide at 29 from the camera, which covers the four pixels around the image's centre.
TEST(NatronUtilityRender, RedCubeShowsColorDepthAndIds)
{
    UtilityRender render(std::string("#usda 1.0\n") + cubeAt("Cube", kRedColor));

    render.render();

    for (const int x : { 7, 8 }) {
        for (const int y : { 7, 8 }) {
            const GfVec4f color = render.colorAt(x, y);

            EXPECT_FLOAT_EQ(1.f, color[0]);
            EXPECT_FLOAT_EQ(0.f, color[1]);
            EXPECT_FLOAT_EQ(0.f, color[2]);
            EXPECT_FLOAT_EQ(1.f, color[3]);
            EXPECT_NEAR(kCameraZ - 1., render.distanceAt(x, y), kDistanceTolerance);
            EXPECT_GE(render.primIdAt(x, y), 0);
            EXPECT_EQ(0, render.instanceIdAt(x, y));
        }
    }
}

TEST(NatronUtilityRender, MissedPixelsKeepTheirClearValues)
{
    UtilityRender render(std::string("#usda 1.0\n") + cubeAt("Cube", kRedColor));

    render.render();

    const int last = kRenderSize - 1;

    for (const auto& corner : { GfVec2i(0, 0), GfVec2i(last, 0), GfVec2i(0, last), GfVec2i(last, last) }) {
        EXPECT_EQ(GfVec4f(0.f), render.colorAt(corner[0], corner[1]));
        EXPECT_EQ(1.f, render.windowDepthAt(corner[0], corner[1]));
        EXPECT_EQ(-1, render.primIdAt(corner[0], corner[1]));
        EXPECT_EQ(-1, render.instanceIdAt(corner[0], corner[1]));
    }
}

TEST(NatronUtilityRender, ImageRowsRunUpwardFromTheBottom)
{
    UtilityRender render(std::string("#usda 1.0\n") + cubeAt("Cube", kRedColor + translated(0., 6.)));

    render.render();
    EXPECT_FLOAT_EQ(1.f, render.colorAt(8, 12)[3]);
    EXPECT_FLOAT_EQ(0.f, render.colorAt(8, 3)[3]);
}

TEST(NatronUtilityRender, AMeshWithoutAMaterialInALightlessSceneIsOpaqueBlack)
{
    UtilityRender render(std::string("#usda 1.0\n") + cubeAt("Cube", kRedColor.substr(0, kRedColor.find('\n') + 1)));

    render.render();

    EXPECT_EQ(GfVec4f(0.f, 0.f, 0.f, 1.f), render.colorAt(8, 8));
    EXPECT_EQ(std::string(), render.textStat(kUtilityStatSceneWarning));
}

TEST(NatronUtilityRender, AnEmissiveMaterialShowsItsEmission)
{
    UtilityRender render(std::string("#usda 1.0\n")
                         + previewSurface("Glow", "        color3f inputs:emissiveColor = (0.25, 0.5, 1)\n")
                         + cubeAt("Cube", boundTo("/Glow")));

    render.render();

    const GfVec4f color = render.colorAt(8, 8);

    EXPECT_FLOAT_EQ(0.25f, color[0]);
    EXPECT_FLOAT_EQ(0.5f, color[1]);
    EXPECT_FLOAT_EQ(1.f, color[2]);
    EXPECT_FLOAT_EQ(1.f, color[3]);
}

TEST(NatronUtilityRender, ANonEmissiveMaterialInALightlessSceneIsBlack)
{
    UtilityRender render(std::string("#usda 1.0\n")
                         + previewSurface("White", "        color3f inputs:diffuseColor = (1, 1, 1)\n"
                                                   "        float inputs:metallic = 0.5\n")
                         + cubeAt("Cube", boundTo("/White")));

    render.render();

    EXPECT_EQ(GfVec4f(0.f, 0.f, 0.f, 1.f), render.colorAt(8, 8));
}

TEST(NatronUtilityRender, AMaterialXMaterialIsMagentaAndTheWarningNamesIt)
{
    UtilityRender render(std::string("#usda 1.0\n")
                         + "def Material \"Standard\" {\n"
                           "    token outputs:surface.connect = </Standard/Surface.outputs:surface>\n"
                           "    def Shader \"Surface\" {\n"
                           "        uniform token info:id = \"ND_standard_surface_surfaceshader\"\n"
                           "        token outputs:surface\n"
                           "    }\n"
                           "}\n"
                         + cubeAt("Left", boundTo("/Standard") + translated(-6., 0.))
                         + cubeAt("Right", kRedColor + translated(6., 0.)));

    render.render();

    EXPECT_EQ(GfVec4f(1.f, 0.f, 1.f, 1.f), render.colorAt(3, 8));
    EXPECT_EQ(GfVec4f(1.f, 0.f, 0.f, 1.f), render.colorAt(12, 8));

    const std::string warning = render.textStat(kUtilityStatSceneWarning);

    EXPECT_NE(std::string::npos, warning.find("/Standard")) << warning;
    EXPECT_EQ(std::string::npos, warning.find("/EmitDisplayColor")) << warning;
    EXPECT_EQ(std::string(), render.textStat(kUtilityStatWarning));
}

TEST(NatronUtilityRender, ColorIsPremultipliedByOpacity)
{
    UtilityRender render(std::string("#usda 1.0\n") + cubeAt("Cube", std::string(kRedColor) + "    float[] primvars:displayOpacity = [0.5] (interpolation = \"constant\")\n"));

    render.render();

    const GfVec4f color = render.colorAt(8, 8);

    EXPECT_FLOAT_EQ(0.5f, color[0]);
    EXPECT_FLOAT_EQ(0.f, color[1]);
    EXPECT_FLOAT_EQ(0.5f, color[3]);
}

TEST(NatronUtilityRender, ATextureSamplesItsTexelsAtTheirCentres)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("grid.png")).toStdString();
    ASSERT_TRUE(writeTexture(path));

    UtilityRender render(std::string("#usda 1.0\n") + texturedQuad(path));

    render.render();
    expectTexelCentres([&render](int x, int y) { return render.colorAt(x, y); });
    EXPECT_EQ(std::string(), render.textStat(kUtilityStatSceneWarning));
}

TEST(NatronUtilityRender, AMissingTextureReadsItsFallbackAndTheWarningNamesIt)
{
    UtilityRender render(std::string("#usda 1.0\n")
                         + "def Material \"Tex\" {\n"
                           "    token outputs:surface.connect = </Tex/Surface.outputs:surface>\n"
                           "    def Shader \"Surface\" {\n"
                           "        uniform token info:id = \"UsdPreviewSurface\"\n"
                           "        color3f inputs:emissiveColor.connect = </Tex/Image.outputs:rgb>\n"
                           "        token outputs:surface\n"
                           "    }\n"
                           "    def Shader \"Image\" {\n"
                           "        uniform token info:id = \"UsdUVTexture\"\n"
                           "        asset inputs:file = @/nonexistent/m281/missing.png@\n"
                           "        float4 inputs:fallback = (0.5, 0.25, 0.125, 1)\n"
                           "        float2 inputs:st.connect = </Tex/St.outputs:result>\n"
                           "        float3 outputs:rgb\n"
                           "    }\n"
                           "    def Shader \"St\" {\n"
                           "        uniform token info:id = \"UsdPrimvarReader_float2\"\n"
                           "        string inputs:varname = \"st\"\n"
                           "        float2 outputs:result\n"
                           "    }\n"
                           "}\n"
                         + cubeAt("Cube", boundTo("/Tex") + "    texCoord2f[] primvars:st = [(0.5, 0.5)] (interpolation = \"constant\")\n"));

    render.render();

    const GfVec4f color = render.colorAt(8, 8);

    EXPECT_FLOAT_EQ(0.5f, color[0]);
    EXPECT_FLOAT_EQ(0.25f, color[1]);
    EXPECT_FLOAT_EQ(0.125f, color[2]);
    EXPECT_FLOAT_EQ(1.f, color[3]);

    const std::string warning = render.textStat(kUtilityStatSceneWarning);

    EXPECT_NE(std::string::npos, warning.find("/nonexistent/m281/missing.png")) << warning;
    EXPECT_EQ(std::string(), render.textStat(kUtilityStatWarning));
}

TEST(NatronUtilityRender, ATexturedPrimWithoutTextureCoordinatesReadsTheFallbackAndIsNamed)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("grid.png")).toStdString();
    ASSERT_TRUE(writeTexture(path));

    std::string usda = texturedQuad(path);
    const std::size_t st = usda.find("    texCoord2f[] primvars:st");
    ASSERT_NE(std::string::npos, st);
    usda.erase(st, usda.find('\n', st) + 1 - st);

    UtilityRender render(std::string("#usda 1.0\n") + usda);

    render.render();
    EXPECT_EQ(GfVec4f(0.f, 0.f, 0.f, 1.f), render.colorAt(8, 8));

    const std::string warning = render.textStat(kUtilityStatSceneWarning);

    EXPECT_NE(std::string::npos, warning.find("/Quad")) << warning;
}

TEST(NatronUtilityRender, TwoMeshesHaveDifferentPrimIds)
{
    UtilityRender render(std::string("#usda 1.0\n")
                         + cubeAt("Left", kRedColor + translated(-6., 0.))
                         + cubeAt("Right", kRedColor + translated(6., 0.)));

    render.render();

    const int left = render.primIdAt(3, 8);
    const int right = render.primIdAt(12, 8);

    EXPECT_GE(left, 0);
    EXPECT_GE(right, 0);
    EXPECT_NE(left, right);
    EXPECT_EQ(-1, render.primIdAt(8, 8));
}

TEST(NatronUtilityRender, InstancesOfOneMeshHaveDifferentInstanceIds)
{
    UtilityRender render(std::string("#usda 1.0\n")
                         + "def PointInstancer \"Scatter\" {\n"
                           "    point3f[] positions = [(-6, 0, 0), (6, 0, 0)]\n"
                           "    int[] protoIndices = [0, 0]\n"
                           "    rel prototypes = </Scatter/Protos/Cube>\n"
                           "    def Scope \"Protos\" {\n"
                         + cubeAt("Cube", kRedColor)
                         + "    }\n"
                           "}\n");

    render.render();

    const int left = render.instanceIdAt(3, 8);
    const int right = render.instanceIdAt(12, 8);

    EXPECT_GE(left, 0);
    EXPECT_GE(right, 0);
    EXPECT_NE(left, right);
    EXPECT_EQ(render.primIdAt(3, 8), render.primIdAt(12, 8));
}

TEST(NatronUtilityRender, AnEmptyStageIsClearAndConverged)
{
    UtilityRender render("#usda 1.0\n");

    render.render();
    EXPECT_EQ(GfVec4f(0.f), render.colorAt(8, 8));
    EXPECT_EQ(1.f, render.windowDepthAt(8, 8));
    EXPECT_EQ(-1, render.primIdAt(8, 8));
}

// A pixel the earlier frame wrote must not survive into the next one.
TEST(NatronUtilityRender, HidingTheMeshClearsItsPixels)
{
    UtilityRender render(std::string("#usda 1.0\n") + cubeAt("Cube", kRedColor));

    render.render();
    EXPECT_FLOAT_EQ(1.f, render.colorAt(8, 8)[3]);

    render.imageable("/Cube").MakeInvisible();
    render.render();
    EXPECT_EQ(GfVec4f(0.f), render.colorAt(8, 8));
    EXPECT_EQ(1.f, render.windowDepthAt(8, 8));
}

TEST(NatronUtilityRender, AStoppedRendererDoesNotConvergeUntilRestarted)
{
    UtilityRender render(std::string("#usda 1.0\n") + cubeAt("Cube", kRedColor));

    EXPECT_TRUE(render.stopRenderer());
    render.renderWithoutChecks();
    EXPECT_FALSE(render.isConverged());

    EXPECT_TRUE(render.restartRenderer());
    render.render();
    EXPECT_FLOAT_EQ(1.f, render.colorAt(8, 8)[0]);
}

TEST(NatronUtilityRender, RestartingAfterAFinishedFrameRedrawsIt)
{
    UtilityRender render(std::string("#usda 1.0\n") + cubeAt("Cube", kRedColor));

    render.render();
    EXPECT_TRUE(render.stopRenderer());
    render.renderWithoutChecks();
    EXPECT_FALSE(render.isConverged());

    EXPECT_TRUE(render.restartRenderer());
    render.render();
    EXPECT_FLOAT_EQ(1.f, render.colorAt(8, 8)[0]);
}

#ifdef NATRON_HAVE_GPU
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

// Puts a GPU context of the test's own in the application's place while it lives, created under the test's
// environment and able to pick a CPU-type device. Must outlive every UtilityRender of the test.
class UtilityGpuRuntime {
public:
    // A non-zero poolBudget caps the buffer pool, so that the frame runs out of device memory.
    explicit UtilityGpuRuntime(std::uint64_t poolBudget = 0)
    {
        GpuContextOptions options;
        std::shared_ptr<std::string> logged = std::make_shared<std::string>();

        options.mode = GpuResolvedMode::Force;
        options.device = gputest::cpuFallbackSelector();
        options.infoLog = [logged](const std::string& line) {
            *logged = line;
        };

        std::unique_ptr<GpuContext> own(new GpuContext(options));

        if (!own->waitForInit(std::chrono::seconds(60)) || !own->isAvailable()) {
            unavailable = logged->empty() ? std::string("the GPU context did not come up") : *logged;

            return;
        }
        if (poolBudget) {
            own->runtime()->pool().setUserBudget(poolBudget);
        }
        _saved = appPTR->swapGpuContextForTests(std::move(own));
        _swapped = true;
    }

    ~UtilityGpuRuntime()
    {
        if (_swapped) {
            appPTR->swapGpuContextForTests(std::move(_saved));
        }
    }

    std::string missingSupport() const
    {
        return WavefrontGpu::missingSupport(*appPTR->getGpuContext()->runtime());
    }

    std::string unavailable;

private:
    std::unique_ptr<GpuContext> _saved;
    bool _swapped = false;
};

const std::uint64_t kStarvedPoolBytes = 1024;
} // anon namespace

TEST(NatronUtilityRender, GpuDrawsTheRedCubeAsTheCpuDoes)
{
    UtilityGpuRuntime runtime;

    GPUTEST_DEVICE_OR_SKIP(runtime.unavailable.empty(), runtime.unavailable);

    const std::string missing = runtime.missingSupport();

    if (!missing.empty()) {
        GPUTEST_SKIP() << missing;
    }

    UtilityRender render(std::string("#usda 1.0\n") + cubeAt("Cube", kRedColor));
    std::vector<GfVec4f> cpuColor;
    std::vector<float> cpuDepth;
    std::vector<int> cpuPrimId;
    std::vector<int> cpuInstanceId;

    render.setDevice(kUtilityDeviceCpu);
    render.render();
    EXPECT_EQ("CPU", render.textStat(kUtilityStatDevice));
    for (int y = 0; y < kRenderSize; ++y) {
        for (int x = 0; x < kRenderSize; ++x) {
            cpuColor.push_back(render.colorAt(x, y));
            cpuDepth.push_back(render.windowDepthAt(x, y));
            cpuPrimId.push_back(render.primIdAt(x, y));
            cpuInstanceId.push_back(render.instanceIdAt(x, y));
        }
    }

    render.setDevice(kUtilityDeviceGpu);
    render.render();
    EXPECT_EQ(0u, render.textStat(kUtilityStatDevice).rfind("GPU", 0)) << render.textStat(kUtilityStatDevice);
    EXPECT_EQ(std::string(), render.textStat(kUtilityStatWarning));

    std::size_t index = 0;

    for (int y = 0; y < kRenderSize; ++y) {
        for (int x = 0; x < kRenderSize; ++x, ++index) {
            const GfVec4f color = render.colorAt(x, y);

            for (int c = 0; c < 4; ++c) {
                EXPECT_NEAR(cpuColor[index][c], color[c], 1e-5) << "pixel " << x << ", " << y;
            }
            EXPECT_NEAR(cpuDepth[index], render.windowDepthAt(x, y), 1e-5) << "pixel " << x << ", " << y;
            // Pixels on the face's diagonal may land on either of its triangles, by a tie broken
            // differently on each device.
            EXPECT_EQ(cpuPrimId[index] >= 0, render.primIdAt(x, y) >= 0) << "pixel " << x << ", " << y;
            EXPECT_EQ(cpuInstanceId[index], render.instanceIdAt(x, y)) << "pixel " << x << ", " << y;
        }
    }
    EXPECT_FLOAT_EQ(1.f, render.colorAt(8, 8)[0]);
}

TEST(NatronUtilityRender, GpuShadesMaterialsAndTexturesAsTheCpuDoes)
{
    UtilityGpuRuntime runtime;

    GPUTEST_DEVICE_OR_SKIP(runtime.unavailable.empty(), runtime.unavailable);

    const std::string missing = runtime.missingSupport();

    if (!missing.empty()) {
        GPUTEST_SKIP() << missing;
    }

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("grid.png")).toStdString();
    ASSERT_TRUE(writeTexture(path));

    UtilityRender render(std::string("#usda 1.0\n")
                         + texturedQuad(path)
                         + "def Material \"Standard\" {\n"
                           "    token outputs:surface.connect = </Standard/Surface.outputs:surface>\n"
                           "    def Shader \"Surface\" {\n"
                           "        uniform token info:id = \"ND_standard_surface_surfaceshader\"\n"
                           "        token outputs:surface\n"
                           "    }\n"
                           "}\n"
                         + previewSurface("Glow", "        color3f inputs:emissiveColor = (0.25, 0.5, 1)\n        float inputs:opacity = 0.5\n")
                         + cubeAt("Left", boundTo("/Standard") + translated(-6., 0.))
                         + cubeAt("Right", boundTo("/Glow") + translated(6., 6.)));
    std::vector<GfVec4f> cpuColor;

    render.setDevice(kUtilityDeviceCpu);
    render.render();
    EXPECT_EQ("CPU", render.textStat(kUtilityStatDevice));
    for (int y = 0; y < kRenderSize; ++y) {
        for (int x = 0; x < kRenderSize; ++x) {
            cpuColor.push_back(render.colorAt(x, y));
        }
    }

    render.setDevice(kUtilityDeviceGpu);
    render.render();
    EXPECT_EQ(0u, render.textStat(kUtilityStatDevice).rfind("GPU", 0)) << render.textStat(kUtilityStatDevice);
    EXPECT_EQ(std::string(), render.textStat(kUtilityStatWarning));

    std::size_t index = 0;

    for (int y = 0; y < kRenderSize; ++y) {
        for (int x = 0; x < kRenderSize; ++x, ++index) {
            const GfVec4f color = render.colorAt(x, y);

            for (int c = 0; c < 4; ++c) {
                EXPECT_NEAR(cpuColor[index][c], color[c], 1e-5) << "pixel " << x << ", " << y;
            }
        }
    }
    EXPECT_EQ(GfVec4f(1.f, 0.f, 1.f, 1.f), render.colorAt(3, 8));
    EXPECT_NEAR(0.125f, render.colorAt(11, 11)[0], 1e-6);
    EXPECT_NEAR(0.5f, render.colorAt(11, 11)[3], 1e-6);
}

TEST(NatronUtilityRender, GpuSamplesTexelsAtTheirCentres)
{
    UtilityGpuRuntime runtime;

    GPUTEST_DEVICE_OR_SKIP(runtime.unavailable.empty(), runtime.unavailable);

    const std::string missing = runtime.missingSupport();

    if (!missing.empty()) {
        GPUTEST_SKIP() << missing;
    }

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("grid.png")).toStdString();
    ASSERT_TRUE(writeTexture(path));

    UtilityRender render(std::string("#usda 1.0\n") + texturedQuad(path));

    render.setDevice(kUtilityDeviceGpu);
    render.render();
    EXPECT_EQ(0u, render.textStat(kUtilityStatDevice).rfind("GPU", 0)) << render.textStat(kUtilityStatDevice);
    expectTexelCentres([&render](int x, int y) { return render.colorAt(x, y); });
}

TEST(NatronUtilityRender, ForcedGpuWithoutRayQueryFailsNamingTheExtension)
{
    const ScopedEnv noRayQuery("NATRON_GPU_NO_RAY_QUERY", "1");
    UtilityGpuRuntime runtime;

    GPUTEST_DEVICE_OR_SKIP(runtime.unavailable.empty(), runtime.unavailable);

    UtilityRender render(std::string("#usda 1.0\n") + cubeAt("Cube", kRedColor));

    render.setDevice(kUtilityDeviceGpu);

    const std::string error = render.renderError();

    EXPECT_NE(std::string::npos, error.find("VK_KHR_ray_query")) << error;
}

TEST(NatronUtilityRender, AutoWithoutRayQueryRendersOnTheCpuWithoutAWarning)
{
    const ScopedEnv noRayQuery("NATRON_GPU_NO_RAY_QUERY", "1");
    UtilityGpuRuntime runtime;

    GPUTEST_DEVICE_OR_SKIP(runtime.unavailable.empty(), runtime.unavailable);

    UtilityRender render(std::string("#usda 1.0\n") + cubeAt("Cube", kRedColor));

    render.setDevice(kUtilityDeviceAuto);
    render.render();
    EXPECT_FLOAT_EQ(1.f, render.colorAt(8, 8)[0]);
    EXPECT_EQ("CPU", render.textStat(kUtilityStatDevice));
    EXPECT_EQ(std::string(), render.textStat(kUtilityStatWarning));
}

TEST(NatronUtilityRender, AutoRendersOnTheCpuWhenTheGpuRunsOutOfMemory)
{
    UtilityGpuRuntime runtime(kStarvedPoolBytes);

    GPUTEST_DEVICE_OR_SKIP(runtime.unavailable.empty(), runtime.unavailable);

    const std::string missing = runtime.missingSupport();

    if (!missing.empty()) {
        GPUTEST_SKIP() << missing;
    }

    UtilityRender render(std::string("#usda 1.0\n") + cubeAt("Cube", kRedColor));

    render.setDevice(kUtilityDeviceAuto);
    render.render();
    EXPECT_FLOAT_EQ(1.f, render.colorAt(8, 8)[0]);
    EXPECT_EQ(-1, render.primIdAt(0, 0));
    EXPECT_EQ("CPU", render.textStat(kUtilityStatDevice));

    const std::string warning = render.textStat(kUtilityStatWarning);

    EXPECT_NE(std::string::npos, warning.find("out of memory")) << warning;
    EXPECT_NE(std::string::npos, warning.find(std::to_string(kStarvedPoolBytes))) << warning;
}

TEST(NatronUtilityRender, ForcedGpuOutOfMemoryFails)
{
    UtilityGpuRuntime runtime(kStarvedPoolBytes);

    GPUTEST_DEVICE_OR_SKIP(runtime.unavailable.empty(), runtime.unavailable);

    const std::string missing = runtime.missingSupport();

    if (!missing.empty()) {
        GPUTEST_SKIP() << missing;
    }

    UtilityRender render(std::string("#usda 1.0\n") + cubeAt("Cube", kRedColor));

    render.setDevice(kUtilityDeviceGpu);

    const std::string error = render.renderError();

    EXPECT_NE(std::string::npos, error.find(std::to_string(kStarvedPoolBytes))) << error;
}
#endif // NATRON_HAVE_GPU

// A recovered device is a new GPU generation with its memory free again, so what ran out of memory before
// is tried on it.
TEST(NatronUtilityRender, TheOutOfMemoryMemoHoldsForOneSceneBudgetAndGpuGeneration)
{
    UtilityScene scene;
    UtilityRenderParam param(&scene, nullptr);
    std::string message;

    EXPECT_FALSE(param.gpuOutOfMemory(3, 7, 11, &message));
    param.rememberGpuOutOfMemory(3, 7, 11, "no room");
    ASSERT_TRUE(param.gpuOutOfMemory(3, 7, 11, &message));
    EXPECT_EQ("no room", message);
    EXPECT_FALSE(param.gpuOutOfMemory(4, 7, 11, &message)) << "another scene version";
    EXPECT_FALSE(param.gpuOutOfMemory(3, 8, 11, &message)) << "another budget";
    EXPECT_FALSE(param.gpuOutOfMemory(3, 7, 12, &message)) << "another GPU generation";
}
