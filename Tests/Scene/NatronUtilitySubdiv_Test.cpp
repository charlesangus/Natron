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
#include <pxr/base/gf/vec2f.h>
#include <pxr/base/gf/vec2i.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/vt/dictionary.h>
#include <pxr/imaging/cameraUtil/framing.h>
#include <pxr/imaging/hd/aov.h>
#include <pxr/imaging/hd/renderBuffer.h>
#include <pxr/imaging/hd/tokens.h>
#include <pxr/imaging/hdx/tokens.h>
#include <pxr/imaging/hio/image.h>
#include <pxr/imaging/pxOsd/meshTopology.h>
#include <pxr/imaging/pxOsd/subdivTags.h>
#include <pxr/imaging/pxOsd/tokens.h>
#include <pxr/usd/sdf/layer.h>
#include <pxr/usd/sdf/path.h>
#include <pxr/usd/usd/prim.h>
#include <pxr/usd/usd/relationship.h>
#include <pxr/usd/usd/stage.h>
#include <pxr/usd/usdGeom/mesh.h>
#include <pxr/usd/usdGeom/tokens.h>
#include <pxr/usdImaging/usdImagingGL/engine.h>
#include <pxr/usdImaging/usdImagingGL/renderParams.h>

#include "Global/Macros.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <QString>
#include <QTemporaryDir>

#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderDelegate.h"
#include "Engine/Nodes/Scene/Utility/UtilitySubdiv.h"
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
const VtIntArray kCubeCounts = { 4, 4, 4, 4, 4, 4 };
const VtIntArray kCubeIndices = { 0, 1, 3, 2, 2, 3, 5, 4, 4, 5, 7, 6, 6, 7, 1, 0, 1, 7, 5, 3, 6, 0, 2, 4 };
const VtVec3fArray kCubePoints = { GfVec3f(-1, -1, 1), GfVec3f(1, -1, 1), GfVec3f(-1, 1, 1), GfVec3f(1, 1, 1),
                                   GfVec3f(-1, 1, -1), GfVec3f(1, 1, -1), GfVec3f(-1, -1, -1), GfVec3f(1, -1, -1) };

const char* const kCubeGeometry = R"(
    int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]
    int[] faceVertexIndices = [0, 1, 3, 2, 2, 3, 5, 4, 4, 5, 7, 6, 6, 7, 1, 0, 1, 7, 5, 3, 6, 0, 2, 4]
    point3f[] points = [(-1, -1, 1), (1, -1, 1), (-1, 1, 1), (1, 1, 1), (-1, 1, -1), (1, 1, -1), (-1, -1, -1), (1, -1, -1)]
)";

// Catmull-Clark triangles of the cube: six quads, four per quad per level, two triangles each.
std::size_t
cubeTriangles(int level)
{
    return level == 0 ? 12 : 12 * static_cast<std::size_t>(std::pow(4., level));
}

PxOsdMeshTopology
cube(const TfToken& scheme,
     const PxOsdSubdivTags& tags = PxOsdSubdivTags(),
     const VtIntArray& holes = VtIntArray())
{
    return PxOsdMeshTopology(scheme, PxOsdOpenSubdivTokens->rightHanded, kCubeCounts, kCubeIndices, holes, tags);
}

UtilitySubdivRefinementPtr
refine(const PxOsdMeshTopology& topology,
       int level,
       const std::vector<VtIntArray>& faceVarying = std::vector<VtIntArray>())
{
    std::string error;
    UtilitySubdivRefinementPtr refinement = UtilitySubdivRefinement::get(topology, level, faceVarying, &error);

    EXPECT_TRUE(refinement != nullptr) << error;

    return refinement;
}

const std::size_t kDefaultCacheBudget = std::size_t(1) << 30;
// Room for the large grids below, whose refiners alone run to about a gigabyte.
const std::size_t kRoomyCacheBudget = std::size_t(16) << 30;

// Holds the refinement cache to a budget while it lives.
class CacheBudget {
public:
    explicit CacheBudget(std::size_t bytes)
    {
        UtilitySubdivRefinement::setCacheBudget(bytes);
    }

    ~CacheBudget()
    {
        UtilitySubdivRefinement::setCacheBudget(kDefaultCacheBudget);
    }
};

bool
near(const GfVec3f& a,
     const GfVec3f& b,
     float tolerance = 1e-5f)
{
    return (a - b).GetLength() < tolerance;
}

std::string
cubeAt(const char* name,
       const std::string& attributes)
{
    return std::string("def Mesh \"") + name + "\" {" + kCubeGeometry + attributes + "}\n";
}

const double kCameraZ = 30.;
const double kFieldOfView = 45.;

// The stage through the engine the node drives the delegate with, at a chosen image size.
class SubdivRender {
public:
    SubdivRender(const std::string& usda,
                 int size = 16)
        : _size(size)
    {
        RenderDelegates::list();
        _layer = SdfLayer::CreateAnonymous(".usda");
        EXPECT_TRUE(_layer->ImportFromString(usda));
        stage = UsdStage::Open(_layer);

        UsdImagingGLEngine::Parameters parameters;

        parameters.rendererPluginId = TfToken(kRenderDelegateUtilityId);
        parameters.gpuEnabled = false;
        _engine.reset(new UsdImagingGLEngine(parameters));
        _engine->SetEnablePresentation(false);
        EXPECT_TRUE(_engine->SetRendererAovs({ HdAovTokens->color }));
    }

    void setSetting(const char* key,
                    int value)
    {
        _engine->SetRendererSetting(TfToken(key), VtValue(value));
    }

    void setDevice(const char* device)
    {
        _engine->SetRendererSetting(TfToken(kUtilitySettingDevice), VtValue(TfToken(device)));
    }

