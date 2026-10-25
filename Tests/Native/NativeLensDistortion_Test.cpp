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

#include <algorithm>
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
#include "Engine/Nodes/Image/ExtentKnobs.h"
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

// Nuke k1 = 0.2, k2 = 0.05: undistort shrinks the frame edges inwards and has no preimage for
// them, so Distort pushes the Source edges past the frame and its distort direction fails there.
LensModels::NukeParams
strongNukeParams()
{
    LensModels::NukeParams p = nukeParams();

    p.k1 = 0.2;
    p.k2 = 0.05;

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

    // A LensDistortion on `input` with the Nuke model of strongNukeParams().
    NodePtr makeStrongNuke(const NodePtr& input,
                           const std::string& direction,
                           const std::string& filter)
    {
        NodePtr node = makeNuke(input, direction, filter);
        if (node) {
            EXPECT_TRUE(setKnobValues(node, kLensDistortionParamK1, { strongNukeParams().k1 }));
            EXPECT_TRUE(setKnobValues(node, kLensDistortionParamK2, { strongNukeParams().k2 }));
        }

        return node;
    }

    // A LensDistortion on `input` with the 3DE fisheye of fisheyeCamera() and fisheyeParams(); an empty `filter` keeps
    // the default one.
    NodePtr makeFisheye(const NodePtr& input,
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
        EXPECT_TRUE(setKnobValue(node, kLensDistortionParamModel, std::string(kLensDistortionModel3DEFisheye8)));
        EXPECT_TRUE(setKnobValue(node, kLensDistortionParamDirection, direction));
        if (!filter.empty()) {
            EXPECT_TRUE(setKnobValue(node, kResamplerParamFilterType, filter));
        }
        const LensModels::Camera3DE camera = fisheyeCamera();
        const LensModels::Fisheye8Params params = fisheyeParams();
        EXPECT_TRUE(setKnobValues(node, kLensDistortionParam3DEFocalLength, { camera.fl_cm }));
        EXPECT_TRUE(setKnobValues(node, kLensDistortionParam3DEFocusDistance, { camera.fd_cm }));
        EXPECT_TRUE(setKnobValues(node, kLensDistortionParam3DEFilmbackWidth, { camera.w_fb_cm }));
        EXPECT_TRUE(setKnobValues(node, kLensDistortionParam3DEFilmbackHeight, { camera.h_fb_cm }));
        EXPECT_TRUE(setKnobValues(node, kLensDistortionParam3DELensCenterOffsetX, { camera.x_lco_cm }));
        EXPECT_TRUE(setKnobValues(node, kLensDistortionParam3DELensCenterOffsetY, { camera.y_lco_cm }));
        EXPECT_TRUE(setKnobValues(node, kLensDistortionParam3DEDistortionDegree2, { params.c2 }));
        EXPECT_TRUE(setKnobValues(node, kLensDistortionParam3DEQuarticDistortionDegree4, { params.c4 }));
        EXPECT_TRUE(setKnobValues(node, kLensDistortionParam3DEDegree6, { params.c6 }));
        EXPECT_TRUE(setKnobValues(node, kLensDistortionParam3DEDegree8, { params.c8 }));

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

// With coefficients whose distort direction has no solution at the frame edges, Distort still
// covers the whole frame, and Undistort chained after it restores the Source in the interior.
TEST_F(NativeLensDistortionTest, StrongDistortThenUndistortRestoresTheSource)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr distort = makeStrongNuke(source, kLensDistortionDirectionDistort, "impulse");
    ASSERT_TRUE(bool(distort));
    NodePtr undistort = makeStrongNuke(distort, kLensDistortionDirectionUndistort, "cubic");
    ASSERT_TRUE(bool(undistort));

    const LensModels::Model model = LensModels::makeNuke(kFrame, strongNukeParams());
    ASSERT_TRUE(model.valid);
    {
        // The middle of the left frame edge has no distorted position.
        LensModels::Sample s;
        ASSERT_FALSE(model.map(LensModels::eDirectionDistort, 0., kParitySourceHeight / 2., &s));
    }
    const RectI window = paritySourceWindow(source, kTime, 0);
    RenderedPlane before;
    RenderedPlane distorted;
    ASSERT_TRUE(renderRGBA(source, 0, window, &before));
    ASSERT_TRUE(renderRGBA(distort, 0, window, &distorted));
    std::size_t checked = 0;
    std::size_t wrong = countMismatches(distorted, window, 4, [&](int x, int y, float* want) {
        LensModels::Sample s;
        int px = 0;
        int py = 0;
        if (!model.map(LensModels::eDirectionUndistort, x + 0.5, y + 0.5, &s) || !impulsePixel(s.x, s.y, window, &px, &py)) {
            return false;
        }
        std::memcpy(want, pixelAt(before, px, py), 4 * sizeof(float));

        return true; }, ParityTolerance::exact(), "Nuke k1 0.2 k2 0.05, distort, impulse", &checked);
    EXPECT_EQ(0u, wrong);
    EXPECT_GT(checked, (std::size_t)(window.width() * window.height() / 2));

    RenderedPlane after;
    ASSERT_TRUE(setKnobValue(distort, kResamplerParamFilterType, std::string("cubic")));
    ASSERT_TRUE(renderRGBA(undistort, 0, window, &after));
    // R, G and B are smooth; alpha holds hard bands that two cubic passes do not restore.
    wrong = countMismatches(after, window, 3, [&](int x, int y, float* want) {
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

        return true; }, ParityTolerance::make(2e-2, 0.), "Nuke k1 0.2 k2 0.05, distort then undistort, cubic", &checked);
    EXPECT_EQ(0u, wrong);
    EXPECT_GT(checked, (std::size_t)(window.width() * window.height() / 3));
}

// Undistort shrinks the picture inside the format; a Distort after it still builds its lens on
// the format, not on that smaller region, so the pair restores the Source.
TEST_F(NativeLensDistortionTest, LensFrameIsTheFormatNotTheInputRegion)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr undistort = makeStrongNuke(source, kLensDistortionDirectionUndistort, "cubic");
    ASSERT_TRUE(bool(undistort));
    NodePtr distort = makeStrongNuke(undistort, kLensDistortionDirectionDistort, "cubic");
    ASSERT_TRUE(bool(distort));

    const LensModels::Model model = LensModels::makeNuke(kFrame, strongNukeParams());
    ASSERT_TRUE(model.valid);
    const RectI window = paritySourceWindow(source, kTime, 0);
    RenderedPlane before;
    RenderedPlane undistorted;
    RenderedPlane after;
    ASSERT_TRUE(renderRGBA(source, 0, window, &before));
    ASSERT_TRUE(renderRGBA(undistort, 0, window, &undistorted));
    ASSERT_TRUE(renderRGBA(distort, 0, window, &after));

    // The Undistort output is black at the middle of the left and right format edges.
    for (int c = 0; c < 4; ++c) {
        EXPECT_EQ(0.f, pixelAt(undistorted, window.x1, (window.y1 + window.y2) / 2)[c]) << "channel " << c;
        EXPECT_EQ(0.f, pixelAt(undistorted, window.x2 - 1, (window.y1 + window.y2) / 2)[c]) << "channel " << c;
    }

    std::size_t checked = 0;
    const std::size_t wrong = countMismatches(after, window, 3, [&](int x, int y, float* want) {
        if ((x < window.x1 + 6) || (x >= window.x2 - 6) || (y < window.y1 + 6) || (y >= window.y2 - 6)) {
            return false;
        }
        std::memcpy(want, pixelAt(before, x, y), 4 * sizeof(float));

        return true; }, ParityTolerance::make(2e-2, 0.), "Nuke k1 0.2 k2 0.05, undistort then distort, cubic", &checked);
    EXPECT_EQ(0u, wrong);
    EXPECT_GT(checked, (std::size_t)(window.width() * window.height() / 2));
}

