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
#include <cstddef>
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
#include "Engine/Nodes/Generator/Noise.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;
const int kWidth = 64;
const int kHeight = 48;

// Every pixel depends only on its own coordinates, so any split of the frame is bit for bit the
// whole-frame render.
const ParityTolerance kNoiseTileTolerance = ParityTolerance::exact();

// The same canonical point reached at another render scale or through the transform: the mapping
// rounds its products differently, which moves the sample by a few float ulps of a noise cell.
const float kNoiseScaleTolerance = 1e-4f;

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

// The RGBA plane of `node` over `window` at `mipmapLevel`, at `time`.
bool
renderRgba(const NodePtr& node,
           const RectI& window,
           RenderedPlane* plane,
           unsigned mipmapLevel = 0,
           double time = kTime)
{
    std::list<ImageLayerDesc> layers(1, ImageLayerDesc::mapNCompsToColorLayer(4));
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, time, ViewIdx(0), mipmapLevel, window, layers, &planes, &error);

    EXPECT_TRUE(ok) << error;
    if (!ok || (planes.size() != 1) || (planes[0].channels.size() != 4)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

// Channel c of pixel (x, y) of a plane rendered with renderRgba().
float
at(const RenderedPlane& plane,
   int x,
   int y,
   int c)
{
    const RectI& w = plane.window;

    return plane.pixels[((std::size_t)(y - w.y1) * w.width() + (std::size_t)(x - w.x1)) * 4 + c];
}

bool
sameBits(float a,
         float b)
{
    return std::memcmp(&a, &b, sizeof(float)) == 0;
}

// How many pixels of the two whole-frame renders differ in channel c.
int
countDifferent(const RenderedPlane& a,
               const RenderedPlane& b,
               int c)
{
    int n = 0;
    for (int y = a.window.y1; y < a.window.y2; ++y) {
        for (int x = a.window.x1; x < a.window.x2; ++x) {
            n += sameBits(at(a, x, y, c), at(b, x, y, c)) ? 0 : 1;
        }
    }

    return n;
}

float
maxAbsDifference(const RenderedPlane& a,
                 const RenderedPlane& b)
{
    float worst = 0.f;
    for (std::size_t i = 0; i < a.pixels.size() && i < b.pixels.size(); ++i) {
        worst = (std::max)(worst, std::fabs(a.pixels[i] - b.pixels[i]));
    }

    return worst;
}

RectI
frame()
{
    return RectI(0, 0, kWidth, kHeight);
}

} // namespace

class NativeNoiseTest
    : public BaseTest {
protected:
    // A fresh project whose default format is 64x48 at pixel aspect ratio 1.
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kWidth, kHeight, "nativeNoise", 1.));
    }

    // A native Noise with small cells, so neighbouring pixels differ and a misplaced sample shows.
    NodePtr makeNoise(int type = NoiseCore::kNoiseTypePerlin)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_NOISE), PLUGIN_MAJOR_NATRON_NOISE);

        EXPECT_TRUE(isNative(node));
        if (node) {
            EXPECT_TRUE(setKnobValues(node, kNoiseParamType, { (double)type }));
            EXPECT_TRUE(setKnobValues(node, kNoiseParamSize, { 9.5, 7.25 }));
            EXPECT_TRUE(setKnobValues(node, kNoiseParamSeed, { 1234. }));
        }

        return node;
    }
};

TEST_F(NativeNoiseTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_NOISE, PLUGIN_MAJOR_NATRON_NOISE, "Noise");
}

TEST_F(NativeNoiseTest, UnversionedRequestsGetTheNativeNoise)
{
    NodePtr unversioned = createNode(QString::fromUtf8(PLUGINID_NATRON_NOISE));
    ASSERT_TRUE(bool(unversioned));
    EXPECT_TRUE(isNative(unversioned));
    EXPECT_EQ(PLUGIN_MAJOR_NATRON_NOISE, unversioned->getMajorVersion());
}