    // The first GPU frame in a process compiles the wavefront kernels, which on a loaded software
    // Vulkan device has taken longer than 30 s.
    void render()
    {
        const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + std::chrono::seconds(180);

        renderOnce();
        while (!_engine->IsConverged() && std::chrono::steady_clock::now() < end) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            renderOnce();
        }
        EXPECT_TRUE(_engine->IsConverged());
    }

    std::size_t stat(const char* key) const
    {
        return VtDictionaryGet<std::size_t>(_engine->GetRenderStats(), key, VtDefault = std::size_t(0));
    }

    // Stops the delegate, from any thread, as an abort does.
    bool stop()
    {
        return _engine->StopRenderer();
    }

    void restart()
    {
        EXPECT_TRUE(_engine->RestartRenderer());
    }

    bool isConverged() const
    {
        return _engine->IsConverged();
    }

    std::string warning() const
    {
        return VtDictionaryGet<std::string>(_engine->GetRenderStats(), kUtilityStatWarning, VtDefault = std::string());
    }

    std::string device() const
    {
        return VtDictionaryGet<std::string>(_engine->GetRenderStats(), kUtilityStatDevice, VtDefault = std::string());
    }

    std::vector<GfVec4f> pixels() const
    {
        std::vector<GfVec4f> all;

        for (int y = 0; y < _size; ++y) {
            for (int x = 0; x < _size; ++x) {
                all.push_back(colorAt(x, y));
            }
        }

        return all;
    }

    GfVec4f colorAt(int x,
                    int y) const
    {
        HdRenderBuffer* buffer = _engine->GetAovRenderBuffer(HdAovTokens->color);

        if (!buffer) {
            ADD_FAILURE() << "no color buffer";

            return GfVec4f(0.f);
        }
        const GfVec4f* pixels = static_cast<const GfVec4f*>(buffer->Map());
        const GfVec4f color = pixels[y * _size + x];

        buffer->Unmap();

        return color;
    }

    float alphaAt(int x,
                  int y) const
    {
        return colorAt(x, y)[3];
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

    void renderOnce()
    {
        GfFrustum camera;

        camera.SetPerspective(kFieldOfView, 1., 0.1, 1000.);
        camera.SetPosition(GfVec3d(0., 0., kCameraZ));
        _engine->SetRenderBufferSize(GfVec2i(_size, _size));
        _engine->SetFraming(CameraUtilFraming(GfRect2i(GfVec2i(0, 0), _size, _size)));
        _engine->SetCameraState(camera.ComputeViewMatrix(), camera.ComputeProjectionMatrix());

        UsdImagingGLRenderParams params;

        params.drawMode = UsdImagingGLDrawMode::DRAW_SHADED_SMOOTH;
        params.clearColor = GfVec4f(0.f);
        params.colorCorrectionMode = HdxColorCorrectionTokens->disabled;
        params.highlight = false;
        _engine->Render(stage->GetPseudoRoot(), params);
    }

    UsdStageRefPtr stage;

private:
    int _size;
    SdfLayerRefPtr _layer;
    std::unique_ptr<UsdImagingGLEngine> _engine;
};

// A sphere cage of radius about the origin, its poles on the view axis so the silhouette runs along
// the equator.
void
defineSphereCage(const UsdStageRefPtr& stage,
                 double radius,
                 int segments,
                 int rings)
{
    VtVec3fArray points;
    VtIntArray counts;
    VtIntArray indices;
    const auto ringPoint = [segments](int ring, int segment) { return 1 + (ring - 1) * segments + segment % segments; };

    points.push_back(GfVec3f(0.f, 0.f, static_cast<float>(radius)));
    for (int ring = 1; ring < rings; ++ring) {
        const double theta = M_PI * ring / rings;

        for (int segment = 0; segment < segments; ++segment) {
            const double phi = 2. * M_PI * segment / segments;

            points.push_back(GfVec3f(static_cast<float>(radius * std::sin(theta) * std::cos(phi)),
                                     static_cast<float>(radius * std::sin(theta) * std::sin(phi)),
                                     static_cast<float>(radius * std::cos(theta))));
        }
    }
    points.push_back(GfVec3f(0.f, 0.f, static_cast<float>(-radius)));

    const int south = static_cast<int>(points.size()) - 1;

    for (int segment = 0; segment < segments; ++segment) {
        counts.push_back(3);
        indices.push_back(0);
        indices.push_back(ringPoint(1, segment));
        indices.push_back(ringPoint(1, segment + 1));
    }
    for (int ring = 1; ring + 1 < rings; ++ring) {
        for (int segment = 0; segment < segments; ++segment) {
            counts.push_back(4);
            indices.push_back(ringPoint(ring, segment));
            indices.push_back(ringPoint(ring + 1, segment));
            indices.push_back(ringPoint(ring + 1, segment + 1));
            indices.push_back(ringPoint(ring, segment + 1));
        }
    }
    for (int segment = 0; segment < segments; ++segment) {
        counts.push_back(3);
        indices.push_back(south);
        indices.push_back(ringPoint(rings - 1, segment + 1));
        indices.push_back(ringPoint(rings - 1, segment));
    }

    UsdGeomMesh mesh = UsdGeomMesh::Define(stage, SdfPath("/Sphere"));

    mesh.CreatePointsAttr(VtValue(points));
    mesh.CreateFaceVertexCountsAttr(VtValue(counts));
    mesh.CreateFaceVertexIndicesAttr(VtValue(indices));
    mesh.CreateSubdivisionSchemeAttr(VtValue(UsdGeomTokens->catmullClark));
}

// A Catmull-Clark grid of n by n quads, its points row by row.
PxOsdMeshTopology
gridTopology(int n)
{
    VtIntArray counts(n * n, 4);
    VtIntArray indices;

    indices.reserve(n * n * 4);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            const int corner = y * (n + 1) + x;

            indices.push_back(corner);
            indices.push_back(corner + 1);
            indices.push_back(corner + n + 2);
            indices.push_back(corner + n + 1);
        }
    }

    return PxOsdMeshTopology(PxOsdOpenSubdivTokens->catmullClark, PxOsdOpenSubdivTokens->rightHanded, counts, indices);
}

// The grid topology of n, 20 units wide and facing the camera.
void
defineGridCage(const UsdStageRefPtr& stage,
               int n)
{
    const PxOsdMeshTopology topology = gridTopology(n);
    VtVec3fArray points;

    points.reserve((n + 1) * (n + 1));
    for (int y = 0; y <= n; ++y) {
        for (int x = 0; x <= n; ++x) {
            points.push_back(GfVec3f(-10.f + 20.f * x / n, -10.f + 20.f * y / n, 0.f));
        }
    }

    UsdGeomMesh mesh = UsdGeomMesh::Define(stage, SdfPath("/Grid"));

    mesh.CreatePointsAttr(VtValue(points));
    mesh.CreateFaceVertexCountsAttr(VtValue(topology.GetFaceVertexCounts()));
    mesh.CreateFaceVertexIndicesAttr(VtValue(topology.GetFaceVertexIndices()));
    mesh.CreateSubdivisionSchemeAttr(VtValue(UsdGeomTokens->catmullClark));
}

const int kRampWidth = 256;

// Grey rising from 0 at the left edge to 1 at the right, as many rows as needed for a 2D texture.
bool
writeRampTexture(const std::string& path)
{
    const int height = 4;
    std::vector<unsigned char> bytes;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < kRampWidth; ++x) {
            const unsigned char value = static_cast<unsigned char>(x * 255 / (kRampWidth - 1));

            bytes.push_back(value);
            bytes.push_back(value);
            bytes.push_back(value);
            bytes.push_back(255);
        }
    }

    HioImage::StorageSpec spec;

    spec.width = kRampWidth;
    spec.height = height;
    spec.depth = 1;
    spec.format = HioFormatUNorm8Vec4;
    spec.flipped = false;
    spec.data = bytes.data();

    const HioImageSharedPtr image = HioImage::OpenForWriting(path);

    return image && image->Write(spec);
}