// With enough overscan and Crop To Format off, Distort keeps all of the Source; Undistort on the same reference format,
// with no overscan, crops back to that format and restores every pixel of it. Black Outside is off on both, so that the
// filter footprints at the Source edges extend the edge pixels instead of reading black.
TEST_F(NativeLensDistortionTest, OverscanRoundTripRestoresTheWholeFormat)
{
    resetProject();
    NodePtr source = makeSource();
    ASSERT_TRUE(bool(source));
    NodePtr distort = makeNuke(source, kLensDistortionDirectionDistort, "cubic");
    ASSERT_TRUE(bool(distort));
    NodePtr undistort = makeNuke(distort, kLensDistortionDirectionUndistort, "cubic");
    ASSERT_TRUE(bool(undistort));
    const double overscan = 50.;
    ASSERT_TRUE(setKnobValues(distort, kLensDistortionParamOverscan, { overscan }));
    ASSERT_TRUE(setKnobValues(distort, kLensDistortionParamCropToFormat, { 0. }));
    for (const NodePtr& node : { distort, undistort }) {
        ASSERT_TRUE(setKnobValue(node, kNativeGeneratorParamExtent, std::string(kNativeGeneratorExtentProject)));
        ASSERT_TRUE(setKnobValues(node, kResamplerParamFilterBlackOutside, { 0. }));
    }

    const RectI window = paritySourceWindow(source, kTime, 0);
    ASSERT_EQ(RectI(0, 0, kParitySourceWidth, kParitySourceHeight), window);
    const int padX = (int)std::floor(kParitySourceWidth * overscan / 200. + 0.5);
    const int padY = (int)std::floor(kParitySourceHeight * overscan / 200. + 0.5);
    const RectI overscanned(-padX, -padY, kParitySourceWidth + padX, kParitySourceHeight + padY);
    EXPECT_EQ(overscanned, distort->getEffectInstance()->getOutputFormat());
    EXPECT_EQ(window, undistort->getEffectInstance()->getOutputFormat());

    // Where the Source edges land in the Distort output: inside the overscan, clear of a cubic footprint.
    const LensModels::Model model = LensModels::makeNuke(kFrame, nukeParams());
    ASSERT_TRUE(model.valid);
    for (int i = 0; i <= 64; ++i) {
        const double t = i / 64.;
        const double edge[4][2] = {
            { t * kParitySourceWidth, 0. },
            { t * kParitySourceWidth, (double)kParitySourceHeight },
            { 0., t * kParitySourceHeight },
            { (double)kParitySourceWidth, t * kParitySourceHeight },
        };
        for (const auto& p : edge) {
            LensModels::Sample s;
            ASSERT_TRUE(model.map(LensModels::eDirectionDistort, p[0], p[1], &s)) << p[0] << ", " << p[1];
            EXPECT_GT(s.x, overscanned.x1 + 3.);
            EXPECT_LT(s.x, overscanned.x2 - 3.);
            EXPECT_GT(s.y, overscanned.y1 + 3.);
            EXPECT_LT(s.y, overscanned.y2 - 3.);
        }
    }

    RenderedPlane before;
    RenderedPlane after;
    ASSERT_TRUE(renderRGBA(source, 0, window, &before));
    ASSERT_TRUE(renderRGBA(undistort, 0, window, &after));
    std::size_t checked = 0;
    // R, G and B are smooth; alpha holds hard bands that two cubic passes do not restore.
    const std::size_t wrong = countMismatches(after, window, 3, [&](int x, int y, float* want) {
        std::memcpy(want, pixelAt(before, x, y), 4 * sizeof(float));

        return true; }, ParityTolerance::make(2e-2, 0.), "Nuke k1 0.1, overscan distort then undistort, cubic", &checked);
    EXPECT_EQ(0u, wrong);
    EXPECT_EQ((std::size_t)(window.width() * window.height()), checked);
}