TEST_F(NativeNoiseTest, DefaultExtentIsTheProjectFormatAndTheNoiseSpansTheColours)
{
    resetProject();
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_NOISE), PLUGIN_MAJOR_NATRON_NOISE);
    ASSERT_TRUE(isNative(node));
    ASSERT_TRUE(setKnobValues(node, kNoiseParamSize, { 8. }));

    RenderedPlane plane;
    ASSERT_TRUE(renderRgba(node, RectI(-4, -4, kWidth + 4, kHeight + 4), &plane));
    float lo = 1.f;
    float hi = 0.f;
    for (int y = -4; y < kHeight + 4; ++y) {
        for (int x = -4; x < kWidth + 4; ++x) {
            const bool inside = (x >= 0) && (x < kWidth) && (y >= 0) && (y < kHeight);
            if (!inside) {
                ASSERT_EQ(0.f, at(plane, x, y, 3)) << "pixel (" << x << ", " << y << ") is outside the format";
                continue;
            }
            const float r = at(plane, x, y, 0);
            ASSERT_TRUE(std::isfinite(r)) << x << ", " << y;
            EXPECT_EQ(1.f, at(plane, x, y, 3)) << "both colours have alpha 1";
            EXPECT_TRUE(sameBits(r, at(plane, x, y, 1)) && sameBits(r, at(plane, x, y, 2))) << x << ", " << y;
            lo = (std::min)(lo, r);
            hi = (std::max)(hi, r);
        }
    }
    // Four octaves of Perlin noise over 48 cells reach well into both halves of the range.
    EXPECT_LT(lo, 0.35f);
    EXPECT_GT(hi, 0.65f);
    EXPECT_GE(lo, -0.5f);
    EXPECT_LE(hi, 1.5f);
}

// Strips, odd tiles and a lone pixel against the whole frame, bit for bit, for every basis.
TEST_F(NativeNoiseTest, TilesMatchTheWholeFrame)
{
    resetProject();
    for (int type = NoiseCore::kNoiseTypePerlin; type <= NoiseCore::kNoiseTypeWhite; ++type) {
        NodePtr node = makeNoise(type);
        ASSERT_TRUE(bool(node));
        ASSERT_TRUE(setKnobValues(node, kNoiseParamWarpType, { (double)NoiseCore::kNoiseWarpSimplex }));
        ASSERT_TRUE(setKnobValues(node, kNoiseParamDecorrelate, { 1. }));
        ASSERT_TRUE(setKnobValues(node, kNoiseParamRotate, { 23. }));

        RenderedPlane whole;
        ASSERT_TRUE(renderRgba(node, frame(), &whole));
        const RectI tiles[] = {
            RectI(0, 0, kWidth, 12),
            RectI(0, 12, kWidth, 13),
            RectI(0, 13, kWidth, kHeight),
            RectI(5, 7, 22, 31),
            RectI(31, 2, 64, 47),
            RectI(17, 29, 18, 30),
        };
        for (const RectI& t : tiles) {
            RenderedPlane tile;
            ASSERT_TRUE(renderRgba(node, t, &tile));
            int mismatches = 0;
            for (int y = t.y1; y < t.y2; ++y) {
                for (int x = t.x1; x < t.x2; ++x) {
                    for (int c = 0; c < 4; ++c) {
                        const float a = at(whole, x, y, c);
                        const float b = at(tile, x, y, c);
                        if (!kNoiseTileTolerance.accepts(a, b) && (++mismatches <= 3)) {
                            ADD_FAILURE() << "type " << type << ", tile (" << t.x1 << ", " << t.y1 << ")-(" << t.x2 << ", " << t.y2 << "), pixel (" << x << ", " << y << ") channel " << c
                                          << ": whole " << a << ", tile " << b;
                        }
                    }
                }
            }
            EXPECT_EQ(0, mismatches) << "type " << type;
        }
    }
}

