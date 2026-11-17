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
#include <pxr/base/gf/rotation.h>
#include <pxr/base/gf/vec3d.h>
#include <pxr/base/gf/vec3f.h>
#include <pxr/base/gf/vec3i.h>
#include <pxr/base/gf/vec4f.h>
#include <pxr/base/vt/types.h>
#include <pxr/base/vt/value.h>
#include <pxr/imaging/hd/types.h>
#include <pxr/usd/sdf/path.h>

#include "Global/Macros.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "Engine/Nodes/Scene/Utility/Tracer.h"
#include "Engine/Nodes/Scene/Utility/UtilityKernels.h"
#include "Engine/Nodes/Scene/Utility/UtilityLight.h"
#include "Engine/Nodes/Scene/Utility/UtilityMaterial.h"
#include "Engine/Nodes/Scene/Utility/UtilityRenderBuffer.h"
#include "Engine/Nodes/Scene/Utility/UtilityScene.h"
#include "Engine/Nodes/Scene/Utility/Wavefront.h"
#include "Engine/Nodes/Scene/Utility/WavefrontCpu.h"

#ifdef NATRON_HAVE_GPU
#include "Engine/Gpu/GpuBufferPool.h"
#include "Engine/Gpu/GpuKernelRegistry.h"
#include "Engine/Nodes/Scene/Utility/GpuTracer.h"
#include "Engine/Nodes/Scene/Utility/WavefrontGpu.h"
#include "Gpu/GpuTestDevice.h"
#endif

PXR_NAMESPACE_USING_DIRECTIVE
NATRON_NAMESPACE_USING

