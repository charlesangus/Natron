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
#include <functional>
#include <iostream>
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
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobChannelSelect.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Color/ColorMatrix.h"
#include "Engine/Nodes/Color/Threshold.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;

// The node sums single-precision products of single-precision coefficients, so against a double
// expectation the error scales with the sum of the terms' magnitudes rather than with the result,
// which can cancel to near 0. Four rounding steps over terms adding up to under 10 stay below
// 5e-6.
const ParityTolerance kMatrixTolerance = ParityTolerance::make(5e-6, 1e-6);

// Rows R, G, B, A over columns r, g, b, a: coefficients above 1, negative and mixing alpha in, so
// the results run well outside [0, 1].
const double kHdrMatrix[4][4] = {
    { 1.75, -0.4, 0.25, 0.5 },
    { 0.3, 2.5, -0.8, 0. },
    { -1.2, 0.1, 3., -0.25 },
    { 0.2, 0.3, 0.1, 0.6 },
};

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

bool
renderRgba(const NodePtr& node,
           const RectI& window,
           RenderedPlane* plane)
{
    std::list<ImageLayerDesc> layers(1, ImageLayerDesc::mapNCompsToColorLayer(4));
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, layers, &planes, &error);

    EXPECT_TRUE(ok) << error;
    if (!ok || (planes.size() != 1)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

float
planeValue(const RenderedPlane& plane,
           int x,
           int y,
           int c)
{
    const std::size_t nComps = plane.channels.size();

    return plane.pixels[(static_cast<std::size_t>(y - plane.window.y1) * plane.window.width() + (x - plane.window.x1)) * nComps + c];
}

bool
setMatrixSize(const NodePtr& node,
              ColorMatrixOp::MatrixSizeEnum size)
{
    return setKnobValues(node, kColorMatrixParamMatrixSize, std::vector<double>(1, (double)size));
}

// Stores all 16 coefficients, rows R, G, B, A over columns r, g, b, a, without touching the size.
bool
storeMatrix(const NodePtr& node,
            const double m[4][4])
{
    std::vector<double> values;
    for (int r = 0; r < 4; ++r) {
        values.insert(values.end(), m[r], m[r] + 4);
    }

    return setKnobValues(node, kColorMatrixParamMatrix, values);
}

// Stores the coefficients and sets the size to 4x4, so that all of them apply.
bool
setMatrix(const NodePtr& node,
          const double m[4][4])
{
    return setMatrixSize(node, ColorMatrixOp::eMatrixSize4x4) && storeMatrix(node, m);
}

} // namespace

class NativeColorMatrixThresholdTest
    : public BaseTest {
protected:
    NodePtr makeSource()
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);

        EXPECT_TRUE(bool(source));

        return source;
    }

    NodePtr makeNode(const char* id,
                     int major,
                     const NodePtr& source)
    {
        NodePtr node = createNode(QString::fromUtf8(id), major);

        EXPECT_TRUE(isNative(node)) << id;
        if (node && source) {
            connectNodes(source, node, 0, true);
        }

        return node;
    }

    bool identityAt(const NodePtr& node,
                    const RectI& window,
                    int* inputNb)
    {
        double inputTime = 0.;
        ViewIdx inputView(0);

        *inputNb = -1;

        return node->getEffectInstance()->isIdentity_public(false, 0, kTime, RenderScale::identity, window, ViewIdx(0), &inputTime, &inputView, inputNb);
    }

    // Renders `node` over the source and checks each channel against `expected`, which gets the
    // channel and the source pixel's RGBA.
    void expectPixels(const NodePtr& node,
                      const NodePtr& source,
                      const std::function<double(int, const double*)>& expected,
                      const std::string& what,
                      const ParityTolerance& tolerance = ParityTolerance::arithmetic())
    {
        const RectI window = paritySourceWindow(source, kTime, 0);
        RenderedPlane plane;

        ASSERT_TRUE(renderRgba(node, window, &plane)) << what;
        ASSERT_EQ(4u, plane.channels.size()) << what;
        for (int y = window.y1; y < window.y2; ++y) {
            for (int x = window.x1; x < window.x2; ++x) {
                double in[4];
                for (int c = 0; c < 4; ++c) {
                    in[c] = paritySourceColorValue(c, x, y, kTime);
                }
                for (int c = 0; c < 4; ++c) {
                    const double want = expected(c, in);
                    const float got = planeValue(plane, x, y, c);
                    ASSERT_TRUE(tolerance.accepts((float)want, got)) << what << " at " << x << "," << y << " c=" << c << ": want " << want << ", got " << got;
                }
            }
        }
    }
};

