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

#include "Global/Macros.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include <gtest/gtest.h>

#include <OpenImageIO/imageio.h>

#include <QDir>
#include <QFileInfo>
#include <QString>

#include "BaseTest.h"
#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobFile.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Generator/CheckerBoard.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/Metadata/ImageMetadata.h"
#include "Engine/Nodes/Scene/HydraRenderImpl.h"
#include "Engine/Nodes/Scene/SceneNodeKnobs.h"
#include "Engine/Nodes/Scene/SceneQueryPool.h"
#include "Engine/Nodes/Scene/USDCamera.h"
#include "Engine/Nodes/Scene/USDCard.h"
#include "Engine/Nodes/Scene/USDEditTransform.h"
#include "Engine/Nodes/Scene/USDLight.h"
#include "Engine/Nodes/Scene/USDMaterial.h"
#include "Engine/Nodes/Scene/USDMerge.h"
#include "Engine/Nodes/Scene/USDProject3D.h"
#include "Engine/Nodes/Scene/USDPrune.h"
#include "Engine/Nodes/Scene/USDRead.h"
#include "Engine/Nodes/Scene/USDRender.h"
#include "Engine/Nodes/Scene/USDSphere.h"
#include "Engine/Nodes/Scene/USDXformPrim.h"
#include "Engine/Project.h"
#include "Engine/ViewIdx.h"
#include "RenderBothWays.h"
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

/**
 * How far the colour may drift from the reference: GL drivers rasterise and shade with their own
 * rounding, and edges land on other pixels. A pixel differs when a channel is off by more than
 * kChannelTolerance; at most kDifferingFraction of them may, and the mean difference stays below
 * kMeanTolerance.
 **/
