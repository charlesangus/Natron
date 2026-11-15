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

#include <pxr/base/tf/diagnosticMgr.h>
#include <pxr/usd/usdGeom/basisCurves.h>
#include <pxr/usd/usdSkel/animation.h>

#include "Global/Macros.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QTemporaryDir>

#include "BaseTest.h"
#include "Engine/AppInstance.h"
#include "Engine/AppManager.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSet.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Invert.h"
#include "Engine/Nodes/Filter/Blur.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/LiveTextureStore.h"
#include "Engine/Nodes/Scene/RenderDelegates.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDAssignMaterial.h"
#include "Engine/Nodes/Scene/USDCamera.h"
#include "Engine/Nodes/Scene/USDCard.h"
#include "Engine/Nodes/Scene/USDEditTransform.h"
#include "Engine/Nodes/Scene/USDEmissiveMaterial.h"
#include "Engine/Nodes/Scene/USDLight.h"
#include "Engine/Nodes/Scene/USDMaterial.h"
#include "Engine/Nodes/Scene/USDMerge.h"
#include "Engine/Nodes/Scene/USDProject3D.h"
#include "Engine/Nodes/Scene/USDPrune.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDRender.h"
#include "Engine/Nodes/Scene/USDRenderSettings.h"
#include "Engine/Nodes/Scene/USDSphere.h"
#include "Engine/Nodes/Scene/USDXformPrim.h"
#include "Engine/Nodes/Transform/Crop.h"
#include "Engine/Nodes/Transform/Transform.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"
#include "RenderBothWays.h"
#include "RenderSettingsTestUtils.h"
#include "SceneTestEffect.h"
#include "Tests/Native/NativeGolden.h"
#include "ofxNatron.h"

NATRON_NAMESPACE_USING

namespace {
const char* const kFixture = NATRON_TESTS_FIXTURES_DIR "/scene-reference.usda";
const int kWidth = 200;
const int kHeight = 100;
const char* const kRenderCamera = "/World/Rig/RenderCam";

// Where the references live under the golden directories: <dir>/USDRender/<case>.f32.
const char* const kGoldenGroup = "USDRender";
const char* const kGoldenCase = "StormReference";
const char* const kGoldenCasePrimitives = "StormPrimitives";
// Crops, not the whole frame: <dir>/USDRender/<case>.exr, half float, zip.
const char* const kGoldenCaseProject3DWorld = "StormProject3DWorld";
const char* const kGoldenCaseProject3DGeometryHeld = "StormProject3DGeometryHeld";
const char* const kGoldenCaseProject3DOverscan = "StormProject3DOverscan";
// A crop, not the whole frame: <dir>/USDRender/StormAssignMaterial.exr, half float, zip.
const char* const kGoldenCaseAssignMaterial = "StormAssignMaterial";

// An asset whose box is bound directly to a grey material of its own.
const char* const kGreyAsset = "#usda 1.0\n"
                               "def Xform \"Asset\"\n{\n"
                               "    def Cube \"Box\" (\n        prepend apiSchemas = [\"MaterialBindingAPI\"]\n    )\n    {\n"
                               "        double size = 2\n"
                               "        rel material:binding = </Asset/Looks/Grey>\n    }\n"
                               "    def Scope \"Looks\"\n    {\n"
                               "        def Material \"Grey\"\n        {\n"
                               "            token outputs:surface.connect = </Asset/Looks/Grey/Surface.outputs:surface>\n"
                               "            def Shader \"Surface\"\n            {\n"
                               "                uniform token info:id = \"UsdPreviewSurface\"\n"
                               "                color3f inputs:diffuseColor = (0.5, 0.5, 0.5)\n"
                               "                token outputs:surface\n            }\n        }\n    }\n}\n";

/**
 * How far the colour may drift from the reference: GL drivers rasterise and shade with their own
 * rounding, and edges land on other pixels. A pixel differs when a channel is off by more than
 * kChannelTolerance; at most kDifferingFraction of them may, and the mean difference stays below
 * kMeanTolerance.
 **/
const float kChannelTolerance = 0.02f;
const double kDifferingFraction = 0.005;
const double kMeanTolerance = 0.002;

/**
 * The projection references were recorded on llvmpipe. Storm's samplers ask for 16x anisotropy, and
 * llvmpipe's anisotropic path blurs some fragment quads across a checker edge even on a card seen
 * square on, which radeonsi does not, so the two legitimately disagree this much within
 * kProjectedEdgeReach plate pixels of an edge. Beyond that they agree within kChannelTolerance, and the unchanged mean bound is what catches a small shift. It stays a tolerance
 * rather than an exclusion because a projection that has moved takes these pixels further off.
 **/
const float kProjectedEdgeTolerance = 0.15f;

// The dev image's /usr/local glvnd lists only NVIDIA; the system directory holds Mesa. Set before
// the first EGL use, as the render thread's context is made once.
void
pointEGLAtMesa()
{
    if (!std::getenv("__EGL_VENDOR_LIBRARY_FILENAMES") && !std::getenv("__EGL_VENDOR_LIBRARY_DIRS") && (::access("/usr/share/glvnd/egl_vendor.d", F_OK) == 0) && (::access("/usr/local/share/glvnd/egl_vendor.d", F_OK) == 0)) {
        ::setenv("__EGL_VENDOR_LIBRARY_DIRS", "/usr/share/glvnd/egl_vendor.d", 1);
    }
}

// True when the test must stop: no 4.5 context could be made, which fails the test only under
// NATRON_TEST_REQUIRE_GL=1.
bool
skipWithoutGL()
{
    pointEGLAtMesa();
    std::string error;
    if (!HydraRender::contextDescription(&error).empty()) {
        return false;
    }
    const char* require = std::getenv("NATRON_TEST_REQUIRE_GL");
    if (require && (std::strcmp(require, "1") == 0)) {
        ADD_FAILURE() << error << " (NATRON_TEST_REQUIRE_GL=1)";

        return true;
    }
    std::cout << "[  SKIPPED ] USDRenderStorm: " << error << std::endl;

    return true;
}

std::uint32_t
bitsOf(float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));

    return bits;
}

// Where a prim landed: its pixel count, centroid (rows counted from the bottom), nearest and
// farthest depth, and mean colour.
struct Coverage {
    int count = 0;
    double x = 0.;
    double y = 0.;
    float minDepth = std::numeric_limits<float>::infinity();
    float maxDepth = -std::numeric_limits<float>::infinity();
    double rgb[3] = { 0., 0., 0. };
};

Coverage
coverageOf(const std::string& path,
           const std::vector<float>& rgba,
           const std::vector<float>& depth,
           const std::vector<float>& primId)
{
    Coverage c;
    const std::uint32_t id = bitsOf(HydraRender::hashToFloat(HydraRender::primPathHash(path)));

    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const std::size_t i = (std::size_t)y * (std::size_t)kWidth + (std::size_t)x;
            if (bitsOf(primId[i]) != id) {
                continue;
            }
            ++c.count;
            c.x += x;
            c.y += y;
            c.minDepth = std::min(c.minDepth, depth[i]);
            c.maxDepth = std::max(c.maxDepth, depth[i]);
            for (int k = 0; k < 3; ++k) {
                c.rgb[k] += rgba[i * 4 + k];
            }
        }
    }
    if (c.count) {
        c.x /= c.count;
        c.y /= c.count;
        for (int k = 0; k < 3; ++k) {
            c.rgb[k] /= c.count;
        }
    }

    return c;
}

// The pixels of rgba more than half covered, alone in a scene on a transparent background. Unlike
// coverageOf it holds for a surface with an opacity, which Storm draws in its translucent pass,
// where no prim id is written.
Coverage
coverageByAlpha(const std::vector<float>& rgba)
{
    Coverage c;

    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const std::size_t i = (std::size_t)y * (std::size_t)kWidth + (std::size_t)x;
            if (rgba[i * 4 + 3] <= 0.5f) {
                continue;
            }
            ++c.count;
            c.x += x;
            c.y += y;
            for (int k = 0; k < 3; ++k) {
                c.rgb[k] += rgba[i * 4 + k];
            }
        }
    }
    if (c.count) {
        c.x /= c.count;
        c.y /= c.count;
        for (int k = 0; k < 3; ++k) {
            c.rgb[k] /= c.count;
        }
    }

    return c;
}

// Records the colour plane under NATRON_GOLDEN_RECORD_DIR, or compares it with the committed one
// within the tolerances above.
void
checkAgainstReference(const RenderedPlane& rgba,
                      const char* goldenCase)
{
    const ImageLayerDesc layer = ImageLayerDesc::getRGBAComponents();
    const std::string recordDir = goldenRecordDir();
    if (!recordDir.empty()) {
        ParityF32Image image;
        image.width = rgba.window.width();
        image.height = rgba.window.height();
        image.nComps = (int)rgba.channels.size();
        image.pixels = rgba.pixels;
        const std::string path = parityReferencePath(recordDir, kGoldenGroup, goldenCase, 0, layer);
        std::string error;
        EXPECT_TRUE(writeParityF32(path, image, &error)) << goldenCase << ": cannot record: " << error;
        std::cout << "[ golden ] recorded " << path << std::endl;

        return;
    }

    const std::string refDir = goldenReferenceDir();
    const std::string path = parityReferencePath(refDir, kGoldenGroup, goldenCase, 0, layer);
    ParityF32Image reference;
    std::string error;
    if (!readParityF32(path, &reference, &error)) {
        ADD_FAILURE() << goldenCase << ": no golden reference " << path << " (" << error << "). Record it from a build that renders the scene "
                      << "correctly by running this test with NATRON_GOLDEN_RECORD_DIR=" << refDir << " and commit the file.";

        return;
    }
    ASSERT_EQ(rgba.window.width(), reference.width) << path;
    ASSERT_EQ(rgba.window.height(), reference.height) << path;
    ASSERT_EQ((int)rgba.channels.size(), reference.nComps) << path;
    ASSERT_EQ(rgba.pixels.size(), reference.pixels.size()) << path;

    const int nComps = reference.nComps;
    const std::size_t pixels = reference.pixels.size() / (std::size_t)nComps;
    std::size_t differing = 0;
    double sum = 0.;
    float worst = 0.f;
    std::size_t worstPixel = 0;
    for (std::size_t p = 0; p < pixels; ++p) {
        bool differs = false;
        for (int k = 0; k < nComps; ++k) {
            const float a = rgba.pixels[p * nComps + k];
            const float b = reference.pixels[p * nComps + k];
            if (!std::isfinite(a) || !std::isfinite(b)) {
                differs = differs || (std::isfinite(a) != std::isfinite(b));
                continue;
            }
            const float d = std::fabs(a - b);
            sum += d;
            if (d > kChannelTolerance) {
                differs = true;
            }
            if (d > worst) {
                worst = d;
                worstPixel = p;
            }
        }
        if (differs) {
            ++differing;
        }
    }
    const double mean = sum / (double)reference.pixels.size();
    const std::string where = "worst difference " + std::to_string(worst) + " at (" + std::to_string(worstPixel % (std::size_t)reference.width) + ", "
        + std::to_string(worstPixel / (std::size_t)reference.width) + ")";
    EXPECT_LE((double)differing, kDifferingFraction * (double)pixels) << differing << " of " << pixels << " pixels differ from " << path << "; " << where;
    EXPECT_LE(mean, kMeanTolerance) << "mean difference from " << path << "; " << where;
} // checkAgainstReference

// Records the crop of the colour plane under NATRON_GOLDEN_RECORD_DIR as a half-float, zip
// compressed EXR, or compares it with the committed one within the tolerances above. Rows are
// kept in the plane's order, bottom first, both ways.
void
checkCropAgainstReference(const RenderedPlane& rgba,
                          const RectI& crop,
                          const char* goldenCase,
                          const std::function<bool(int, int)>& nearProjectedEdge = nullptr)
{
    const int nComps = (int)rgba.channels.size();
    const int width = crop.width();
    const int height = crop.height();
    ASSERT_EQ(4, nComps);
    ASSERT_TRUE((crop.x1 >= rgba.window.x1) && (crop.y1 >= rgba.window.y1) && (crop.x2 <= rgba.window.x2) && (crop.y2 <= rgba.window.y2));
    ASSERT_LE(width, 256);
    ASSERT_LE(height, 256);
    std::vector<float> pixels;
    pixels.reserve((std::size_t)width * (std::size_t)height * (std::size_t)nComps);
    for (int y = crop.y1; y < crop.y2; ++y) {
        const std::size_t row = (std::size_t)(y - rgba.window.y1) * (std::size_t)rgba.window.width();
        for (int x = crop.x1; x < crop.x2; ++x) {
            const std::size_t i = (row + (std::size_t)(x - rgba.window.x1)) * (std::size_t)nComps;
            pixels.insert(pixels.end(), rgba.pixels.begin() + i, rgba.pixels.begin() + i + nComps);
        }
    }

    const std::string name = std::string(kGoldenGroup) + "/" + goldenCase + ".exr";
    const std::string recordDir = goldenRecordDir();
    if (!recordDir.empty()) {
        const QString path = QDir(QString::fromStdString(recordDir)).filePath(QString::fromStdString(name));
        ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath())) << path.toStdString();
        std::unique_ptr<OIIO::ImageOutput> out = OIIO::ImageOutput::create(path.toStdString());
        ASSERT_TRUE(out != nullptr) << OIIO::geterror();
        OIIO::ImageSpec spec(width, height, nComps, OIIO::TypeDesc::HALF);
        spec.attribute("compression", "zip");
        ASSERT_TRUE(out->open(path.toStdString(), spec)) << out->geterror();
        EXPECT_TRUE(out->write_image(OIIO::TypeDesc::FLOAT, pixels.data())) << out->geterror();
        EXPECT_TRUE(out->close()) << out->geterror();
        std::cout << "[ golden ] recorded " << path.toStdString() << std::endl;

        return;
    }

    const std::string path = goldenReferenceDir() + "/" + name;
    std::unique_ptr<OIIO::ImageInput> in = OIIO::ImageInput::open(path);
    if (!in) {
        ADD_FAILURE() << goldenCase << ": no golden reference " << path << " (" << OIIO::geterror() << "). Record it from a build that renders the scene "
                      << "with NATRON_GOLDEN_RECORD_DIR set.";

        return;
    }
    const OIIO::ImageSpec& spec = in->spec();
    ASSERT_EQ(width, spec.width) << path;
    ASSERT_EQ(height, spec.height) << path;
    ASSERT_EQ(nComps, spec.nchannels) << path;
    std::vector<float> reference(pixels.size());
    ASSERT_TRUE(in->read_image(0, 0, 0, nComps, OIIO::TypeDesc::FLOAT, reference.data())) << in->geterror();
    in->close();

    std::size_t differing = 0;
    double sum = 0.;
    float worst = 0.;
    for (std::size_t p = 0; p < pixels.size() / (std::size_t)nComps; ++p) {
        const int x = crop.x1 + (int)(p % (std::size_t)width);
        const int y = crop.y1 + (int)(p / (std::size_t)width);
        const float tolerance = (nearProjectedEdge && nearProjectedEdge(x, y)) ? kProjectedEdgeTolerance : kChannelTolerance;
        bool differs = false;
        for (int k = 0; k < nComps; ++k) {
            const float d = std::fabs(pixels[p * nComps + k] - reference[p * nComps + k]);
            sum += d;
            worst = std::max(worst, d);
            differs = differs || (d > tolerance);
        }
        differing += differs ? 1 : 0;
    }
    EXPECT_LE((double)differing, kDifferingFraction * (double)(pixels.size() / (std::size_t)nComps)) << goldenCase << ": worst difference " << worst;
    EXPECT_LT(sum / (double)pixels.size(), kMeanTolerance) << goldenCase << ": worst difference " << worst;
} // checkCropAgainstReference

/**
 * Collects what USD reports while it is installed: Storm reports a shader that fails to compile or
 * link, or a glslfx it cannot read, as a warning or an error and draws the fallback surface
 * instead, so the render itself succeeds.
 **/
class USDDiagnostics
    : public PXR_NS::TfDiagnosticMgr::Delegate {
public:
    USDDiagnostics()
    {
        PXR_NS::TfDiagnosticMgr::GetInstance().AddDelegate(this);
    }

    virtual ~USDDiagnostics()
    {
        PXR_NS::TfDiagnosticMgr::GetInstance().RemoveDelegate(this);
    }

    virtual void IssueStatus(const PXR_NS::TfStatus& /*status*/) OVERRIDE FINAL
    {
    }

    virtual void IssueWarning(const PXR_NS::TfWarning& warning) OVERRIDE FINAL
    {
        record("warning", warning.GetSourceFunction(), warning.GetCommentary());
    }

    virtual void IssueError(const PXR_NS::TfError& error) OVERRIDE FINAL
    {
        record("error", error.GetSourceFunction(), error.GetCommentary());
    }

    virtual void IssueFatalError(const PXR_NS::TfCallContext& /*context*/,
                                 const std::string& message) OVERRIDE FINAL
    {
        std::cerr << "USD fatal error: " << message << std::endl;
        _UnhandledAbort();
    }

    // Every report naming a shader, a glslfx, or a compile or link, one per line; all reports are
    // printed as they come.
    std::string shaderProblems() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        std::string problems;
        for (const std::string& message : _messages) {
            std::string lower = message;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            if ((lower.find("shader") != std::string::npos) || (lower.find("glslfx") != std::string::npos) || (lower.find("compil") != std::string::npos)
                || (lower.find("link") != std::string::npos)) {
                problems += message + "\n";
            }
        }

        return problems;
    }