namespace {

// One world unit is one pixel: pixel p spans x, and y, from p - 8 to p - 7.
const int kSize = 16;
const double kHalfSize = 8.;
const double kCameraZ = 30.;
const double kTolerance = 1e-3;

double
pixelCentre(int p)
{
    return p + 0.5 - kHalfSize;
}

GfVec3d
toward(double degrees)
{
    const double radians = degrees * M_PI / 180.;

    return GfVec3d(0., std::sin(radians), std::cos(radians));
}

int gNextPrimId = 1;

std::string
materialNamed(UtilityScene& scene,
              const std::string& name,
              const UtilityMaterialDesc& desc)
{
    const std::string path = "/Materials/" + name;

    scene.setMaterial(SdfPath(path), std::make_shared<const UtilityMaterialDesc>(desc));

    return path;
}

// A rectangle centred at centre spanning +-u and +-v, whose outward side is u x v.
void
addQuad(UtilityScene& scene,
        const std::string& name,
        const GfVec3d& centre,
        const GfVec3d& u,
        const GfVec3d& v,
        const std::string& material)
{
    const UtilityScene::GeometryId id = scene.addGeometry(SdfPath("/" + name), gNextPrimId++);
    const GfVec3f c(centre);
    const GfVec3f a(u);
    const GfVec3f b(v);

    scene.setTopology(id, VtVec3fArray { c - a - b, c + a - b, c + a + b, c - a + b }, VtVec3iArray { GfVec3i(0, 1, 2), GfVec3i(0, 2, 3) });
    scene.setColors(id, UtilityScene::eColorBindingConstant, VtVec4fArray { GfVec4f(1.f, 1.f, 1.f, 1.f) });
    scene.setInstances(id, { GfMatrix4d(1.) });
    scene.setMaterialId(id, SdfPath(material));
}

// A square of side 2 * half through centre facing outward, its edges along x and along outward x x.
void
addPlane(UtilityScene& scene,
         const std::string& name,
         const GfVec3d& centre,
         const GfVec3d& outward,
         double half,
         const std::string& material)
{
    const GfVec3d u(half, 0., 0.);

    addQuad(scene, name, centre, u, GfCross(outward.GetNormalized(), u), material);
}

void
addSphere(UtilityScene& scene,
          const std::string& name,
          const GfVec3d& centre,
          double radius,
          const std::string& material)
{
    const int rings = 32;
    const int segments = 64;
    VtVec3fArray points;
    VtVec3iArray triangles;

    for (int i = 0; i <= rings; ++i) {
        const double theta = M_PI * i / rings;

        for (int j = 0; j < segments; ++j) {
            const double phi = 2. * M_PI * j / segments;

            points.push_back(GfVec3f(centre + radius * GfVec3d(std::sin(theta) * std::cos(phi), std::cos(theta), std::sin(theta) * std::sin(phi))));
        }
    }
    for (int i = 0; i < rings; ++i) {
        for (int j = 0; j < segments; ++j) {
            const int a = i * segments + j;
            const int b = i * segments + (j + 1) % segments;

            triangles.push_back(GfVec3i(a, a + segments, b));
            triangles.push_back(GfVec3i(b, a + segments, b + segments));
        }
    }

    const UtilityScene::GeometryId id = scene.addGeometry(SdfPath("/" + name), gNextPrimId++);

    scene.setTopology(id, points, triangles);
    scene.setColors(id, UtilityScene::eColorBindingConstant, VtVec4fArray { GfVec4f(1.f, 1.f, 1.f, 1.f) });
    scene.setInstances(id, { GfMatrix4d(1.) });
    scene.setMaterialId(id, SdfPath(material));
}

UtilityMaterialDesc
emitter(const GfVec3f& color)
{
    UtilityMaterialDesc desc;

    desc.diffuseColor = GfVec3f(0.f);
    desc.emissiveColor = color;
    desc.metallic = 1.f;

    return desc;
}

UtilityMaterialDesc
mirror(const GfVec3f& color)
{
    UtilityMaterialDesc desc;

    desc.diffuseColor = color;
    desc.metallic = 1.f;
    desc.roughness = 0.f;

    return desc;
}

UtilityMaterialDesc
glass()
{
    UtilityMaterialDesc desc;

    desc.diffuseColor = GfVec3f(1.f);
    desc.opacity = 0.f;
    desc.roughness = 0.f;
    desc.ior = 1.5f;

    return desc;
}

void
addDome(UtilityScene& scene,
        const GfVec3f& radiance)
{
    UtilityLightDesc dome;

    dome.type = UtilityLightDesc::eTypeDome;
    dome.emission = radiance;
    scene.setLight(SdfPath("/Dome"), std::make_shared<const UtilityLightDesc>(dome));
}

// Fresnel's unpolarised reflectance going from a medium of index n1 into one of n2 at cosI, 1 past
// the critical angle.
double
fresnel(double cosI,
        double n1,
        double n2)
{
    const double sinT2 = (n1 / n2) * (n1 / n2) * (1. - cosI * cosI);

    if (sinT2 >= 1.) {
        return 1.;
    }

    const double cosT = std::sqrt(1. - sinT2);
    const double rs = (n1 * cosI - n2 * cosT) / (n1 * cosI + n2 * cosT);
    const double rp = (n2 * cosI - n1 * cosT) / (n2 * cosI + n1 * cosT);

    return 0.5 * (rs * rs + rp * rp);
}

WavefrontFrame
makeFrame(int samplesPerSide)
{
    GfFrustum camera;

    camera.SetOrthographic(-kHalfSize, kHalfSize, -kHalfSize, kHalfSize, 1., 100.);
    camera.SetPosition(GfVec3d(0., 0., kCameraZ));

    WavefrontFrame frame;

    frame.worldToView = camera.ComputeViewMatrix();
    frame.projection = camera.ComputeProjectionMatrix();
    frame.window.width = kSize;
    frame.window.height = kSize;
    frame.sampling.samplesPerSide = samplesPerSide;
    frame.sampling.filter = eKernelPixelFilterBox;
    frame.sampling.filterWidth = 1.f;
    frame.sampling.seed = 11;

    return frame;
}

// The render pass's colour film, filtered.
class ColorFilm : public WavefrontFilm {
public:
    explicit ColorFilm(bool colorOnDevice)
        : _color(new UtilityRenderBuffer(SdfPath("/color")))
        , _colorOnDevice(colorOnDevice)
    {
        EXPECT_TRUE(_color->Allocate(GfVec3i(kSize, kSize, 1), HdFormatFloat32Vec4, false));
        _color->clear(VtValue(GfVec4f(0.f, 0.f, 0.f, 0.f)));
    }

