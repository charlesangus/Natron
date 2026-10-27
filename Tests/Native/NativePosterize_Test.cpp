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

#include <bitset>
#include <cmath>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeParity.h"
#include "NativePluginIdentity.h"
#include "RenderBothWays.h"

#include "Engine/EffectInstance.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/Posterize.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;
} // namespace

class NativePosterizeTest
    : public BaseTest {
protected:
    NodePtr makeNode()
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_POSTERIZE), PLUGIN_MAJOR_NATRON_POSTERIZE);

        EXPECT_TRUE(node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get()));

        return node;
    }

    Posterize* effectOf(const NodePtr& node)
    {
        Posterize* effect = node ? dynamic_cast<Posterize*>(node->getEffectInstance().get()) : NULL;

        EXPECT_TRUE(effect != NULL);

        return effect;
    }

    PosterizeOp::Params bake(const NodePtr& node,
                             double time = kTime)
    {
        Posterize* effect = effectOf(node);

        return effect ? effect->bakePosterize(time, ViewIdx(0)) : PosterizeOp::Params();
    }

    // A width x height RGBA image of the constant value v, through the op's rows with the
    // image's bottom-left at (x0, y0).
    std::vector<float> renderRows(const NodePtr& node,
                                  const PosterizeOp::Params& params,
                                  float v,
                                  int x0,
                                  int y0,
                                  int width,
                                  int height)
    {
        Posterize* effect = effectOf(node);
        std::vector<float> src((size_t)width * height * 4, v);
        std::vector<float> dst(src.size(), -100.f);

        if (!effect) {
            return dst;
        }
        for (int y = 0; y < height; ++y) {
            RowIO io;
            io.nSrc = 1;
            io.src[0] = src.data() + (size_t)y * width * 4;
            io.dst = dst.data() + (size_t)y * width * 4;
            io.x0 = x0;
            io.y = y0 + y;
            io.width = width;
            io.nComps = 4;
            io.channels = std::bitset<4>(0x7);
            effect->posterizeRow(params, io);
        }

        return dst;
    }
};

// Every control of openfx-misc's Quantize, with its default, plus the range.
TEST_F(NativePosterizeTest, ControlsAndDefaults)
{
    NodePtr node = makeNode();

    ASSERT_TRUE(bool(node));
    struct Default {
        const char* name;
        double value;
    };
    const Default defaults[] = {
        { kPosterizeParamLevels, 16. },
        { kPosterizeParamRangeMin, 0. },
        { kPosterizeParamRangeMax, 1. },
    };
    for (const Default& d : defaults) {
        KnobDoublePtr knob = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(d.name));
        ASSERT_TRUE(bool(knob)) << d.name;
        EXPECT_DOUBLE_EQ(d.value, knob->getValue()) << d.name;
    }
    KnobChoicePtr dither = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kPosterizeParamDither));
    ASSERT_TRUE(bool(dither));
    EXPECT_EQ((int)PosterizeOp::eDitherNone, dither->getValue());
    EXPECT_EQ((int)PosterizeOp::eDitherCount, dither->getNumEntries());
    KnobIntPtr seed = std::dynamic_pointer_cast<KnobInt>(node->getKnobByName(kPosterizeParamSeed));
    ASSERT_TRUE(bool(seed));
    EXPECT_EQ(2000, seed->getValue());
    KnobBoolPtr staticSeed = std::dynamic_pointer_cast<KnobBool>(node->getKnobByName(kPosterizeParamStaticSeed));
    ASSERT_TRUE(bool(staticSeed));
    EXPECT_FALSE(staticSeed->getValue());
}