TEST_F(NativeNoiseTest, RendersTheSameInBothSchedulerModes)
{
    resetProject();
    NodePtr node = makeNoise(NoiseCore::kNoiseTypeSimplex);
    ASSERT_TRUE(bool(node));

    std::vector<int> poolSizes;
    poolSizes.push_back(1);
    poolSizes.push_back(4);
    for (unsigned mipmapLevel = 0; mipmapLevel <= 2; ++mipmapLevel) {
        std::vector<int> unplannedPulls;
        const RectI window = RectD(0., 0., kWidth, kHeight).toPixelEnclosing(mipmapLevel, 1.);
        const RenderMismatch m = renderBothWaysDirect(node, kTime, ViewIdx(0), mipmapLevel, window, poolSizes, std::function<void()>(), 0.f, &unplannedPulls);
        EXPECT_FALSE(m.any) << "mipmap " << mipmapLevel << ": " << describe(m);
        for (std::size_t p = 0; p < unplannedPulls.size(); ++p) {
            EXPECT_EQ(0, unplannedPulls[p]) << "mipmap " << mipmapLevel << ", pool " << poolSizes[p];
        }
    }
}

// Pixel (x, y) at render scale 1/2 has its centre at canonical (2x + 1, 2y + 1). Translated by
// (-1/2, -1/2), full-scale pixel (2x, 2y) samples the pattern there too.
TEST_F(NativeNoiseTest, RenderScaleSamplesTheSameCanonicalPoints)
{
    resetProject();
    const int types[] = { NoiseCore::kNoiseTypePerlin, NoiseCore::kNoiseTypeSimplex, NoiseCore::kNoiseTypeValue, NoiseCore::kNoiseTypeCellular };
    for (int type : types) {
        NodePtr node = makeNoise(type);
        ASSERT_TRUE(bool(node));
        ASSERT_TRUE(setKnobValues(node, kNoiseParamRotate, { 31. }));
        ASSERT_TRUE(setKnobValues(node, kNoiseParamScale, { 1.5, 0.75 }));
        ASSERT_TRUE(setKnobValues(node, kNoiseParamTranslate, { 3., -2. }));

        RenderedPlane half;
        ASSERT_TRUE(renderRgba(node, RectI(0, 0, kWidth / 2, kHeight / 2), &half, 1));
        ASSERT_TRUE(setKnobValues(node, kNoiseParamTranslate, { 2.5, -2.5 }));
        RenderedPlane full;
        ASSERT_TRUE(renderRgba(node, frame(), &full, 0));

        float worst = 0.f;
        for (int y = 0; y < kHeight / 2; ++y) {
            for (int x = 0; x < kWidth / 2; ++x) {
                worst = (std::max)(worst, std::fabs(at(half, x, y, 0) - at(full, 2 * x, 2 * y, 0)));
            }
        }
        EXPECT_LE(worst, kNoiseScaleTolerance) << "type " << type;
    }
}