    virtual void writeTile(const WavefrontTile& tile) OVERRIDE
    {
        std::size_t index = 0;

        for (int row = 0; row < tile.rect.height; ++row) {
            for (int column = 0; column < tile.rect.width; ++column, ++index) {
                const bool hit = tile.hitGeometry[index] != Tracer::kNoHit;
                const float weight = tile.weight ? tile.weight[index] : 1.f;

                if (tile.color) {
                    _color->addSample(static_cast<unsigned int>(tile.rect.x + column), static_cast<unsigned int>(tile.rect.y + row), hit ? tile.color + index * 4 : nullptr, 4, weight, true);
                }
            }
        }
    }

    virtual UtilityRenderBuffer* deviceColor() const OVERRIDE
    {
        return _colorOnDevice ? _color.get() : nullptr;
    }

    virtual void passDrawn() OVERRIDE
    {
        _color->publishPass();
    }

    std::vector<GfVec4f> colors() const
    {
        const GfVec4f* pixels = static_cast<const GfVec4f*>(_color->Map());
        std::vector<GfVec4f> out;

        if (pixels) {
            out.assign(pixels, pixels + kSize * kSize);
        }
        _color->Unmap();
        EXPECT_EQ(static_cast<std::size_t>(kSize * kSize), out.size());

        return out;
    }

private:
    std::unique_ptr<UtilityRenderBuffer> _color;
    bool _colorOnDevice;
};

enum class Backend {
    Cpu,
    // The GPU handing its colour back in tiles, as for a film with several colour AOVs.
    GpuTiles,
    // The GPU keeping the colour on the device, as for the viewer.
    GpuFilm,
};

void
PrintTo(Backend backend,
        std::ostream* os)
{
    *os << (backend == Backend::Cpu ? "Cpu" : (backend == Backend::GpuTiles ? "GpuTiles" : "GpuFilm"));
}

class NatronUtilityReflect : public ::testing::TestWithParam<Backend> {
protected:
    void SetUp() OVERRIDE
    {
        gNextPrimId = 1;
        if (GetParam() == Backend::Cpu) {
            _cpu.reset(new WavefrontCpu());
            _executor = _cpu.get();

            return;
        }
#ifdef NATRON_HAVE_GPU
        const gpu::GpuStatus s = gputest::createDevice(_device);

        GPUTEST_DEVICE_OR_SKIP(s.ok(), s.message);

        const std::string missing = GpuTracer::missingRayQuerySupport(*_device);

        if (!missing.empty()) {
            GPUTEST_SKIP() << missing;
        }
        _registry.reset(new gpu::GpuKernelRegistry(*_device));
        _pool.reset(new gpu::GpuBufferPool(*_device));
        // A device film without a runtime hold counts as released, so the colour could not stay on the GPU.
        _gpu.reset(new WavefrontGpu(*_registry, *_pool, std::make_shared<int>(0)));
        _executor = _gpu.get();
#else
        std::printf("[  SKIPPED ] Natron was built without the Vulkan backend, so there is no GPU executor\n");
#endif
    }

    void TearDown() OVERRIDE
    {
        _executor = nullptr;
#ifdef NATRON_HAVE_GPU
        _gpu.reset();
        if (_device) {
            _device->waitIdle();
        }
        _pool.reset();
        _registry.reset();
        _device.reset();
        _check.expectClean("the test");
#endif
    }

    bool available() const
    {
        return _executor != nullptr;
    }