// Four levels over 0..1 are 0, 1/3, 2/3 and 1, each owning a quarter of the range, the bins
// closed at the top as in Quantize; a shifted range steps the same way.
TEST_F(NativePosterizeTest, StepPositions)
{
    NodePtr node = makeNode();

    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kPosterizeParamLevels, { 4. }));
    PosterizeOp::Params params = bake(node);
    ASSERT_FALSE(params.identity);
    struct Step {
        float in;
        float level;
    };
    const Step steps[] = {
        { 0.f, 0.f },
        { 0.1f, 0.f },
        { 0.25f, 0.f },
        { 0.26f, 1.f },
        { 0.5f, 1.f },
        { 0.51f, 2.f },
        { 0.74f, 2.f },
        { 0.76f, 3.f },
        { 1.f, 3.f },
    };
    for (const Step& s : steps) {
        EXPECT_FLOAT_EQ(s.level / 3.f, PosterizeOp::quantize(params, s.in, 0, 0, 0)) << s.in;
    }

    ASSERT_TRUE(setKnobValues(node, kPosterizeParamLevels, { 3. }));
    ASSERT_TRUE(setKnobValues(node, kPosterizeParamRangeMin, { 0.2 }));
    ASSERT_TRUE(setKnobValues(node, kPosterizeParamRangeMax, { 0.8 }));
    params = bake(node);
    const Step shifted[] = { { 0.2f, 0.f }, { 0.35f, 0.f }, { 0.45f, 1.f }, { 0.55f, 1.f }, { 0.65f, 2.f }, { 0.79f, 2.f } };
    for (const Step& s : shifted) {
        EXPECT_NEAR(0.2f + s.level * 0.3f, PosterizeOp::quantize(params, s.in, 0, 0, 0), 1e-6f) << s.in;
    }
}

// Values above the range and below it keep stepping at the same spacing; nothing is clamped.
TEST_F(NativePosterizeTest, ValuesOutsideTheRangeKeepStepping)
{
    NodePtr node = makeNode();

    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kPosterizeParamLevels, { 4. }));
    const PosterizeOp::Params params = bake(node);
    EXPECT_FLOAT_EQ(4.f / 3.f, PosterizeOp::quantize(params, 1.1f, 0, 0, 0));
    EXPECT_FLOAT_EQ(4.f / 3.f, PosterizeOp::quantize(params, 1.25f, 0, 0, 0));
    EXPECT_FLOAT_EQ(5.f / 3.f, PosterizeOp::quantize(params, 1.3f, 0, 0, 0));
    EXPECT_FLOAT_EQ(7.f / 3.f, PosterizeOp::quantize(params, 2.f, 0, 0, 0));
    EXPECT_FLOAT_EQ(-1.f / 3.f, PosterizeOp::quantize(params, -0.1f, 0, 0, 0));
    EXPECT_FLOAT_EQ(-2.f / 3.f, PosterizeOp::quantize(params, -0.3f, 0, 0, 0));
}

