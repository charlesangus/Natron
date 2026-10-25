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

#include "Global/Macros.h"

#include <cmath>
#include <cstring>
#include <functional>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "NativePluginIdentity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Image/Resampler.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Nodes/Transform/LensDistortion.h"
#include "Engine/Nodes/Transform/LensModels.h"
#include "Engine/Nodes/Transform/LensModels3DE.h"
#include "Engine/Nodes/Transform/STMap.h"
#include "Engine/Project.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

// The model a LensDistortion on the 64x48 parity source builds, in its pixels.
const LensModels::Frame kFrame = { 0., 0., kParitySourceWidth, kParitySourceHeight, 1. };

// Nuke k1 = 0.1: Distort reads inside the Source everywhere, Undistort reads outside it at the corners.
LensModels::NukeParams
nukeParams()
{
    LensModels::NukeParams p;

    p.k1 = 0.1;
    p.k2 = 0.;
    p.centerX = 0.;
    p.centerY = 0.;
    p.anamorphicSqueeze = 1.;
    p.asymmetricX = 0.;
    p.asymmetricY = 0.;

    return p;
}

// A wide fisheye with no distort-direction solution towards the corners of the 64x48 frame.
LensModels::Camera3DE
fisheyeCamera()
{
    LensModels::Camera3DE c;

    c.xa_fov_unit = 0.;
    c.ya_fov_unit = 0.;
    c.xb_fov_unit = 1.;
    c.yb_fov_unit = 1.;
    c.fl_cm = 1.;
    c.fd_cm = 100.;
    c.w_fb_cm = 3.6;
    c.h_fb_cm = 2.025;
    c.x_lco_cm = 0.01;
    c.y_lco_cm = -0.02;
    c.pa = 1.;

    return c;
}

LensModels::Fisheye8Params
fisheyeParams()
{
    LensModels::Fisheye8Params p;

    p.c2 = -0.05;
    p.c4 = 0.01;
    p.c6 = 0.;
    p.c8 = 0.001;

    return p;
}

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

bool
renderRGBA(const NodePtr& node,
           unsigned mipmapLevel,
           const RectI& window,
           RenderedPlane* plane)
{
    std::list<ImageLayerDesc> layers;
    layers.push_back(ImageLayerDesc::getRGBAComponents());
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), mipmapLevel, window, layers, &planes, &error);
    EXPECT_TRUE(ok) << node->getScriptName() << ": " << error;
    if (!ok || (planes.size() != 1) || (planes[0].channels.size() != 4)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

const float*
pixelAt(const RenderedPlane& plane,
        int x,
        int y)
{
    const std::size_t n = plane.channels.size();

    return &plane.pixels[((std::size_t)(y - plane.window.y1) * plane.window.width() + (x - plane.window.x1)) * n];
}

// Counts the pixels of `actual` over `window` whose first `nChannels` channels differ from
// `expected(x, y)` beyond `tolerance`, reporting the first; `checked` gets how many were compared.
// `expected` returns false for a pixel not to check.
std::size_t
countMismatches(const RenderedPlane& actual,
                const RectI& window,
                int nChannels,
                const std::function<bool(int, int, float*)>& expected,
                const ParityTolerance& tolerance,
                const std::string& what,
                std::size_t* checked)
{
    std::size_t wrong = 0;
    float want[4];

    *checked = 0;
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            if (!expected(x, y, want)) {
                continue;
            }
            ++*checked;
            const float* got = pixelAt(actual, x, y);
            for (int c = 0; c < nChannels; ++c) {
                if (!std::isfinite(got[c]) || !tolerance.accepts(want[c], got[c])) {
                    if (wrong == 0) {
                        ADD_FAILURE() << what << ": first mismatch at (" << x << ", " << y << ") channel " << c << ": " << got[c] << " expected " << want[c];
                    }
                    ++wrong;
                    break;
                }
            }
        }
    }

    return wrong;
}

// The Source pixel an impulse sample at (sx, sy) takes, when it is clearly inside one pixel of
// `bounds`: false within 1e-3 of a pixel edge, where float rounding may pick either neighbour.
bool
impulsePixel(double sx,
             double sy,
             const RectI& bounds,
             int* px,
             int* py)
{
    const double fx = std::floor(sx);
    const double fy = std::floor(sy);
    if ((sx - fx < 1e-3) || (fx + 1. - sx < 1e-3) || (sy - fy < 1e-3) || (fy + 1. - sy < 1e-3)) {
        return false;
    }
    *px = (int)fx;
    *py = (int)fy;

    return (bounds.x1 <= *px) && (*px < bounds.x2) && (bounds.y1 <= *py) && (*py < bounds.y2);
}

} // namespace