// Two quads wider than the frame, side by side across its centre, emitting the ramp at path through
// an unindexed face-varying st: two UV islands that meet at u = 0.5 along the shared edge, so the
// edge is a UV seam with the same value on both sides of it.
std::string
rampSeamCage(const std::string& path,
             const TfToken& rule)
{
    return std::string("#usda 1.0\n"
                       "def Material \"Tex\" {\n"
                       "    token outputs:surface.connect = </Tex/Surface.outputs:surface>\n"
                       "    def Shader \"Surface\" {\n"
                       "        uniform token info:id = \"UsdPreviewSurface\"\n"
                       "        color3f inputs:diffuseColor = (0, 0, 0)\n"
                       "        color3f inputs:emissiveColor.connect = </Tex/Image.outputs:rgb>\n"
                       "        token outputs:surface\n"
                       "    }\n"
                       "    def Shader \"Image\" {\n"
                       "        uniform token info:id = \"UsdUVTexture\"\n"
                       "        asset inputs:file = @")
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
                 "def Mesh \"Seam\" (prepend apiSchemas = [\"MaterialBindingAPI\"]) {\n"
                 "    int[] faceVertexCounts = [4, 4]\n"
                 "    int[] faceVertexIndices = [0, 1, 4, 3, 1, 2, 5, 4]\n"
                 "    point3f[] points = [(-14, -14, 0), (0, -14, 0), (14, -14, 0), (-14, 14, 0), (0, 14, 0), (14, 14, 0)]\n"
                 "    uniform token faceVaryingLinearInterpolation = \""
        + rule.GetString() + "\"\n"
                             "    texCoord2f[] primvars:st = [(0, 0), (0.5, 0), (0.5, 1), (0, 1), (0.5, 0), (1, 0), (1, 1), (0.5, 1)] "
                             "(interpolation = \"faceVarying\")\n"
                             "    rel material:binding = </Tex>\n"
                             "}\n";
}

// Pure diffuse white under a distant light of intensity pi along -Z, so that a pixel's value is the
// cosine between its shading normal and the view axis.
const char* const kLitWhite = R"(
def Material "White" {
    token outputs:surface.connect = </White/Surface.outputs:surface>
    def Shader "Surface" {
        uniform token info:id = "UsdPreviewSurface"
        color3f inputs:diffuseColor = (1, 1, 1)
        int inputs:useSpecularWorkflow = 1
        color3f inputs:specularColor = (0, 0, 0)
        token outputs:surface
    }
}
def DistantLight "Sun" {
    float inputs:angle = 0
    float inputs:intensity = 3.14159265
}
)";

void
bindWhite(const UsdStageRefPtr& stage,
          const char* path)
{
    UsdPrim prim = stage->GetPrimAtPath(SdfPath(path));

    prim.AddAppliedSchema(TfToken("MaterialBindingAPI"));
    prim.CreateRelationship(TfToken("material:binding")).SetTargets({ SdfPath("/White") });
}

const int kShadedSize = 64;

// The largest red step between horizontal or vertical neighbours, both fully covered and within
// fraction of the frame's half-width of its centre.
float
largestStep(const std::vector<GfVec4f>& pixels,
            double fraction)
{
    const double reach = fraction * kShadedSize / 2.;
    float largest = 0.f;

    for (int y = 0; y < kShadedSize; ++y) {
        for (int x = 0; x < kShadedSize; ++x) {
            const GfVec4f& here = pixels[y * kShadedSize + x];
            const double dx = x + 0.5 - kShadedSize / 2.;
            const double dy = y + 0.5 - kShadedSize / 2.;

            if (here[3] < 0.999f || std::sqrt(dx * dx + dy * dy) > reach) {
                continue;
            }
            for (int step = 0; step < 2; ++step) {
                const int nx = x + (step == 0 ? 1 : 0);
                const int ny = y + (step == 1 ? 1 : 0);

                if (nx >= kShadedSize || ny >= kShadedSize) {
                    continue;
                }

                const GfVec4f& next = pixels[ny * kShadedSize + nx];

                if (next[3] >= 0.999f) {
                    largest = std::max(largest, std::fabs(here[0] - next[0]));
                }
            }
        }
    }

    return largest;
}

// A cube of side 16 facing the camera, each corner's normal pointing out along its diagonal through a
// face-varying primvar, as a cage would carry them to look rounded.
std::string
cornerNormalCube(const char* scheme)
{
    return std::string("#usda 1.0\n") + kLitWhite
        + "def Mesh \"Cube\" (prepend apiSchemas = [\"MaterialBindingAPI\"]) {\n"
          "    int[] faceVertexCounts = [4, 4, 4, 4, 4, 4]\n"
          "    int[] faceVertexIndices = [0, 1, 3, 2, 2, 3, 5, 4, 4, 5, 7, 6, 6, 7, 1, 0, 1, 7, 5, 3, 6, 0, 2, 4]\n"
          "    point3f[] points = [(-8, -8, 8), (8, -8, 8), (-8, 8, 8), (8, 8, 8), (-8, 8, -8), (8, 8, -8), (-8, -8, -8), (8, -8, -8)]\n"
          "    normal3f[] primvars:normals = [(-1, -1, 1), (1, -1, 1), (-1, 1, 1), (1, 1, 1), (-1, 1, -1), (1, 1, -1), (-1, -1, -1), (1, -1, -1)] "
          "(interpolation = \"faceVarying\")\n"
          "    int[] primvars:normals:indices = [0, 1, 3, 2, 2, 3, 5, 4, 4, 5, 7, 6, 6, 7, 1, 0, 1, 7, 5, 3, 6, 0, 2, 4]\n"
          "    uniform token subdivisionScheme = \""
        + scheme + "\"\n"
                   "    rel material:binding = </White>\n"
                   "}\n";
}

#ifdef NATRON_HAVE_GPU
// Puts a GPU context of the test's own in the application's place while it lives, able to pick a
// CPU-type device. Must outlive every SubdivRender of the test.
class SubdivGpuRuntime {
public:
    SubdivGpuRuntime()
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
        _saved = appPTR->swapGpuContextForTests(std::move(own));
        _swapped = true;
    }

    ~SubdivGpuRuntime()
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
#endif // NATRON_HAVE_GPU
} // anon namespace