TEST_F(NativeColorMatrixThresholdTest, ColorMatrixHdrMaths)
{
    NodePtr source = makeSource();
    NodePtr node = makeNode(PLUGINID_NATRON_COLORMATRIX, PLUGIN_MAJOR_NATRON_COLORMATRIX, source);
    ASSERT_TRUE(bool(source) && bool(node));
    ASSERT_TRUE(setMatrix(node, kHdrMatrix));

    const std::function<double(int, const double*)> product = [](int c, const double* in) {
        return kHdrMatrix[c][0] * in[0] + kHdrMatrix[c][1] * in[1] + kHdrMatrix[c][2] * in[2] + kHdrMatrix[c][3] * in[3];
    };
    expectPixels(node, source, product, "no clamp", kMatrixTolerance);

    ASSERT_TRUE(setKnobValues(node, kColorMatrixParamClampBlack, std::vector<double>(1, 1.)));
    expectPixels(node, source, [&product](int c, const double* in) { return std::max(0., product(c, in)); }, "clamp black", kMatrixTolerance);

    ASSERT_TRUE(setKnobValues(node, kColorMatrixParamClampWhite, std::vector<double>(1, 1.)));
    expectPixels(node, source, [&product](int c, const double* in) { return std::min(1., std::max(0., product(c, in))); }, "clamp both", kMatrixTolerance);

    ASSERT_TRUE(setKnobValues(node, kColorMatrixParamClampBlack, std::vector<double>(1, 0.)));
    expectPixels(node, source, [&product](int c, const double* in) { return std::min(1., product(c, in)); }, "clamp white", kMatrixTolerance);
}

TEST_F(NativeColorMatrixThresholdTest, ColorMatrixIdentityDetection)
{
    NodePtr source = makeSource();
    NodePtr node = makeNode(PLUGINID_NATRON_COLORMATRIX, PLUGIN_MAJOR_NATRON_COLORMATRIX, source);
    ASSERT_TRUE(bool(source) && bool(node));
    const RectI window = paritySourceWindow(source, kTime, 0);
    int inputNb = -1;

    EXPECT_TRUE(identityAt(node, window, &inputNb));
    EXPECT_EQ(0, inputNb);
    ASSERT_TRUE(setMatrixSize(node, ColorMatrixOp::eMatrixSize4x4));
    EXPECT_TRUE(identityAt(node, window, &inputNb));

    ASSERT_TRUE(setKnobValues(node, kColorMatrixParamClampBlack, std::vector<double>(1, 1.)));
    EXPECT_FALSE(identityAt(node, window, &inputNb));
    ASSERT_TRUE(setKnobValues(node, kColorMatrixParamClampBlack, std::vector<double>(1, 0.)));
    ASSERT_TRUE(setKnobValues(node, kColorMatrixParamClampWhite, std::vector<double>(1, 1.)));
    EXPECT_FALSE(identityAt(node, window, &inputNb));
    ASSERT_TRUE(setKnobValues(node, kColorMatrixParamClampWhite, std::vector<double>(1, 0.)));
    EXPECT_TRUE(identityAt(node, window, &inputNb));

    // At 4x4 each off-diagonal and diagonal coefficient alone breaks the identity; at 3x3 only
    // those outside the alpha row and column do.
    for (int size = 0; size < 2; ++size) {
        const bool fourByFour = (size == 1);
        ASSERT_TRUE(setMatrixSize(node, fourByFour ? ColorMatrixOp::eMatrixSize4x4 : ColorMatrixOp::eMatrixSize3x3));
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                std::vector<double> m(16, 0.);
                for (int d = 0; d < 4; ++d) {
                    m[d * 4 + d] = 1.;
                }
                m[r * 4 + c] += 0.5;
                ASSERT_TRUE(setKnobValues(node, kColorMatrixParamMatrix, m));
                const bool readsCoefficient = fourByFour || ((r < 3) && (c < 3));
                EXPECT_EQ(!readsCoefficient, identityAt(node, window, &inputNb)) << "size " << (fourByFour ? 4 : 3) << ", row " << r << " column " << c;
                m[r * 4 + c] -= 0.5;
                ASSERT_TRUE(setKnobValues(node, kColorMatrixParamMatrix, m));
                EXPECT_TRUE(identityAt(node, window, &inputNb)) << "size " << (fourByFour ? 4 : 3) << ", row " << r << " column " << c;
            }
        }
    }

    ASSERT_TRUE(setMatrix(node, kHdrMatrix));
    EXPECT_FALSE(identityAt(node, window, &inputNb));
    ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, std::vector<double>(1, 0.)));
    EXPECT_TRUE(identityAt(node, window, &inputNb));
    EXPECT_EQ(0, inputNb);
}