// A quarter turn about the centre (32, 24): the rotated pattern at (x, y) is the plain one at
// (y + 8, 55 - x). Translation shifts it by whole pixels.
TEST_F(NativeNoiseTest, TransformMovesThePattern)
{
    resetProject();
    NodePtr node = makeNoise(NoiseCore::kNoiseTypeSimplex);
    ASSERT_TRUE(bool(node));
    RenderedPlane plain;
    ASSERT_TRUE(renderRgba(node, frame(), &plain));

    ASSERT_TRUE(setKnobValues(node, kNoiseParamRotate, { 90. }));
    RenderedPlane rotated;
    ASSERT_TRUE(renderRgba(node, frame(), &rotated));
    float worst = 0.f;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 8; x < 56; ++x) {
            worst = (std::max)(worst, std::fabs(at(rotated, x, y, 0) - at(plain, y + 8, 55 - x, 0)));
        }
    }
    EXPECT_LE(worst, kNoiseScaleTolerance) << "rotate";
    EXPECT_GT(countDifferent(plain, rotated, 0), kWidth * kHeight / 2);

    ASSERT_TRUE(setKnobValues(node, kNoiseParamRotate, { 0. }));
    ASSERT_TRUE(setKnobValues(node, kNoiseParamTranslate, { 5., -3. }));
    RenderedPlane moved;
    ASSERT_TRUE(renderRgba(node, frame(), &moved));
    worst = 0.f;
    for (int y = 0; y < kHeight - 3; ++y) {
        for (int x = 5; x < kWidth; ++x) {
            worst = (std::max)(worst, std::fabs(at(moved, x, y, 0) - at(plain, x - 5, y + 3, 0)));
        }
    }
    EXPECT_LE(worst, kNoiseScaleTolerance) << "translate";

    // Scale 2 about the centre: (x, y) samples the plain pattern at the centre plus half the offset.
    ASSERT_TRUE(setKnobValues(node, kNoiseParamTranslate, { 0., 0. }));
    ASSERT_TRUE(setKnobValues(node, kNoiseParamScale, { 2. }));
    ASSERT_TRUE(setKnobValues(node, kNoiseParamCenter, { 32.5, 24.5 }));
    RenderedPlane scaled;
    ASSERT_TRUE(renderRgba(node, frame(), &scaled));
    worst = 0.f;
    for (int y = 0; y < kHeight; y += 2) {
        for (int x = 0; x < kWidth; x += 2) {
            // Centre (x + 1/2) maps to 32.5 + (x - 32) / 2 = x / 2 + 16.5, the centre of pixel x / 2 + 16.
            worst = (std::max)(worst, std::fabs(at(scaled, x, y, 0) - at(plain, x / 2 + 16, y / 2 + 12, 0)));
        }
    }
    EXPECT_LE(worst, kNoiseScaleTolerance) << "scale";
}

TEST_F(NativeNoiseTest, EvolutionAnimatesAndLoops)
{
    resetProject();
    NodePtr node = makeNoise(NoiseCore::kNoiseTypePerlin);
    ASSERT_TRUE(bool(node));
    RenderedPlane start;
    ASSERT_TRUE(renderRgba(node, frame(), &start));

    // A small step changes every pixel a little.
    ASSERT_TRUE(setKnobValues(node, kNoiseParamEvolution, { 0.01 }));
    RenderedPlane step;
    ASSERT_TRUE(renderRgba(node, frame(), &step));
    EXPECT_GT(countDifferent(start, step, 0), kWidth * kHeight * 9 / 10);
    EXPECT_LT(maxAbsDifference(start, step), 0.1f) << "evolution is continuous";

    // Without a loop the pattern does not come back.
    ASSERT_TRUE(setKnobValues(node, kNoiseParamEvolution, { 3. }));
    RenderedPlane far;
    ASSERT_TRUE(renderRgba(node, frame(), &far));
    EXPECT_GT(maxAbsDifference(start, far), 0.05f);

    // With a loop period of 3 it comes back after 3 and 6, and differs halfway.
    ASSERT_TRUE(setKnobValues(node, kNoiseParamLoopPeriod, { 3. }));
    ASSERT_TRUE(setKnobValues(node, kNoiseParamEvolution, { 0. }));
    RenderedPlane loop0;
    ASSERT_TRUE(renderRgba(node, frame(), &loop0));
    for (double e : { 3., 6., -3. }) {
        ASSERT_TRUE(setKnobValues(node, kNoiseParamEvolution, { e }));
        RenderedPlane again;
        ASSERT_TRUE(renderRgba(node, frame(), &again));
        EXPECT_LE(maxAbsDifference(loop0, again), 1e-5f) << "evolution " << e;
    }
    ASSERT_TRUE(setKnobValues(node, kNoiseParamEvolution, { 1.5 }));
    RenderedPlane halfway;
    ASSERT_TRUE(renderRgba(node, frame(), &halfway));
    EXPECT_GT(maxAbsDifference(loop0, halfway), 0.05f);
}