private:
    void record(const char* kind,
                const std::string& function,
                const std::string& commentary)
    {
        const std::string message = std::string("USD ") + kind + " in " + function + ": " + commentary;
        std::cout << message << std::endl;
        std::lock_guard<std::mutex> lock(_mutex);
        _messages.push_back(message);
    }

    mutable std::mutex _mutex;
    std::vector<std::string> _messages;
};

/**
 * The projection scenes: the render camera at (0, 0, 10) and the projector at (0, 0, 5), both
 * looking down -Z with a 2:1 frame as wide as twice their distance, so the render's 200 x 100
 * pixels span 20 x 10 units on the z = 0 plane and the projector's frame 10 x 5 there. The plate is
 * a 200 x 100 checker of 25 pixel squares centred on its format, red where the squares' indices
 * sum to an even number and blue elsewhere; the card, 16 x 8 units, is unlit and Crop leaves it
 * transparent black around the frame.
 **/
const double kRenderCameraZ = 10.;
const double kProjectorZ = 5.;
const double kPlateBox = 25.;
const double kCardWidth = 16.;
const double kCardHeight = 8.;
// The rigid and reference point paths round the plate's coordinates differently, and the texture level
// chosen from those coordinates' derivatives turns that into up to 0.071 along the checker's edges. A
// slide of an eighth of a render pixel already reaches 0.16.
const float kReferencePathTolerance = 0.08f;
const double kProjectedEdgeReach = 2.5;

// The z = 0 point render pixel (x, y)'s centre sees, rows counted from the bottom.
void
renderPixelToPlane(int x,
                   int y,
                   double* X,
                   double* Y)
{
    const double unitsPerPixel = 2. * kRenderCameraZ / kWidth;

    *X = (x + 0.5 - kWidth / 2.) * unitsPerPixel;
    *Y = (y + 0.5 - kHeight / 2.) * unitsPerPixel;
}

// Where the projector puts the z = 0 point (X, Y) on the plate, in the format's pixels.
void
planeToPlate(double X,
             double Y,
             double* u,
             double* v)
{
    const double pixelsPerUnit = kWidth / (2. * kProjectorZ);

    *u = kWidth / 2. + X * pixelsPerUnit;
    *v = kHeight / 2. + Y * pixelsPerUnit;
}

enum ProjectedColourEnum {
    eProjectedUnsure = 0,
    eProjectedRed,
    eProjectedBlue,
    eProjectedTransparent
};

// What the projection shows at plate pixel (u, v) with Crop on: unsure within margin of an edge.
ProjectedColourEnum
expectedProjection(double u,
                   double v,
                   double margin)
{
    const bool outside = (u < -margin) || (u > kWidth + margin) || (v < -margin) || (v > kHeight + margin);
    const bool inside = (u > margin) && (u < kWidth - margin) && (v > margin) && (v < kHeight - margin);
    if (outside) {
        return eProjectedTransparent;
    }
    if (!inside) {
        return eProjectedUnsure;
    }
    const double bu = (u - kWidth / 2.) / kPlateBox;
    const double bv = (v - kHeight / 2.) / kPlateBox;
    const double edge = margin / kPlateBox;
    if ((std::fabs(bu - std::round(bu)) < edge) || (std::fabs(bv - std::round(bv)) < edge)) {
        return eProjectedUnsure;
    }
    const long sum = (long)std::floor(bu) + (long)std::floor(bv);

    return (sum % 2 == 0) ? eProjectedRed : eProjectedBlue;
}

// The format's edges are checker edges too, as it holds a whole number of squares each side of its
// centre; beyond the format nothing projects, so bleed there stays under the strict tolerance.
bool
isNearProjectedEdge(int x,
                    int y,
                    double cardShift)
{
    double X = 0., Y = 0., u = 0., v = 0.;
    renderPixelToPlane(x, y, &X, &Y);
    planeToPlate(X - cardShift, Y, &u, &v);
    if ((u < 0.) || (u > kWidth) || (v < 0.) || (v > kHeight)) {
        return false;
    }
    const double bu = (u - kWidth / 2.) / kPlateBox;
    const double bv = (v - kHeight / 2.) / kPlateBox;

    return std::min(std::fabs(bu - std::round(bu)), std::fabs(bv - std::round(bv))) * kPlateBox <= kProjectedEdgeReach;
}

ProjectedColourEnum
classifyColour(const float* rgba)
{
    const float r = rgba[0], g = rgba[1], b = rgba[2];
    if ((r > 0.1f) && (r > 4.f * g) && (r > 4.f * b)) {
        return eProjectedRed;
    }
    if ((b > 0.1f) && (b > 4.f * r) && (b > 4.f * g)) {
        return eProjectedBlue;
    }
    if (std::max(std::max(r, g), std::max(b, rgba[3])) < 0.02f) {
        return eProjectedTransparent;
    }

    return eProjectedUnsure;
}

const char*
colourName(ProjectedColourEnum c)
{
    switch (c) {
    case eProjectedRed:
        return "red";
    case eProjectedBlue:
        return "blue";
    case eProjectedTransparent:
        return "transparent black";
    case eProjectedUnsure:
        break;
    }

    return "neither red, blue nor transparent black";
}

// What the render shows over render pixels [x1, x2) x [y1, y2), rows counted from the bottom: the
// largest of any channel and how many pixels are red, blue and fully opaque.
struct RegionColours {
    float maximum = 0.f;
    int red = 0;
    int blue = 0;
    int opaque = 0;
    int count = 0;
};

RegionColours
regionColours(const std::vector<float>& rgba,
              int x1,
              int y1,
              int x2,
              int y2)
{
    RegionColours region;
    for (int y = y1; y < y2; ++y) {
        for (int x = x1; x < x2; ++x) {
            const std::size_t i = ((std::size_t)y * (std::size_t)kWidth + (std::size_t)x) * 4;
            for (int c = 0; c < 4; ++c) {
                region.maximum = std::max(region.maximum, rgba[i + c]);
            }
            const ProjectedColourEnum colour = classifyColour(&rgba[i]);
            region.red += (colour == eProjectedRed) ? 1 : 0;
            region.blue += (colour == eProjectedBlue) ? 1 : 0;
            region.opaque += (rgba[i + 3] > 0.98f) ? 1 : 0;
            ++region.count;
        }
    }

    return region;
}

/**
 * Checks every card pixel away from the card's, the frame's and the squares' edges against where the
 * projection puts the checker, the card having moved by cardShift along X since the projection was
 * set up (0 in World space or without movement).
 **/
void
checkProjectedChecker(const std::vector<RenderedPlane>& planes,
                      double cardShift,
                      const char* what)
{
    ASSERT_EQ(3u, planes.size()) << what;
    const std::vector<float>& rgba = planes[0].pixels;
    // The card by its depth, not its primId: the projection is translucent, and Storm's translucent
    // pass writes no primId, while its opaque fragments still write depth.
    const std::vector<float>& depth = planes[1].pixels;
    ASSERT_EQ((std::size_t)kWidth * kHeight * 4, rgba.size()) << what;
    ASSERT_EQ((std::size_t)kWidth * kHeight, depth.size()) << what;
    // Two render pixels: rasterisation and texture filtering stay within that of an edge.
    const double margin = 4.;
    const double cardMargin = 0.5;

    int counts[4] = { 0, 0, 0, 0 };
    int checked = 0;
    int wrong = 0;
    double cardRgb[3] = { 0., 0., 0. };
    int cardPixels = 0;
    std::string firstWrong;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            double X = 0., Y = 0.;
            renderPixelToPlane(x, y, &X, &Y);
            const double localX = X - cardShift;
            if ((std::fabs(localX) > kCardWidth / 2. - cardMargin) || (std::fabs(Y) > kCardHeight / 2. - cardMargin)) {
                continue;
            }
            const std::size_t i = (std::size_t)y * (std::size_t)kWidth + (std::size_t)x;
            double u = 0., v = 0.;
            planeToPlate(localX, Y, &u, &v);
            const ProjectedColourEnum expected = expectedProjection(u, v, margin);
            // Storm's translucent pass writes depth only where the projection is opaque. Within the
            // margin of the frame's edge, filtering may leave a pixel partly transparent, or
            // transparent, either way.
            if (!std::isfinite(depth[i])) {
                if (expected == eProjectedUnsure) {
                    continue;
                }
                if ((expected == eProjectedTransparent) && (classifyColour(&rgba[i * 4]) == eProjectedTransparent)) {
                    ++checked;
                    ++counts[eProjectedTransparent];
                    continue;
                }
                ++wrong;
                if (firstWrong.empty()) {
                    firstWrong = "(" + std::to_string(x) + ", " + std::to_string(y) + ") is not the card";
                }
                continue;
            }
            for (int k = 0; k < 3; ++k) {
                cardRgb[k] += rgba[i * 4 + k];
            }
            ++cardPixels;
            if (expected == eProjectedUnsure) {
                continue;
            }
            ++checked;
            const ProjectedColourEnum got = classifyColour(&rgba[i * 4]);
            if (got == expected) {
                ++counts[expected];
                continue;
            }
            ++wrong;
            if (firstWrong.empty()) {
                firstWrong = "(" + std::to_string(x) + ", " + std::to_string(y) + ") on plate (" + std::to_string(u) + ", " + std::to_string(v) + ") is "
                    + colourName(got) + " (" + std::to_string(rgba[i * 4]) + ", " + std::to_string(rgba[i * 4 + 1]) + ", " + std::to_string(rgba[i * 4 + 2])
                    + ", " + std::to_string(rgba[i * 4 + 3]) + "), not " + colourName(expected);
            }
        }
    }
    ASSERT_GT(cardPixels, 0) << what << ": the card is not in the render";
    // Storm draws its fallback surface, lit grey, when the projection shader does not compile.
    EXPECT_GT(counts[eProjectedRed] + counts[eProjectedBlue], 0) << what << ": no checker on the card, whose mean colour is (" << cardRgb[0] / cardPixels << ", "
                                                                 << cardRgb[1] / cardPixels << ", " << cardRgb[2] / cardPixels
                                                                 << "): the projection shader did not run";
    // The frame spans 100 x 50 render pixels and the card around it 160 x 80.
    EXPECT_GT(counts[eProjectedRed], 800) << what;
    EXPECT_GT(counts[eProjectedBlue], 800) << what;
    EXPECT_GT(counts[eProjectedTransparent], 2000) << what;
    EXPECT_LE(wrong, checked / 200) << what << ": " << wrong << " of " << checked << " checked pixels are wrong, first " << firstWrong;
} // checkProjectedChecker