    // Every sample of the frame, filtered, as the render pass draws them.
    std::vector<GfVec4f> render(const UtilityScene& scene,
                                WavefrontFrame frame)
    {
        ColorFilm film(GetParam() == Backend::GpuFilm);
        const UtilityScene::Snapshot snapshot = scene.snapshot();
        const std::atomic<bool> stop(false);

        for (int sample = 0; sample < frame.sampling.samplesPerPixel(); ++sample) {
            frame.sampling.sampleIndex = sample;
            if (!_executor->render(snapshot, frame, film, stop, 2)) {
                ADD_FAILURE() << "sample " << sample << " did not render";

                return std::vector<GfVec4f>();
            }
            film.passDrawn();
        }

        return film.colors();
    }

    std::uint32_t bouncesTraced() const
    {
        return _executor->bouncesTraced();
    }

private:
    WavefrontExecutor* _executor = nullptr;
    std::unique_ptr<WavefrontCpu> _cpu;
#ifdef NATRON_HAVE_GPU
    gputest::ValidationCheck _check;
    std::unique_ptr<gpu::GpuDevice> _device;
    std::unique_ptr<gpu::GpuKernelRegistry> _registry;
    std::unique_ptr<gpu::GpuBufferPool> _pool;
    std::unique_ptr<WavefrontGpu> _gpu;
#endif
};

void
expectColor(const GfVec4f& actual,
            const GfVec3d& expected,
            double tolerance,
            const std::string& where)
{
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(expected[c], actual[c], tolerance) << where << ", channel " << c;
    }
}

// Two facing mirrors that each reflect r of the light and emit e: a path reflects between them until
// its depth is spent, gathering e r^k at its k-th bounce.
void
addFacingMirrors(UtilityScene& scene,
                 float r,
                 float e)
{
    UtilityMaterialDesc desc = mirror(GfVec3f(r));

    desc.emissiveColor = GfVec3f(e);

    const std::string material = materialNamed(scene, "Mirror", desc);

    addPlane(scene, "Far", GfVec3d(0., 0., -50.), GfVec3d(0., 0., 1.), 100., material);
    addPlane(scene, "Near", GfVec3d(0., 0., 50.), GfVec3d(0., 0., -1.), 100., material);
}

double
mirrorSeries(double r,
             double e,
             int bounces)
{
    double sum = 0.;
    double carried = 1.;

    for (int k = 0; k <= bounces; ++k, carried *= r) {
        sum += e * carried;
    }

    return sum;
}

} // anon namespace

TEST_P(NatronUtilityReflect, ABounceBetweenMirrorsEndsAtTheRayDepth)
{
    if (!available()) {
        return;
    }

    const float r = 0.5f;
    const float e = 0.25f;
    UtilityScene scene;

    addFacingMirrors(scene, r, e);

    const struct {
        std::uint32_t maxRayDepth;
        std::uint32_t reflectionDepth;
        int bounces;
    } cases[] = {
        { 4, 10, 4 },
        { 10, 2, 2 },
        { 0, 10, 0 },
        { 10, 0, 0 },
    };

    for (const auto& depths : cases) {
        WavefrontFrame frame = makeFrame(1);

        frame.maxRayDepth = depths.maxRayDepth;
        frame.reflectionDepth = depths.reflectionDepth;

        const std::vector<GfVec4f> colors = render(scene, frame);
        const std::string where = "max ray depth " + std::to_string(depths.maxRayDepth) + ", reflection depth " + std::to_string(depths.reflectionDepth);

        ASSERT_EQ(static_cast<std::size_t>(kSize * kSize), colors.size());
        EXPECT_EQ(static_cast<std::uint32_t>(depths.bounces), bouncesTraced()) << where;

        const double expected = mirrorSeries(r, e, depths.bounces);

        for (int i = 0; i < kSize * kSize; i += 17) {
            expectColor(colors[i], GfVec3d(expected), kTolerance, where + ", pixel " + std::to_string(i));
            EXPECT_NEAR(1., colors[i][3], kTolerance) << where;
        }
    }
}