TEST_F(NativeNoiseTest, WhiteNoiseReseedsPerFrameOnlyWhenAsked)
{
    resetProject();
    NodePtr node = makeNoise(NoiseCore::kNoiseTypeWhite);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kNoiseParamSize, { 1. }));

    RenderedPlane a;
    RenderedPlane b;
    ASSERT_TRUE(renderRgba(node, frame(), &a, 0, 1.));
    ASSERT_TRUE(renderRgba(node, frame(), &b, 0, 2.));
    EXPECT_EQ(0, countDifferent(a, b, 0)) << "off, the pattern holds across frames";
    // At a size of 1 neighbours are independent.
    int sameAsLeft = 0;
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 1; x < kWidth; ++x) {
            sameAsLeft += sameBits(at(a, x, y, 0), at(a, x - 1, y, 0)) ? 1 : 0;
        }
    }
    EXPECT_LT(sameAsLeft, kWidth);

    ASSERT_TRUE(setKnobValues(node, kNoiseParamReseedPerFrame, { 1. }));
    RenderedPlane c;
    RenderedPlane d;
    ASSERT_TRUE(renderRgba(node, frame(), &c, 0, 1.));
    ASSERT_TRUE(renderRgba(node, frame(), &d, 0, 2.));
    EXPECT_GT(countDifferent(c, d, 0), kWidth * kHeight * 9 / 10);
}

TEST_F(NativeNoiseTest, DecorrelationGivesEachChannelItsOwnNoise)
{
    resetProject();
    NodePtr node = makeNoise(NoiseCore::kNoiseTypeSimplex);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kNoiseParamColor0, { 0., 0., 0., 0. }));
    ASSERT_TRUE(setKnobValues(node, kNoiseParamColor1, { 1., 1., 1., 1. }));

    RenderedPlane together;
    ASSERT_TRUE(renderRgba(node, frame(), &together));
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            const float r = at(together, x, y, 0);
            for (int c = 1; c < 4; ++c) {
                ASSERT_TRUE(sameBits(r, at(together, x, y, c))) << "pixel (" << x << ", " << y << ") channel " << c;
            }
        }
    }

    ASSERT_TRUE(setKnobValues(node, kNoiseParamDecorrelate, { 1. }));
    RenderedPlane apart;
    ASSERT_TRUE(renderRgba(node, frame(), &apart));
    // Red keeps the knob's seed; the other channels each get another pattern.
    EXPECT_EQ(0, countDifferent(together, apart, 0));
    for (int c = 1; c < 4; ++c) {
        EXPECT_GT(countDifferent(together, apart, c), kWidth * kHeight * 9 / 10) << "channel " << c;
        for (int other = 1; other < c; ++other) {
            int same = 0;
            for (int y = 0; y < kHeight; ++y) {
                for (int x = 0; x < kWidth; ++x) {
                    same += sameBits(at(apart, x, y, c), at(apart, x, y, other)) ? 1 : 0;
                }
            }
            EXPECT_LT(same, kWidth * kHeight / 10) << "channels " << other << " and " << c;
        }
    }
}

// The octave count is capped at 16: asking for more renders exactly 16, and 16 differs from 15.
TEST_F(NativeNoiseTest, OctavesAreCapped)
{
    resetProject();
    NodePtr node = makeNoise(NoiseCore::kNoiseTypeValue);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kNoiseParamGain, { 0.9 }));
    ASSERT_TRUE(setKnobValues(node, kNoiseParamLacunarity, { 1.3 }));

    ASSERT_TRUE(setKnobValues(node, kNoiseParamOctaves, { (double)NoiseCore::kNoiseMaxOctaves }));
    RenderedPlane capped;
    ASSERT_TRUE(renderRgba(node, frame(), &capped));
    ASSERT_TRUE(setKnobValues(node, kNoiseParamOctaves, { 40. }));
    RenderedPlane many;
    ASSERT_TRUE(renderRgba(node, frame(), &many));
    EXPECT_EQ(0, countDifferent(capped, many, 0));

    ASSERT_TRUE(setKnobValues(node, kNoiseParamOctaves, { (double)NoiseCore::kNoiseMaxOctaves - 1 }));
    RenderedPlane fewer;
    ASSERT_TRUE(renderRgba(node, frame(), &fewer));
    EXPECT_GT(countDifferent(capped, fewer, 0), kWidth * kHeight / 2);
}