class USDRenderStormTest
    : public BaseTest {
protected:
    virtual void TearDown() OVERRIDE
    {
        SceneQueryPool::instance().releaseIdle();
        BaseTest::TearDown();
    }

    template <typename KNOB>
    static KNOB* knob(const NodePtr& node,
                      const char* name)
    {
        KNOB* k = dynamic_cast<KNOB*>(node->getKnobByName(name).get());
        EXPECT_TRUE(k != nullptr) << node->getPluginID() << " has no knob " << name;

        return k;
    }

    static bool setString(const NodePtr& node,
                          const char* name,
                          const char* value)
    {
        KnobString* k = knob<KnobString>(node, name);
        if (k) {
            k->setValue(std::string(value));
        }

        return k != nullptr;
    }

    static bool setTriple(const NodePtr& node,
                          const char* name,
                          double x,
                          double y,
                          double z)
    {
        KnobDouble* k = knob<KnobDouble>(node, name);
        if (k) {
            k->setValue(x, ViewSpec::all(), 0);
            k->setValue(y, ViewSpec::all(), 1);
            k->setValue(z, ViewSpec::all(), 2);
        }

        return k != nullptr;
    }

    /**
     * The reference scene, laid out by the create and edit nodes over the fixture's box and ball:
     * the ball lifted by 1, the box given a green material by a geometry stand-in defining the same
     * cube at its path and carrying the material, the fixture's camera pruned, and a rig
     * holding the camera rendered through, two units further back than the pruned one, and a key
     * light. Returns the USDRender, kWidth x kHeight, null after a gtest failure.
     **/
    NodePtr createReferenceGraph()
    {
        NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
        NodePtr transform = createNode(QString::fromUtf8(PLUGINID_NATRON_USDEDITTRANSFORM));
        NodePtr material = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMATERIAL));
        NodePtr box = createNode(QString::fromUtf8(kTestPluginIDSceneGeometry));
        NodePtr rig = createNode(QString::fromUtf8(PLUGINID_NATRON_USDXFORMPRIM));
        NodePtr camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
        NodePtr light = createNode(QString::fromUtf8(PLUGINID_NATRON_USDLIGHT));
        NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
        NodePtr prune = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPRUNE));
        NodePtr render = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
        if (!reader || !transform || !material || !box || !rig || !camera || !light || !merge || !prune || !render) {
            ADD_FAILURE() << "a 3D node is not registered";

            return NodePtr();
        }

        KnobFile* file = knob<KnobFile>(reader, kUSDReadParamFile);
        KnobColor* diffuse = knob<KnobColor>(material, kUSDMaterialParamDiffuseColor);
        if (!file || !diffuse) {
            return NodePtr();
        }
        file->setValue(std::string(kFixture));

        bool ok = true;
        ok = ok && setString(transform, kUSDEditTransformParamPrimPattern, "/World/Ball");
        ok = ok && setTriple(transform, kSceneParamTranslate, 0., 1., 0.);

        ok = ok && setString(material, kSceneParamPrimPath, "/Green");
        ok = ok && setString(box, kSceneParamPrimPath, "/World/Box");
        diffuse->setValue(0.1, ViewSpec::all(), 0);
        diffuse->setValue(0.8, ViewSpec::all(), 1);
        diffuse->setValue(0.2, ViewSpec::all(), 2);

        ok = ok && setString(rig, kSceneParamPrimPath, "/World/Rig");
        ok = ok && setTriple(rig, kSceneParamTranslate, 1., 1., 0.);
        ok = ok && setString(camera, kSceneParamPrimPath, "/RenderCam");
        ok = ok && setTriple(camera, kSceneParamTranslate, 0., 0., 12.);
        ok = ok && setString(light, kSceneParamPrimPath, "/Key");

        ok = ok && setString(prune, kUSDPruneParamPrimPattern, "/World/Cam");
        if (!ok) {
            return NodePtr();
        }

        connectNodes(reader, transform, 0, true);
        connectNodes(material, box, 0, true);
        connectNodes(camera, rig, 0, true);
        connectNodes(light, rig, 1, true);
        connectNodes(box, merge, 0, true);
        connectNodes(transform, merge, 1, true);
        connectNodes(rig, merge, 2, true);
        connectNodes(merge, prune, 0, true);
        // The render settings prim frames the camera at kWidth x kHeight.
        NodePtr settings = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDERSETTINGS));
        if (!settings) {
            ADD_FAILURE() << "USDRenderSettings is not registered";

            return NodePtr();
        }
        connectNodes(prune, settings, 0, true);
        connectNodes(settings, render, 0, true);
        if (!RenderSettingsTestUtils::configure(settings, kRenderCamera, kWidth, kHeight)) {
            return NodePtr();
        }

        return render;
    } // createReferenceGraph

    /**
     * Primitives as a user lays them out: a card and a sphere, each placed by a USDXformPrim, merged
     * with a camera and a light. The unit card, scaled to 2 x 1, stands at (-1.5, 0, 0), the sphere of radius 1,
     * bound to a red material through its Material input, at (1.5, 0, 0), both facing the camera
     * at (0, 0, 10) looking down -Z; the distant light shines down -Z. Returns the USDRender,
     * kWidth x kHeight, null after a gtest failure.
     **/
    NodePtr createPrimitivesGraph()
    {
        NodePtr card = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
        NodePtr left = createNode(QString::fromUtf8(PLUGINID_NATRON_USDXFORMPRIM));
        NodePtr material = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMATERIAL));
        NodePtr sphere = createNode(QString::fromUtf8(PLUGINID_NATRON_USDSPHERE));
        NodePtr right = createNode(QString::fromUtf8(PLUGINID_NATRON_USDXFORMPRIM));
        NodePtr camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
        NodePtr light = createNode(QString::fromUtf8(PLUGINID_NATRON_USDLIGHT));
        NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
        NodePtr render = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
        if (!card || !left || !material || !sphere || !right || !camera || !light || !merge || !render) {
            ADD_FAILURE() << "a 3D node is not registered";

            return NodePtr();
        }

        KnobColor* diffuse = knob<KnobColor>(material, kUSDMaterialParamDiffuseColor);
        if (!diffuse) {
            return NodePtr();
        }
        diffuse->setValue(0.8, ViewSpec::all(), 0);
        diffuse->setValue(0.1, ViewSpec::all(), 1);
        diffuse->setValue(0.1, ViewSpec::all(), 2);

        bool ok = true;
        ok = ok && setString(card, kSceneParamPrimPath, "/Card");
        ok = ok && setTriple(card, kSceneParamScale, 2., 1., 1.);
        ok = ok && setString(left, kSceneParamPrimPath, "/Left");
        ok = ok && setTriple(left, kSceneParamTranslate, -1.5, 0., 0.);
        ok = ok && setString(material, kSceneParamPrimPath, "/Red");
        ok = ok && setString(sphere, kSceneParamPrimPath, "/Ball");
        ok = ok && setString(right, kSceneParamPrimPath, "/Right");
        ok = ok && setTriple(right, kSceneParamTranslate, 1.5, 0., 0.);
        ok = ok && setString(camera, kSceneParamPrimPath, "/Cam");
        ok = ok && setTriple(camera, kSceneParamTranslate, 0., 0., 10.);
        ok = ok && setString(light, kSceneParamPrimPath, "/Key");
        if (!ok) {
            return NodePtr();
        }

        connectNodes(card, left, 0, true);
        connectNodes(material, sphere, 0, true);
        connectNodes(sphere, right, 0, true);
        connectNodes(left, merge, 0, true);
        connectNodes(right, merge, 1, true);
        connectNodes(camera, merge, 2, true);
        connectNodes(light, merge, 3, true);
        // The render settings prim frames the camera at kWidth x kHeight.
        NodePtr settings = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDERSETTINGS));
        if (!settings) {
            ADD_FAILURE() << "USDRenderSettings is not registered";

            return NodePtr();
        }
        connectNodes(merge, settings, 0, true);
        connectNodes(settings, render, 0, true);
        if (!RenderSettingsTestUtils::configure(settings, "/Cam", kWidth, kHeight)) {
            return NodePtr();
        }

        return render;
    } // createPrimitivesGraph

    /**
     * A card at the origin facing the camera at (0, 0, 10), its material's Diffuse Color input
     * fed by a Constant, lit head on by a key light unless withLight is false, when the scene has
     * no light. Sets *constant to the Constant and returns the USDRender, kWidth x kHeight, null
     * after a gtest failure.
     **/
    // The constant feeds the diffuse input of a USDMaterial, or with emissive the one input of a
    // USDEmissiveMaterial.
    NodePtr createLiveTextureGraph(NodePtr* constant,
                                   bool withLight = true,
                                   bool emissive = false)
    {
        NodePtr source = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
        NodePtr material = createNode(QString::fromUtf8(emissive ? PLUGINID_NATRON_USDEMISSIVEMATERIAL : PLUGINID_NATRON_USDMATERIAL));
        NodePtr card = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
        NodePtr camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
        NodePtr light = withLight ? createNode(QString::fromUtf8(PLUGINID_NATRON_USDLIGHT)) : NodePtr();
        NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
        NodePtr render = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
        if (!source || !material || !card || !camera || (withLight && !light) || !merge || !render) {
            ADD_FAILURE() << "a node is not registered";

            return NodePtr();
        }
        bool ok = true;
        ok = ok && setString(material, kSceneParamPrimPath, "/Live");
        ok = ok && setString(card, kSceneParamPrimPath, "/Card");
        ok = ok && setString(camera, kSceneParamPrimPath, "/Cam");
        ok = ok && setTriple(camera, kSceneParamTranslate, 0., 0., 10.);
        ok = ok && (!light || setString(light, kSceneParamPrimPath, "/Key"));
        if (!ok) {
            return NodePtr();
        }
        connectNodes(source, material, emissive ? kUSDEmissiveMaterialInputEmissive : kUSDMaterialInputDiffuseColor, true);
        connectNodes(material, card, 0, true);
        connectNodes(card, merge, 0, true);
        connectNodes(camera, merge, 1, true);
        if (light) {
            connectNodes(light, merge, 2, true);
        }
        // The render settings prim frames the camera at kWidth x kHeight.
        NodePtr settings = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDERSETTINGS));
        if (!settings) {
            ADD_FAILURE() << "USDRenderSettings is not registered";

            return NodePtr();
        }
        connectNodes(merge, settings, 0, true);
        connectNodes(settings, render, 0, true);
        if (!RenderSettingsTestUtils::configure(settings, "/Cam", kWidth, kHeight)) {
            return NodePtr();
        }
        *constant = source;

        return render;
    } // createLiveTextureGraph

    // The card's mean colour in render at frame, through the live texture graph. Rendered off the
    // main thread, where renders run outside the tests: LiveTextureStore::prepare does nothing on
    // the main thread, and the textures cannot be prepared beforehand because the render empties
    // the caches, the store included.
    static Coverage renderCard(const NodePtr& render,
                               double frame)
    {
        return coverageByAlpha(renderColour(render, frame));
    }

    // The colour plane of render at frame, rendered off the main thread as renderCard explains.
    static std::vector<float> renderColour(const NodePtr& render,
                                           double frame)
    {
        std::vector<RenderedPlane> planes;
        std::string error;
        const bool rendered = renderOffMainThread(render, frame, &planes, &error);

        EXPECT_TRUE(rendered) << error;
        if (planes.size() != 3u) {
            ADD_FAILURE() << "frame " << frame << " rendered " << planes.size() << " planes";

            return std::vector<float>((std::size_t)kWidth * (std::size_t)kHeight * 4u, 0.f);
        }

        return planes[0].pixels;
    }

    // The mean colour and alpha of the 5 by 5 pixels at the centre of rgba.
    static void centreOf(const std::vector<float>& rgba,
                         double out[4])
    {
        for (int k = 0; k < 4; ++k) {
            out[k] = 0.;
        }
        for (int y = kHeight / 2 - 2; y <= kHeight / 2 + 2; ++y) {
            for (int x = kWidth / 2 - 2; x <= kWidth / 2 + 2; ++x) {
                const std::size_t i = (std::size_t)y * (std::size_t)kWidth + (std::size_t)x;
                for (int k = 0; k < 4; ++k) {
                    out[k] += rgba[i * 4 + k] / 25.;
                }
            }
        }
    }

    // Every plane of render at frame, rendered off the main thread as renderCard does.
    // clearAllCaches() belongs to the main thread, and the harness's cache-bypassing render would
    // run it on the worker. Emptying the caches here on both sides starts the render just as cold.
    static bool renderOffMainThread(const NodePtr& render,
                                    double frame,
                                    std::vector<RenderedPlane>* planes,
                                    std::string* error)
    {
        bool rendered = false;

        appPTR->clearAllCaches();
        std::thread worker([&]() {
            rendered = renderNodePlanesDirect(render, frame, ViewIdx(0), 0, RectI(0, 0, kWidth, kHeight), allPlanes(), planes, error, false /*byPassCache*/);
        });
        worker.join();
        appPTR->clearAllCaches();

        return rendered;
    }

    /**
     * The projection scene described above kRenderCameraZ: a CheckerBoard projected by a
     * USDProject3D through a camera at (0, 0, kProjectorZ), the material of a 16 x 8 USDCard at
     * /Card, rendered through /Cam at (0, 0, kRenderCameraZ). The project format, and so the plate's,
     * is kWidth x kHeight. Sets *plate and *projector and returns the USDRender, null after a gtest
     * failure.
     **/
    NodePtr createProjectionGraph(NodePtr* plate,
                                  NodePtr* projector,
                                  NodePtr* card)
    {
        ProjectPtr project = getApp()->getProject();
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kWidth, kHeight, "projection2to1", 1.));

        NodePtr checker = createNode(QString::fromUtf8(PLUGINID_NATRON_CHECKERBOARD), PLUGIN_MAJOR_NATRON_CHECKERBOARD);
        NodePtr projectorCamera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
        NodePtr project3D = createNode(QString::fromUtf8(PLUGINID_NATRON_USDPROJECT3D));
        NodePtr geometry = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
        NodePtr camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
        NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
        NodePtr render = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
        if (!checker || !projectorCamera || !project3D || !geometry || !camera || !merge || !render) {
            ADD_FAILURE() << "a node is not registered";

            return NodePtr();
        }

        KnobDouble* boxSize = knob<KnobDouble>(checker, kCheckerBoardParamBoxSize);
        KnobDouble* centerlineWidth = knob<KnobDouble>(checker, kCheckerBoardParamCenterLineWidth);
        KnobDouble* lineWidth = knob<KnobDouble>(checker, kCheckerBoardParamLineWidth);
        if (!boxSize || !centerlineWidth || !lineWidth) {
            return NodePtr();
        }
        boxSize->setValue(kPlateBox, ViewSpec::all(), 0);
        boxSize->setValue(kPlateBox, ViewSpec::all(), 1);
        centerlineWidth->setValue(0.);
        lineWidth->setValue(0.);
        const char* const colours[4] = { kCheckerBoardParamColor0, kCheckerBoardParamColor1, kCheckerBoardParamColor2, kCheckerBoardParamColor3 };
        for (int c = 0; c < 4; ++c) {
            KnobColor* colour = knob<KnobColor>(checker, colours[c]);
            if (!colour) {
                return NodePtr();
            }
            // Colours 0 and 2 fill the squares whose indices sum to an even number.
            const bool red = (c % 2) == 0;
            colour->setValue(red ? 0.9 : 0.05, ViewSpec::all(), 0);
            colour->setValue(0.05, ViewSpec::all(), 1);
            colour->setValue(red ? 0.05 : 0.9, ViewSpec::all(), 2);
            colour->setValue(1., ViewSpec::all(), 3);
        }

        bool ok = true;
        for (const NodePtr& cam : { projectorCamera, camera }) {
            KnobDouble* focal = knob<KnobDouble>(cam, kUSDCameraParamFocalLength);
            KnobDouble* horizontal = knob<KnobDouble>(cam, kUSDCameraParamHorizontalAperture);
            KnobDouble* vertical = knob<KnobDouble>(cam, kUSDCameraParamVerticalAperture);
            if (!focal || !horizontal || !vertical) {
                return NodePtr();
            }
            // The frame is as wide as twice the distance.
            focal->setValue(18.);
            horizontal->setValue(36.);
            vertical->setValue(18.);
        }
        ok = ok && setString(projectorCamera, kSceneParamPrimPath, "/Projector");
        ok = ok && setTriple(projectorCamera, kSceneParamTranslate, 0., 0., kProjectorZ);
        ok = ok && setString(geometry, kSceneParamPrimPath, "/Card");
        ok = ok && setTriple(geometry, kSceneParamScale, kCardWidth, kCardHeight, 1.);
        ok = ok && setString(camera, kSceneParamPrimPath, "/Cam");
        ok = ok && setTriple(camera, kSceneParamTranslate, 0., 0., kRenderCameraZ);
        if (!ok) {
            return NodePtr();
        }

        connectNodes(checker, project3D, kUSDProject3DInputImage, true);
        connectNodes(projectorCamera, project3D, kUSDProject3DInputCamera, true);
        connectNodes(project3D, geometry, 0, true);
        connectNodes(geometry, merge, 0, true);
        connectNodes(camera, merge, 1, true);
        // The render settings prim frames the camera at kWidth x kHeight.
        NodePtr settings = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDERSETTINGS));
        if (!settings) {
            ADD_FAILURE() << "USDRenderSettings is not registered";

            return NodePtr();
        }
        connectNodes(merge, settings, 0, true);
        connectNodes(settings, render, 0, true);
        if (!RenderSettingsTestUtils::configure(settings, "/Cam", kWidth, kHeight)) {
            return NodePtr();
        }
        *plate = checker;
        *projector = project3D;
        *card = geometry;

        return render;
    } // createProjectionGraph

    // Moves the projection off the card's Material input onto a USDAssignMaterial naming the card,
    // between the card and the merge.
    NodePtr assignProjectionToCard(const NodePtr& render,
                                   const NodePtr& projector,
                                   const NodePtr& card)
    {
        NodePtr merge = render->getInput(0)->getInput(0);
        NodePtr assign = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));
        if (!merge || !assign || !setString(assign, kUSDAssignMaterialParamPrimPattern, "/Card")) {
            ADD_FAILURE() << "could not set up the assign";

            return NodePtr();
        }
        card->disconnectInput(0);
        merge->disconnectInput(0);
        connectNodes(card, assign, kUSDAssignMaterialInputScene, true);
        connectNodes(projector, assign, kUSDAssignMaterialInputMaterial, true);
        connectNodes(assign, merge, 0, true);

        return assign;
    }

    // Renders the projection scene at frame, failing on a render error, a node message or a shader
    // Storm reports it could not build, and checks the checker lands where it should.
    static void renderProjection(const NodePtr& render,
                                 const NodePtr& projector,
                                 double frame,
                                 double cardShift,
                                 const RectI& goldenCrop,
                                 const char* goldenCase)
    {
        std::vector<RenderedPlane> planes;
        std::string error;
        bool rendered = false;
        std::string shaderProblems;
        {
            USDDiagnostics diagnostics;
            rendered = renderOffMainThread(render, frame, &planes, &error);
            shaderProblems = diagnostics.shaderProblems();
        }
        EXPECT_TRUE(shaderProblems.empty()) << goldenCase << ": Storm could not build the projection shader:\n"
                                            << shaderProblems;
        ASSERT_TRUE(rendered) << goldenCase << ": " << error;
        EXPECT_FALSE(render->hasPersistentMessage()) << goldenCase;
        EXPECT_FALSE(projector->hasPersistentMessage()) << goldenCase;
        checkProjectedChecker(planes, cardShift, goldenCase);
        if (goldenCrop.isNull()) {
            return;
        }
        ASSERT_EQ(3u, planes.size());
        checkCropAgainstReference(planes[0], goldenCrop, goldenCase, [cardShift](int x, int y) { return isNearProjectedEdge(x, y, cardShift); });
    }

    // The colour plane of render at frame, failing on a render error or a shader Storm could not
    // build.
    static void renderColour(const NodePtr& render,
                             const char* what,
                             std::vector<float>* rgba,
                             double frame = 1.)
    {
        std::vector<RenderedPlane> planes;
        std::string error;
        bool rendered = false;
        std::string shaderProblems;
        {
            USDDiagnostics diagnostics;
            rendered = renderOffMainThread(render, frame, &planes, &error);
            shaderProblems = diagnostics.shaderProblems();
        }
        EXPECT_TRUE(shaderProblems.empty()) << what << ": " << shaderProblems;
        ASSERT_TRUE(rendered) << what << ": " << error;
        ASSERT_EQ(3u, planes.size()) << what;
        ASSERT_EQ((std::size_t)kWidth * kHeight * 4, planes[0].pixels.size()) << what;
        *rgba = planes[0].pixels;
    }

    // The projection graph with two cards apart, each moving its own way by frame 11, and a held
    // Geometry projection connected directly to both: the reference the shared material must match.
    // *cards holds what the merge's first input needs to carry once the projection leaves their
    // Material inputs; *rgba is the reference at frame 11, which shows the checker on both cards.
    NodePtr createTwoPlacementGraph(NodePtr* projector,
                                    NodePtr* cardA,
                                    NodePtr* cardB,
                                    NodePtr* cards,
                                    std::vector<float>* rgba)
    {
        double first = 0., last = 0.;
        getApp()->getFrameRange(&first, &last);
        EXPECT_LE(first, 1.);
        EXPECT_GE(last, 11.);
        NodePtr plate;
        NodePtr render = createProjectionGraph(&plate, projector, cardA);
        if (!render) {
            return render;
        }
        *cardB = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
        *cards = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
        KnobChoice* space = knob<KnobChoice>(*projector, kUSDProject3DParamProjectionSpace);
        KnobBool* useHold = knob<KnobBool>(*projector, kUSDProject3DParamUseFrameHold);
        KnobDouble* hold = knob<KnobDouble>(*projector, kUSDProject3DParamFrameHold);
        KnobDouble* translateA = knob<KnobDouble>(*cardA, kSceneParamTranslate);
        KnobDouble* translateB = *cardB ? knob<KnobDouble>(*cardB, kSceneParamTranslate) : NULL;
        if (!*cards || !space || !useHold || !hold || !translateA || !translateB
            || !setTriple(*cardA, kSceneParamScale, 4., 4., 1.)
            || !setString(*cardB, kSceneParamPrimPath, "/Card2")
            || !setTriple(*cardB, kSceneParamScale, 4., 4., 1.)) {
            ADD_FAILURE() << "could not set up the second card";

            return NodePtr();
        }
        space->setValue((int)USDProject3D::eProjectionSpaceGeometry);
        useHold->setValue(true);
        hold->setValue(1.);
        translateA->setValueAtTime(1., -2.5, ViewSpec::all(), 0);
        translateA->setValueAtTime(11., -3.5, ViewSpec::all(), 0);
        translateB->setValueAtTime(1., 2.5, ViewSpec::all(), 0);
        translateB->setValueAtTime(11., 4.5, ViewSpec::all(), 0);
        NodePtr merge = render->getInput(0)->getInput(0);
        connectNodes(*projector, *cardB, 0, true);
        connectNodes(*cardB, merge, 2, true);
        renderColour(render, "the direct connection", rgba, 11.);
        if (HasFatalFailure()) {
            return NodePtr();
        }

        for (const std::pair<int, int>& halves : { std::make_pair(0, kWidth / 2), std::make_pair(kWidth / 2, kWidth) }) {
            const RegionColours region = regionColours(*rgba, halves.first, 0, halves.second, kHeight);
            EXPECT_GT(region.red, 100) << "the direct connection, pixels " << halves.first << " to " << halves.second;
            EXPECT_GT(region.blue, 100) << "the direct connection, pixels " << halves.first << " to " << halves.second;
        }

        (*cardA)->disconnectInput(0);
        (*cardB)->disconnectInput(0);
        merge->disconnectInput(0);
        merge->disconnectInput(2);
        connectNodes(*cardA, *cards, 0, true);
        connectNodes(*cardB, *cards, 1, true);

        return render;
    } // createTwoPlacementGraph

    // Renders frame 11 and compares it to the direct connection's, pixel for pixel.
    static void expectMatchesReference(const NodePtr& render,
                                       const std::vector<float>& reference,
                                       const char* what,
                                       float tolerance = 0.01f)
    {
        std::vector<float> rgba;
        renderColour(render, what, &rgba, 11.);
        ASSERT_EQ(reference.size(), rgba.size()) << what;
        std::size_t different = 0;
        float worst = 0.f;
        for (std::size_t i = 0; i < rgba.size(); i += 4) {
            bool same = true;
            for (std::size_t c = 0; c < 4; ++c) {
                const float difference = std::fabs(rgba[i + c] - reference[i + c]);
                worst = std::max(worst, difference);
                same = same && (difference < tolerance);
            }
            different += same ? 0 : 1;
        }
        EXPECT_EQ((std::size_t)0, different) << what << ": worst difference " << worst;
    }

    static PXR_NS::VtVec3fArray quadPoints(double x,
                                           double halfWidth,
                                           double halfHeight)
    {
        PXR_NS::VtVec3fArray points(4);
        points[0] = PXR_NS::GfVec3f((float)(x - halfWidth), (float)-halfHeight, 0.f);
        points[1] = PXR_NS::GfVec3f((float)(x + halfWidth), (float)-halfHeight, 0.f);
        points[2] = PXR_NS::GfVec3f((float)(x + halfWidth), (float)halfHeight, 0.f);
        points[3] = PXR_NS::GfVec3f((float)(x - halfWidth), (float)halfHeight, 0.f);

        return points;
    }

    static PXR_NS::UsdGeomMesh defineQuad(const PXR_NS::UsdStageRefPtr& stage,
                                          const char* path)
    {
        PXR_NS::UsdGeomMesh mesh = PXR_NS::UsdGeomMesh::Define(stage, PXR_NS::SdfPath(path));
        PXR_NS::VtIntArray counts(1, 4);
        PXR_NS::VtIntArray indices(4);
        for (int i = 0; i < 4; ++i) {
            indices[i] = i;
        }
        mesh.CreateFaceVertexCountsAttr().Set(counts);
        mesh.CreateFaceVertexIndicesAttr().Set(indices);
        mesh.CreateDoubleSidedAttr().Set(true);
        mesh.CreateSubdivisionSchemeAttr().Set(PXR_NS::UsdGeomTokens->none);

        return mesh;
    }

    // A file at path with a quad at /Moving whose points go from fromX at frame 1 to toX at frame 11.
    static void writeMovingQuad(const std::string& path,
                                double fromX,
                                double toX,
                                double halfWidth,
                                double halfHeight)
    {
        const PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::CreateNew(path);
        ASSERT_TRUE(bool(stage));
        const PXR_NS::UsdGeomMesh mesh = defineQuad(stage, "/Moving");
        mesh.CreatePointsAttr().Set(quadPoints(fromX, halfWidth, halfHeight), PXR_NS::UsdTimeCode(1.));
        mesh.CreatePointsAttr().Set(quadPoints(toX, halfWidth, halfHeight), PXR_NS::UsdTimeCode(11.));
        ASSERT_TRUE(stage->GetRootLayer()->Save());
    }

    // The same quad moved by a joint: its points stay at x, its skeleton's only joint goes from 0 to shift.
    static void writeSkinnedQuad(const std::string& path,
                                 double x,
                                 double shift,
                                 double halfWidth,
                                 double halfHeight)
    {
        const PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::CreateNew(path);
        ASSERT_TRUE(bool(stage));
        PXR_NS::UsdSkelRoot::Define(stage, PXR_NS::SdfPath("/Rig"));
        const PXR_NS::UsdSkelSkeleton skeleton = PXR_NS::UsdSkelSkeleton::Define(stage, PXR_NS::SdfPath("/Rig/Skeleton"));
        const PXR_NS::VtTokenArray joints(1, PXR_NS::TfToken("Joint"));
        const PXR_NS::VtMatrix4dArray identity(1, PXR_NS::GfMatrix4d(1.));
        skeleton.CreateJointsAttr().Set(joints);
        skeleton.CreateBindTransformsAttr().Set(identity);
        skeleton.CreateRestTransformsAttr().Set(identity);
        const PXR_NS::UsdSkelAnimation animation = PXR_NS::UsdSkelAnimation::Define(stage, PXR_NS::SdfPath("/Rig/Skeleton/Animation"));
        animation.CreateJointsAttr().Set(joints);
        animation.CreateRotationsAttr().Set(PXR_NS::VtQuatfArray(1, PXR_NS::GfQuatf(1.f)));
        animation.CreateScalesAttr().Set(PXR_NS::VtVec3hArray(1, PXR_NS::GfVec3h(1.f, 1.f, 1.f)));
        animation.CreateTranslationsAttr().Set(PXR_NS::VtVec3fArray(1, PXR_NS::GfVec3f(0.f)), PXR_NS::UsdTimeCode(1.));
        animation.CreateTranslationsAttr().Set(PXR_NS::VtVec3fArray(1, PXR_NS::GfVec3f((float)shift, 0.f, 0.f)), PXR_NS::UsdTimeCode(11.));
        PXR_NS::UsdSkelBindingAPI::Apply(skeleton.GetPrim()).CreateAnimationSourceRel().SetTargets({ animation.GetPath() });
        const PXR_NS::UsdGeomMesh mesh = defineQuad(stage, "/Rig/Moving");
        mesh.CreatePointsAttr().Set(quadPoints(x, halfWidth, halfHeight));
        const PXR_NS::UsdSkelBindingAPI binding = PXR_NS::UsdSkelBindingAPI::Apply(mesh.GetPrim());
        binding.CreateSkeletonRel().SetTargets({ skeleton.GetPath() });
        binding.CreateJointIndicesPrimvar(true, 1).Set(PXR_NS::VtIntArray(1, 0));
        binding.CreateJointWeightsPrimvar(true, 1).Set(PXR_NS::VtFloatArray(1, 1.f));
        ASSERT_TRUE(stage->GetRootLayer()->Save());
    }

    // createProjectionGraph's result with the projection held at frame 1, and what file holds in place
    // of the card, unbound.
    NodePtr createHeldGraphReading(const QString& file,
                                   NodePtr* projector)
    {
        NodePtr plate, card;
        NodePtr render = createProjectionGraph(&plate, projector, &card);
        if (!render) {
            return render;
        }
        KnobChoice* space = knob<KnobChoice>(*projector, kUSDProject3DParamProjectionSpace);
        KnobBool* useHold = knob<KnobBool>(*projector, kUSDProject3DParamUseFrameHold);
        KnobDouble* hold = knob<KnobDouble>(*projector, kUSDProject3DParamFrameHold);
        NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
        KnobFile* fileKnob = reader ? knob<KnobFile>(reader, kUSDReadParamFile) : NULL;
        if (!space || !useHold || !hold || !fileKnob) {
            ADD_FAILURE() << "could not set up the held graph";

            return NodePtr();
        }
        space->setValue((int)USDProject3D::eProjectionSpaceGeometry);
        useHold->setValue(true);
        hold->setValue(1.);
        fileKnob->setValue(file.toStdString());
        card->disconnectInput(0);
        NodePtr merge = render->getInput(0)->getInput(0);
        merge->disconnectInput(0);
        connectNodes(reader, merge, 0, true);

        return render;
    }

    // Between the merge and what its first input reads, a USDAssignMaterial binding the projection to pattern.
    NodePtr assignProjection(const NodePtr& render,
                             const NodePtr& projector,
                             const char* pattern)
    {
        NodePtr merge = render->getInput(0)->getInput(0);
        NodePtr scene = merge ? merge->getInput(0) : NodePtr();
        NodePtr assign = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));
        if (!scene || !assign || !setString(assign, kUSDAssignMaterialParamPrimPattern, pattern)) {
            ADD_FAILURE() << "could not set up the assign";

            return NodePtr();
        }
        merge->disconnectInput(0);
        connectNodes(scene, assign, kUSDAssignMaterialInputScene, true);
        connectNodes(projector, assign, kUSDAssignMaterialInputMaterial, true);
        connectNodes(assign, merge, 0, true);

        return assign;
    }

    // In createTwoPlacementGraph's scene, what file holds in place of the second card.
    NodePtr readInPlaceOfSecondCard(const NodePtr& cards,
                                    const QString& file)
    {
        NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
        KnobFile* fileKnob = reader ? knob<KnobFile>(reader, kUSDReadParamFile) : NULL;
        if (!fileKnob) {
            ADD_FAILURE() << "could not set up the reader";

            return NodePtr();
        }
        fileKnob->setValue(file.toStdString());
        cards->disconnectInput(1);
        connectNodes(reader, cards, 1, true);

        return reader;
    }

    // Whether the coloured pixels of first are the same colours shift pixels to the right in second:
    // what a surface that took shift pixels of motion with the image on it renders. A slide would show
    // otherwise, as the shift is not a whole number of the checker's periods.
    static void expectColoursMovedBy(const std::vector<float>& first,
                                     const std::vector<float>& second,
                                     int shift,
                                     const char* what)
    {
        int compared = 0;
        int different = 0;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x + shift < kWidth; ++x) {
                const ProjectedColourEnum colour = classifyColour(&first[((std::size_t)y * kWidth + x) * 4]);
                if ((colour != eProjectedRed) && (colour != eProjectedBlue)) {
                    continue;
                }
                ++compared;
                different += (classifyColour(&second[((std::size_t)y * kWidth + x + shift) * 4]) == colour) ? 0 : 1;
            }
        }
        EXPECT_GT(compared, 30) << what;
        EXPECT_LE(different, compared / 20) << what << ": " << different << " of " << compared;
    }

    /**
     * The shared-material graph of USDWrite's round trip, rendered: an asset whose box is bound
     * directly to a grey material, a USDAssignMaterial binding a red USDMaterial to /Asset with
     * strength, optionally a second binding a blue one to /Asset/Box, the result moved under a
     * USDXformPrim /T1, and a camera at (0, 0, 10) looking down -Z with a key light. Returns the
     * USDRender, kWidth x kHeight, null after a gtest failure.
     **/
    NodePtr createAssignMaterialGraph(const QString& assetPath,
                                      USDAssignMaterial::StrengthEnum strength,
                                      bool secondAssign)
    {
        NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
        NodePtr red = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMATERIAL));
        NodePtr assign = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));
        NodePtr blue = secondAssign ? createNode(QString::fromUtf8(PLUGINID_NATRON_USDMATERIAL)) : NodePtr();
        NodePtr assignOver = secondAssign ? createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL)) : NodePtr();
        NodePtr moved = createNode(QString::fromUtf8(PLUGINID_NATRON_USDXFORMPRIM));
        NodePtr camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
        NodePtr light = createNode(QString::fromUtf8(PLUGINID_NATRON_USDLIGHT));
        NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
        NodePtr render = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
        if (!reader || !red || !assign || (secondAssign && (!blue || !assignOver)) || !moved || !camera || !light || !merge || !render) {
            ADD_FAILURE() << "a 3D node is not registered";

            return NodePtr();
        }

        KnobFile* file = knob<KnobFile>(reader, kUSDReadParamFile);
        KnobColor* redDiffuse = knob<KnobColor>(red, kUSDMaterialParamDiffuseColor);
        KnobChoice* strengthKnob = knob<KnobChoice>(assign, kUSDAssignMaterialParamStrength);
        if (!file || !redDiffuse || !strengthKnob) {
            return NodePtr();
        }
        file->setValue(assetPath.toStdString());
        redDiffuse->setValue(0.8, ViewSpec::all(), 0);
        redDiffuse->setValue(0.1, ViewSpec::all(), 1);
        redDiffuse->setValue(0.1, ViewSpec::all(), 2);
        strengthKnob->setValue((int)strength);

        bool ok = true;
        ok = ok && setString(red, kSceneParamPrimPath, "/Red");
        ok = ok && setString(assign, kUSDAssignMaterialParamPrimPattern, "/Asset");
        ok = ok && setString(moved, kSceneParamPrimPath, "/T1");
        ok = ok && setTriple(moved, kSceneParamTranslate, 0., 0., 0.);
        ok = ok && setString(camera, kSceneParamPrimPath, "/Cam");
        ok = ok && setTriple(camera, kSceneParamTranslate, 0., 0., 10.);
        ok = ok && setString(light, kSceneParamPrimPath, "/Key");
        if (!ok) {
            return NodePtr();
        }
        connectNodes(reader, assign, kUSDAssignMaterialInputScene, true);
        connectNodes(red, assign, kUSDAssignMaterialInputMaterial, true);
        NodePtr last = assign;
        if (secondAssign) {
            KnobColor* blueDiffuse = knob<KnobColor>(blue, kUSDMaterialParamDiffuseColor);
            if (!blueDiffuse || !setString(blue, kSceneParamPrimPath, "/Blue") || !setString(assignOver, kUSDAssignMaterialParamPrimPattern, "/Asset/Box")) {
                return NodePtr();
            }
            blueDiffuse->setValue(0.1, ViewSpec::all(), 0);
            blueDiffuse->setValue(0.1, ViewSpec::all(), 1);
            blueDiffuse->setValue(0.8, ViewSpec::all(), 2);
            connectNodes(assign, assignOver, kUSDAssignMaterialInputScene, true);
            connectNodes(blue, assignOver, kUSDAssignMaterialInputMaterial, true);
            last = assignOver;
        }
        connectNodes(last, moved, 0, true);
        connectNodes(moved, merge, 0, true);
        connectNodes(camera, merge, 1, true);
        connectNodes(light, merge, 2, true);
        // The render settings prim frames the camera at kWidth x kHeight.
        NodePtr settings = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDERSETTINGS));
        if (!settings) {
            ADD_FAILURE() << "USDRenderSettings is not registered";

            return NodePtr();
        }
        connectNodes(merge, settings, 0, true);
        connectNodes(settings, render, 0, true);
        if (!RenderSettingsTestUtils::configure(settings, "/Cam", kWidth, kHeight)) {
            return NodePtr();
        }

        return render;
    } // createAssignMaterialGraph

    static std::list<ImageLayerDesc> allPlanes()
    {
        std::list<ImageLayerDesc> layers;
        layers.push_back(ImageLayerDesc::getRGBAComponents());
        layers.push_back(ImageLayerDesc("depth", "depth", "", std::vector<std::string>(1, "Z")));
        layers.push_back(ImageLayerDesc("primId", "primId", "", std::vector<std::string>(1, "id")));

        return layers;
    }
};
} // anonymous namespace