const float kChannelTolerance = 0.02f;
const double kDifferingFraction = 0.005;
const double kMeanTolerance = 0.002;

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
                          const char* goldenCase)
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
        bool differs = false;
        for (int k = 0; k < nComps; ++k) {
            const float d = std::fabs(pixels[p * nComps + k] - reference[p * nComps + k]);
            sum += d;
            worst = std::max(worst, d);
            differs = differs || (d > kChannelTolerance);
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
 * sum to an even number and blue elsewhere; the card, 16 x 8 units, is unlit and Crop shows
 * black around the frame.
 **/
const double kRenderCameraZ = 10.;
const double kProjectorZ = 5.;
const double kPlateBox = 25.;
const double kCardWidth = 16.;
const double kCardHeight = 8.;

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
    eProjectedBlack
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
        return eProjectedBlack;
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
    if ((std::max(r, std::max(g, b)) < 0.02f) && (rgba[3] > 0.5f)) {
        return eProjectedBlack;
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
    case eProjectedBlack:
        return "black";
    case eProjectedUnsure:
        break;
    }

    return "neither red, blue nor black";
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
    const std::vector<float>& primId = planes[2].pixels;
    ASSERT_EQ((std::size_t)kWidth * kHeight * 4, rgba.size()) << what;
    ASSERT_EQ((std::size_t)kWidth * kHeight, primId.size()) << what;
    const std::uint32_t cardId = bitsOf(HydraRender::hashToFloat(HydraRender::primPathHash("/Card")));
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
            if (bitsOf(primId[i]) != cardId) {
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
            double u = 0., v = 0.;
            planeToPlate(localX, Y, &u, &v);
            const ProjectedColourEnum expected = expectedProjection(u, v, margin);
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
    EXPECT_GT(counts[eProjectedBlack], 2000) << what;
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
        KnobChoice* renderCamera = knob<KnobChoice>(render, kUSDRenderParamCamera);
        KnobInt* size = knob<KnobInt>(render, kNatronParamFormatSize);
        KnobDouble* par = knob<KnobDouble>(render, kNatronParamFormatPar);
        if (!file || !diffuse || !renderCamera || !size || !par) {
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
        connectNodes(prune, render, 0, true);

        renderCamera->setActiveEntry(ChoiceOption(kRenderCamera, kRenderCamera, ""));
        size->setValue(kWidth, ViewSpec::all(), 0);
        size->setValue(kHeight, ViewSpec::all(), 1);
        par->setValue(1.);

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
        KnobChoice* renderCamera = knob<KnobChoice>(render, kUSDRenderParamCamera);
        KnobInt* size = knob<KnobInt>(render, kNatronParamFormatSize);
        KnobDouble* par = knob<KnobDouble>(render, kNatronParamFormatPar);
        if (!diffuse || !renderCamera || !size || !par) {
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
        connectNodes(merge, render, 0, true);

        renderCamera->setActiveEntry(ChoiceOption("/Cam", "/Cam", ""));
        size->setValue(kWidth, ViewSpec::all(), 0);
        size->setValue(kHeight, ViewSpec::all(), 1);
        par->setValue(1.);

        return render;
    } // createPrimitivesGraph

    /**
     * A card at the origin facing the camera at (0, 0, 10), its material's Diffuse Color input
     * fed by a Constant, lit head on. Sets *constant to the Constant and returns the USDRender,
     * kWidth x kHeight, null after a gtest failure.
     **/
    NodePtr createLiveTextureGraph(NodePtr* constant)
    {
        NodePtr source = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT));
        NodePtr material = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMATERIAL));
        NodePtr card = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCARD));
        NodePtr camera = createNode(QString::fromUtf8(PLUGINID_NATRON_USDCAMERA));
        NodePtr light = createNode(QString::fromUtf8(PLUGINID_NATRON_USDLIGHT));
        NodePtr merge = createNode(QString::fromUtf8(PLUGINID_NATRON_USDMERGE));
        NodePtr render = createNode(QString::fromUtf8(PLUGINID_NATRON_USDRENDER));
        if (!source || !material || !card || !camera || !light || !merge || !render) {
            ADD_FAILURE() << "a node is not registered";

            return NodePtr();
        }
        KnobChoice* renderCamera = knob<KnobChoice>(render, kUSDRenderParamCamera);
        KnobInt* size = knob<KnobInt>(render, kNatronParamFormatSize);
        KnobDouble* par = knob<KnobDouble>(render, kNatronParamFormatPar);
        if (!renderCamera || !size || !par) {
            return NodePtr();
        }
        bool ok = true;
        ok = ok && setString(material, kSceneParamPrimPath, "/Live");
        ok = ok && setString(card, kSceneParamPrimPath, "/Card");
        ok = ok && setString(camera, kSceneParamPrimPath, "/Cam");
        ok = ok && setTriple(camera, kSceneParamTranslate, 0., 0., 10.);
        ok = ok && setString(light, kSceneParamPrimPath, "/Key");
        if (!ok) {
            return NodePtr();
        }
        connectNodes(source, material, kUSDMaterialInputDiffuseColor, true);
        connectNodes(material, card, 0, true);
        connectNodes(card, merge, 0, true);
        connectNodes(camera, merge, 1, true);
        connectNodes(light, merge, 2, true);
        connectNodes(merge, render, 0, true);
        renderCamera->setActiveEntry(ChoiceOption("/Cam", "/Cam", ""));
        size->setValue(kWidth, ViewSpec::all(), 0);
        size->setValue(kHeight, ViewSpec::all(), 1);
        par->setValue(1.);
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
        std::vector<RenderedPlane> planes;
        std::string error;
        bool rendered = false;

        std::thread worker([&]() {
            rendered = renderNodePlanesDirect(render, frame, ViewIdx(0), 0, RectI(0, 0, kWidth, kHeight), allPlanes(), &planes, &error);
        });
        worker.join();
        EXPECT_TRUE(rendered) << error;
        if (planes.size() != 3u) {
            ADD_FAILURE() << "frame " << frame << " rendered " << planes.size() << " planes";

            return Coverage();
        }

        return coverageOf("/Card", planes[0].pixels, planes[1].pixels, planes[2].pixels);
    }

    // Every plane of render at frame, rendered off the main thread as renderCard does.
    static bool renderOffMainThread(const NodePtr& render,
                                    double frame,
                                    std::vector<RenderedPlane>* planes,
                                    std::string* error)
    {
        bool rendered = false;

        std::thread worker([&]() {
            rendered = renderNodePlanesDirect(render, frame, ViewIdx(0), 0, RectI(0, 0, kWidth, kHeight), allPlanes(), planes, error);
        });
        worker.join();

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
        KnobChoice* renderCamera = knob<KnobChoice>(render, kUSDRenderParamCamera);
        KnobInt* size = knob<KnobInt>(render, kNatronParamFormatSize);
        KnobDouble* par = knob<KnobDouble>(render, kNatronParamFormatPar);
        if (!boxSize || !centerlineWidth || !lineWidth || !renderCamera || !size || !par) {
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
        connectNodes(merge, render, 0, true);

        renderCamera->setActiveEntry(ChoiceOption("/Cam", "/Cam", ""));
        size->setValue(kWidth, ViewSpec::all(), 0);
        size->setValue(kHeight, ViewSpec::all(), 1);
        par->setValue(1.);
        *plate = checker;
        *projector = project3D;
        *card = geometry;

        return render;
    } // createProjectionGraph

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
        checkCropAgainstReference(planes[0], goldenCrop, goldenCase);
    }

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