class NativeLensDistortionTest
    : public BaseTest {
protected:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kParitySourceWidth, kParitySourceHeight, "nativeLensDistortion64x48", 1.));
    }

    NodePtr makeSource()
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
        EXPECT_TRUE(bool(source));

        return source;
    }

    // A LensDistortion on `input` with the Nuke model of nukeParams().
    NodePtr makeNuke(const NodePtr& input,
                     const std::string& direction,
                     const std::string& filter)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_LENSDISTORTION), PLUGIN_MAJOR_NATRON_LENSDISTORTION);
        EXPECT_TRUE(isNative(node));
        if (!node) {
            return node;
        }
        if (input) {
            connectNodes(input, node, kLensDistortionInputSource, true);
        }
        EXPECT_TRUE(setKnobValue(node, kLensDistortionParamModel, std::string(kLensDistortionModelNuke)));
        EXPECT_TRUE(setKnobValue(node, kLensDistortionParamDirection, direction));
        EXPECT_TRUE(setKnobValue(node, kResamplerParamFilterType, filter));
        EXPECT_TRUE(setKnobValues(node, kLensDistortionParamK1, { nukeParams().k1 }));

        return node;
    }
};

TEST_F(NativeLensDistortionTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_LENSDISTORTION, PLUGIN_MAJOR_NATRON_LENSDISTORTION, "LensDistortion");
}

TEST_F(NativeLensDistortionTest, KnobsAndInputs)
{
    resetProject();
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_LENSDISTORTION), PLUGIN_MAJOR_NATRON_LENSDISTORTION);
    ASSERT_TRUE(isNative(node));
    EXPECT_EQ(2, node->getNInputs());
    const char* const names[] = {
        kLensDistortionParamModel,
        kLensDistortionParamDirection,
        kLensDistortionParamOutputMode,
        kLensDistortionParamOverscan,
        kLensDistortionParamCropToFormat,
        kLensDistortionParamUseRoD,
        kLensDistortionParamK1,
        kLensDistortionParamCenter,
        kLensDistortionParamSqueeze,
        kLensDistortionParamAsymmetric,
        kLensDistortionParamPFFile,
        kLensDistortionParamPFC3,
        kLensDistortionParamPFC5,
        kLensDistortionParamPFP,
        kLensDistortionParamPFSqueeze,
        kLensDistortionParam3DEFovXA,
        kLensDistortionParam3DEFocalLength,
        kLensDistortionParam3DEPixelAspect,
        kLensDistortionParam3DEDistortion,
        kLensDistortionParam3DEDistortionDegree2,
        kLensDistortionParam3DEBCylindricBending,
        kLensDistortionParam3DEDegree8,
        kLensDistortionParam3DECx02,
        kLensDistortionParam3DECx66,
        kLensDistortionParam3DELensRotation,
        kLensDistortionParam3DESqueezeY,
        kLensDistortionParamPanoToolsA,
        kLensDistortionParamPanoToolsT,
        kResamplerParamFilterType,
        kResamplerParamFilterClamp,
        kResamplerParamFilterBlackOutside,
        kOfxMixParamName,
        kOfxMaskInvertParamName,
    };
    for (const char* name : names) {
        EXPECT_TRUE(bool(node->getKnobByName(name))) << name;
    }
    // Only the selected model's parameters show.
    EXPECT_FALSE(node->getKnobByName(kLensDistortionParamK1)->getIsSecret());
    EXPECT_TRUE(node->getKnobByName(kLensDistortionParamPFC3)->getIsSecret());
    EXPECT_TRUE(node->getKnobByName(kLensDistortionParam3DEDegree8)->getIsSecret());
    ASSERT_TRUE(setKnobValue(node, kLensDistortionParamModel, std::string(kLensDistortionModel3DEFisheye8)));
    EXPECT_TRUE(node->getKnobByName(kLensDistortionParamK1)->getIsSecret());
    EXPECT_FALSE(node->getKnobByName(kLensDistortionParam3DEDegree8)->getIsSecret());
    EXPECT_FALSE(node->getKnobByName(kLensDistortionParam3DEFocalLength)->getIsSecret());
    EXPECT_TRUE(node->getKnobByName(kLensDistortionParam3DECx02)->getIsSecret());
}