TEST_F(USDRenderStormTest, TheReferenceSceneMatchesItsImageDepthAndPrimIds)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr node = createReferenceGraph();
    ASSERT_TRUE(node != nullptr);
    std::vector<RenderedPlane> planes;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, RectI(0, 0, kWidth, kHeight), allPlanes(), &planes, &error)) << error;
    EXPECT_FALSE(node->hasPersistentMessage());
    ASSERT_EQ(3u, planes.size());
    const std::size_t pixels = (std::size_t)kWidth * kHeight;
    ASSERT_EQ(pixels * 4, planes[0].pixels.size());
    ASSERT_EQ(pixels, planes[1].pixels.size());
    ASSERT_EQ(pixels, planes[2].pixels.size());
    const std::vector<float>& rgba = planes[0].pixels;
    const std::vector<float>& depth = planes[1].pixels;
    const std::vector<float>& primId = planes[2].pixels;

    checkAgainstReference(planes[0], kGoldenCase);

    // The camera is at (1, 1, 12) looking down -Z: the box's front face, at z = 1, is 11 away, and
    // the lifted ball's nearest point, at (3, 1, 0.5), 11.5.
    const Coverage box = coverageOf("/World/Box", rgba, depth, primId);
    const Coverage ball = coverageOf("/World/Ball", rgba, depth, primId);
    ASSERT_GT(box.count, 100);
    ASSERT_GT(ball.count, 20);
    EXPECT_NEAR(11., box.minDepth, 0.01);
    // Storm draws the sphere as a mesh inside it, so its nearest point may lie a little further.
    EXPECT_NEAR(11.5, ball.minDepth, 0.05);
    EXPECT_LT(ball.maxDepth, 12.);
    // The box sits left of and below the camera, the ball right of it at its height.
    EXPECT_LT(box.x, kWidth / 2.);
    EXPECT_LT(box.y, kHeight / 2.);
    EXPECT_GT(ball.x, kWidth / 2.);
    EXPECT_NEAR(kHeight / 2., ball.y, 3.);
    // The material made the box green; the ball keeps its blue display colour. The key light,
    // normalized and of intensity 1, faces the box's front face head on and lights the ball's
    // camera side, so both are well lit and not merely tinted.
    EXPECT_GT(box.rgb[1], 0.2) << "box colour " << box.rgb[0] << ", " << box.rgb[1] << ", " << box.rgb[2];
    EXPECT_GT(box.rgb[1], 2. * box.rgb[0]);
    EXPECT_GT(box.rgb[1], 2. * box.rgb[2]);
    EXPECT_GT(ball.rgb[2], 0.1) << "ball colour " << ball.rgb[0] << ", " << ball.rgb[1] << ", " << ball.rgb[2];
    EXPECT_GT(ball.rgb[2], 2. * ball.rgb[0]);
    EXPECT_GT(ball.rgb[2], ball.rgb[1]);

    // Where nothing was hit: no colour, depth +inf, primId 0. Elsewhere depth is finite and positive.
    const std::size_t corner = (std::size_t)(kHeight - 1) * (std::size_t)kWidth;
    EXPECT_EQ(0u, bitsOf(primId[corner]));
    EXPECT_TRUE(std::isinf(depth[corner]) && (depth[corner] > 0.f)) << depth[corner];
    EXPECT_EQ(0.f, rgba[corner * 4 + 3]);
    std::set<std::uint32_t> ids;
    int badDepths = 0;
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::uint32_t id = bitsOf(primId[i]);
        if (id == 0) {
            badDepths += std::isinf(depth[i]) ? 0 : 1;
            continue;
        }
        ids.insert(id);
        badDepths += (std::isfinite(depth[i]) && (depth[i] > 0.f)) ? 0 : 1;
    }
    EXPECT_EQ(0, badDepths);
    // Only the box and the ball are drawn; the pruned camera, the rig and the light are not.
    EXPECT_EQ(2u, ids.size());

    // The frame metadata names every id the plane holds.
    NativeEffectBase* effect = dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect);
    const ImageMetadata metadata = effect->getOutputMetadata(1., ViewIdx(0));
    const std::uint32_t boxId = bitsOf(HydraRender::hashToFloat(HydraRender::primPathHash("/World/Box")));
    const std::uint32_t ballId = bitsOf(HydraRender::hashToFloat(HydraRender::primPathHash("/World/Ball")));
    for (std::uint32_t id : ids) {
        char key[32];
        std::snprintf(key, sizeof(key), kUSDRenderMetadataPrimIdPrefix "%08x", id);
        const std::optional<std::string> path = metadata.getString(key);
        ASSERT_TRUE(path.has_value()) << key;
        EXPECT_TRUE((id == boxId) ? (*path == "/World/Box") : (id == ballId) ? (*path == "/World/Ball")
                                                                             : false)
            << key << " names " << *path;
    }
} // TEST_F