// Every dither picks the level below or above, gives the same output for the same seed and
// position, averages to the input over a tile, and the random one changes with the seed and,
// unless static, with the frame.
TEST_F(NativePosterizeTest, DitherIsDeterministicPerSeed)
{
    NodePtr node = makeNode();

    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kPosterizeParamLevels, { 4. }));
    const float value = 0.3f;
    // 0.3 lies between the levels 1/3 and 0 at 0.9 of the way up.
    const float lower = 0.f;
    const float upper = 1.f / 3.f;
    for (int mode = PosterizeOp::eDitherBayer2; mode < PosterizeOp::eDitherCount; ++mode) {
        ASSERT_TRUE(setKnobValues(node, kPosterizeParamDither, { (double)mode }));
        const PosterizeOp::Params params = bake(node);
        // Whole tiles of the matrix.
        const int size = (mode == PosterizeOp::eDitherBlueNoise14) ? 56 : ((mode == PosterizeOp::eDitherBayer8 || mode == PosterizeOp::eDitherBayer4) ? 48 : 50);
        const std::vector<float> a = renderRows(node, params, value, -7, 3, size, size);
        const std::vector<float> b = renderRows(node, params, value, -7, 3, size, size);
        EXPECT_EQ(a, b) << "mode " << mode;
        double sum = 0.;
        int count = 0;
        for (size_t i = 0; i < a.size(); ++i) {
            if (i % 4 == 3) {
                EXPECT_EQ(-100.f, a[i]) << "alpha is not processed";
                continue;
            }
            ASSERT_TRUE((a[i] == lower) || (a[i] == upper)) << "mode " << mode << ": " << a[i];
            sum += a[i];
            ++count;
        }
        // An ordered matrix of side M splits a fraction into M² steps.
        const int side = PosterizeOp::matrixSize(mode);
        const double tolerance = (mode == PosterizeOp::eDitherRandom) ? 0.01 : (1. / (3. * side * side) + 1e-4);
        EXPECT_NEAR(value, sum / count, tolerance) << "mode " << mode;
    }

    // The random dither: another seed gives other noise, and the frame is hashed in unless the
    // seed is static.
    ASSERT_TRUE(setKnobValues(node, kPosterizeParamDither, { (double)PosterizeOp::eDitherRandom }));
    const std::vector<float> first = renderRows(node, bake(node), value, 0, 0, 16, 16);
    const std::vector<float> nextFrame = renderRows(node, bake(node, kTime + 1.), value, 0, 0, 16, 16);
    EXPECT_NE(first, nextFrame);
    ASSERT_TRUE(setKnobValues(node, kPosterizeParamSeed, { 7. }));
    const std::vector<float> otherSeed = renderRows(node, bake(node), value, 0, 0, 16, 16);
    EXPECT_NE(first, otherSeed);
    ASSERT_TRUE(setKnobValues(node, kPosterizeParamStaticSeed, { 1. }));
    EXPECT_EQ(renderRows(node, bake(node), value, 0, 0, 16, 16), renderRows(node, bake(node, kTime + 5.), value, 0, 0, 16, 16));
    // The channels of one pixel get independent noise.
    const std::vector<float> wide = renderRows(node, bake(node), 0.5f / 3.f, 0, 0, 32, 32);
    int differ = 0;
    for (size_t i = 0; i < wide.size(); i += 4) {
        differ += (wide[i] != wide[i + 1]) ? 1 : 0;
    }
    EXPECT_GT(differ, 0);
}

// The ordered dither tiles its matrix: the pattern repeats every side pixels in x and y.
TEST_F(NativePosterizeTest, OrderedDitherTiles)
{
    NodePtr node = makeNode();

    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kPosterizeParamDither, { (double)PosterizeOp::eDitherBayer8 }));
    const PosterizeOp::Params params = bake(node);
    for (int y = -9; y < 9; ++y) {
        for (int x = -9; x < 9; ++x) {
            const float v = 0.37f;
            EXPECT_EQ(PosterizeOp::quantize(params, v, x, y, 0), PosterizeOp::quantize(params, v, x + 8, y - 16, 0)) << x << "," << y;
        }
    }
}

// Fewer than 2 levels, or an empty range, leave the image as it is.
TEST_F(NativePosterizeTest, LevelsBelowTwoIsIdentity)
{
    NodePtr node = makeNode();

    ASSERT_TRUE(bool(node));
    const double counts[] = { 1.999, 1., 0. };
    for (double n : counts) {
        ASSERT_TRUE(setKnobValues(node, kPosterizeParamLevels, { n }));
        for (int mode = 0; mode < PosterizeOp::eDitherCount; ++mode) {
            ASSERT_TRUE(setKnobValues(node, kPosterizeParamDither, { (double)mode }));
            EXPECT_TRUE(PosterizeOp().isIdentity(bake(node))) << n << " mode " << mode;
        }
    }
    ASSERT_TRUE(setKnobValues(node, kPosterizeParamLevels, { 2. }));
    EXPECT_FALSE(PosterizeOp().isIdentity(bake(node)));
    ASSERT_TRUE(setKnobValues(node, kPosterizeParamRangeMax, { 0. }));
    EXPECT_TRUE(PosterizeOp().isIdentity(bake(node)));
}

TEST_F(NativePosterizeTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_POSTERIZE, PLUGIN_MAJOR_NATRON_POSTERIZE, "Posterize");
}