// The cube's level-1 points by the Catmull-Clark rules: each corner moves to 5/9 of itself, an edge
// point is the mean of its ends and the two face centres, and a face point is the face's centre.
TEST(NatronUtilitySubdiv, ACubeAtLevelOneIsTwentyFourQuadsOnTheCatmullClarkPoints)
{
    const UtilitySubdivRefinementPtr refinement = refine(cube(PxOsdOpenSubdivTokens->catmullClark), 1);

    ASSERT_TRUE(refinement != nullptr);
    EXPECT_EQ(48u, refinement->triangles().size());
    EXPECT_EQ(26u, refinement->refinedPointCount());

    VtVec3fArray refined;

    refinement->refine(kCubePoints, &refined);
    ASSERT_EQ(26u, refined.size());

    int corners = 0;
    int edges = 0;
    int faces = 0;

    for (const GfVec3f& p : refined) {
        int ones = 0;
        int threeQuarters = 0;
        int fiveNinths = 0;
        int zeros = 0;

        for (int axis = 0; axis < 3; ++axis) {
            const float a = std::fabs(p[axis]);

            ones += std::fabs(a - 1.f) < 1e-5f;
            threeQuarters += std::fabs(a - 0.75f) < 1e-5f;
            fiveNinths += std::fabs(a - 5.f / 9.f) < 1e-5f;
            zeros += a < 1e-5f;
        }
        corners += fiveNinths == 3;
        edges += threeQuarters == 2 && zeros == 1;
        faces += ones == 1 && zeros == 2;
    }
    EXPECT_EQ(8, corners);
    EXPECT_EQ(12, edges);
    EXPECT_EQ(6, faces);
}

TEST(NatronUtilitySubdiv, LimitNormalsPointOutOfTheCubeAndAgreeWithTheWinding)
{
    const UtilitySubdivRefinementPtr refinement = refine(cube(PxOsdOpenSubdivTokens->catmullClark), 2);

    ASSERT_TRUE(refinement != nullptr);
    VtVec3fArray points;
    VtVec3fArray normals;

    refinement->evaluate(kCubePoints, &points, &normals);
    ASSERT_EQ(refinement->refinedPointCount(), points.size());
    ASSERT_EQ(points.size(), normals.size());
    for (std::size_t i = 0; i < points.size(); ++i) {
        EXPECT_NEAR(1.f, normals[i].GetLength(), 1e-4f);
        EXPECT_GT(GfDot(points[i], normals[i]), 0.f) << "point " << i;
    }
    for (const GfVec3i& t : refinement->triangles()) {
        const GfVec3f facing = GfCross(points[t[1]] - points[t[0]], points[t[2]] - points[t[0]]);

        EXPECT_GT(GfDot(facing, normals[t[0]]), 0.f);
    }
}

// The four top edges creased at 10 stay sharp through every level, so each top corner's limit is
// the crease curve's, (p- + 4 p + p+) / 6 along the loop, and nothing rises above the top face.
TEST(NatronUtilitySubdiv, ACreaseOfSharpnessTenKeepsItsEdgeOnTheLimit)
{
    PxOsdSubdivTags tags;

    tags.SetCreaseIndices(VtIntArray { 2, 3, 5, 4, 2 });
    tags.SetCreaseLengths(VtIntArray { 5 });
    tags.SetCreaseWeights(VtFloatArray { 10.f });

    const UtilitySubdivRefinementPtr refinement = refine(cube(PxOsdOpenSubdivTokens->catmullClark, tags), 2);

    ASSERT_TRUE(refinement != nullptr);
    VtVec3fArray points;
    VtVec3fArray normals;

    refinement->evaluate(kCubePoints, &points, &normals);

    bool cornerOnCrease = false;
    float top = -1.f;

    for (const GfVec3f& p : points) {
        cornerOnCrease = cornerOnCrease || near(p, GfVec3f(2.f / 3.f, 1.f, 2.f / 3.f));
        top = std::max(top, p[1]);
    }
    EXPECT_TRUE(cornerOnCrease);
    EXPECT_NEAR(1.f, top, 1e-5f);

    // Uncreased, the same corner is pulled well inside.
    const UtilitySubdivRefinementPtr smooth = refine(cube(PxOsdOpenSubdivTokens->catmullClark), 2);

    ASSERT_TRUE(smooth != nullptr);
    smooth->evaluate(kCubePoints, &points, &normals);
    for (const GfVec3f& p : points) {
        EXPECT_LT(p[1], 0.9f);
    }
}

TEST(NatronUtilitySubdiv, AHoleFaceProducesNoTriangles)
{
    const UtilitySubdivRefinementPtr refinement = refine(cube(PxOsdOpenSubdivTokens->catmullClark, PxOsdSubdivTags(), VtIntArray { 0 }), 1);

    ASSERT_TRUE(refinement != nullptr);
    EXPECT_EQ(40u, refinement->triangles().size());
    ASSERT_EQ(refinement->triangles().size(), refinement->coarseFaces().size());
    for (const int face : refinement->coarseFaces()) {
        EXPECT_NE(0, face);
    }
}

TEST(NatronUtilitySubdiv, TheSameTopologyAndLevelComeFromTheCache)
{
    const UtilitySubdivRefinementPtr first = refine(cube(PxOsdOpenSubdivTokens->catmullClark), 3);
    const std::size_t builds = UtilitySubdivRefinement::stats().refinerBuilds;
    const UtilitySubdivRefinementPtr again = refine(cube(PxOsdOpenSubdivTokens->catmullClark), 3);

    EXPECT_EQ(first.get(), again.get());
    EXPECT_EQ(builds, UtilitySubdivRefinement::stats().refinerBuilds);

    const UtilitySubdivRefinementPtr otherLevel = refine(cube(PxOsdOpenSubdivTokens->catmullClark), 2);

    EXPECT_NE(first.get(), otherLevel.get());
    EXPECT_EQ(3, first->level());
    EXPECT_EQ(2, otherLevel->level());
}

TEST(NatronUtilitySubdiv, AnEvictedRefinementLivesOnWhileHeld)
{
    const UtilitySubdivRefinementPtr held = refine(cube(PxOsdOpenSubdivTokens->catmullClark), 4);

    ASSERT_TRUE(held != nullptr);
    {
        const CacheBudget none(0);

        EXPECT_EQ(0u, UtilitySubdivRefinement::stats().cachedEntries);
        EXPECT_EQ(0u, UtilitySubdivRefinement::stats().cachedBytes);

        const std::size_t builds = UtilitySubdivRefinement::stats().refinerBuilds;
        const UtilitySubdivRefinementPtr other = refine(cube(PxOsdOpenSubdivTokens->catmullClark), 1);

        EXPECT_TRUE(other != nullptr);
        EXPECT_EQ(builds + 1, UtilitySubdivRefinement::stats().refinerBuilds);
        EXPECT_EQ(0u, UtilitySubdivRefinement::stats().cachedEntries);
    }

    VtVec3fArray refined;

    held->refine(kCubePoints, &refined);
    EXPECT_EQ(held->refinedPointCount(), refined.size());
}