TEST_P(NatronUtilityReflect, ASmoothDielectricReflectsTheDomeByItsFresnel)
{
    if (!available()) {
        return;
    }

    const GfVec3f dome(0.5f, 0.75f, 1.f);
    UtilityMaterialDesc desc;

    desc.diffuseColor = GfVec3f(0.f);
    desc.roughness = 0.f;
    desc.ior = 1.5f;

    UtilityScene scene;

    addDome(scene, dome);
    // At 60 degrees to the camera, the reflection goes on away from it into the dome.
    addPlane(scene, "Plane", GfVec3d(0.), toward(60.), 100., materialNamed(scene, "Dielectric", desc));

    const std::vector<GfVec4f> colors = render(scene, makeFrame(1));

    ASSERT_EQ(static_cast<std::size_t>(kSize * kSize), colors.size());

    const double f0 = 0.04;
    const double reflectance = f0 + (1. - f0) * std::pow(1. - std::cos(60. * M_PI / 180.), 5.);

    for (int i = 0; i < kSize * kSize; i += 13) {
        expectColor(colors[i], reflectance * GfVec3d(dome), kTolerance, "pixel " + std::to_string(i));
    }
}

TEST_P(NatronUtilityReflect, AMirrorShowsASphereWhereItsReflectionFalls)
{
    if (!available()) {
        return;
    }

    UtilityScene scene;

    // At 45 degrees, the mirror turns the camera's rays up along +y, out of the frame; the sphere sits
    // there, and the mirror point (x, y, -y) sees it where (x - 3.5)^2 + (y - 1)^2 < 2^2.
    addPlane(scene, "Mirror", GfVec3d(0.), toward(45.), 100., materialNamed(scene, "Mirror", mirror(GfVec3f(1.f))));
    addSphere(scene, "Ball", GfVec3d(3.5, 20., -1.), 2., materialNamed(scene, "Red", emitter(GfVec3f(1.f, 0.f, 0.f))));

    const std::vector<GfVec4f> colors = render(scene, makeFrame(1));

    ASSERT_EQ(static_cast<std::size_t>(kSize * kSize), colors.size());

    int inside = 0;

    for (int y = 0; y < kSize; ++y) {
        for (int x = 0; x < kSize; ++x) {
            const double distance = std::hypot(pixelCentre(x) - 3.5, pixelCentre(y) - 1.);
            const std::string where = "pixel " + std::to_string(x) + ", " + std::to_string(y);

            if (distance < 1.7) {
                expectColor(colors[y * kSize + x], GfVec3d(1., 0., 0.), kTolerance, where);
                ++inside;
            } else if (distance > 2.3) {
                expectColor(colors[y * kSize + x], GfVec3d(0.), kTolerance, where);
            }
        }
    }
    EXPECT_GT(inside, 6);
}

TEST_P(NatronUtilityReflect, ARoughReflectionOfTheDomeConvergesToItsIntegral)
{
    if (!available()) {
        return;
    }

    const double roughness = 0.5;
    const GfVec3f dome(1.f, 1.f, 1.f);
    UtilityMaterialDesc desc;

    desc.diffuseColor = GfVec3f(0.f);
    desc.roughness = static_cast<float>(roughness);
    desc.ior = 1.5f;

    UtilityScene scene;

    addDome(scene, dome);
    addPlane(scene, "Plane", GfVec3d(0.), GfVec3d(0., 0., 1.), 100., materialNamed(scene, "Rough", desc));

    // Facing the camera, V is the normal: the half vector is at half the reflection's angle, and the
    // integral of F G D / (4 NdotV) over the hemisphere is one dimensional.
    const double alpha = roughness * roughness;
    const double k = alpha * 0.5;
    const int steps = 20000;
    double expected = 0.;

    for (int i = 0; i < steps; ++i) {
        const double theta = (i + 0.5) * (0.5 * M_PI) / steps;
        const double cosL = std::cos(theta);
        const double cosH = std::cos(0.5 * theta);
        const double d = alpha * alpha / (M_PI * std::pow(cosH * cosH * (alpha * alpha - 1.) + 1., 2.));
        const double f = 0.04 + 0.96 * std::pow(1. - cosH, 5.);
        const double g = cosL / (cosL * (1. - k) + k);

        expected += f * g * d / 4. * 2. * M_PI * std::sin(theta) * (0.5 * M_PI) / steps;
    }

    const std::vector<GfVec4f> colors = render(scene, makeFrame(16));

    ASSERT_EQ(static_cast<std::size_t>(kSize * kSize), colors.size());

    double mean = 0.;

    for (int i = 0; i < kSize * kSize; ++i) {
        mean += colors[i][1];
        EXPECT_NEAR(expected, colors[i][1], 0.05 * expected) << "pixel " << i;
    }
    mean /= kSize * kSize;
    EXPECT_NEAR(expected, mean, 0.01 * expected);
}