TEST_F(USDRenderStormTest, PrimitivesRenderWhereTheirTransformsPlaceThem)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr node = createPrimitivesGraph();
    ASSERT_TRUE(node != nullptr);
    std::vector<RenderedPlane> planes;
    std::string error;
    ASSERT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, RectI(0, 0, kWidth, kHeight), allPlanes(), &planes, &error)) << error;
    EXPECT_FALSE(node->hasPersistentMessage());
    ASSERT_EQ(3u, planes.size());
    const std::size_t pixels = (std::size_t)kWidth * kHeight;
    ASSERT_EQ(pixels * 4, planes[0].pixels.size());
    ASSERT_EQ(pixels, planes[1].pixels.size());
    ASSERT_EQ(pixels, planes[2].pixels.size());
    const std::vector<float>& rgba = planes[0].pixels;
    const std::vector<float>& depth = planes[1].pixels;
    const std::vector<float>& primId = planes[2].pixels;

    checkAgainstReference(planes[0], kGoldenCasePrimitives);

    // The camera is 10 from the card's plane and 9 from the sphere's nearest point, which is a
    // vertex of the mesh. The sphere's visible cap ends at its silhouette, about 9.9 away, and
    // its pixel-edge depths can overshoot that slightly, so the far bound is the sphere's centre
    // plane plus a margin.
    const Coverage card = coverageOf("/Left/Card", rgba, depth, primId);
    const Coverage ball = coverageOf("/Right/Ball", rgba, depth, primId);
    ASSERT_GT(card.count, 500);
    ASSERT_GT(ball.count, 500);
    EXPECT_NEAR(10., card.minDepth, 0.01);
    EXPECT_NEAR(10., card.maxDepth, 0.01);
    EXPECT_NEAR(9., ball.minDepth, 0.05);
    EXPECT_LT(ball.maxDepth, 10.25);
    // Mirror images about the centre: the card left, the ball right, both at the camera's height.
    EXPECT_LT(card.x, kWidth / 2.);
    EXPECT_GT(ball.x, kWidth / 2.);
    EXPECT_NEAR(kWidth / 2., (card.x + ball.x) / 2., 3.);
    EXPECT_NEAR(kHeight / 2., card.y, 3.);
    EXPECT_NEAR(kHeight / 2., ball.y, 3.);
    // The card is twice as wide as it is high.
    int cardLeft = kWidth, cardRight = -1, cardBottom = kHeight, cardTop = -1;
    const std::uint32_t cardId = bitsOf(HydraRender::hashToFloat(HydraRender::primPathHash("/Left/Card")));
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            if (bitsOf(primId[(std::size_t)y * (std::size_t)kWidth + (std::size_t)x]) == cardId) {
                cardLeft = std::min(cardLeft, x);
                cardRight = std::max(cardRight, x);
                cardBottom = std::min(cardBottom, y);
                cardTop = std::max(cardTop, y);
            }
        }
    }
    ASSERT_GT(cardTop, cardBottom);
    EXPECT_NEAR(2., (double)(cardRight - cardLeft + 1) / (double)(cardTop - cardBottom + 1), 0.15);
    // The light faces both head on. The bound material makes the ball red; the card keeps the
    // neutral fallback colour.
    EXPECT_GT(ball.rgb[0], 0.1) << "ball colour " << ball.rgb[0] << ", " << ball.rgb[1] << ", " << ball.rgb[2];
    EXPECT_GT(ball.rgb[0], 3. * ball.rgb[1]);
    EXPECT_GT(ball.rgb[0], 3. * ball.rgb[2]);
    EXPECT_GT(card.rgb[0], 0.1) << "card colour " << card.rgb[0] << ", " << card.rgb[1] << ", " << card.rgb[2];
    EXPECT_NEAR(card.rgb[0], card.rgb[1], 0.05);
    EXPECT_NEAR(card.rgb[0], card.rgb[2], 0.05);

    // Only the two meshes are drawn; the material, camera and light are not.
    std::set<std::uint32_t> ids;
    for (std::size_t i = 0; i < pixels; ++i) {
        const std::uint32_t id = bitsOf(primId[i]);
        if (id != 0) {
            ids.insert(id);
        }
    }
    EXPECT_EQ(2u, ids.size());
} // TEST_F

TEST_F(USDRenderStormTest, ALiveTextureColoursTheCardWithTheImageConnected)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr constant;
    NodePtr render = createLiveTextureGraph(&constant);
    ASSERT_TRUE(render != nullptr);
    KnobColor* color = knob<KnobColor>(constant, kConstantParamColor);
    ASSERT_TRUE(color != nullptr);
    color->setValue(1., ViewSpec::all(), 0);
    color->setValue(0., ViewSpec::all(), 1);
    color->setValue(0., ViewSpec::all(), 2);
    color->setValue(1., ViewSpec::all(), 3);

    const Coverage card = renderCard(render, 1.);
    ASSERT_GT(card.count, 500);
    EXPECT_FALSE(render->hasPersistentMessage());
    // Red, not Storm's grey fallback.
    EXPECT_GT(card.rgb[0], 0.1) << "card colour " << card.rgb[0] << ", " << card.rgb[1] << ", " << card.rgb[2];
    EXPECT_GT(card.rgb[0], 5. * card.rgb[1]);
    EXPECT_GT(card.rgb[0], 5. * card.rgb[2]);
}

// With no light in the scene a lit card is black, unless Headlight, off by default, adds a light at
// the camera; turning it off again takes the light back.
TEST_F(USDRenderStormTest, TheHeadlightLightsAnUnlitSceneOnlyWhenOn)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr constant;
    NodePtr render = createLiveTextureGraph(&constant, false);
    ASSERT_TRUE(render != nullptr);
    KnobColor* color = knob<KnobColor>(constant, kConstantParamColor);
    KnobBool* headlight = knob<KnobBool>(render, kUSDRenderParamHeadlight);
    ASSERT_TRUE(color != nullptr);
    ASSERT_TRUE(headlight != nullptr);
    EXPECT_FALSE(headlight->getValue()) << "Headlight is off by default";
    EXPECT_EQ(std::string("Headlight"), headlight->getLabel());
    color->setValue(1., ViewSpec::all(), 0);
    color->setValue(0., ViewSpec::all(), 1);
    color->setValue(0., ViewSpec::all(), 2);
    color->setValue(1., ViewSpec::all(), 3);

    const Coverage off = renderCard(render, 1.);
    ASSERT_GT(off.count, 500);
    EXPECT_LT(off.rgb[0], 0.02) << "card colour, headlight off " << off.rgb[0] << ", " << off.rgb[1] << ", " << off.rgb[2];
    EXPECT_LT(off.rgb[1], 0.02);
    EXPECT_LT(off.rgb[2], 0.02);

    headlight->setValue(true);
    const Coverage on = renderCard(render, 1.);
    ASSERT_GT(on.count, 500);
    EXPECT_GT(on.rgb[0], 0.1) << "card colour, headlight on " << on.rgb[0] << ", " << on.rgb[1] << ", " << on.rgb[2];
    EXPECT_GT(on.rgb[0], 5. * on.rgb[1]);
    EXPECT_GT(on.rgb[0], 5. * on.rgb[2]);

    headlight->setValue(false);
    const Coverage offAgain = renderCard(render, 1.);
    ASSERT_GT(offAgain.count, 500);
    EXPECT_LT(offAgain.rgb[0], 0.02) << "card colour, headlight off again " << offAgain.rgb[0];
}

// With no light and no headlight, the emissive material's card shows its input as it is.
TEST_F(USDRenderStormTest, TheEmissiveMaterialShowsItsImageUnchangedWithNoLights)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr constant;
    NodePtr render = createLiveTextureGraph(&constant, false, true);
    ASSERT_TRUE(render != nullptr);
    KnobColor* color = knob<KnobColor>(constant, kConstantParamColor);
    KnobBool* headlight = knob<KnobBool>(render, kUSDRenderParamHeadlight);
    ASSERT_TRUE(color != nullptr);
    ASSERT_TRUE(headlight != nullptr);
    ASSERT_FALSE(headlight->getValue());
    const double source[3] = { 0.6, 0.3, 0.1 };
    for (int c = 0; c < 3; ++c) {
        color->setValue(source[c], ViewSpec::all(), c);
    }
    color->setValue(1., ViewSpec::all(), 3);

    const Coverage card = renderCard(render, 1.);
    ASSERT_GT(card.count, 500);
    EXPECT_FALSE(render->hasPersistentMessage());
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(source[c], card.rgb[c], 0.03) << "channel " << c << ", card colour " << card.rgb[0] << ", " << card.rgb[1] << ", " << card.rgb[2];
    }
}

// A card showing a Constant of half-transparent red, premultiplied as Natron's images are, in front
// of an opaque blue card: the red composites over the blue, so half of each shows through.
TEST_F(USDRenderStormTest, AHalfTransparentLiveTextureShowsTheBackgroundThroughIt)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr constant;
    NodePtr render = createLiveTextureGraph(&constant, false, true);
    ASSERT_TRUE(render != nullptr);
    NodePtr merge = render->getInput(0)->getInput(0);
    ASSERT_TRUE(merge != nullptr);
    NodePtr backMaterial = createNode(QString::fromUtf8(PLUGINID_NATRON_USDEMISSIVEMATERIAL));
    NodePtr backCard = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
    ASSERT_TRUE(backMaterial && backCard);
    ASSERT_TRUE(setString(backMaterial, kSceneParamPrimPath, "/Blue"));
    ASSERT_TRUE(setString(backCard, kSceneParamPrimPath, "/Back"));
    ASSERT_TRUE(setTriple(backCard, kSceneParamTranslate, 0., 0., -1.));
    KnobColor* blue = knob<KnobColor>(backMaterial, kUSDEmissiveMaterialParamEmissiveColor);
    KnobColor* color = knob<KnobColor>(constant, kConstantParamColor);
    ASSERT_TRUE(blue && color);
    blue->setValue(0., ViewSpec::all(), 0);
    blue->setValue(0., ViewSpec::all(), 1);
    blue->setValue(1., ViewSpec::all(), 2);
    connectNodes(backMaterial, backCard, 0, true);
    connectNodes(backCard, merge, 2, true);
    color->setValue(0.5, ViewSpec::all(), 0);
    color->setValue(0., ViewSpec::all(), 1);
    color->setValue(0., ViewSpec::all(), 2);
    color->setValue(0.5, ViewSpec::all(), 3);

    double centre[4];
    centreOf(renderColour(render, 1.), centre);
    EXPECT_FALSE(render->hasPersistentMessage());
    const double expected[4] = { 0.5, 0., 0.5, 1. };
    for (int k = 0; k < 4; ++k) {
        EXPECT_NEAR(expected[k], centre[k], 0.04) << "channel " << k << ", centre " << centre[0] << ", " << centre[1] << ", " << centre[2] << ", " << centre[3];
    }
}

// A card showing red over the left half of its image, its edge softened, in front of a white card: the
// red composites over the white, so across the edge every pixel lies between red and white. Black
// filtered in from the transparent texels would show as a band darker than both. The camera is
// zoomed in on the edge to about 2 texels a pixel, a minification the fill band covers: at the
// default view a 1920-texel card is some 40 texels a pixel, where the edge is under a pixel.
TEST_F(USDRenderStormTest, ASoftLiveTextureEdgeHasNoDarkFringe)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr constant;
    NodePtr render = createLiveTextureGraph(&constant, false, true);
    ASSERT_TRUE(render != nullptr);
    NodePtr merge = render->getInput(0)->getInput(0);
    NodePtr material = render->getInput(0)->getInput(0)->getInput(0)->getInput(0);
    NodePtr camera = merge ? merge->getInput(1) : NodePtr();
    ASSERT_TRUE(merge && material && camera);
    ASSERT_EQ(std::string(PLUGINID_NATRON_USDEMISSIVEMATERIAL), material->getPluginID());
    ASSERT_EQ(std::string(PLUGINID_NATRON_USDCAMERA), camera->getPluginID());
    KnobDouble* focalLength = knob<KnobDouble>(camera, kUSDCameraParamFocalLength);
    ASSERT_TRUE(focalLength != nullptr);
    focalLength->setValue(1000.);
    NodePtr blur = createNode(QString::fromUtf8(PLUGINID_NATRON_BLUR), PLUGIN_MAJOR_NATRON_BLUR);
    NodePtr backMaterial = createNode(QString::fromUtf8(PLUGINID_NATRON_USDEMISSIVEMATERIAL));
    NodePtr backCard = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
    ASSERT_TRUE(blur && backMaterial && backCard);
    ASSERT_TRUE(setString(backMaterial, kSceneParamPrimPath, "/White"));
    ASSERT_TRUE(setString(backCard, kSceneParamPrimPath, "/Back"));
    ASSERT_TRUE(setTriple(backCard, kSceneParamTranslate, 0., 0., -1.));
    ASSERT_TRUE(setTriple(backCard, kSceneParamScale, 2., 2., 1.));
    KnobColor* white = knob<KnobColor>(backMaterial, kUSDEmissiveMaterialParamEmissiveColor);
    KnobColor* color = knob<KnobColor>(constant, kConstantParamColor);
    KnobChoice* extent = knob<KnobChoice>(constant, kNativeGeneratorParamExtent);
    KnobDouble* bottomLeft = knob<KnobDouble>(constant, kNativeGeneratorParamBottomLeft);
    KnobDouble* size = knob<KnobDouble>(constant, kNativeGeneratorParamSize);
    KnobDouble* blurSize = knob<KnobDouble>(blur, kBlurParamSize);
    ASSERT_TRUE(white && color && extent && bottomLeft && size && blurSize);
    for (int k = 0; k < 3; ++k) {
        white->setValue(1., ViewSpec::all(), k);
    }
    connectNodes(backMaterial, backCard, 0, true);
    connectNodes(backCard, merge, 2, true);

    Format format;
    getApp()->getProject()->getProjectDefaultFormat(&format);
    color->setValue(1., ViewSpec::all(), 0);
    color->setValue(0., ViewSpec::all(), 1);
    color->setValue(0., ViewSpec::all(), 2);
    color->setValue(1., ViewSpec::all(), 3);
    extent->setValueFromID(kNativeGeneratorExtentSize, 0);
    bottomLeft->setValue(format.x1 - format.width(), ViewSpec::all(), 0);
    bottomLeft->setValue(format.y1 - format.height(), ViewSpec::all(), 1);
    size->setValue(format.width() + format.width() / 2, ViewSpec::all(), 0);
    size->setValue(3 * format.height(), ViewSpec::all(), 1);
    // An edge a few texels wide: one wide enough to fade to almost nothing before its alpha reaches
    // 0 would hide the black, which Storm's filtering blends in a texel or so from where the alpha
    // is 0.
    blurSize->setValue(3., ViewSpec::all(), 0);
    blurSize->setValue(3., ViewSpec::all(), 1);
    disconnectNodes(constant, material, true);
    connectNodes(constant, blur, 0, true);
    connectNodes(blur, material, kUSDEmissiveMaterialInputEmissive, true);

    const std::vector<float> rgba = renderColour(render, 1.);
    EXPECT_FALSE(render->hasPersistentMessage());
    bool sawRed = false;
    bool sawWhite = false;
    for (int y = kHeight / 2 - 2; y <= kHeight / 2 + 2; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const std::size_t i = (std::size_t)y * (std::size_t)kWidth + (std::size_t)x;
            const float* p = &rgba[i * 4];
            if (p[3] < 0.99f) {
                continue;
            }
            sawRed = sawRed || (p[1] < 0.05f);
            sawWhite = sawWhite || (p[1] > 0.95f);
            EXPECT_GT(p[0], 0.96f) << "pixel " << x << ", " << y << ": " << p[0] << ", " << p[1] << ", " << p[2];
            EXPECT_GT(p[1], -0.04f) << "pixel " << x << ", " << y;
            EXPECT_LT(p[1], 1.04f) << "pixel " << x << ", " << y;
            EXPECT_NEAR(p[1], p[2], 0.04) << "pixel " << x << ", " << y << ": " << p[0] << ", " << p[1] << ", " << p[2];
        }
    }
    EXPECT_TRUE(sawRed) << "the row crosses the red";
    EXPECT_TRUE(sawWhite) << "the row crosses the white";
}