// Distort reads each output pixel from where the model's undistort sends it.
TEST_F(NativeLensDistortionTest, ImpulseFollowsTheModel)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr node = makeNuke(source, kLensDistortionDirectionDistort, "impulse");
    ASSERT_TRUE(bool(node));

    const LensModels::Model model = LensModels::makeNuke(kFrame, nukeParams());
    ASSERT_TRUE(model.valid);
    const RectI window = paritySourceWindow(source, kTime, 0);
    RenderedPlane before;
    RenderedPlane after;
    ASSERT_TRUE(renderRGBA(source, 0, window, &before));
    ASSERT_TRUE(renderRGBA(node, 0, window, &after));
    std::size_t checked = 0;
    const std::size_t wrong = countMismatches(after, window, 4, [&](int x, int y, float* want) {
        LensModels::Sample s;
        int px = 0;
        int py = 0;
        if (!model.map(LensModels::eDirectionUndistort, x + 0.5, y + 0.5, &s) || !impulsePixel(s.x, s.y, window, &px, &py)) {
            return false;
        }
        std::memcpy(want, pixelAt(before, px, py), 4 * sizeof(float));

        return true; }, ParityTolerance::exact(), "Nuke k1 0.1, distort, impulse", &checked);
    EXPECT_EQ(0u, wrong);
    EXPECT_GT(checked, (std::size_t)(window.width() * window.height() / 2));
}

TEST_F(NativeLensDistortionTest, DistortThenUndistortRestoresTheSource)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr distort = makeNuke(source, kLensDistortionDirectionDistort, "cubic");
    ASSERT_TRUE(bool(distort));
    NodePtr undistort = makeNuke(distort, kLensDistortionDirectionUndistort, "cubic");
    ASSERT_TRUE(bool(undistort));

    // Undistort reads the Distort output where the model's distort sends each pixel; it holds
    // the Source only well inside the frame, clear of the cubic footprint at its edges.
    const LensModels::Model model = LensModels::makeNuke(kFrame, nukeParams());
    ASSERT_TRUE(model.valid);
    const RectI window = paritySourceWindow(source, kTime, 0);
    RenderedPlane before;
    RenderedPlane after;
    ASSERT_TRUE(renderRGBA(source, 0, window, &before));
    ASSERT_TRUE(renderRGBA(undistort, 0, window, &after));
    std::size_t checked = 0;
    // R, G and B are smooth; alpha holds hard bands that two cubic passes do not restore.
    const std::size_t wrong = countMismatches(after, window, 3, [&](int x, int y, float* want) {
        LensModels::Sample s;
        if (!model.map(LensModels::eDirectionDistort, x + 0.5, y + 0.5, &s)) {
            return false;
        }
        const double margin = 4.;
        if ((s.x < window.x1 + margin) || (s.x > window.x2 - margin) || (s.y < window.y1 + margin) || (s.y > window.y2 - margin)) {
            return false;
        }
        if ((x < window.x1 + 3) || (x >= window.x2 - 3) || (y < window.y1 + 3) || (y >= window.y2 - 3)) {
            return false;
        }
        std::memcpy(want, pixelAt(before, x, y), 4 * sizeof(float));

        return true; }, ParityTolerance::make(2e-2, 0.), "Nuke k1 0.1, distort then undistort, cubic", &checked);
    EXPECT_EQ(0u, wrong);
    EXPECT_GT(checked, (std::size_t)(window.width() * window.height() / 3));
}