TEST(NatronUtilitySubdiv, ARefinementOverTheBudgetIsReturnedButNotCached)
{
    UtilitySubdivRefinement::clearCache();

    const UtilitySubdivRefinementPtr first = refine(cube(PxOsdOpenSubdivTokens->catmullClark), 2);

    ASSERT_TRUE(first != nullptr);
    EXPECT_EQ(1u, UtilitySubdivRefinement::stats().cachedEntries);

    const CacheBudget tooSmall(first->memoryBytes() - 1);

    EXPECT_EQ(0u, UtilitySubdivRefinement::stats().cachedEntries);

    const std::size_t builds = UtilitySubdivRefinement::stats().refinerBuilds;
    const UtilitySubdivRefinementPtr second = refine(cube(PxOsdOpenSubdivTokens->catmullClark), 2);

    ASSERT_TRUE(second != nullptr);
    EXPECT_EQ(first->triangles(), second->triangles());
    EXPECT_EQ(0u, UtilitySubdivRefinement::stats().cachedEntries);
    EXPECT_EQ(0u, UtilitySubdivRefinement::stats().cachedBytes);

    const UtilitySubdivRefinementPtr third = refine(cube(PxOsdOpenSubdivTokens->catmullClark), 2);

    EXPECT_EQ(builds + 2, UtilitySubdivRefinement::stats().refinerBuilds);
}

// What a refinement counts covers what it keeps: its triangles and coarse topology, and more for a
// face-varying channel, whose indices, stencils and corners it also keeps.
TEST(NatronUtilitySubdiv, TheCountedBytesCoverWhatTheRefinementKeeps)
{
    const PxOsdMeshTopology topology = cube(PxOsdOpenSubdivTokens->catmullClark);
    const UtilitySubdivRefinementPtr plain = refine(topology, 3);
    const UtilitySubdivRefinementPtr channel = refine(topology, 3, { kCubeIndices });

    ASSERT_TRUE(plain != nullptr);
    ASSERT_TRUE(channel != nullptr);

    const std::size_t kept = plain->triangles().size() * (sizeof(GfVec3i) + sizeof(int))
        + (kCubeCounts.size() + kCubeIndices.size()) * sizeof(int);

    EXPECT_GT(plain->memoryBytes(), kept);
    EXPECT_GT(channel->memoryBytes(), plain->memoryBytes() + plain->triangles().size() * sizeof(GfVec3i));
}

TEST(NatronUtilitySubdiv, TheEstimateMatchesTheRefinementAndTheBudgetLowersTheLevel)
{
    const PxOsdMeshTopology topology = cube(PxOsdOpenSubdivTokens->catmullClark);

    for (int level = 0; level <= 3; ++level) {
        EXPECT_EQ(cubeTriangles(level), UtilitySubdivRefinement::estimateTriangles(topology, level));
    }
    const UtilitySubdivRefinementPtr refinement = refine(topology, 2);

    ASSERT_TRUE(refinement != nullptr);
    EXPECT_EQ(cubeTriangles(2), refinement->triangles().size());
    EXPECT_EQ(1, UtilitySubdivRefinement::fitLevel(topology, 3, 100));
    EXPECT_EQ(3, UtilitySubdivRefinement::fitLevel(topology, 3, cubeTriangles(3)));
    EXPECT_EQ(0, UtilitySubdivRefinement::fitLevel(topology, 3, 10));
}

TEST(NatronUtilitySubdiv, NoneAndLoopOnQuadsAreNotRefined)
{
    std::string error;

    EXPECT_TRUE(UtilitySubdivRefinement::get(cube(PxOsdOpenSubdivTokens->none), 2, std::vector<VtIntArray>(), &error) == nullptr);
    EXPECT_TRUE(UtilitySubdivRefinement::get(cube(PxOsdOpenSubdivTokens->loop), 2, std::vector<VtIntArray>(), &error) == nullptr);
    EXPECT_FALSE(error.empty());
}

TEST(NatronUtilitySubdiv, BilinearKeepsTheCubeFlatWithoutNormals)
{
    const UtilitySubdivRefinementPtr refinement = refine(cube(PxOsdOpenSubdivTokens->bilinear), 2);

    ASSERT_TRUE(refinement != nullptr);
    VtVec3fArray points;
    VtVec3fArray normals;

    refinement->evaluate(kCubePoints, &points, &normals);
    EXPECT_TRUE(normals.empty());
    for (const GfVec3f& p : points) {
        EXPECT_NEAR(1.f, std::max({ std::fabs(p[0]), std::fabs(p[1]), std::fabs(p[2]) }), 1e-5f);
    }
}

// Two quads side by side, x from 0 to 2, whose UV islands meet along x = 1: the left one's u runs
// 0 to 1 and the right one's 5 to 6, with v from 0 to 1 on both.
TEST(NatronUtilitySubdiv, AUvSeamStaysContinuousPerTheInterpolationRule)
{
    const VtIntArray counts = { 4, 4 };
    const VtIntArray indices = { 0, 1, 4, 3, 1, 2, 5, 4 };
    const VtVec3fArray points = { GfVec3f(0, 0, 0), GfVec3f(1, 0, 0), GfVec3f(2, 0, 0), GfVec3f(0, 1, 0), GfVec3f(1, 1, 0), GfVec3f(2, 1, 0) };
    const VtIntArray uvIndices = { 0, 1, 2, 3, 4, 5, 6, 7 };
    const VtVec2fArray uvs = { GfVec2f(0, 0), GfVec2f(1, 0), GfVec2f(1, 1), GfVec2f(0, 1), GfVec2f(5, 0), GfVec2f(6, 0), GfVec2f(6, 1), GfVec2f(5, 1) };

    for (const TfToken& rule : { PxOsdOpenSubdivTokens->all, PxOsdOpenSubdivTokens->cornersPlus1, PxOsdOpenSubdivTokens->none }) {
        SCOPED_TRACE(rule.GetString());
        PxOsdSubdivTags tags;

        tags.SetFaceVaryingInterpolationRule(rule);

        const PxOsdMeshTopology topology(PxOsdOpenSubdivTokens->catmullClark, PxOsdOpenSubdivTokens->rightHanded, counts, indices, VtIntArray(), tags);
        const UtilitySubdivRefinementPtr refinement = refine(topology, 2, { uvIndices });

        ASSERT_TRUE(refinement != nullptr);
        ASSERT_EQ(1, refinement->faceVaryingChannelCount());

        VtVec3fArray refined;
        const VtVec2fArray refinedUvs = refinement->interpolateFaceVarying(0, uvs);
        const VtVec3iArray& corners = refinement->faceVaryingTriangles(0);

        refinement->refine(points, &refined);
        ASSERT_EQ(refinement->refinedFaceVaryingCount(0), refinedUvs.size());
        ASSERT_EQ(refinement->triangles().size(), corners.size());

        // Per seam point, the UV each side gives it.
        std::map<int, GfVec2f> left;
        std::map<int, GfVec2f> right;

        for (std::size_t t = 0; t < corners.size(); ++t) {
            for (int corner = 0; corner < 3; ++corner) {
                const int point = refinement->triangles()[t][corner];

                if (std::fabs(refined[point][0] - 1.f) < 1e-6f) {
                    (refinement->coarseFaces()[t] == 0 ? left : right)[point] = refinedUvs[corners[t][corner]];
                }
            }
        }
        ASSERT_EQ(5u, left.size());
        ASSERT_EQ(left.size(), right.size());

        bool cornersMoved = false;

        for (const auto& entry : left) {
            const GfVec2f& a = entry.second;
            const GfVec2f& b = right[entry.first];

            EXPECT_NEAR(a[1], b[1], 1e-6f);
            EXPECT_NEAR(a[0] - 1.f, 5.f - b[0], 1e-6f);
            if (rule == PxOsdOpenSubdivTokens->none) {
                cornersMoved = cornersMoved || a[0] < 1.f - 1e-3f;
            } else {
                EXPECT_NEAR(1.f, a[0], 1e-6f);
                EXPECT_NEAR(5.f, b[0], 1e-6f);
            }
        }
        if (rule == PxOsdOpenSubdivTokens->none) {
            EXPECT_TRUE(cornersMoved) << "smoothing every UV boundary should pull the island corners in";
        }
    }
}