// The render goes through the node's pixel function: a few pixels against noisePixel() at the
// parameters the node bakes from its knobs (defaults except where set), rebuilt here.
TEST_F(NativeNoiseTest, PixelsAreTheCoreNoiseThroughTheColourMapping)
{
    resetProject();
    NodePtr node = makeNoise(NoiseCore::kNoiseTypeCellular);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kNoiseParamOutput, { (double)NoiseCore::kNoiseCellularF2MinusF1 }));
    ASSERT_TRUE(setKnobValues(node, kNoiseParamColor0, { 0.25, -1., 2., 0.5 }));
    ASSERT_TRUE(setKnobValues(node, kNoiseParamColor1, { 0.75, 1., -2., 1. }));
    ASSERT_TRUE(setKnobValues(node, kNoiseParamAmplitude, { 0.6 }));
    ASSERT_TRUE(setKnobValues(node, kNoiseParamOffset, { 0.1 }));
    ASSERT_TRUE(setKnobValues(node, kNoiseParamZOffset, { 4.25 }));

    RenderedPlane plane;
    ASSERT_TRUE(renderRgba(node, frame(), &plane));

    NoiseOp::Params params;
    params.noise = NoiseCore::defaultNoiseParams();
    params.noise.type = NoiseCore::kNoiseTypeCellular;
    params.noise.seed = 1234u;
    params.noise.fractal = NoiseCore::kNoiseFractalFBm;
    params.noise.octaves = 4;
    params.noise.lacunarity = 2.f;
    params.noise.gain = 0.5f;
    params.noise.weightedStrength = 0.f;
    params.noise.cellularMetric = NoiseCore::kNoiseMetricEuclidean;
    params.noise.cellularOutput = NoiseCore::kNoiseCellularF2MinusF1;
    params.noise.jitter = 1.f;
    params.noise.smoothness = 0.1f;
    params.noise.warpType = NoiseCore::kNoiseWarpNone;
    params.noise.warpAmount = 0.5f;
    params.noise.warpFrequency = 1.f;
    params.sizeX = 9.5;
    params.sizeY = 7.25;
    params.zOffset = 4.25;
    params.evolution = 0.;
    params.loopPeriod = 0.;
    params.translateX = 0.;
    params.translateY = 0.;
    params.rotate = 0.;
    params.scaleX = 1.;
    params.scaleY = 1.;
    params.centerX = kWidth / 2.;
    params.centerY = kHeight / 2.;
    params.par = 1.;
    const float c0[4] = { 0.25f, -1.f, 2.f, 0.5f };
    const float c1[4] = { 0.75f, 1.f, -2.f, 1.f };
    for (int c = 0; c < 4; ++c) {
        params.color0[c] = c0[c];
        params.color1[c] = c1[c];
    }
    params.amplitude = 0.6f;
    params.offset = 0.1f;
    params.decorrelate = false;
    const NoisePixelParams p = NoiseOp::pixelParams(params, 1., 1.);

    const int samples[][2] = { { 0, 0 }, { 63, 47 }, { 17, 5 }, { 40, 33 }, { 8, 44 } };
    for (const auto& s : samples) {
        float rgba[4];
        noisePixel(p, s[0], s[1], 15u, rgba);
        const float n = noisePixelValue(p, s[0], s[1], 0);
        for (int c = 0; c < 4; ++c) {
            EXPECT_TRUE(sameBits(rgba[c], at(plane, s[0], s[1], c))) << "pixel (" << s[0] << ", " << s[1] << ") channel " << c << ": core " << rgba[c] << ", render "
                                                                     << at(plane, s[0], s[1], c);
            const float t = 0.5f * (1.f + 0.6f * n) + 0.1f;
            EXPECT_NEAR(c0[c] + (c1[c] - c0[c]) * t, rgba[c], 1e-6f);
        }
    }
}