TEST_P(NatronUtilityReflect, AGlassSlabShiftsWhatIsBehindItSideways)
{
    if (!available()) {
        return;
    }

    // Through a slab of thickness t at theta, a ray comes out parallel, moved d = t sin(theta - thetaT)
    // / cos(thetaT) towards -y: the edge of an emitter on y > 0 then shows at y = d.
    const double theta = 45. * M_PI / 180.;
    const double ior = 1.5;
    const double thetaT = std::asin(std::sin(theta) / ior);
    const double shift = 1.5;
    const double thickness = shift * std::cos(thetaT) / std::sin(theta - thetaT);
    const GfVec3d n = toward(45.);
    UtilityScene scene;
    const std::string material = materialNamed(scene, "Glass", glass());

    addPlane(scene, "Front", GfVec3d(0.), n, 100., material);
    addPlane(scene, "Back", -thickness * n, -n, 100., material);
    addQuad(scene, "Light", GfVec3d(0., 30., -40.), GfVec3d(60., 0., 0.), GfVec3d(0., 30., 0.), materialNamed(scene, "White", emitter(GfVec3f(1.f))));

    WavefrontFrame frame = makeFrame(8);

    // Without reflections each interface passes on 1 - F of the light with no noise.
    frame.reflectionDepth = 0;

    const std::vector<GfVec4f> colors = render(scene, frame);

    ASSERT_EQ(static_cast<std::size_t>(kSize * kSize), colors.size());

    const double f = fresnel(std::cos(theta), 1., ior);
    const double through = (1. - f) * (1. - f);

    for (int y = 0; y < kSize; ++y) {
        const double low = y - kHalfSize;
        const double lit = std::clamp(low + 1. - shift, 0., 1.);

        for (int x = 0; x < kSize; x += 5) {
            const std::string where = "pixel " + std::to_string(x) + ", " + std::to_string(y);

            EXPECT_NEAR(through * lit, colors[y * kSize + x][0], 1. / 8. + kTolerance) << where;
            if (lit == 0. || lit == 1.) {
                EXPECT_NEAR(through * lit, colors[y * kSize + x][0], kTolerance) << where;
            }
        }
    }
}