// The STMap output, fed to the native STMap, reproduces the Image output.
TEST_F(NativeLensDistortionTest, STMapOutputMatchesImageOutput)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr image = makeNuke(source, kLensDistortionDirectionUndistort, "impulse");
    NodePtr map = makeNuke(source, kLensDistortionDirectionUndistort, "impulse");
    ASSERT_TRUE(image && map);
    ASSERT_TRUE(setKnobValue(map, kLensDistortionParamOutputMode, std::string(kLensDistortionOutputSTMap)));
    NodePtr stmap = createNode(QString::fromUtf8(PLUGINID_NATRON_STMAP), PLUGIN_MAJOR_NATRON_STMAP);
    ASSERT_TRUE(isNative(stmap));
    connectNodes(map, stmap, kSTMapInputUV, true);
    connectNodes(source, stmap, kSTMapInputSource, true);
    ASSERT_TRUE(setKnobValue(stmap, kResamplerParamFilterType, std::string("impulse")));

    const LensModels::Model model = LensModels::makeNuke(kFrame, nukeParams());
    ASSERT_TRUE(model.valid);
    const RectI window = paritySourceWindow(source, kTime, 0);
    RenderedPlane fromImage;
    RenderedPlane fromMap;
    RenderedPlane uv;
    ASSERT_TRUE(renderRGBA(image, 0, window, &fromImage));
    ASSERT_TRUE(renderRGBA(stmap, 0, window, &fromMap));
    ASSERT_TRUE(renderRGBA(map, 0, window, &uv));

    // Where the model reads inside the Source, both outputs take the same pixel. Elsewhere Image
    // is black and STMap clamps to the Source edge, which is STMap's own behaviour.
    std::size_t checked = 0;
    std::size_t wrong = countMismatches(fromMap, window, 4, [&](int x, int y, float* want) {
        LensModels::Sample s;
        int px = 0;
        int py = 0;
        if (!model.map(LensModels::eDirectionDistort, x + 0.5, y + 0.5, &s) || !impulsePixel(s.x, s.y, window, &px, &py)) {
            return false;
        }
        std::memcpy(want, pixelAt(fromImage, x, y), 4 * sizeof(float));

        return true; }, ParityTolerance::exact(), "Nuke k1 0.1, undistort, STMap through STMap", &checked);
    EXPECT_EQ(0u, wrong);
    EXPECT_GT(checked, (std::size_t)(window.width() * window.height() / 3));

    // The map itself: u, v normalized to the Source format, blue 0, alpha whether it converged.
    wrong = countMismatches(uv, window, 4, [&](int x, int y, float* want) {
        LensModels::Sample s;
        if (!model.map(LensModels::eDirectionDistort, x + 0.5, y + 0.5, &s)) {
            std::memset(want, 0, 4 * sizeof(float));
        } else {
            want[0] = (float)(s.x / kParitySourceWidth);
            want[1] = (float)(s.y / kParitySourceHeight);
            want[2] = 0.f;
            want[3] = 1.f;
        }

        return true; }, ParityTolerance::make(1e-6, 0.), "Nuke k1 0.1, undistort, STMap values", &checked);
    EXPECT_EQ(0u, wrong);
}

// Where a wide fisheye has no solution, Image is black and STMap's alpha is 0, never NaN.
TEST_F(NativeLensDistortionTest, NonConvergentFisheyeEdgeIsBlack)
{
    const LensModels::Model model = LensModels::make3DEFisheye8(kFrame, fisheyeCamera(), fisheyeParams());
    ASSERT_TRUE(model.valid);
    {
        LensModels::Sample s;
        ASSERT_TRUE(model.map(LensModels::eDirectionUndistort, kParitySourceWidth / 2., kParitySourceHeight / 2., &s));
        ASSERT_FALSE(model.map(LensModels::eDirectionUndistort, 0.5, 0.5, &s));
    }

    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_LENSDISTORTION), PLUGIN_MAJOR_NATRON_LENSDISTORTION);
    ASSERT_TRUE(isNative(node));
    connectNodes(source, node, kLensDistortionInputSource, true);
    ASSERT_TRUE(setKnobValue(node, kLensDistortionParamModel, std::string(kLensDistortionModel3DEFisheye8)));
    ASSERT_TRUE(setKnobValue(node, kLensDistortionParamDirection, std::string(kLensDistortionDirectionDistort)));
    const LensModels::Camera3DE camera = fisheyeCamera();
    const LensModels::Fisheye8Params params = fisheyeParams();
    ASSERT_TRUE(setKnobValues(node, kLensDistortionParam3DEFocalLength, { camera.fl_cm }));
    ASSERT_TRUE(setKnobValues(node, kLensDistortionParam3DEFocusDistance, { camera.fd_cm }));
    ASSERT_TRUE(setKnobValues(node, kLensDistortionParam3DEFilmbackWidth, { camera.w_fb_cm }));
    ASSERT_TRUE(setKnobValues(node, kLensDistortionParam3DEFilmbackHeight, { camera.h_fb_cm }));
    ASSERT_TRUE(setKnobValues(node, kLensDistortionParam3DELensCenterOffsetX, { camera.x_lco_cm }));
    ASSERT_TRUE(setKnobValues(node, kLensDistortionParam3DELensCenterOffsetY, { camera.y_lco_cm }));
    ASSERT_TRUE(setKnobValues(node, kLensDistortionParam3DEDistortionDegree2, { params.c2 }));
    ASSERT_TRUE(setKnobValues(node, kLensDistortionParam3DEQuarticDistortionDegree4, { params.c4 }));
    ASSERT_TRUE(setKnobValues(node, kLensDistortionParam3DEDegree6, { params.c6 }));
    ASSERT_TRUE(setKnobValues(node, kLensDistortionParam3DEDegree8, { params.c8 }));

    const RectI window = paritySourceWindow(source, kTime, 0);
    for (int mode = 0; mode < 2; ++mode) {
        const bool stmap = (mode == 1);
        ASSERT_TRUE(setKnobValue(node, kLensDistortionParamOutputMode, std::string(stmap ? kLensDistortionOutputSTMap : kLensDistortionOutputImage)));
        RenderedPlane out;
        ASSERT_TRUE(renderRGBA(node, 0, window, &out));
        std::size_t failed = 0;
        std::size_t converged = 0;
        for (int y = window.y1; y < window.y2; ++y) {
            for (int x = window.x1; x < window.x2; ++x) {
                const float* got = pixelAt(out, x, y);
                for (int c = 0; c < 4; ++c) {
                    ASSERT_TRUE(std::isfinite(got[c])) << (stmap ? "STMap" : "Image") << " at (" << x << ", " << y << ")";
                }
                LensModels::Sample s;
                if (model.map(LensModels::eDirectionUndistort, x + 0.5, y + 0.5, &s)) {
                    ++converged;
                    if (stmap) {
                        EXPECT_EQ(1.f, got[3]) << "STMap at (" << x << ", " << y << ")";
                    }
                    continue;
                }
                ++failed;
                for (int c = 0; c < 4; ++c) {
                    EXPECT_EQ(0.f, got[c]) << (stmap ? "STMap" : "Image") << " at (" << x << ", " << y << ") channel " << c;
                }
            }
        }
        EXPECT_GT(failed, 0u);
        EXPECT_GT(converged, 0u);
    }
}