namespace {

// Where the STMap output says a pixel reads the Source: inside its format, in the double arithmetic the node uses.
bool
mapsInsideSource(const LensModels::Sample& s)
{
    const double u = s.x / kParitySourceWidth;
    const double v = s.y / kParitySourceHeight;

    return (u >= 0.) && (u < 1.) && (v >= 0.) && (v < 1.);
}

// The pixels an impulse sample at (sx, sy) of `source` may take: the one it falls in, or, within 1e-3 of a pixel edge
// where float rounding may pick either side, a neighbour. Black for a candidate outside `bounds`.
std::vector<std::vector<float>>
impulseCandidates(const RenderedPlane& source,
                  const RectI& bounds,
                  double sx,
                  double sy)
{
    std::vector<std::vector<float>> candidates;
    for (int px : { (int)std::floor(sx - 1e-3), (int)std::floor(sx + 1e-3) }) {
        for (int py : { (int)std::floor(sy - 1e-3), (int)std::floor(sy + 1e-3) }) {
            if ((bounds.x1 <= px) && (px < bounds.x2) && (bounds.y1 <= py) && (py < bounds.y2)) {
                const float* p = pixelAt(source, px, py);
                candidates.push_back(std::vector<float>(p, p + 4));
            } else {
                candidates.push_back(std::vector<float>(4, 0.f));
            }
        }
    }

    return candidates;
}

bool
isOneOf(const float* got,
        const std::vector<std::vector<float>>& candidates)
{
    for (const std::vector<float>& c : candidates) {
        if (std::equal(c.begin(), c.end(), got)) {
            return true;
        }
    }

    return false;
}

} // namespace