// The default size is 3x3: R, G and B mix over r, g and b only, and alpha comes through bit for
// bit although the stored alpha row and column are far from identity. Switching to 4x4 brings
// the stored alpha coefficients back into play.
TEST_F(NativeColorMatrixThresholdTest, ColorMatrixThreeByThreeLeavesAlphaUntouched)
{
    NodePtr source = makeSource();
    NodePtr node = makeNode(PLUGINID_NATRON_COLORMATRIX, PLUGIN_MAJOR_NATRON_COLORMATRIX, source);
    ASSERT_TRUE(bool(source) && bool(node));
    ASSERT_TRUE(storeMatrix(node, kHdrMatrix));
    ColorMatrix* effect = dynamic_cast<ColorMatrix*>(node->getEffectInstance().get());
    ASSERT_TRUE(effect != NULL);
    EXPECT_TRUE(effect->bakeColorMatrix(kTime, ViewIdx(0)).threeByThree);

    const RectI window = paritySourceWindow(source, kTime, 0);
    RenderedPlane plane;
    ASSERT_TRUE(renderRgba(node, window, &plane));
    ASSERT_EQ(4u, plane.channels.size());
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            double in[4];
            for (int c = 0; c < 4; ++c) {
                in[c] = paritySourceColorValue(c, x, y, kTime);
            }
            for (int c = 0; c < 3; ++c) {
                const double want = kHdrMatrix[c][0] * in[0] + kHdrMatrix[c][1] * in[1] + kHdrMatrix[c][2] * in[2];
                const float got = planeValue(plane, x, y, c);
                ASSERT_TRUE(kMatrixTolerance.accepts((float)want, got)) << "at " << x << "," << y << " c=" << c << ": want " << want << ", got " << got;
            }
            ASSERT_EQ((float)in[3], planeValue(plane, x, y, 3)) << "alpha at " << x << "," << y;
        }
    }

    ASSERT_TRUE(setMatrixSize(node, ColorMatrixOp::eMatrixSize4x4));
    EXPECT_FALSE(effect->bakeColorMatrix(kTime, ViewIdx(0)).threeByThree);
    expectPixels(node, source, [](int c, const double* in) { return kHdrMatrix[c][0] * in[0] + kHdrMatrix[c][1] * in[1] + kHdrMatrix[c][2] * in[2] + kHdrMatrix[c][3] * in[3]; }, "4x4 after 3x3", kMatrixTolerance);
}

// At softness 0 a value equal to the level gives 1. The levels are taken from source pixels so
// the equality is actually met, and alpha's level of 1 meets its exact-1 bands.
TEST_F(NativeColorMatrixThresholdTest, ThresholdBoundaryAtSoftnessZero)
{
    NodePtr source = makeSource();
    NodePtr node = makeNode(PLUGINID_NATRON_THRESHOLD, PLUGIN_MAJOR_NATRON_THRESHOLD, source);
    ASSERT_TRUE(bool(source) && bool(node));
    const RectI window = paritySourceWindow(source, kTime, 0);
    const int px = window.x1 + window.width() / 3;
    const int py = window.y1 + window.height() / 2;
    const double level[4] = { paritySourceColorValue(0, px, py, kTime), paritySourceColorValue(1, px, py, kTime), paritySourceColorValue(2, px, py, kTime), 1. };
    ASSERT_TRUE(setKnobValues(node, kThresholdParamLevel, std::vector<double>(level, level + 4)));
    ASSERT_TRUE(setKnobValues(node, kThresholdParamSoftness, std::vector<double>(4, 0.)));

    int equal[4] = { 0, 0, 0, 0 };
    const std::function<double(int, const double*)> step = [&level, &equal](int c, const double* in) {
        if ((float)in[c] == (float)level[c]) {
            ++equal[c];
        }

        return ((float)in[c] >= (float)level[c]) ? 1. : 0.;
    };
    expectPixels(node, source, step, "softness 0");
    for (int c = 0; c < 4; ++c) {
        EXPECT_GT(equal[c], 0) << "no value equals the level of channel " << c;
    }
}