// A render at scale 0.5 reads the half-scale Source where the full-resolution model sends each
// pixel centre, and matches a full render downscaled.
TEST_F(NativeLensDistortionTest, HalfScaleMatchesTheModel)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr node = makeNuke(source, kLensDistortionDirectionDistort, "impulse");
    ASSERT_TRUE(bool(node));

    const LensModels::Model model = LensModels::makeNuke(kFrame, nukeParams());
    ASSERT_TRUE(model.valid);
    const RectI half = paritySourceWindow(source, kTime, 1);
    RenderedPlane sourceHalf;
    RenderedPlane outHalf;
    ASSERT_TRUE(renderRGBA(source, 1, half, &sourceHalf));
    ASSERT_TRUE(renderRGBA(node, 1, half, &outHalf));
    std::size_t checked = 0;
    std::size_t wrong = countMismatches(outHalf, half, 4, [&](int x, int y, float* want) {
        LensModels::Sample s;
        int px = 0;
        int py = 0;
        if (!model.map(LensModels::eDirectionUndistort, (x + 0.5) * 2., (y + 0.5) * 2., &s) || !impulsePixel(s.x * 0.5, s.y * 0.5, half, &px, &py)) {
            return false;
        }
        std::memcpy(want, pixelAt(sourceHalf, px, py), 4 * sizeof(float));

        return true; }, ParityTolerance::exact(), "Nuke k1 0.1, distort, impulse, scale 0.5", &checked);
    EXPECT_EQ(0u, wrong);
    EXPECT_GT(checked, (std::size_t)(half.width() * half.height() / 2));

    // Cubic: the half render against the 2x2 average of the full one, on the smooth channels and
    // away from the edges.
    ASSERT_TRUE(setKnobValue(node, kResamplerParamFilterType, std::string("cubic")));
    const RectI full = paritySourceWindow(source, kTime, 0);
    RenderedPlane outFull;
    ASSERT_TRUE(renderRGBA(node, 1, half, &outHalf));
    ASSERT_TRUE(renderRGBA(node, 0, full, &outFull));
    wrong = countMismatches(outHalf, half, 3, [&](int x, int y, float* want) {
        if ((x < half.x1 + 2) || (x >= half.x2 - 2) || (y < half.y1 + 2) || (y >= half.y2 - 2)) {
            return false;
        }
        for (int c = 0; c < 4; ++c) {
            want[c] = 0.25f * (pixelAt(outFull, 2 * x, 2 * y)[c] + pixelAt(outFull, 2 * x + 1, 2 * y)[c] +
                               pixelAt(outFull, 2 * x, 2 * y + 1)[c] + pixelAt(outFull, 2 * x + 1, 2 * y + 1)[c]);
        }

        return true; }, ParityTolerance::make(5e-2, 0.), "Nuke k1 0.1, distort, cubic, scale 0.5 against downscaled", &checked);
    EXPECT_EQ(0u, wrong);
}