// The STMap output, fed to the native STMap, reproduces the Image output over the whole frame: where the model reads
// inside the Source, where it reads outside it, and where it has no solution.
TEST_F(NativeLensDistortionTest, STMapOutputMatchesImageOutput)
{
    struct Case {
        std::string what;
        std::function<NodePtr(const NodePtr&)> make;
        LensModels::Model model;
        // The direction the output reads the Source in: the opposite of the node's.
        LensModels::Direction reads;
    };
    const std::vector<Case> cases = {
        { "Nuke k1 0.1, undistort",
          [this](const NodePtr& source) { return makeNuke(source, kLensDistortionDirectionUndistort, "impulse"); },
          LensModels::makeNuke(kFrame, nukeParams()), LensModels::eDirectionDistort },
        { "Nuke k1 0.2 k2 0.05, undistort",
          [this](const NodePtr& source) { return makeStrongNuke(source, kLensDistortionDirectionUndistort, "impulse"); },
          LensModels::makeNuke(kFrame, strongNukeParams()), LensModels::eDirectionDistort },
        { "3DE fisheye, distort",
          [this](const NodePtr& source) { return makeFisheye(source, kLensDistortionDirectionDistort, "impulse"); },
          LensModels::make3DEFisheye8(kFrame, fisheyeCamera(), fisheyeParams()), LensModels::eDirectionUndistort },
    };

    std::size_t allFailed = 0;
    std::size_t allOutside = 0;
    for (const Case& c : cases) {
        SCOPED_TRACE(c.what);
        ASSERT_TRUE(c.model.valid);
        resetProject();
        NodePtr source = makeSource();
        ASSERT_TRUE(bool(source));
        NodePtr image = c.make(source);
        NodePtr map = c.make(source);
        ASSERT_TRUE(image && map);
        ASSERT_TRUE(setKnobValue(map, kLensDistortionParamOutputMode, std::string(kLensDistortionOutputSTMap)));
        NodePtr stmap = createNode(QString::fromUtf8(PLUGINID_NATRON_STMAP), PLUGIN_MAJOR_NATRON_STMAP);
        ASSERT_TRUE(isNative(stmap));
        connectNodes(map, stmap, kSTMapInputUV, true);
        connectNodes(source, stmap, kSTMapInputSource, true);
        ASSERT_TRUE(setKnobValue(stmap, kResamplerParamFilterType, std::string("impulse")));

        const RectI window = paritySourceWindow(source, kTime, 0);
        RenderedPlane before;
        RenderedPlane fromImage;
        RenderedPlane fromMap;
        RenderedPlane uv;
        ASSERT_TRUE(renderRGBA(source, 0, window, &before));
        ASSERT_TRUE(renderRGBA(image, 0, window, &fromImage));
        ASSERT_TRUE(renderRGBA(stmap, 0, window, &fromMap));
        ASSERT_TRUE(renderRGBA(map, 0, window, &uv));

        // Every pixel: both outputs take the Source pixel the model reads, or black where it reads outside the Source
        // or has no solution. Only within 1e-3 of a pixel edge may either take a neighbour.
        std::size_t failed = 0;
        std::size_t outside = 0;
        std::size_t inside = 0;
        std::size_t wrong = 0;
        std::size_t wrongMap = 0;
        for (int y = window.y1; y < window.y2; ++y) {
            for (int x = window.x1; x < window.x2; ++x) {
                const float* gotImage = pixelAt(fromImage, x, y);
                const float* gotMap = pixelAt(fromMap, x, y);
                const float* gotUV = pixelAt(uv, x, y);
                LensModels::Sample s;
                float wantUV[4] = { 0.f, 0.f, 0.f, 0.f };
                std::vector<std::vector<float>> candidates(1, std::vector<float>(4, 0.f));
                if (!c.model.map(c.reads, x + 0.5, y + 0.5, &s)) {
                    ++failed;
                } else {
                    const bool in = mapsInsideSource(s);
                    ++(in ? inside : outside);
                    candidates = impulseCandidates(before, window, s.x, s.y);
                    wantUV[0] = (float)(s.x / kParitySourceWidth);
                    wantUV[1] = (float)(s.y / kParitySourceHeight);
                    wantUV[3] = in ? 1.f : 0.f;
                }
                const bool unique = (candidates.size() == 1) || std::all_of(candidates.begin(), candidates.end(), [&](const std::vector<float>& other) {
                                        return other == candidates.front();
                                    });
                const bool ok = isOneOf(gotImage, candidates) && isOneOf(gotMap, candidates) && (!unique || std::equal(gotImage, gotImage + 4, gotMap));
                if (!ok) {
                    if (wrong == 0) {
                        ADD_FAILURE() << "first mismatch at (" << x << ", " << y << "): Image (" << gotImage[0] << ", " << gotImage[1] << ", " << gotImage[2] << ", " << gotImage[3]
                                      << ") STMap (" << gotMap[0] << ", " << gotMap[1] << ", " << gotMap[2] << ", " << gotMap[3] << ")";
                    }
                    ++wrong;
                }
                // The map itself: u, v normalized to the Source format, blue 0, alpha whether it reads inside the
                // Source; all 0 where the model has no solution.
                bool uvOk = (gotUV[2] == 0.f) && (gotUV[3] == wantUV[3]);
                for (int k = 0; k < 2; ++k) {
                    uvOk = uvOk && ParityTolerance::make(1e-6, 0.).accepts(wantUV[k], gotUV[k]);
                }
                if (!uvOk) {
                    if (wrongMap == 0) {
                        ADD_FAILURE() << "first map mismatch at (" << x << ", " << y << "): (" << gotUV[0] << ", " << gotUV[1] << ", " << gotUV[2] << ", " << gotUV[3]
                                      << ") expected (" << wantUV[0] << ", " << wantUV[1] << ", " << wantUV[2] << ", " << wantUV[3] << ")";
                    }
                    ++wrongMap;
                }
            }
        }
        EXPECT_EQ(0u, wrong);
        EXPECT_EQ(0u, wrongMap);
        EXPECT_GT(inside, (std::size_t)(window.width() * window.height() / 3));
        allFailed += failed;
        allOutside += outside;
    }
    // The cases cover both kinds of pixel the map marks invalid.
    EXPECT_GT(allFailed, 0u);
    EXPECT_GT(allOutside, 0u);
}

// Where a wide fisheye has no solution, Image is black and the STMap output is all 0, never NaN.
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
    NodePtr node = makeFisheye(source, kLensDistortionDirectionDistort, std::string());
    ASSERT_TRUE(bool(node));

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
                        EXPECT_EQ(mapsInsideSource(s) ? 1.f : 0.f, got[3]) << "STMap at (" << x << ", " << y << ")";
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