// The node feeding a live texture, disabled, passes its input through, as it does in the 2D graph:
// an Invert of red shows cyan, and red once the Invert is disabled.
TEST_F(USDRenderStormTest, ALiveTextureFollowsADisabledNodeToItsInput)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr constant;
    NodePtr render = createLiveTextureGraph(&constant, false, true);
    ASSERT_TRUE(render != nullptr);
    NodePtr material = render->getInput(0)->getInput(0)->getInput(0)->getInput(0);
    ASSERT_TRUE(material != nullptr);
    ASSERT_EQ(std::string(PLUGINID_NATRON_USDEMISSIVEMATERIAL), material->getPluginID());
    NodePtr invert = createNode(QString::fromUtf8(PLUGINID_NATRON_INVERT));
    ASSERT_TRUE(invert != nullptr);
    KnobChannelSetPtr channels = std::dynamic_pointer_cast<KnobChannelSet>(invert->getKnobByName(kNodeParamChannelSet));
    ASSERT_TRUE(channels != nullptr);
    std::vector<std::string> rgb;
    rgb.push_back("R");
    rgb.push_back("G");
    rgb.push_back("B");
    channels->setChannels(0, rgb);
    disconnectNodes(constant, material, true);
    connectNodes(constant, invert, 0, true);
    connectNodes(invert, material, kUSDEmissiveMaterialInputEmissive, true);
    KnobColor* color = knob<KnobColor>(constant, kConstantParamColor);
    ASSERT_TRUE(color != nullptr);
    color->setValue(1., ViewSpec::all(), 0);
    color->setValue(0., ViewSpec::all(), 1);
    color->setValue(0., ViewSpec::all(), 2);
    color->setValue(1., ViewSpec::all(), 3);

    // The centre, not the card's mean: the mean takes in the antialiased edges, which pull a full
    // channel about 3% below 1.
    double inverted[4];
    centreOf(renderColour(render, 1.), inverted);
    EXPECT_NEAR(0., inverted[0], 0.03) << "inverted " << inverted[0] << ", " << inverted[1] << ", " << inverted[2];
    EXPECT_NEAR(1., inverted[1], 0.03);
    EXPECT_NEAR(1., inverted[2], 0.03);

    invert->setNodeDisabled(true);
    double passed[4];
    centreOf(renderColour(render, 1.), passed);
    EXPECT_FALSE(render->hasPersistentMessage());
    EXPECT_NEAR(1., passed[0], 0.03) << "disabled " << passed[0] << ", " << passed[1] << ", " << passed[2];
    EXPECT_NEAR(0., passed[1], 0.03);
    EXPECT_NEAR(0., passed[2], 0.03);
}

TEST_F(USDRenderStormTest, DeletingALiveTexturesSourceShowsMagentaUntilUndone)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr constant;
    NodePtr render = createLiveTextureGraph(&constant, false, true);
    ASSERT_TRUE(render != nullptr);
    NodePtr material = render->getInput(0)->getInput(0)->getInput(0)->getInput(0);
    ASSERT_TRUE(material != nullptr);
    ASSERT_EQ(std::string(PLUGINID_NATRON_USDEMISSIVEMATERIAL), material->getPluginID());
    KnobColor* color = knob<KnobColor>(constant, kConstantParamColor);
    ASSERT_TRUE(color != nullptr);
    color->setValue(0., ViewSpec::all(), 0);
    color->setValue(1., ViewSpec::all(), 1);
    color->setValue(0., ViewSpec::all(), 2);
    color->setValue(1., ViewSpec::all(), 3);
    const std::string name = constant->getFullyQualifiedName();

    // What the delete commands' redo and undo do, with the same arguments.
    const NodesList outputs(1, material);
    constant->deactivate(outputs, false, false, true, false);
    constant->notifyOutputsOfDeletion();
    double missing[4];
    centreOf(renderColour(render, 1.), missing);
    for (int k = 0; k < 3; ++k) {
        EXPECT_NEAR(kLiveTextureMissingColour[k], missing[k], 0.03) << "deleted " << missing[0] << ", " << missing[1] << ", " << missing[2];
    }
    QString message;
    int type = 0;
    ASSERT_TRUE(material->hasPersistentMessage());
    material->getPersistentMessage(&message, &type, false);
    EXPECT_NE(-1, message.indexOf(QString::fromStdString(name))) << message.toStdString();

    constant->activate(outputs, false, false);
    double restored[4];
    centreOf(renderColour(render, 1.), restored);
    EXPECT_NEAR(0., restored[0], 0.03) << "restored " << restored[0] << ", " << restored[1] << ", " << restored[2];
    EXPECT_NEAR(1., restored[1], 0.03);
    EXPECT_NEAR(0., restored[2], 0.03);
    EXPECT_FALSE(material->hasPersistentMessage());
}

// The plan's animated Ramp is an animated Constant here: the Ramp is an OpenFX plug-in the test
// binary may not load, and an animated colour exercises the same per-frame URIs.
TEST_F(USDRenderStormTest, AnAnimatedLiveTextureFollowsTheFrame)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr constant;
    NodePtr render = createLiveTextureGraph(&constant);
    ASSERT_TRUE(render != nullptr);
    KnobColor* color = knob<KnobColor>(constant, kConstantParamColor);
    ASSERT_TRUE(color != nullptr);
    color->setValueAtTime(1., 1., ViewSpec::all(), 0);
    color->setValueAtTime(10., 0., ViewSpec::all(), 0);
    color->setValueAtTime(1., 0., ViewSpec::all(), 1);
    color->setValueAtTime(10., 1., ViewSpec::all(), 1);
    color->setValue(0., ViewSpec::all(), 2);
    color->setValue(1., ViewSpec::all(), 3);

    const Coverage first = renderCard(render, 1.);
    const Coverage tenth = renderCard(render, 10.);
    ASSERT_GT(first.count, 500);
    ASSERT_GT(tenth.count, 500);
    EXPECT_GT(first.rgb[0], 5. * first.rgb[1]) << "frame 1 " << first.rgb[0] << ", " << first.rgb[1] << ", " << first.rgb[2];
    EXPECT_GT(tenth.rgb[1], 5. * tenth.rgb[0]) << "frame 10 " << tenth.rgb[0] << ", " << tenth.rgb[1] << ", " << tenth.rgb[2];
}

TEST_F(USDRenderStormTest, StormProject3DWorld)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr plate, projector, card;
    NodePtr render = createProjectionGraph(&plate, &projector, &card);
    ASSERT_TRUE(render != nullptr);

    // The card covers render pixels 20..180 by 10..90.
    renderProjection(render, projector, 1., 0., RectI(16, 6, 184, 94), kGoldenCaseProject3DWorld);
}

// The direct connection's reference is the expected image: the assign must change nothing.
TEST_F(USDRenderStormTest, StormProject3DWorldThroughAssignMaterial)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr plate, projector, card;
    NodePtr render = createProjectionGraph(&plate, &projector, &card);
    ASSERT_TRUE(render != nullptr);
    NodePtr assign = assignProjectionToCard(render, projector, card);
    ASSERT_TRUE(assign != nullptr);

    renderProjection(render, projector, 1., 0., RectI(16, 6, 184, 94), kGoldenCaseProject3DWorld);
    EXPECT_FALSE(assign->hasPersistentMessage());
}

// The projection is unlit and alpha-blended: a half-transparent premultiplied plate, held straight
// in its live texture, shows as itself over the empty background, whether or not the scene has a
// light.
TEST_F(USDRenderStormTest, StormProject3DBlendsItsAlphaUnlit)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr plate, projector, card;
    NodePtr render = createProjectionGraph(&plate, &projector, &card);
    ASSERT_TRUE(render != nullptr);
    NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
    ASSERT_TRUE(bool(constant));
    KnobColor* colour = knob<KnobColor>(constant, kConstantParamColor);
    ASSERT_TRUE(colour != nullptr);
    const double premultiplied[4] = { 0.2, 0., 0., 0.5 };
    for (int c = 0; c < 4; ++c) {
        colour->setValue(premultiplied[c], ViewSpec::all(), c);
    }
    projector->disconnectInput(kUSDProject3DInputImage);
    connectNodes(constant, projector, kUSDProject3DInputImage, true);
    NodePtr merge = render->getInput(0)->getInput(0);
    ASSERT_TRUE(bool(merge));

    for (int lit = 0; lit < 2; ++lit) {
        if (lit) {
            NodePtr light = createNode(QString::fromUtf8(PLUGINID_NATRON_USDLIGHT));
            ASSERT_TRUE(bool(light));
            ASSERT_TRUE(setString(light, kSceneParamPrimPath, "/Key"));
            connectNodes(light, merge, 2, true);
        }
        std::vector<RenderedPlane> planes;
        std::string error;
        std::string shaderProblems;
        bool rendered = false;
        {
            USDDiagnostics diagnostics;
            rendered = renderOffMainThread(render, 1., &planes, &error);
            shaderProblems = diagnostics.shaderProblems();
        }
        EXPECT_TRUE(shaderProblems.empty()) << shaderProblems;
        ASSERT_TRUE(rendered) << error;
        ASSERT_EQ(3u, planes.size());
        const std::vector<float>& rgba = planes[0].pixels;
        ASSERT_EQ((std::size_t)kWidth * kHeight * 4, rgba.size());
        // The middle of the projector's frame, which spans 100 x 50 render pixels at the centre.
        double sum[4] = { 0., 0., 0., 0. };
        int count = 0;
        for (int y = kHeight / 2 - 10; y < kHeight / 2 + 10; ++y) {
            for (int x = kWidth / 2 - 20; x < kWidth / 2 + 20; ++x) {
                const std::size_t i = (std::size_t)y * (std::size_t)kWidth + (std::size_t)x;
                for (int c = 0; c < 4; ++c) {
                    sum[c] += rgba[i * 4 + c];
                }
                ++count;
            }
        }
        for (int c = 0; c < 4; ++c) {
            EXPECT_NEAR(premultiplied[c], sum[c] / count, 0.02) << (lit ? "lit" : "unlit") << ", channel " << c;
        }
    }
}

// In Geometry space with the camera held, the projection is taken where the card is at the hold
// frame and goes with it after: two units right at frame 11, the checker is there too.
TEST_F(USDRenderStormTest, StormProject3DGeometryHeld)
{
    if (skipWithoutGL()) {
        return;
    }
    double first = 0., last = 0.;
    getApp()->getFrameRange(&first, &last);
    ASSERT_LE(first, 1.);
    ASSERT_GE(last, 11.);
    NodePtr plate, projector, card;
    NodePtr render = createProjectionGraph(&plate, &projector, &card);
    ASSERT_TRUE(render != nullptr);
    KnobChoice* space = knob<KnobChoice>(projector, kUSDProject3DParamProjectionSpace);
    KnobBool* useHold = knob<KnobBool>(projector, kUSDProject3DParamUseFrameHold);
    KnobDouble* hold = knob<KnobDouble>(projector, kUSDProject3DParamFrameHold);
    KnobDouble* translate = knob<KnobDouble>(card, kSceneParamTranslate);
    ASSERT_TRUE(space && useHold && hold && translate);
    space->setValue((int)USDProject3D::eProjectionSpaceGeometry);
    useHold->setValue(true);
    hold->setValue(1.);
    const double shift = 2.;
    translate->setValueAtTime(1., 0., ViewSpec::all(), 0);
    translate->setValueAtTime(11., shift, ViewSpec::all(), 0);

    renderProjection(render, projector, 1., 0., RectI(), kGoldenCaseProject3DGeometryHeld);
    // The card covers render pixels 40..200 by 10..90; World space would leave the checker
    // 40 plate pixels, 1.6 squares, behind.
    renderProjection(render, projector, 11., shift, RectI(36, 6, 200, 94), kGoldenCaseProject3DGeometryHeld);
}

TEST_F(USDRenderStormTest, StormProject3DGeometryHeldThroughAssignMaterial)
{
    if (skipWithoutGL()) {
        return;
    }
    double first = 0., last = 0.;
    getApp()->getFrameRange(&first, &last);
    ASSERT_LE(first, 1.);
    ASSERT_GE(last, 11.);
    NodePtr plate, projector, card;
    NodePtr render = createProjectionGraph(&plate, &projector, &card);
    ASSERT_TRUE(render != nullptr);
    KnobChoice* space = knob<KnobChoice>(projector, kUSDProject3DParamProjectionSpace);
    KnobBool* useHold = knob<KnobBool>(projector, kUSDProject3DParamUseFrameHold);
    KnobDouble* hold = knob<KnobDouble>(projector, kUSDProject3DParamFrameHold);
    KnobDouble* translate = knob<KnobDouble>(card, kSceneParamTranslate);
    ASSERT_TRUE(space && useHold && hold && translate);
    space->setValue((int)USDProject3D::eProjectionSpaceGeometry);
    useHold->setValue(true);
    hold->setValue(1.);
    const double shift = 2.;
    translate->setValueAtTime(1., 0., ViewSpec::all(), 0);
    translate->setValueAtTime(11., shift, ViewSpec::all(), 0);
    NodePtr assign = assignProjectionToCard(render, projector, card);
    ASSERT_TRUE(assign != nullptr);

    renderProjection(render, projector, 11., shift, RectI(36, 6, 200, 94), kGoldenCaseProject3DGeometryHeld);
    EXPECT_FALSE(assign->hasPersistentMessage());
}

// A mirror after the assign reverses the card's winding on screen but not the frame it was placed
// with, so the face that looked at the projector there still does: Front shows it, Back hides it.
TEST_F(USDRenderStormTest, StormProject3DProjectOnFrontSurvivesAMirrorAfterTheAssign)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr plate, projector, card;
    NodePtr render = createProjectionGraph(&plate, &projector, &card);
    ASSERT_TRUE(render != nullptr);
    KnobChoice* space = knob<KnobChoice>(projector, kUSDProject3DParamProjectionSpace);
    KnobBool* useHold = knob<KnobBool>(projector, kUSDProject3DParamUseFrameHold);
    KnobDouble* hold = knob<KnobDouble>(projector, kUSDProject3DParamFrameHold);
    KnobChoice* projectOn = knob<KnobChoice>(projector, kUSDProject3DParamProjectOn);
    ASSERT_TRUE(space && useHold && hold && projectOn);
    space->setValue((int)USDProject3D::eProjectionSpaceGeometry);
    useHold->setValue(true);
    hold->setValue(1.);
    NodePtr assign = assignProjectionToCard(render, projector, card);
    ASSERT_TRUE(assign != nullptr);
    NodePtr merge = render->getInput(0)->getInput(0);
    NodePtr mirror = createNode(QString::fromUtf8(PLUGINID_NATRON_USDEDITTRANSFORM));
    ASSERT_TRUE(merge && mirror);
    ASSERT_TRUE(setString(mirror, kUSDEditTransformParamPrimPattern, "/Card"));
    ASSERT_TRUE(setTriple(mirror, kSceneParamScale, -1., 1., 1.));
    merge->disconnectInput(0);
    connectNodes(assign, mirror, 0, true);
    connectNodes(mirror, merge, 0, true);

    std::vector<float> rgba;
    projectOn->setValue((int)USDProject3D::eProjectOnFront);
    ASSERT_NO_FATAL_FAILURE(renderColour(render, "Front on the mirrored card", &rgba));
    RegionColours region = regionColours(rgba, 0, 0, kWidth, kHeight);
    EXPECT_GT(region.red, 100);
    EXPECT_GT(region.blue, 100);
    projectOn->setValue((int)USDProject3D::eProjectOnBack);
    ASSERT_NO_FATAL_FAILURE(renderColour(render, "Back on the mirrored card", &rgba));
    region = regionColours(rgba, 0, 0, kWidth, kHeight);
    EXPECT_EQ(0, region.red);
    EXPECT_EQ(0, region.blue);
    EXPECT_FALSE(assign->hasPersistentMessage());
}

// One material on two cards apart looks as the two grafted copies of a direct connection do.
TEST_F(USDRenderStormTest, StormProject3DTwoPlacementsThroughOneAssign)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr projector, cardA, cardB, cards;
    std::vector<float> reference;
    NodePtr render = createTwoPlacementGraph(&projector, &cardA, &cardB, &cards, &reference);
    ASSERT_TRUE(render != nullptr);
    NodePtr assign = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));
    ASSERT_TRUE(assign && setString(assign, kUSDAssignMaterialParamPrimPattern, "/Card /Card2"));
    connectNodes(cards, assign, kUSDAssignMaterialInputScene, true);
    connectNodes(projector, assign, kUSDAssignMaterialInputMaterial, true);
    connectNodes(assign, render->getInput(0)->getInput(0), 0, true);
    expectMatchesReference(render, reference, "one assign");
    EXPECT_FALSE(assign->hasPersistentMessage());
}