TEST_F(NativeColorMatrixThresholdTest, ThresholdRamp)
{
    NodePtr source = makeSource();
    NodePtr node = makeNode(PLUGINID_NATRON_THRESHOLD, PLUGIN_MAJOR_NATRON_THRESHOLD, source);
    ASSERT_TRUE(bool(source) && bool(node));
    const double level[4] = { 0.5, 0.25, 1.2, 0.5 };
    const double softness[4] = { 0.25, 0.5, 0.4, 0.1 };
    ASSERT_TRUE(setKnobValues(node, kThresholdParamLevel, std::vector<double>(level, level + 4)));
    ASSERT_TRUE(setKnobValues(node, kThresholdParamSoftness, std::vector<double>(softness, softness + 4)));

    int inRamp[4] = { 0, 0, 0, 0 };
    const std::function<double(int, const double*)> expected = [&level, &softness, &inRamp](int c, const double* in) {
        const double low = level[c] - softness[c];
        const double high = level[c] + softness[c];
        if (in[c] >= high) {
            return 1.;
        }
        if (in[c] <= low) {
            return 0.;
        }
        ++inRamp[c];

        return (in[c] - low) / (high - low);
    };
    expectPixels(node, source, expected, "ramp");
    for (int c = 0; c < 4; ++c) {
        EXPECT_GT(inRamp[c], 0) << "no value of channel " << c << " falls on the ramp";
    }
}

// Threshold changes every value at any setting, so only mix 0 makes it an identity.
TEST_F(NativeColorMatrixThresholdTest, ThresholdIdentityOnlyAtMixZero)
{
    NodePtr source = makeSource();
    NodePtr node = makeNode(PLUGINID_NATRON_THRESHOLD, PLUGIN_MAJOR_NATRON_THRESHOLD, source);
    ASSERT_TRUE(bool(source) && bool(node));
    const RectI window = paritySourceWindow(source, kTime, 0);
    int inputNb = -1;

    EXPECT_FALSE(identityAt(node, window, &inputNb));
    ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, std::vector<double>(1, 0.)));
    EXPECT_TRUE(identityAt(node, window, &inputNb));
    EXPECT_EQ(0, inputNb);
}

TEST_F(NativeColorMatrixThresholdTest, BothSchedulerModesAgreeWithNoUnplannedPull)
{
    const char* const ids[2] = { PLUGINID_NATRON_COLORMATRIX, PLUGINID_NATRON_THRESHOLD };
    for (int n = 0; n < 2; ++n) {
        NodePtr source = makeSource();
        NodePtr mask = makeSource();
        NodePtr node = makeNode(ids[n], 1, source);
        ASSERT_TRUE(bool(source) && bool(mask) && bool(node));
        setParitySourceOrigin(mask, 8, 4);
        int maskInput = -1;
        for (int i = 0; i < node->getNInputs(); ++i) {
            if (node->getInputLabel(i) == "Mask") {
                maskInput = i;
            }
        }
        ASSERT_GE(maskInput, 0);
        connectNodes(mask, node, maskInput, true);
        ASSERT_TRUE(setKnobValues(node, "enableMask_Mask", std::vector<double>(1, 1.)));
        KnobChannelSelect* maskChannel = dynamic_cast<KnobChannelSelect*>(node->getKnobByName("maskChannel_Mask").get());
        ASSERT_TRUE(maskChannel != NULL);
        maskChannel->set("rgba.A");
        KnobChannelSelect* unPremultBy = dynamic_cast<KnobChannelSelect*>(node->getKnobByName(kUnPremultByKnobName).get());
        ASSERT_TRUE(unPremultBy != NULL);
        unPremultBy->set("rgba.A");
        ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, std::vector<double>(1, 0.75)));
        if (n == 0) {
            ASSERT_TRUE(setMatrix(node, kHdrMatrix));
        } else {
            ASSERT_TRUE(setKnobValues(node, kThresholdParamLevel, std::vector<double>(4, 0.5)));
            ASSERT_TRUE(setKnobValues(node, kThresholdParamSoftness, std::vector<double>(4, 0.25)));
        }

        std::vector<int> poolSizes;
        poolSizes.push_back(1);
        poolSizes.push_back(4);
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            const RectI window = paritySourceWindow(source, kTime, mipmapLevel);
            std::vector<int> unplannedPulls;
            const RenderMismatch m = renderBothWaysDirect(node, kTime, ViewIdx(0), mipmapLevel, window, poolSizes, std::function<void()>(), 0.f, &unplannedPulls);
            EXPECT_FALSE(m.any) << ids[n] << ", mipmap " << mipmapLevel << ": " << describe(m);
            ASSERT_EQ(poolSizes.size(), unplannedPulls.size()) << ids[n];
            for (std::size_t i = 0; i < unplannedPulls.size(); ++i) {
                EXPECT_EQ(0, unplannedPulls[i]) << ids[n] << ", mipmap " << mipmapLevel << ", pool " << poolSizes[i];
            }
        }
    }
}

TEST_F(NativeColorMatrixThresholdTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_COLORMATRIX, PLUGIN_MAJOR_NATRON_COLORMATRIX, "ColorMatrix");
    expectNativeIdentity(PLUGINID_NATRON_THRESHOLD, PLUGIN_MAJOR_NATRON_THRESHOLD, "Threshold");
}