// The grid refines for seconds. A caller that stops must let go within the bound while the
// refinement goes on, unrepeated, for the caller still waiting on it.
TEST(NatronUtilitySubdiv, AStoppedCallerLeavesASharedRefinementToTheOthers)
{
    typedef std::chrono::steady_clock Clock;
    typedef UtilitySubdivRefinement::Outcome Outcome;
    const int grid = 96;
    const int level = 4;
    const PxOsdMeshTopology topology = gridTopology(grid);
    const std::chrono::milliseconds bound(500);
    const CacheBudget roomy(kRoomyCacheBudget);

    UtilitySubdivRefinement::clearCache();

    const UtilitySubdivRefinement::Stats before = UtilitySubdivRefinement::stats();
    std::atomic<bool> stop { false };
    std::atomic<bool> stoppingReturned { false };
    Outcome stoppingOutcome = Outcome::Done;
    UtilitySubdivRefinementPtr stoppingGot;
    Clock::time_point stoppingReturnedAt;
    std::thread stopping([&]() {
        std::string error;

        stoppingGot = UtilitySubdivRefinement::get(topology, level, std::vector<VtIntArray>(), [&]() { return stop.load(); }, &stoppingOutcome, &error);
        stoppingReturnedAt = Clock::now();
        stoppingReturned.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    Outcome waitingOutcome = Outcome::Failed;
    UtilitySubdivRefinementPtr waited;
    std::thread waiting([&]() {
        std::string error;

        waited = UtilitySubdivRefinement::get(topology, level, std::vector<VtIntArray>(), UtilitySubdivRefinement::StopCheck(), &waitingOutcome, &error);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    const bool refining = !stoppingReturned.load();
    const Clock::time_point stoppedAt = Clock::now();

    stop.store(true);
    stopping.join();
    waiting.join();
    ASSERT_TRUE(refining) << "the grid refines too fast to test this";
    EXPECT_EQ(Outcome::Stopped, stoppingOutcome);
    EXPECT_TRUE(stoppingGot == nullptr);
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(stoppingReturnedAt - stoppedAt).count(), bound.count());

    const UtilitySubdivRefinement::Stats after = UtilitySubdivRefinement::stats();

    EXPECT_EQ(Outcome::Done, waitingOutcome);
    ASSERT_TRUE(waited != nullptr);
    EXPECT_EQ(static_cast<std::size_t>(grid) * grid * 2 * static_cast<std::size_t>(std::pow(4., level)), waited->triangles().size());
    EXPECT_EQ(before.refinementsStarted + 1, after.refinementsStarted);
    EXPECT_EQ(before.refinerBuilds + 1, after.refinerBuilds);
    EXPECT_EQ(1u, after.cachedEntries);
}

// With no one else waiting, the refinement a stopped caller leaves behind is dropped once it ends,
// not kept for a join that cannot come.
TEST(NatronUtilitySubdiv, ALoneStoppedCallerLeavesNoBuildingJob)
{
    typedef UtilitySubdivRefinement::Outcome Outcome;
    const PxOsdMeshTopology topology = gridTopology(96);
    const CacheBudget roomy(kRoomyCacheBudget);

    UtilitySubdivRefinement::clearCache();

    std::atomic<bool> stop { false };
    Outcome outcome = Outcome::Done;
    std::thread caller([&]() {
        std::string error;

        UtilitySubdivRefinement::get(topology, 4, std::vector<VtIntArray>(), [&]() { return stop.load(); }, &outcome, &error);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    stop.store(true);
    caller.join();
    EXPECT_EQ(Outcome::Stopped, outcome);

    const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + std::chrono::seconds(120);

    while (UtilitySubdivRefinement::stats().buildingJobs != 0 && std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_EQ(0u, UtilitySubdivRefinement::stats().buildingJobs);
}

TEST(NatronUtilitySubdivRender, MeshesRefineToTheSettingsLevel)
{
    SubdivRender render(std::string("#usda 1.0\n") + cubeAt("Cube", ""));

    render.render();
    EXPECT_EQ(cubeTriangles(0), render.stat(kUtilityStatTriangleCount));
    render.setSetting(kUtilitySettingSubdivLevel, 2);
    render.render();
    EXPECT_EQ(cubeTriangles(2), render.stat(kUtilityStatTriangleCount));
    render.setSetting(kUtilitySettingSubdivLevel, 0);
    render.render();
    EXPECT_EQ(cubeTriangles(0), render.stat(kUtilityStatTriangleCount));
    render.setSetting(kUtilitySettingSubdivLevel, 3);
    render.render();
    EXPECT_EQ(cubeTriangles(3), render.stat(kUtilityStatTriangleCount));
}

TEST(NatronUtilitySubdivRender, EditingACagePointReRunsTheStencilsWithoutANewRefiner)
{
    SubdivRender render(std::string("#usda 1.0\n") + cubeAt("Cube", ""));

    render.setSetting(kUtilitySettingSubdivLevel, 2);
    render.render();
    const UtilitySubdivRefinement::Stats before = UtilitySubdivRefinement::stats();
    const std::size_t topologyBuilds = render.stat(kUtilityStatTopologyBuilds);
    const std::size_t pointUpdates = render.stat(kUtilityStatPointUpdates);

    render.movePoint("/Cube", 0, GfVec3f(-1.5f, -1.f, 1.f));
    render.render();

    const UtilitySubdivRefinement::Stats after = UtilitySubdivRefinement::stats();

    EXPECT_EQ(before.refinerBuilds, after.refinerBuilds);
    EXPECT_GT(after.evaluations, before.evaluations);
    EXPECT_EQ(topologyBuilds, render.stat(kUtilityStatTopologyBuilds));
    EXPECT_EQ(pointUpdates + 1, render.stat(kUtilityStatPointUpdates));
}

TEST(NatronUtilitySubdivRender, ThePrimvarOverrideBeatsTheSetting)
{
    SubdivRender render(std::string("#usda 1.0\n")
                        + cubeAt("Override", "    int primvars:natron:subdivLevel = 1 (interpolation = \"constant\")\n")
                        + cubeAt("Setting", ""));

    render.setSetting(kUtilitySettingSubdivLevel, 3);
    render.render();
    EXPECT_EQ(cubeTriangles(1) + cubeTriangles(3), render.stat(kUtilityStatTriangleCount));
}

TEST(NatronUtilitySubdivRender, SchemeNoneIgnoresALevelOverride)
{
    SubdivRender render(std::string("#usda 1.0\n")
                        + cubeAt("Cube", "    uniform token subdivisionScheme = \"none\"\n"
                                         "    int primvars:natron:subdivLevel = 3 (interpolation = \"constant\")\n"));

    render.render();
    EXPECT_EQ(cubeTriangles(0), render.stat(kUtilityStatTriangleCount));
}

TEST(NatronUtilitySubdivRender, TheTriangleBudgetLowersTheLevelAndWarns)
{
    SubdivRender render(std::string("#usda 1.0\n") + cubeAt("Cube", ""));

    render.setSetting(kUtilitySettingSubdivLevel, 3);
    render.setSetting(kUtilitySettingMaxSubdivTriangles, 100);
    render.render();
    EXPECT_EQ(cubeTriangles(1), render.stat(kUtilityStatTriangleCount));

    const std::string warning = render.warning();

    EXPECT_NE(std::string::npos, warning.find("/Cube")) << warning;
    EXPECT_NE(std::string::npos, warning.find("lowered")) << warning;
}

TEST(NatronUtilitySubdivRender, TopologyOpenSubdivRejectsRendersTheCageAndWarns)
{
    SubdivRender render(std::string("#usda 1.0\n") + cubeAt("Cube", "    uniform token subdivisionScheme = \"loop\"\n"));

    render.setSetting(kUtilitySettingSubdivLevel, 2);
    render.render();
    EXPECT_EQ(cubeTriangles(0), render.stat(kUtilityStatTriangleCount));
    EXPECT_NE(std::string::npos, render.warning().find("/Cube")) << render.warning();
}

// Every pixel whose centre the render calls covered, or not, against the analytic sphere the cage
// samples must sit within half a pixel of that sphere's outline.
TEST(NatronUtilitySubdivRender, ACatmullClarkSphereCageMatchesTheSphereSilhouetteAtLevelThree)
{
    const int size = 128;
    const double radius = 8.;
    SubdivRender render("#usda 1.0\n", size);

    defineSphereCage(render.stage, radius, 64, 48);
    render.setSetting(kUtilitySettingSubdivLevel, 3);
    render.render();
    ASSERT_GT(render.stat(kUtilityStatTriangleCount), 100000u);

    const double halfHeight = std::tan(kFieldOfView * M_PI / 360.);
    const double outline = std::tan(std::asin(radius / kCameraZ)) / halfHeight * size / 2.;
    double worst = 0.;
    int covered = 0;

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double dx = x + 0.5 - size / 2.;
            const double dy = y + 0.5 - size / 2.;
            const double fromCentre = std::sqrt(dx * dx + dy * dy);
            const bool inside = fromCentre < outline;
            const bool hit = render.alphaAt(x, y) > 0.5f;

            covered += hit;
            if (hit != inside) {
                worst = std::max(worst, std::fabs(fromCentre - outline));
            }
        }
    }
    EXPECT_GT(covered, 0);
    EXPECT_LT(worst, 0.5);
}

// Limit normals carry the lighting smoothly across the level-1 facets of a coarse sphere cage, while
// the bilinear cage, which has none, shades each facet flat and steps at its edges.
TEST(NatronUtilitySubdivRender, ASubdividedSphereShadesSmoothlyAndTheFlatCageDoesNot)
{
    SubdivRender render(std::string("#usda 1.0\n") + kLitWhite, kShadedSize);

    defineSphereCage(render.stage, 8., 8, 6);
    bindWhite(render.stage, "/Sphere");
    render.setSetting(kUtilitySettingPixelSamples, 1);
    render.setSetting(kUtilitySettingSubdivLevel, 1);
    render.render();

    const float smooth = largestStep(render.pixels(), 0.5);

    EXPECT_GT(render.colorAt(kShadedSize / 2, kShadedSize / 2)[0], 0.9f);
    EXPECT_LT(smooth, 0.1f);

    UsdGeomMesh(render.stage->GetPrimAtPath(SdfPath("/Sphere"))).GetSubdivisionSchemeAttr().Set(UsdGeomTokens->bilinear);
    render.render();

    const float flat = largestStep(render.pixels(), 0.5);

    EXPECT_GT(flat, 0.15f);
    EXPECT_GT(flat, 2.f * smooth);
}

// Authored corner normals shade the cage itself: the face toward the camera is brightest at its
// centre and falls off smoothly toward its corners, with or without a subdivision scheme.
TEST(NatronUtilitySubdivRender, AuthoredCornerNormalsShadeTheCageSmoothlyAtLevelZero)
{
    for (const char* scheme : { "catmullClark", "none" }) {
        SubdivRender render(cornerNormalCube(scheme), kShadedSize);

        render.setSetting(kUtilitySettingPixelSamples, 1);
        render.render();
        ASSERT_EQ(12u, render.stat(kUtilityStatTriangleCount)) << scheme;

        const float centre = render.colorAt(kShadedSize / 2, kShadedSize / 2)[0];
        const float nearCorner = render.colorAt(kShadedSize / 2 + 20, kShadedSize / 2 + 20)[0];

        EXPECT_GT(centre, 0.95f) << scheme;
        EXPECT_LT(nearCorner, centre - 0.2f) << scheme;
        EXPECT_LT(largestStep(render.pixels(), 1.), 0.08f) << scheme;
    }
}

#ifdef NATRON_HAVE_GPU
TEST(NatronUtilitySubdivRender, GpuShadesTheSubdividedSphereAsTheCpuDoes)
{
    SubdivGpuRuntime runtime;

    GPUTEST_DEVICE_OR_SKIP(runtime.unavailable.empty(), runtime.unavailable);

    const std::string missing = runtime.missingSupport();

    if (!missing.empty()) {
        GPUTEST_SKIP() << missing;
    }

    SubdivRender render(std::string("#usda 1.0\n") + kLitWhite, kShadedSize);

    defineSphereCage(render.stage, 8., 8, 6);
    bindWhite(render.stage, "/Sphere");
    render.setSetting(kUtilitySettingPixelSamples, 1);
    render.setSetting(kUtilitySettingSubdivLevel, 1);
    render.setDevice(kUtilityDeviceCpu);
    render.render();
    EXPECT_EQ("CPU", render.device());

    const std::vector<GfVec4f> cpu = render.pixels();

    render.setDevice(kUtilityDeviceGpu);
    render.render();
    EXPECT_EQ(0u, render.device().rfind("GPU", 0)) << render.device();
    EXPECT_EQ(std::string(), render.warning());

    const std::vector<GfVec4f> gpu = render.pixels();

    EXPECT_LT(largestStep(gpu, 0.5), 0.1f);
    for (std::size_t i = 0; i < cpu.size(); ++i) {
        for (int c = 0; c < 4; ++c) {
            EXPECT_NEAR(cpu[i][c], gpu[i][c], 1e-4f) << "pixel " << i % kShadedSize << ", " << i / kShadedSize;
        }
    }
}

TEST(NatronUtilitySubdivRender, GpuShadesAuthoredCornerNormalsAsTheCpuDoes)
{
    SubdivGpuRuntime runtime;

    GPUTEST_DEVICE_OR_SKIP(runtime.unavailable.empty(), runtime.unavailable);

    const std::string missing = runtime.missingSupport();

    if (!missing.empty()) {
        GPUTEST_SKIP() << missing;
    }

    SubdivRender render(cornerNormalCube("none"), kShadedSize);

    render.setSetting(kUtilitySettingPixelSamples, 1);
    render.setDevice(kUtilityDeviceCpu);
    render.render();

    const std::vector<GfVec4f> cpu = render.pixels();

    render.setDevice(kUtilityDeviceGpu);
    render.render();
    EXPECT_EQ(0u, render.device().rfind("GPU", 0)) << render.device();

    const std::vector<GfVec4f> gpu = render.pixels();

    for (std::size_t i = 0; i < cpu.size(); ++i) {
        for (int c = 0; c < 4; ++c) {
            EXPECT_NEAR(cpu[i][c], gpu[i][c], 1e-4f) << "pixel " << i % kShadedSize << ", " << i / kShadedSize;
        }
    }
}
#endif // NATRON_HAVE_GPU

// The grid refines for seconds: a stop well into it must end the render within the bound, cache
// nothing, and leave the mesh to refine in full once the delegate restarts.
TEST(NatronUtilitySubdivRender, AStopDuringARefinementReturnsWithinTheBoundAndCachesNothing)
{
    typedef std::chrono::steady_clock Clock;
    const int grid = 96;
    const int level = 4;
    const std::chrono::milliseconds stopAfter(100);
    const std::chrono::milliseconds bound(500);
    const CacheBudget roomy(kRoomyCacheBudget);

    UtilitySubdivRefinement::clearCache();

    SubdivRender render("#usda 1.0\n");

    defineGridCage(render.stage, grid);
    render.setSetting(kUtilitySettingSubdivLevel, level);

    const std::size_t buildsBefore = UtilitySubdivRefinement::stats().refinerBuilds;
    std::atomic<bool> returned { false };
    Clock::time_point returnedAt;
    std::thread thread([&]() {
        render.renderOnce();
        returnedAt = Clock::now();
        returned.store(true);
    });

    std::this_thread::sleep_for(stopAfter);

    const bool refining = !returned.load();
    const Clock::time_point stoppedAt = Clock::now();
    const bool stopped = render.stop();

    thread.join();
    ASSERT_TRUE(refining) << "the grid refines too fast to test this";
    EXPECT_TRUE(stopped);

    const long took = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(returnedAt - stoppedAt).count());

    std::cout << "[   NOTE   ] the render returned " << took << " ms after the stop" << std::endl;
    EXPECT_LT(took, static_cast<long>(bound.count()));
    EXPECT_FALSE(render.isConverged());

    const UtilitySubdivRefinement::Stats afterStop = UtilitySubdivRefinement::stats();

    EXPECT_EQ(0u, afterStop.cachedEntries);
    EXPECT_EQ(buildsBefore, afterStop.refinerBuilds);

    render.restart();
    render.render();
    EXPECT_EQ(1u, UtilitySubdivRefinement::stats().cachedEntries);
    EXPECT_EQ(static_cast<std::size_t>(grid) * grid * 2 * static_cast<std::size_t>(std::pow(4., level)), render.stat(kUtilityStatTriangleCount));
}

// The texture reads the refined face-varying st. With every UV linear, u runs on across the seam at
// the image's centre as it does elsewhere. With none linear, each island's border is smoothed on its
// own, pulling the two sides of the seam apart in u, so the ramp jumps there.
TEST(NatronUtilitySubdivRender, ATexturedUvSeamStaysContinuousPerTheInterpolationRule)
{
    QTemporaryDir dir;

    ASSERT_TRUE(dir.isValid());
    const std::string path = dir.filePath(QString::fromUtf8("ramp.png")).toStdString();

    ASSERT_TRUE(writeRampTexture(path));

    const int size = 64;
    const int row = size / 2;
    const int seam = size / 2;
    std::map<TfToken, float> rises;

    for (const TfToken& rule : { PxOsdOpenSubdivTokens->all, PxOsdOpenSubdivTokens->none }) {
        SCOPED_TRACE(rule.GetString());
        SubdivRender render(rampSeamCage(path, rule), size);

        render.setSetting(kUtilitySettingSubdivLevel, 3);
        render.render();
        EXPECT_EQ(std::string(), render.warning());

        std::vector<float> grey(size);

        for (int x = 0; x < size; ++x) {
            grey[x] = render.colorAt(x, row)[0];
        }
        EXPECT_LT(grey[2], 0.25f);
        EXPECT_GT(grey[size - 3], 0.75f);
        for (int x = 2; x + 3 < size; ++x) {
            EXPECT_GE(grey[x + 1], grey[x] - 0.004f) << "column " << x;
        }
        rises[rule] = grey[seam + 1] - grey[seam - 2];
        if (rule == PxOsdOpenSubdivTokens->all) {
            std::vector<float> steps;

            for (int x = 8; x < seam - 6; ++x) {
                steps.push_back(grey[x + 1] - grey[x]);
                steps.push_back(grey[size - x] - grey[size - x - 1]);
            }
            std::nth_element(steps.begin(), steps.begin() + steps.size() / 2, steps.end());

            const float typical = steps[steps.size() / 2];

            EXPECT_GT(typical, 0.f);
            EXPECT_LT(rises[rule], 3.f * typical + 0.012f) << "three steps across the seam, typically " << typical << " each";
        }
    }
    EXPECT_GT(rises[PxOsdOpenSubdivTokens->none], rises[PxOsdOpenSubdivTokens->all] + 0.02f);
}