TEST_F(USDRenderStormTest, StormProject3DTwoPlacementsThroughTwoAssigns)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr projector, cardA, cardB, cards;
    std::vector<float> reference;
    NodePtr render = createTwoPlacementGraph(&projector, &cardA, &cardB, &cards, &reference);
    ASSERT_TRUE(render != nullptr);
    NodePtr assignA = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));
    NodePtr assignB = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));
    ASSERT_TRUE(assignA && setString(assignA, kUSDAssignMaterialParamPrimPattern, "/Card"));
    ASSERT_TRUE(assignB && setString(assignB, kUSDAssignMaterialParamPrimPattern, "/Card2"));
    connectNodes(cards, assignA, kUSDAssignMaterialInputScene, true);
    connectNodes(projector, assignA, kUSDAssignMaterialInputMaterial, true);
    connectNodes(assignA, assignB, kUSDAssignMaterialInputScene, true);
    connectNodes(projector, assignB, kUSDAssignMaterialInputMaterial, true);
    connectNodes(assignB, render->getInput(0)->getInput(0), 0, true);
    expectMatchesReference(render, reference, "two assigns");
}

TEST_F(USDRenderStormTest, StormProject3DTwoPlacementsByMaterialPath)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr projector, cardA, cardB, cards;
    std::vector<float> reference;
    NodePtr render = createTwoPlacementGraph(&projector, &cardA, &cardB, &cards, &reference);
    ASSERT_TRUE(render != nullptr);
    ASSERT_TRUE(setString(projector, kSceneParamPrimPath, "/Looks/Projection"));
    NodePtr assign = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));
    ASSERT_TRUE(assign && setString(assign, kUSDAssignMaterialParamPrimPattern, "/Card /Card2"));
    ASSERT_TRUE(setString(assign, kUSDAssignMaterialParamMaterialPath, "/Looks/Projection"));
    connectNodes(projector, cards, 2, true);
    connectNodes(cards, assign, kUSDAssignMaterialInputScene, true);
    connectNodes(assign, render->getInput(0)->getInput(0), 0, true);
    expectMatchesReference(render, reference, "Material Path");
    EXPECT_FALSE(assign->hasPersistentMessage());
}

// The file names the material and carries the primvar itself, so nothing but the surface is under
// test. Without the primvar, as with any binding no node places, the image stays where the camera is.
TEST_F(USDRenderStormTest, StormProject3DReferencePointsAuthoredByHandStick)
{
    if (skipWithoutGL()) {
        return;
    }
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const double shift = 2.;
    const auto writeBound = [&](const char* name, bool withPrimvar) {
        const QString file = tmp.path() + QLatin1Char('/') + QString::fromUtf8(name);
        writeMovingQuad(file.toStdString(), 0., shift, kCardWidth / 2., kCardHeight / 2.);
        const PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::Open(file.toStdString());
        const PXR_NS::UsdPrim prim = stage->GetPrimAtPath(PXR_NS::SdfPath("/Moving"));
        PXR_NS::UsdShadeMaterialBindingAPI::Apply(prim);
        prim.CreateRelationship(PXR_NS::UsdShadeTokens->materialBinding, false).SetTargets({ PXR_NS::SdfPath("/Looks/Projection") });
        if (withPrimvar) {
            PXR_NS::UsdGeomPrimvarsAPI(prim).CreatePrimvar(PXR_NS::TfToken("natronReferencePointsHold1"), PXR_NS::SdfValueTypeNames->Point3fArray, PXR_NS::UsdGeomTokens->vertex).Set(quadPoints(0., kCardWidth / 2., kCardHeight / 2.));
        }
        EXPECT_TRUE(stage->GetRootLayer()->Save());

        return file;
    };
    const QString bare = writeBound("bare.usda", false);
    const QString sticky = writeBound("sticky.usda", true);
    NodePtr projector;
    NodePtr render = createHeldGraphReading(sticky, &projector);
    ASSERT_TRUE(render != nullptr);
    ASSERT_TRUE(setString(projector, kSceneParamPrimPath, "/Looks/Projection"));
    connectNodes(projector, render->getInput(0)->getInput(0), 2, true);

    renderProjection(render, projector, 1., 0., RectI(), "reference points at the hold");
    renderProjection(render, projector, 11., shift, RectI(), "reference points after the points moved");

    NodePtr reader = render->getInput(0)->getInput(0)->getInput(0);
    ASSERT_TRUE(reader && knob<KnobFile>(reader, kUSDReadParamFile));
    knob<KnobFile>(reader, kUSDReadParamFile)->setValue(bare.toStdString());
    std::vector<float> atHold, later;
    ASSERT_NO_FATAL_FAILURE(renderColour(render, "no reference points at the hold", &atHold, 1.));
    ASSERT_NO_FATAL_FAILURE(renderColour(render, "no reference points after the points moved", &later, 11.));
    ASSERT_EQ(atHold.size(), later.size());
    // The projector's frame, which the quad covers at both frames.
    int different = 0;
    for (int y = 30; y < 70; ++y) {
        for (int x = 55; x < 145; ++x) {
            const std::size_t i = ((std::size_t)y * kWidth + x) * 4;
            different += (classifyColour(&atHold[i]) == classifyColour(&later[i])) ? 0 : 1;
        }
    }
    EXPECT_EQ(0, different);
}

// The quad turns half a revolution about Y by frame 11. Judged on the surface as it is then, Front would
// hide it and Back show it.
TEST_F(USDRenderStormTest, StormProject3DProjectOnKeepsTheFacesOfTheHoldAsTheMeshTurns)
{
    if (skipWithoutGL()) {
        return;
    }
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString file = tmp.path() + QString::fromUtf8("/turning.usda");
    {
        const PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::CreateNew(file.toStdString());
        ASSERT_TRUE(bool(stage));
        const PXR_NS::UsdGeomMesh mesh = defineQuad(stage, "/Moving");
        const PXR_NS::VtVec3fArray front = quadPoints(0., kCardWidth / 2., kCardHeight / 2.);
        PXR_NS::VtVec3fArray turned = front;
        for (PXR_NS::GfVec3f& point : turned) {
            point[0] = -point[0];
        }
        mesh.CreatePointsAttr().Set(front, PXR_NS::UsdTimeCode(1.));
        mesh.CreatePointsAttr().Set(turned, PXR_NS::UsdTimeCode(11.));
        PXR_NS::UsdGeomPrimvarsAPI(mesh.GetPrim()).CreatePrimvar(PXR_NS::TfToken("natronReferencePointsHold1"), PXR_NS::SdfValueTypeNames->Point3fArray, PXR_NS::UsdGeomTokens->vertex).Set(front);
        PXR_NS::UsdShadeMaterialBindingAPI::Apply(mesh.GetPrim());
        mesh.GetPrim().CreateRelationship(PXR_NS::UsdShadeTokens->materialBinding, false).SetTargets({ PXR_NS::SdfPath("/Looks/Projection") });
        ASSERT_TRUE(stage->GetRootLayer()->Save());
    }
    NodePtr projector;
    NodePtr render = createHeldGraphReading(file, &projector);
    ASSERT_TRUE(render != nullptr);
    ASSERT_TRUE(setString(projector, kSceneParamPrimPath, "/Looks/Projection"));
    connectNodes(projector, render->getInput(0)->getInput(0), 2, true);
    KnobChoice* projectOn = knob<KnobChoice>(projector, kUSDProject3DParamProjectOn);
    ASSERT_TRUE(projectOn != nullptr);

    for (const double frame : { 1., 11. }) {
        std::vector<float> rgba;
        projectOn->setValue((int)USDProject3D::eProjectOnFront);
        ASSERT_NO_FATAL_FAILURE(renderColour(render, "Front", &rgba, frame));
        RegionColours region = regionColours(rgba, 0, 0, kWidth, kHeight);
        EXPECT_GT(region.red, 100) << "Front at frame " << frame;
        EXPECT_GT(region.blue, 100) << "Front at frame " << frame;
        projectOn->setValue((int)USDProject3D::eProjectOnBack);
        ASSERT_NO_FATAL_FAILURE(renderColour(render, "Back", &rgba, frame));
        region = regionColours(rgba, 0, 0, kWidth, kHeight);
        EXPECT_EQ(0, region.red) << "Back at frame " << frame;
        EXPECT_EQ(0, region.blue) << "Back at frame " << frame;
    }
}

// The moving quad stands where the second card does and moves as far, so a stuck image renders the
// same pixels, beside a prim that takes the rigid path in the same material.
TEST_F(USDRenderStormTest, StormProject3DDeformingMeshBesideARigidPrimThroughTheMaterialInput)
{
    if (skipWithoutGL()) {
        return;
    }
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString file = tmp.path() + QString::fromUtf8("/moving.usda");
    writeMovingQuad(file.toStdString(), 2.5, 4.5, 2., 2.);
    NodePtr projector, cardA, cardB, cards;
    std::vector<float> reference;
    NodePtr render = createTwoPlacementGraph(&projector, &cardA, &cardB, &cards, &reference);
    ASSERT_TRUE(render != nullptr);
    ASSERT_TRUE(readInPlaceOfSecondCard(cards, file) != nullptr);
    NodePtr assign = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));
    ASSERT_TRUE(assign && setString(assign, kUSDAssignMaterialParamPrimPattern, "/Card /Moving"));
    connectNodes(cards, assign, kUSDAssignMaterialInputScene, true);
    connectNodes(projector, assign, kUSDAssignMaterialInputMaterial, true);
    connectNodes(assign, render->getInput(0)->getInput(0), 0, true);
    expectMatchesReference(render, reference, "a deforming mesh through the Material input", kReferencePathTolerance);
    EXPECT_FALSE(assign->hasPersistentMessage());
}

TEST_F(USDRenderStormTest, StormProject3DDeformingMeshByMaterialPath)
{
    if (skipWithoutGL()) {
        return;
    }
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString file = tmp.path() + QString::fromUtf8("/moving.usda");
    writeMovingQuad(file.toStdString(), 2.5, 4.5, 2., 2.);
    NodePtr projector, cardA, cardB, cards;
    std::vector<float> reference;
    NodePtr render = createTwoPlacementGraph(&projector, &cardA, &cardB, &cards, &reference);
    ASSERT_TRUE(render != nullptr);
    ASSERT_TRUE(readInPlaceOfSecondCard(cards, file) != nullptr);
    ASSERT_TRUE(setString(projector, kSceneParamPrimPath, "/Looks/Projection"));
    NodePtr assign = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));
    ASSERT_TRUE(assign && setString(assign, kUSDAssignMaterialParamPrimPattern, "/Card /Moving"));
    ASSERT_TRUE(setString(assign, kUSDAssignMaterialParamMaterialPath, "/Looks/Projection"));
    connectNodes(projector, cards, 2, true);
    connectNodes(cards, assign, kUSDAssignMaterialInputScene, true);
    connectNodes(assign, render->getInput(0)->getInput(0), 0, true);
    expectMatchesReference(render, reference, "a deforming mesh by Material Path", kReferencePathTolerance);
    EXPECT_FALSE(assign->hasPersistentMessage());
}

TEST_F(USDRenderStormTest, StormProject3DSkinnedMeshSticks)
{
    if (skipWithoutGL()) {
        return;
    }
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString file = tmp.path() + QString::fromUtf8("/skinned.usda");
    writeSkinnedQuad(file.toStdString(), 2.5, 2., 2., 2.);
    NodePtr projector, cardA, cardB, cards;
    std::vector<float> reference;
    NodePtr render = createTwoPlacementGraph(&projector, &cardA, &cardB, &cards, &reference);
    ASSERT_TRUE(render != nullptr);
    ASSERT_TRUE(readInPlaceOfSecondCard(cards, file) != nullptr);
    NodePtr assign = createNode(QString::fromUtf8(PLUGINID_NATRON_USDASSIGNMATERIAL));
    ASSERT_TRUE(assign && setString(assign, kUSDAssignMaterialParamPrimPattern, "/Card /Rig/Moving"));
    connectNodes(cards, assign, kUSDAssignMaterialInputScene, true);
    connectNodes(projector, assign, kUSDAssignMaterialInputMaterial, true);
    connectNodes(assign, render->getInput(0)->getInput(0), 0, true);
    expectMatchesReference(render, reference, "a skinned mesh", kReferencePathTolerance);
    EXPECT_FALSE(assign->hasPersistentMessage());
}

// Moving two units is 20 render pixels at the prims' depth. Project On has no faces to judge on them.
TEST_F(USDRenderStormTest, StormProject3DDeformingCurvesAndPointsStick)
{
    if (skipWithoutGL()) {
        return;
    }
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const float shift = 2.f;
    const auto sampleGrid = [&](float offset) {
        PXR_NS::VtVec3fArray points;
        for (int y = -2; y <= 2; ++y) {
            for (int x = -10; x <= 10; ++x) {
                points.push_back(PXR_NS::GfVec3f(x * 0.5f + offset, (float)y, 0.f));
            }
        }

        return points;
    };
    const QString curvesFile = tmp.path() + QString::fromUtf8("/curves.usda");
    const QString pointsFile = tmp.path() + QString::fromUtf8("/points.usda");
    {
        const PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::CreateNew(curvesFile.toStdString());
        ASSERT_TRUE(bool(stage));
        PXR_NS::UsdGeomBasisCurves curves = PXR_NS::UsdGeomBasisCurves::Define(stage, PXR_NS::SdfPath("/Moving"));
        curves.CreateTypeAttr().Set(PXR_NS::UsdGeomTokens->linear);
        curves.CreateCurveVertexCountsAttr().Set(PXR_NS::VtIntArray(5, 21));
        curves.CreateWidthsAttr().Set(PXR_NS::VtFloatArray(1, 0.4f));
        curves.SetWidthsInterpolation(PXR_NS::UsdGeomTokens->constant);
        curves.CreatePointsAttr().Set(sampleGrid(0.f), PXR_NS::UsdTimeCode(1.));
        curves.CreatePointsAttr().Set(sampleGrid(shift), PXR_NS::UsdTimeCode(11.));
        ASSERT_TRUE(stage->GetRootLayer()->Save());
    }
    {
        const PXR_NS::UsdStageRefPtr stage = PXR_NS::UsdStage::CreateNew(pointsFile.toStdString());
        ASSERT_TRUE(bool(stage));
        PXR_NS::UsdGeomPoints points = PXR_NS::UsdGeomPoints::Define(stage, PXR_NS::SdfPath("/Moving"));
        points.CreateWidthsAttr().Set(PXR_NS::VtFloatArray(1, 0.4f));
        points.SetWidthsInterpolation(PXR_NS::UsdGeomTokens->constant);
        points.CreatePointsAttr().Set(sampleGrid(0.f), PXR_NS::UsdTimeCode(1.));
        points.CreatePointsAttr().Set(sampleGrid(shift), PXR_NS::UsdTimeCode(11.));
        ASSERT_TRUE(stage->GetRootLayer()->Save());
    }
    NodePtr projector;
    NodePtr render = createHeldGraphReading(curvesFile, &projector);
    ASSERT_TRUE(render != nullptr);
    NodePtr reader = render->getInput(0)->getInput(0)->getInput(0);
    NodePtr assign = assignProjection(render, projector, "/Moving");
    ASSERT_TRUE(reader && assign);
    KnobChoice* projectOn = knob<KnobChoice>(projector, kUSDProject3DParamProjectOn);
    ASSERT_TRUE(projectOn != nullptr);

    for (const QString& file : { curvesFile, pointsFile }) {
        const std::string name = file.toStdString();
        knob<KnobFile>(reader, kUSDReadParamFile)->setValue(name);
        projectOn->setValue((int)USDProject3D::eProjectOnBoth);
        std::vector<float> atHold, later, front;
        ASSERT_NO_FATAL_FAILURE(renderColour(render, name.c_str(), &atHold, 1.));
        ASSERT_NO_FATAL_FAILURE(renderColour(render, name.c_str(), &later, 11.));
        expectColoursMovedBy(atHold, later, 20, name.c_str());
        EXPECT_FALSE(assign->hasPersistentMessage()) << name;
        projectOn->setValue((int)USDProject3D::eProjectOnFront);
        ASSERT_NO_FATAL_FAILURE(renderColour(render, name.c_str(), &front, 11.));
        expectColoursMovedBy(atHold, front, 20, name.c_str());
    }
}

// The plate's data reaches 50 pixels beyond its format on every side; its format still fills the
// projector's frame, and Crop keeps the rest off the card.
TEST_F(USDRenderStormTest, StormProject3DOverscan)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr plate, projector, card;
    NodePtr render = createProjectionGraph(&plate, &projector, &card);
    ASSERT_TRUE(render != nullptr);
    KnobChoice* extent = knob<KnobChoice>(plate, kNativeGeneratorParamExtent);
    KnobDouble* bottomLeft = knob<KnobDouble>(plate, kNativeGeneratorParamBottomLeft);
    KnobDouble* size = knob<KnobDouble>(plate, kNativeGeneratorParamSize);
    KnobBool* crop = knob<KnobBool>(projector, kUSDProject3DParamCrop);
    ASSERT_TRUE(extent && bottomLeft && size && crop);
    extent->setValue((int)ExtentKnobs::eExtentSize);
    bottomLeft->setValue(-50., ViewSpec::all(), 0);
    bottomLeft->setValue(-25., ViewSpec::all(), 1);
    size->setValue(kWidth + 100., ViewSpec::all(), 0);
    size->setValue(kHeight + 50., ViewSpec::all(), 1);
    crop->setValue(true);

    renderProjection(render, projector, 1., 0., RectI(16, 6, 184, 94), kGoldenCaseProject3DOverscan);
}