TEST_P(NatronUtilityReflect, LeavingGlassPastTheCriticalAngleReflectsEverything)
{
    if (!available()) {
        return;
    }

    const double ior = 1.5;

    for (double degrees : { 30., 50. }) {
        for (std::uint32_t refractionDepth : { 0u, 2u }) {
            UtilityScene scene;
            const GfVec3d away = -toward(degrees);

            // The camera meets the glass from inside it, going out into air.
            addPlane(scene, "Glass", GfVec3d(0.), away, 100., materialNamed(scene, "Glass", glass()));
            // What the refracted ray finds at 30 degrees, and what the reflection finds at 50.
            addQuad(scene, "Below", GfVec3d(0., 0., -5.), GfVec3d(60., 0., 0.), GfVec3d(0., 4., 0.), materialNamed(scene, "Green", emitter(GfVec3f(0.f, 1.f, 0.f))));
            addPlane(scene, "Above", GfVec3d(0., 20., 0.), GfVec3d(0., -1., 0.), 60., materialNamed(scene, "Blue", emitter(GfVec3f(0.f, 0.f, 1.f))));

            WavefrontFrame frame = makeFrame(1);

            // A total internal reflection counts as a refraction, so it still happens without reflections.
            frame.reflectionDepth = 0;
            frame.refractionDepth = refractionDepth;

            const std::vector<GfVec4f> colors = render(scene, frame);

            ASSERT_EQ(static_cast<std::size_t>(kSize * kSize), colors.size());

            const double f = fresnel(std::cos(degrees * M_PI / 180.), ior, 1.);
            GfVec3d expected(0.);

            if (refractionDepth > 0) {
                expected = degrees < 40. ? GfVec3d(0., 1. - f, 0.) : GfVec3d(0., 0., 1.);
            }
            if (degrees > 40.) {
                EXPECT_EQ(1., f);
            }
            for (int y = 7; y < 9; ++y) {
                for (int x = 6; x < 10; ++x) {
                    expectColor(colors[y * kSize + x], expected, kTolerance,
                                std::to_string(degrees) + " degrees, refraction depth " + std::to_string(refractionDepth) + ", pixel " + std::to_string(x) + ", " + std::to_string(y));
                }
            }
        }
    }
}

TEST_P(NatronUtilityReflect, AHalfOpaqueRedPaneCastsAHalfRedShadow)
{
    if (!available()) {
        return;
    }

    UtilityLightDesc sun;

    sun.type = UtilityLightDesc::eTypeDistant;
    sun.transform.SetRotate(GfRotation(GfVec3d(0., 1., 0.), 45.));

    // The pane, at a height of 10, casts its shadow 10 across towards the light's side.
    const GfVec3d toLight = sun.transform.TransformDir(GfVec3d(0., 0., 1.));
    const double side = toLight[0] > 0. ? 1. : -1.;
    UtilityMaterialDesc pane;

    pane.diffuseColor = GfVec3f(1.f, 0.f, 0.f);
    pane.opacity = 0.5f;

    UtilityMaterialDesc floor;

    floor.diffuseColor = GfVec3f(1.f);

    UtilityScene scene;

    scene.setLight(SdfPath("/Sun"), std::make_shared<const UtilityLightDesc>(sun));
    addPlane(scene, "Floor", GfVec3d(0.), GfVec3d(0., 0., 1.), 100., materialNamed(scene, "Floor", floor));
    addQuad(scene, "Pane", GfVec3d(side * 25., 0., 10.), GfVec3d(15., 0., 0.), GfVec3d(0., 60., 0.), materialNamed(scene, "Pane", pane));

    WavefrontFrame frame = makeFrame(1);

    frame.maxRayDepth = 0;

    const std::vector<GfVec4f> colors = render(scene, frame);

    ASSERT_EQ(static_cast<std::size_t>(kSize * kSize), colors.size());

    const int shadowed = side > 0. ? 13 : 2;
    const int lit = side > 0. ? 2 : 13;

    for (int y = 0; y < kSize; y += 3) {
        const GfVec4f& dark = colors[y * kSize + shadowed];
        const GfVec4f& bright = colors[y * kSize + lit];

        ASSERT_GT(bright[0], 0.01f);
        EXPECT_NEAR(0.5, dark[0] / bright[0], kTolerance) << "row " << y;
        EXPECT_NEAR(0., dark[1], kTolerance) << "row " << y;
        EXPECT_NEAR(0., dark[2], kTolerance) << "row " << y;
    }
}

INSTANTIATE_TEST_CASE_P(Backends, NatronUtilityReflect, ::testing::Values(Backend::Cpu, Backend::GpuTiles, Backend::GpuFilm));