// With Crop on, the card around the projector's frame is transparent, not opaque black, and the
// frame is opaque.
TEST_F(USDRenderStormTest, StormProject3DCropIsTransparentOutsideTheFrame)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr plate, projector, card;
    NodePtr render = createProjectionGraph(&plate, &projector, &card);
    ASSERT_TRUE(render != nullptr);
    KnobBool* crop = knob<KnobBool>(projector, kUSDProject3DParamCrop);
    KnobChoice* outside = knob<KnobChoice>(projector, kUSDProject3DParamOutside);
    ASSERT_TRUE(crop && outside);
    crop->setValue(true);
    outside->setValue((int)USDProject3D::eOutsideBlack);

    std::vector<float> rgba;
    renderColour(render, "crop", &rgba);
    ASSERT_FALSE(rgba.empty());
    EXPECT_FALSE(projector->hasPersistentMessage());
    // The card spans render pixels 20..180 by 10..90 and the frame 50..150 by 25..75.
    for (const RectI& band : { RectI(24, 14, 46, 86), RectI(154, 14, 176, 86), RectI(54, 14, 146, 21), RectI(54, 79, 146, 86) }) {
        const RegionColours around = regionColours(rgba, band.x1, band.y1, band.x2, band.y2);
        EXPECT_LT(around.maximum, 0.02f) << "around the frame, " << band.x1 << ", " << band.y1 << " to " << band.x2 << ", " << band.y2;
    }
    const RegionColours inside = regionColours(rgba, 54, 29, 146, 71);
    EXPECT_EQ(inside.count, inside.opaque);
    EXPECT_GT(inside.red, inside.count / 3);
    EXPECT_GT(inside.blue, inside.count / 3);
}

// A Camera Path with nothing on the Camera input is read from the scene the projection renders in:
// the camera merged in beside the card projects as it would through the Camera input, and neither
// node warns. A path that scene lacks too is USDRender's warning.
TEST_F(USDRenderStormTest, StormProject3DCameraPathFromTheMergedScene)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr plate, projector, card;
    NodePtr render = createProjectionGraph(&plate, &projector, &card);
    ASSERT_TRUE(render != nullptr);
    NodePtr projectorCamera = projector->getInput(kUSDProject3DInputCamera);
    NodePtr merge = render->getInput(0)->getInput(0);
    ASSERT_TRUE(projectorCamera && merge);
    projector->disconnectInput(kUSDProject3DInputCamera);
    ASSERT_TRUE(setString(projector, kUSDProject3DParamCameraPath, "/Projector"));
    connectNodes(projectorCamera, merge, 2, true);

    renderProjection(render, projector, 1., 0., RectI(), "StormProject3DCameraPathFromTheMergedScene");

    ASSERT_TRUE(setString(projector, kUSDProject3DParamCameraPath, "/Nowhere"));
    std::vector<float> rgba;
    renderColour(render, "missing camera", &rgba);
    EXPECT_FALSE(projector->hasPersistentMessage());
    ASSERT_TRUE(render->hasPersistentMessage());
    QString message;
    int type = -1;
    render->getPersistentMessage(&message, &type, false);
    EXPECT_EQ((int)eMessageTypeWarning, type);
    EXPECT_TRUE(message.contains(QString::fromUtf8("/Nowhere"))) << message.toStdString();
}

// A disabled node above the projector renders as its input, and so does the projected texture.
TEST_F(USDRenderStormTest, StormProject3DFollowsADisabledNodeToItsInput)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr plate, projector, card;
    NodePtr render = createProjectionGraph(&plate, &projector, &card);
    ASSERT_TRUE(render != nullptr);
    NodePtr invert = createNode(QString::fromUtf8(PLUGINID_NATRON_INVERT), PLUGIN_MAJOR_NATRON_INVERT);
    ASSERT_TRUE(bool(invert));
    connectNodes(plate, invert, 0, true);
    projector->disconnectInput(kUSDProject3DInputImage);
    connectNodes(invert, projector, kUSDProject3DInputImage, true);
    invert->setNodeDisabled(true);

    renderProjection(render, projector, 1., 0., RectI(), "StormProject3DFollowsADisabledNodeToItsInput");
}

// The image's data window ends inside the frame: a checker cropped to its bottom half and moved up
// a quarter of the frame. Above and below the data the card is transparent black, not the data's
// edge stretched out.
TEST_F(USDRenderStormTest, StormProject3DIsTransparentBeyondTheDataWindow)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr plate, projector, card;
    NodePtr render = createProjectionGraph(&plate, &projector, &card);
    ASSERT_TRUE(render != nullptr);
    NodePtr crop = createNode(QString::fromUtf8(PLUGINID_NATRON_CROP), PLUGIN_MAJOR_NATRON_CROP);
    NodePtr transform = createNode(QString::fromUtf8(PLUGINID_NATRON_TRANSFORM), PLUGIN_MAJOR_NATRON_TRANSFORM);
    ASSERT_TRUE(crop && transform);
    KnobDouble* bottomLeft = knob<KnobDouble>(crop, kNativeGeneratorParamBottomLeft);
    KnobDouble* size = knob<KnobDouble>(crop, kNativeGeneratorParamSize);
    KnobDouble* softness = knob<KnobDouble>(crop, kCropParamSoftness);
    KnobBool* blackOutside = knob<KnobBool>(crop, kCropParamBlackOutside);
    KnobDouble* translate = knob<KnobDouble>(transform, kTransformNodeParamTranslate);
    ASSERT_TRUE(bottomLeft && size && softness && blackOutside && translate);
    bottomLeft->setValue(0., ViewSpec::all(), 0);
    bottomLeft->setValue(0., ViewSpec::all(), 1);
    size->setValue(kWidth, ViewSpec::all(), 0);
    size->setValue(kHeight / 2., ViewSpec::all(), 1);
    softness->setValue(0.);
    blackOutside->setValue(true);
    translate->setValue(kHeight / 4., ViewSpec::all(), 1);
    connectNodes(plate, crop, 0, true);
    connectNodes(crop, transform, 0, true);
    projector->disconnectInput(kUSDProject3DInputImage);
    connectNodes(transform, projector, kUSDProject3DInputImage, true);

    for (int cropOn = 0; cropOn < 2; ++cropOn) {
        KnobBool* cropKnob = knob<KnobBool>(projector, kUSDProject3DParamCrop);
        ASSERT_TRUE(cropKnob != nullptr);
        cropKnob->setValue(cropOn != 0);
        const char* what = cropOn ? "crop on" : "crop off";
        std::vector<float> rgba;
        renderColour(render, what, &rgba);
        ASSERT_FALSE(rgba.empty());
        EXPECT_FALSE(projector->hasPersistentMessage()) << what;
        // The frame spans render rows 25..75, two plate rows a render row; the data plate rows
        // 25..75, render rows 37..63 (a pixel of Crop's black edge beyond them).
        for (const RectI& band : { RectI(54, 27, 146, 34), RectI(54, 66, 146, 73) }) {
            const RegionColours beyond = regionColours(rgba, band.x1, band.y1, band.x2, band.y2);
            EXPECT_LT(beyond.maximum, 0.02f) << what << ", rows " << band.y1 << " to " << band.y2;
        }
        const RegionColours data = regionColours(rgba, 54, 41, 146, 59);
        EXPECT_GT(data.red + data.blue, data.count * 3 / 4) << what;
    }
}

// A quad whose u runs from -0.5 to 1, bound to an emissive material showing a Constant that covers
// half a format past the format's left edge and the format's left half. Coordinates 0 to 1 span the
// format, so the overscan shows at negative u, the left third of the quad: the image covers its
// left two thirds, and the right third, the format's right half, is black.
TEST_F(USDRenderStormTest, OverscanShowsWhereTextureCoordinatesLeaveZeroToOne)
{
    if (skipWithoutGL()) {
        return;
    }
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString quadPath = tmp.path() + QString::fromUtf8("/quad.usda");
    {
        QFile file(quadPath);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("#usda 1.0\n"
                   "(\n    upAxis = \"Y\"\n    metersPerUnit = 1\n)\n"
                   "def Mesh \"Quad\" (\n    prepend apiSchemas = [\"MaterialBindingAPI\"]\n)\n{\n"
                   "    int[] faceVertexCounts = [4]\n"
                   "    int[] faceVertexIndices = [0, 1, 2, 3]\n"
                   "    point3f[] points = [(-1.5, -0.5, 0), (1.5, -0.5, 0), (1.5, 0.5, 0), (-1.5, 0.5, 0)]\n"
                   "    texCoord2f[] primvars:st = [(-0.5, 0), (1, 0), (1, 1), (-0.5, 1)] (\n        interpolation = \"vertex\"\n    )\n"
                   "    uniform token subdivisionScheme = \"none\"\n"
                   "    rel material:binding = </Live>\n"
                   "}\n");
    }

    Format format;
    getApp()->getProject()->getProjectDefaultFormat(&format);
    ASSERT_GT(format.width(), 8);
    NodePtr constant = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
    NodePtr material = createNode(QString::fromUtf8(PLUGINID_NATRON_USDEMISSIVEMATERIAL));
    NodePtr reader = createNode(QString::fromUtf8(PLUGINID_NATRON_USDREAD));
    NodePtr camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
    NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
    NodePtr render = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
    ASSERT_TRUE(constant && material && reader && camera && merge && render);
    KnobColor* color = knob<KnobColor>(constant, kConstantParamColor);
    KnobChoice* extent = knob<KnobChoice>(constant, kNativeGeneratorParamExtent);
    KnobDouble* bottomLeft = knob<KnobDouble>(constant, kNativeGeneratorParamBottomLeft);
    KnobDouble* size = knob<KnobDouble>(constant, kNativeGeneratorParamSize);
    KnobFile* file = knob<KnobFile>(reader, kUSDReadParamFile);
    ASSERT_TRUE(color && extent && bottomLeft && size && file);
    color->setValue(1., ViewSpec::all(), 0);
    color->setValue(0., ViewSpec::all(), 1);
    color->setValue(0., ViewSpec::all(), 2);
    color->setValue(1., ViewSpec::all(), 3);
    extent->setValueFromID(kNativeGeneratorExtentSize, 0);
    const int half = format.width() / 2;
    bottomLeft->setValue(format.x1 - half, ViewSpec::all(), 0);
    bottomLeft->setValue(format.y1, ViewSpec::all(), 1);
    size->setValue(2 * half, ViewSpec::all(), 0);
    size->setValue(format.height(), ViewSpec::all(), 1);
    file->setValue(quadPath.toStdString());
    ASSERT_TRUE(setString(material, kSceneParamPrimPath, "/Live"));
    KnobBool* useAlpha = knob<KnobBool>(material, kUSDEmissiveMaterialParamUseAlpha);
    ASSERT_TRUE(useAlpha != nullptr);
    // The quad is found by its prim id, which Storm does not write for a surface with an opacity.
    useAlpha->setValue(false);
    ASSERT_TRUE(setString(camera, kSceneParamPrimPath, "/Cam"));
    ASSERT_TRUE(setTriple(camera, kSceneParamTranslate, 0., 0., 10.));
    connectNodes(constant, material, kUSDEmissiveMaterialInputEmissive, true);
    connectNodes(reader, merge, 0, true);
    connectNodes(material, merge, 1, true);
    connectNodes(camera, merge, 2, true);
    NodePtr settings = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDERSETTINGS));
    ASSERT_TRUE(settings != nullptr);
    connectNodes(merge, settings, 0, true);
    connectNodes(settings, render, 0, true);
    ASSERT_TRUE(RenderSettingsTestUtils::configure(settings, "/Cam", kWidth, kHeight));

    std::vector<RenderedPlane> planes;
    std::string error;
    ASSERT_TRUE(renderOffMainThread(render, 1., &planes, &error)) << error;
    ASSERT_EQ(3u, planes.size());
    const std::vector<float>& rgba = planes[0].pixels;
    const std::vector<float>& primId = planes[2].pixels;
    const std::uint32_t quadId = bitsOf(HydraRender::hashToFloat(HydraRender::primPathHash("/Quad")));
    int left = kWidth;
    int right = -1;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            if (bitsOf(primId[(std::size_t)y * (std::size_t)kWidth + (std::size_t)x]) == quadId) {
                left = std::min(left, x);
                right = std::max(right, x);
            }
        }
    }
    ASSERT_GT(right - left, 30) << "the quad is drawn";

    // Mean red and green over the quad's pixels whose horizontal position across it is in [from, to].
    const auto meanOver = [&](double from,
                              double to,
                              double rg[2]) {
        int count = 0;
        rg[0] = rg[1] = 0.;
        for (int y = 0; y < kHeight; ++y) {
            for (int x = 0; x < kWidth; ++x) {
                const std::size_t i = (std::size_t)y * (std::size_t)kWidth + (std::size_t)x;
                const double across = (x - left + 0.5) / (double)(right - left + 1);
                if ((bitsOf(primId[i]) != quadId) || (across < from) || (across > to)) {
                    continue;
                }
                rg[0] += rgba[i * 4 + 0];
                rg[1] += rgba[i * 4 + 1];
                ++count;
            }
        }
        if (count) {
            rg[0] /= count;
            rg[1] /= count;
        }

        return count;
    };
    double overscan[2];
    double formatLeft[2];
    double formatRight[2];
    ASSERT_GT(meanOver(0.05, 0.28, overscan), 50);
    ASSERT_GT(meanOver(0.39, 0.61, formatLeft), 50);
    ASSERT_GT(meanOver(0.72, 0.95, formatRight), 50);
    EXPECT_GT(overscan[0], 0.9) << "u < 0 reads the overscan, red " << overscan[0];
    EXPECT_LT(overscan[1], 0.05);
    EXPECT_GT(formatLeft[0], 0.9) << "0 < u < 0.5 is the format's left half, red " << formatLeft[0];
    EXPECT_LT(formatRight[0], 0.05) << "0.5 < u < 1 is the format's right half the image does not cover, red " << formatRight[0];
}

TEST_F(USDRenderStormTest, ASharedMaterialAssignedByPatternRendersAsBound)
{
    if (skipWithoutGL()) {
        return;
    }
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString assetPath = tmp.filePath(QString::fromUtf8("asset.usda"));
    {
        QFile f(assetPath);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(kGreyAsset);
    }

    struct Case {
        USDAssignMaterial::StrengthEnum strength;
        bool secondAssign;
        int dominant; // the channel the box's colour is led by, or -1 for grey
    };
    // Stronger overrides the box's own grey binding; Weaker leaves it; a later assign wins.
    const Case cases[] = {
        { USDAssignMaterial::eStrengthStrongerThanDescendants, false, 0 },
        { USDAssignMaterial::eStrengthWeakerThanDescendants, false, -1 },
        { USDAssignMaterial::eStrengthStrongerThanDescendants, true, 2 },
    };
    for (const Case& c : cases) {
        NodePtr node = createAssignMaterialGraph(assetPath, c.strength, c.secondAssign);
        ASSERT_TRUE(node != nullptr);
        std::vector<RenderedPlane> planes;
        std::string error;
        ASSERT_TRUE(renderNodePlanesDirect(node, 1., ViewIdx(0), 0, RectI(0, 0, kWidth, kHeight), allPlanes(), &planes, &error)) << error;
        EXPECT_FALSE(node->hasPersistentMessage());
        ASSERT_EQ(3u, planes.size());

        const Coverage box = coverageOf("/T1/Asset/Box", planes[0].pixels, planes[1].pixels, planes[2].pixels);
        ASSERT_GT(box.count, 100) << "case " << c.dominant;
        const std::string colour = "box colour " + std::to_string(box.rgb[0]) + ", " + std::to_string(box.rgb[1]) + ", " + std::to_string(box.rgb[2]);
        if (c.dominant < 0) {
            EXPECT_GT(box.rgb[0], 0.1) << colour;
            EXPECT_NEAR(box.rgb[0], box.rgb[1], 0.05) << colour;
            EXPECT_NEAR(box.rgb[0], box.rgb[2], 0.05) << colour;
        } else {
            for (int k = 0; k < 3; ++k) {
                if (k != c.dominant) {
                    EXPECT_GT(box.rgb[c.dominant], 3. * box.rgb[k]) << colour;
                }
            }
        }
        if ((c.strength == USDAssignMaterial::eStrengthStrongerThanDescendants) && !c.secondAssign) {
            // The middle of the frame, where the box stands.
            checkCropAgainstReference(planes[0], RectI(kWidth / 2 - 32, kHeight / 2 - 32, kWidth / 2 + 32, kHeight / 2 + 32), kGoldenCaseAssignMaterial);
        }
    }
} // TEST_F

// An exception out of Hydra fails the render with its text on the node and leaves the render thread
// able to render again.
TEST_F(USDRenderStormTest, AnExceptionFromHydraFailsTheRenderAndTheNextRenderSucceeds)
{
    if (skipWithoutGL()) {
        return;
    }
    NodePtr node = createReferenceGraph();
    ASSERT_TRUE(node != nullptr);

    RenderDelegates::setRenderFaultHook([]() { throw std::runtime_error("texture commit failed"); });
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool failed = renderOffMainThread(node, 1., &planes, &error);
    RenderDelegates::setRenderFaultHook(nullptr);

    EXPECT_FALSE(failed);
    ASSERT_TRUE(node->hasPersistentMessage());
    QString message;
    int type = 0;
    node->getPersistentMessage(&message, &type, false);
    EXPECT_NE(std::string::npos, message.toStdString().find("texture commit failed")) << message.toStdString();

    planes.clear();
    error.clear();
    ASSERT_TRUE(renderOffMainThread(node, 1., &planes, &error)) << error;
    ASSERT_EQ(3u, planes.size());
    checkAgainstReference(planes[0], kGoldenCase);
}
