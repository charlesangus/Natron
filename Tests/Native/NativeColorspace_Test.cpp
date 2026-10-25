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
#include "Engine/Nodes/Color/Colorspace.h"
#include "Engine/Nodes/Image/ColorMath.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Project.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;
const char* const kLinearRec709Space = "Linear Rec.709 (sRGB)";
const char* const kAcesCgSpace = "ACEScg";

// Values on both sides of 0 and 1 and well above, as HDR renders have them.
const float kValues[] = { 0.f, 1.f, -0.25f, 0.5f, 1.5f, 0.1f, 0.9f, -1.f, 2.f, 0.3333f, 0.7f, 0.05f, 2.75f, 0.2f, -0.6f, 0.45f, 1.1f, 0.8f, 6.f };

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

ColorMath::ColorspaceModelEnum
model(int m)
{
    return (ColorMath::ColorspaceModelEnum)m;
}

void
convert(const ColorMath::ColorspaceColorimetry& cm,
        int from,
        int to,
        float* v)
{
    ColorMath::colorspaceConvert(cm, ColorMath::colorspaceSide(model(from)), ColorMath::colorspaceSide(model(to)), v);
}

// Whether rgb is away from the points where model m loses the information to invert it: hue at
// zero value, lightness 0 or 1, or intensity, and chromaticity at zero luminance. Near them a
// round trip is ill-conditioned rather than wrong.
bool
invertibleNear(const ColorMath::ColorspaceColorimetry& cm,
               int m,
               const float* rgb)
{
    const float maxv = (std::max)((std::max)(rgb[0], rgb[1]), rgb[2]);
    const float minv = (std::min)((std::min)(rgb[0], rgb[1]), rgb[2]);
    const bool chroma = maxv != minv;
    const float sum = rgb[0] + rgb[1] + rgb[2];
    float xyz[3] = { rgb[0], rgb[1], rgb[2] };

    ColorMath::colorspaceMul3(cm.toXYZ, xyz);
    switch (m) {
    case ColorMath::eColorspaceModelHSV:
        return !chroma || (std::fabs(maxv) >= 0.05f);
    case ColorMath::eColorspaceModelHSL:
        return !chroma || ((std::fabs(maxv + minv) >= 0.05f) && (std::fabs(2.f - maxv - minv) >= 0.05f));
    case ColorMath::eColorspaceModelHSI:
        return !chroma || (std::fabs(sum) >= 0.05f);
    case ColorMath::eColorspaceModelXyY:
        return ((rgb[0] == 0.f) && (rgb[1] == 0.f) && (rgb[2] == 0.f)) || ((std::fabs(xyz[1]) >= 0.05f) && (std::fabs(xyz[0] + xyz[1] + xyz[2]) >= 0.05f));
    default:
        return true;
    }
}

void
expectRoundTrips(const ColorMath::ColorspaceColorimetry& cm,
                 const std::string& space)
{
    const int n = sizeof(kValues) / sizeof(kValues[0]);

    for (int m = 1; m < ColorMath::eColorspaceModelCount; ++m) {
        int checked = 0;
        for (int a = 0; a < n; ++a) {
            for (int b = 0; b < n; ++b) {
                for (int c = 0; c < n; ++c) {
                    const float rgb[3] = { kValues[a], kValues[b], kValues[c] };
                    if (!invertibleNear(cm, m, rgb)) {
                        continue;
                    }
                    float v[3] = { rgb[0], rgb[1], rgb[2] };
                    convert(cm, ColorMath::eColorspaceModelRGB, m, v);
                    for (int k = 0; k < 3; ++k) {
                        ASSERT_TRUE(std::isfinite(v[k])) << space << " model " << m << " of " << rgb[0] << " " << rgb[1] << " " << rgb[2];
                    }
                    convert(cm, m, ColorMath::eColorspaceModelRGB, v);
                    for (int k = 0; k < 3; ++k) {
                        const double bound = 1e-5 * (std::max)(1., (double)std::fabs(rgb[k]));
                        ASSERT_NEAR(rgb[k], v[k], bound) << space << " model " << m << " of " << rgb[0] << " " << rgb[1] << " " << rgb[2] << " channel " << k;
                    }
                    ++checked;
                }
            }
        }
        EXPECT_GT(checked, n * n * n / 2) << space << " model " << m;
    }
}

} // namespace

class NativeColorspaceTest
    : public BaseTest {
protected:
    void TearDown() OVERRIDE
    {
        setWorkingSpace(kAcesCgSpace);
        BaseTest::TearDown();
    }

    void setWorkingSpace(const char* space)
    {
        KnobChoicePtr workingSpace = getApp()->getProject()->getKnobByNameAndType<KnobChoice>("workingSpace");
        ASSERT_TRUE(bool(workingSpace));
        workingSpace->setValueFromID(space, 0);
        ASSERT_EQ(std::string(space), getApp()->getProject()->getWorkingColorSpace());
    }

    NodePtr makeSource()
    {
        NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);

        EXPECT_TRUE(bool(source));

        return source;
    }

    NodePtr makeNode(const NodePtr& source)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_COLORSPACE), PLUGIN_MAJOR_NATRON_COLORSPACE);

        EXPECT_TRUE(isNative(node));
        if (node && source) {
            connectNodes(source, node, 0, true);
        }

        return node;
    }

    // The colorimetry the node reads from the project.
    ColorMath::ColorspaceColorimetry workingColorimetry(const NodePtr& node)
    {
        double m[9];

        ColorMath::workingToXYZ(node->getEffectInstance().get(), m);

        return ColorMath::colorspaceColorimetry(m);
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

    bool renderRgba(const NodePtr& node,
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
};

TEST_F(NativeColorspaceTest, RoundTripsEveryModel)
{
    expectRoundTrips(ColorMath::colorspaceColorimetry(ColorMath::acesCgToXYZ()), kAcesCgSpace);

    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    setWorkingSpace(kLinearRec709Space);
    expectRoundTrips(workingColorimetry(node), kLinearRec709Space);
}

TEST_F(NativeColorspaceTest, WorkingWhiteIsTheWhitePoint)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    setWorkingSpace(kLinearRec709Space);
    const ColorMath::ColorspaceColorimetry cm = workingColorimetry(node);

    // D65, which the project's matrices to XYZ are adapted to.
    const double d65[3] = { 0.95047, 1., 1.08883 };
    float white[3] = { 1.f, 1.f, 1.f };
    convert(cm, ColorMath::eColorspaceModelRGB, ColorMath::eColorspaceModelXYZ, white);
    for (int c = 0; c < 3; ++c) {
        EXPECT_NEAR(d65[c], white[c], 1e-3) << c;
        EXPECT_NEAR(cm.white[c], white[c], 1e-6) << c;
    }

    float xyY[3] = { 1.f, 1.f, 1.f };
    convert(cm, ColorMath::eColorspaceModelRGB, ColorMath::eColorspaceModelXyY, xyY);
    EXPECT_NEAR(0.3127, xyY[0], 1e-4);
    EXPECT_NEAR(0.3290, xyY[1], 1e-4);
    EXPECT_NEAR(1., xyY[2], 1e-5);

    for (float grey : { 1.f, 0.18f, 4.f }) {
        float lab[3] = { grey, grey, grey };
        convert(cm, ColorMath::eColorspaceModelRGB, ColorMath::eColorspaceModelLab, lab);
        EXPECT_NEAR(0., lab[1], 1e-4) << grey;
        EXPECT_NEAR(0., lab[2], 1e-4) << grey;
    }
    float lab[3] = { 1.f, 1.f, 1.f };
    convert(cm, ColorMath::eColorspaceModelRGB, ColorMath::eColorspaceModelLab, lab);
    EXPECT_NEAR(100., lab[0], 1e-4);

    // ACEScg's white is its own, about D60, as no adaptation applies to the fallback.
    const ColorMath::ColorspaceColorimetry aces = ColorMath::colorspaceColorimetry(ColorMath::acesCgToXYZ());
    EXPECT_NEAR(0.32168, aces.whiteXy[0], 1e-4);
    EXPECT_NEAR(0.33767, aces.whiteXy[1], 1e-4);
}

TEST_F(NativeColorspaceTest, Rec709PrimariesGiveTheirLab)
{
    NodePtr node = makeNode(NodePtr());
    ASSERT_TRUE(bool(node));
    setWorkingSpace(kLinearRec709Space);
    const ColorMath::ColorspaceColorimetry cm = workingColorimetry(node);

    // CIE L*a*b* (D65) of the sRGB primaries.
    const double want[3][3] = {
        { 53.24, 80.09, 67.20 },
        { 87.73, -86.18, 83.18 },
        { 32.30, 79.19, -107.86 },
    };
    for (int p = 0; p < 3; ++p) {
        float v[3] = { 0.f, 0.f, 0.f };
        v[p] = 1.f;
        convert(cm, ColorMath::eColorspaceModelRGB, ColorMath::eColorspaceModelLab, v);
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR(want[p][c], v[c], 0.05) << "primary " << p << " channel " << c;
        }
    }
}

TEST_F(NativeColorspaceTest, WorkingSpaceFallsBackToAcesCg)
{
    double m[9];

    ColorMath::workingToXYZ(NULL, m);
    for (int i = 0; i < 9; ++i) {
        EXPECT_EQ(ColorMath::acesCgToXYZ()[i], m[i]) << i;
    }

    const double singular[9] = { 1., 1., 1., 1., 1., 1., 0., 0., 1. };
    const ColorMath::ColorspaceColorimetry fallback = ColorMath::colorspaceColorimetry(singular);
    const ColorMath::ColorspaceColorimetry aces = ColorMath::colorspaceColorimetry(ColorMath::acesCgToXYZ());
    for (int i = 0; i < 9; ++i) {
        EXPECT_EQ(aces.toXYZ[i], fallback.toXYZ[i]) << i;
        EXPECT_EQ(aces.fromXYZ[i], fallback.fromXYZ[i]) << i;
    }
}

// Known values of each model, which the round trips alone would not catch.
TEST_F(NativeColorspaceTest, ModelsGiveKnownValues)
{
    const ColorMath::ColorspaceColorimetry cm = ColorMath::colorspaceColorimetry(ColorMath::acesCgToXYZ());
    struct Known {
        int model;
        float rgb[3];
        float want[3];
    };
    const Known known[] = {
        { ColorMath::eColorspaceModelHSV, { 1.f, 0.f, 0.f }, { 0.f, 1.f, 1.f } },
        { ColorMath::eColorspaceModelHSV, { 0.f, 0.5f, 0.f }, { 1.f / 3.f, 1.f, 0.5f } },
        { ColorMath::eColorspaceModelHSV, { 0.f, 0.f, 2.f }, { 2.f / 3.f, 1.f, 2.f } },
        { ColorMath::eColorspaceModelHSV, { 1.f, 0.f, 1.f }, { 5.f / 6.f, 1.f, 1.f } },
        { ColorMath::eColorspaceModelHSL, { 1.f, 0.f, 0.f }, { 0.f, 1.f, 0.5f } },
        { ColorMath::eColorspaceModelHSL, { 0.75f, 0.25f, 0.25f }, { 0.f, 0.5f, 0.5f } },
        { ColorMath::eColorspaceModelHSI, { 1.f, 0.f, 0.f }, { 0.f, 1.f, 1.f / 3.f } },
        { ColorMath::eColorspaceModelHSI, { 0.f, 1.f, 0.f }, { 1.f / 3.f, 1.f, 1.f / 3.f } },
        { ColorMath::eColorspaceModelHSI, { 1.f, 1.f, 0.f }, { 1.f / 6.f, 1.f, 2.f / 3.f } },
        { ColorMath::eColorspaceModelYPbPr709, { 1.f, 0.f, 0.f }, { 0.2126f, -0.1146f, 0.5f } },
        { ColorMath::eColorspaceModelYPbPr601, { 0.f, 0.f, 1.f }, { 0.114f, 0.5f, -0.0813f } },
        { ColorMath::eColorspaceModelYPbPr2020, { 1.f, 1.f, 1.f }, { 1.f, 0.f, 0.f } },
        { ColorMath::eColorspaceModelYCbCr709, { 0.f, 0.f, 0.f }, { 16.f / 255.f, 128.f / 255.f, 128.f / 255.f } },
        { ColorMath::eColorspaceModelYCbCr709, { 1.f, 1.f, 1.f }, { 235.f / 255.f, 128.f / 255.f, 128.f / 255.f } },
        { ColorMath::eColorspaceModelYCbCr601, { 0.f, 0.f, 1.f }, { (16.f + 219.f * 0.114f) / 255.f, 240.f / 255.f, (128.f - 224.f * 0.0813f) / 255.f } },
        { ColorMath::eColorspaceModelYUV601, { 0.f, 0.f, 1.f }, { 0.114f, 0.436f, -0.1001f } },
        { ColorMath::eColorspaceModelYUV709, { 1.f, 0.f, 0.f }, { 0.2126f, -0.0999f, 0.615f } },
    };
    for (const Known& k : known) {
        float v[3] = { k.rgb[0], k.rgb[1], k.rgb[2] };
        convert(cm, ColorMath::eColorspaceModelRGB, k.model, v);
        for (int c = 0; c < 3; ++c) {
            EXPECT_NEAR(k.want[c], v[c], 1e-4) << "model " << k.model << " of " << k.rgb[0] << " " << k.rgb[1] << " " << k.rgb[2] << " channel " << c;
        }
    }

    float xyz[3] = { 0.2f, 0.3f, 0.5f };
    convert(cm, ColorMath::eColorspaceModelXYZ, ColorMath::eColorspaceModelXyY, xyz);
    EXPECT_NEAR(0.2, xyz[0], 1e-6);
    EXPECT_NEAR(0.3, xyz[1], 1e-6);
    EXPECT_NEAR(0.3, xyz[2], 1e-6);
}

// Negative, above-1 and grey values stay finite in every conversion; hue is 0 at zero chroma and
// xyY gives the white's chromaticity where there is no luminance.
TEST_F(NativeColorspaceTest, EdgeCasesStayDefined)
{
    const ColorMath::ColorspaceColorimetry cm = ColorMath::colorspaceColorimetry(ColorMath::acesCgToXYZ());
    const int n = sizeof(kValues) / sizeof(kValues[0]);

    for (int from = 0; from < ColorMath::eColorspaceModelCount; ++from) {
        for (int to = 0; to < ColorMath::eColorspaceModelCount; ++to) {
            for (int a = 0; a < n; ++a) {
                for (int b = 0; b < n; b += 2) {
                    for (int c = 0; c < n; c += 3) {
                        float v[3] = { kValues[a], kValues[b], kValues[c] };
                        convert(cm, from, to, v);
                        for (int k = 0; k < 3; ++k) {
                            ASSERT_TRUE(std::isfinite(v[k])) << from << " to " << to << " of " << kValues[a] << " " << kValues[b] << " " << kValues[c];
                        }
                    }
                }
            }
        }
    }

    for (float grey : { 0.f, -0.5f, 0.25f, 1.f, 3.f }) {
        for (int m : { ColorMath::eColorspaceModelHSV, ColorMath::eColorspaceModelHSL, ColorMath::eColorspaceModelHSI }) {
            float v[3] = { grey, grey, grey };
            convert(cm, ColorMath::eColorspaceModelRGB, m, v);
            EXPECT_EQ(0.f, v[0]) << "model " << m << " grey " << grey;
            EXPECT_EQ(0.f, v[1]) << "model " << m << " grey " << grey;
        }
    }

    float black[3] = { 0.f, 0.f, 0.f };
    convert(cm, ColorMath::eColorspaceModelRGB, ColorMath::eColorspaceModelXyY, black);
    EXPECT_EQ(cm.whiteXy[0], black[0]);
    EXPECT_EQ(cm.whiteXy[1], black[1]);
    EXPECT_EQ(0.f, black[2]);
    float noLuminance[3] = { 0.5f, 0.f, -0.2f };
    convert(cm, ColorMath::eColorspaceModelXYZ, ColorMath::eColorspaceModelXyY, noLuminance);
    EXPECT_EQ(cm.whiteXy[0], noLuminance[0]);
    EXPECT_EQ(cm.whiteXy[1], noLuminance[1]);
    EXPECT_EQ(0.f, noLuminance[2]);

    // The hue wraps, so a hue of 1.25 or -0.75 is a hue of 0.25.
    for (int m : { ColorMath::eColorspaceModelHSV, ColorMath::eColorspaceModelHSL, ColorMath::eColorspaceModelHSI }) {
        float base[3] = { 0.25f, 0.6f, 0.4f };
        convert(cm, m, ColorMath::eColorspaceModelRGB, base);
        for (float h : { 1.25f, -0.75f }) {
            float v[3] = { h, 0.6f, 0.4f };
            convert(cm, m, ColorMath::eColorspaceModelRGB, v);
            for (int c = 0; c < 3; ++c) {
                EXPECT_NEAR(base[c], v[c], 1e-6) << "model " << m << " hue " << h;
            }
        }
    }
}

// The node converts the processed colour channels as the shared maths does, with the project's
// working space, and leaves alpha.
TEST_F(NativeColorspaceTest, RendersTheConversion)
{
    NodePtr source = makeSource();
    NodePtr node = makeNode(source);
    ASSERT_TRUE(bool(source) && bool(node));
    const RectI window = paritySourceWindow(source, kTime, 0);

    const int pairs[][2] = {
        { ColorMath::eColorspaceModelRGB, ColorMath::eColorspaceModelLab },
        { ColorMath::eColorspaceModelRGB, ColorMath::eColorspaceModelHSV },
        { ColorMath::eColorspaceModelHSL, ColorMath::eColorspaceModelRGB },
        { ColorMath::eColorspaceModelXyY, ColorMath::eColorspaceModelYCbCr2020 },
    };
    for (const char* space : { kAcesCgSpace, kLinearRec709Space }) {
        setWorkingSpace(space);
        const ColorMath::ColorspaceColorimetry cm = workingColorimetry(node);
        for (const int* pair : pairs) {
            ASSERT_TRUE(setKnobValues(node, kColorspaceParamIn, std::vector<double>(1, pair[0])));
            ASSERT_TRUE(setKnobValues(node, kColorspaceParamOut, std::vector<double>(1, pair[1])));
            RenderedPlane plane;
            ASSERT_TRUE(renderRgba(node, window, &plane));
            ASSERT_EQ(4u, plane.channels.size());
            for (int y = window.y1; y < window.y2; ++y) {
                for (int x = window.x1; x < window.x2; ++x) {
                    float v[3];
                    for (int c = 0; c < 3; ++c) {
                        v[c] = paritySourceColorValue(c, x, y, kTime);
                    }
                    convert(cm, pair[0], pair[1], v);
                    const float* got = &plane.pixels[((std::size_t)(y - window.y1) * window.width() + (x - window.x1)) * 4];
                    for (int c = 0; c < 3; ++c) {
                        ASSERT_TRUE(ParityTolerance::arithmetic().accepts(v[c], got[c])) << space << " " << pair[0] << " to " << pair[1] << " at " << x << "," << y << " c=" << c << ": want " << v[c] << ", got " << got[c];
                    }
                    ASSERT_EQ(paritySourceColorValue(3, x, y, kTime), got[3]) << x << "," << y;
                }
            }
        }
    }
}

// A pixel whose alpha is between 0 and 1 is converted from its RGB exactly as stored, with no
// division by alpha and no multiplication afterwards, and keeps its alpha. The node has no
// "(Un)premult by" knob.
TEST_F(NativeColorspaceTest, ConvertsPartiallyTransparentPixelsAsTheyAre)
{
    NodePtr source = makeSource();
    NodePtr node = makeNode(source);
    ASSERT_TRUE(bool(source) && bool(node));
    EXPECT_TRUE(node->getKnobByName(kUnPremultByKnobName) == NULL);
    const RectI window = paritySourceWindow(source, kTime, 0);
    const ColorMath::ColorspaceColorimetry cm = workingColorimetry(node);

    ASSERT_TRUE(setKnobValues(node, kColorspaceParamIn, std::vector<double>(1, ColorMath::eColorspaceModelRGB)));
    ASSERT_TRUE(setKnobValues(node, kColorspaceParamOut, std::vector<double>(1, ColorMath::eColorspaceModelLab)));
    RenderedPlane plane;
    ASSERT_TRUE(renderRgba(node, window, &plane));
    ASSERT_EQ(4u, plane.channels.size());
    int translucent = 0;
    for (int y = window.y1; y < window.y2; ++y) {
        for (int x = window.x1; x < window.x2; ++x) {
            const float alpha = paritySourceColorValue(3, x, y, kTime);
            if ((alpha <= 0.1f) || (alpha >= 0.9f)) {
                continue;
            }
            ++translucent;
            float v[3];
            for (int c = 0; c < 3; ++c) {
                v[c] = paritySourceColorValue(c, x, y, kTime);
            }
            convert(cm, ColorMath::eColorspaceModelRGB, ColorMath::eColorspaceModelLab, v);
            const float* got = &plane.pixels[((std::size_t)(y - window.y1) * window.width() + (x - window.x1)) * 4];
            for (int c = 0; c < 3; ++c) {
                ASSERT_TRUE(ParityTolerance::arithmetic().accepts(v[c], got[c])) << "at " << x << "," << y << " alpha " << alpha << " c=" << c << ": want " << v[c] << ", got " << got[c];
            }
            ASSERT_EQ(alpha, got[3]) << x << "," << y;
        }
    }
    EXPECT_GT(translucent, 0);
}

TEST_F(NativeColorspaceTest, IdentityWhenInIsOut)
{
    NodePtr source = makeSource();
    NodePtr node = makeNode(source);
    ASSERT_TRUE(bool(source) && bool(node));
    const RectI window = paritySourceWindow(source, kTime, 0);
    int inputNb = -1;

    EXPECT_TRUE(identityAt(node, window, &inputNb));
    EXPECT_EQ(0, inputNb);
    for (int m = 0; m < ColorMath::eColorspaceModelCount; ++m) {
        ASSERT_TRUE(setKnobValues(node, kColorspaceParamIn, std::vector<double>(1, m)));
        ASSERT_TRUE(setKnobValues(node, kColorspaceParamOut, std::vector<double>(1, m)));
        EXPECT_TRUE(identityAt(node, window, &inputNb)) << m;
        ASSERT_TRUE(setKnobValues(node, kColorspaceParamOut, std::vector<double>(1, (m + 1) % ColorMath::eColorspaceModelCount)));
        EXPECT_FALSE(identityAt(node, window, &inputNb)) << m;
    }
}

TEST_F(NativeColorspaceTest, BothSchedulerModesAgreeWithNoUnplannedPull)
{
    NodePtr source = makeSource();
    NodePtr mask = makeSource();
    NodePtr node = makeNode(source);
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
    ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, std::vector<double>(1, 0.75)));
    ASSERT_TRUE(setKnobValues(node, kColorspaceParamIn, std::vector<double>(1, ColorMath::eColorspaceModelRGB)));
    ASSERT_TRUE(setKnobValues(node, kColorspaceParamOut, std::vector<double>(1, ColorMath::eColorspaceModelLab)));

    std::vector<int> poolSizes;
    poolSizes.push_back(1);
    poolSizes.push_back(4);
    for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
        const RectI window = paritySourceWindow(source, kTime, mipmapLevel);
        std::vector<int> unplannedPulls;
        const RenderMismatch m = renderBothWaysDirect(node, kTime, ViewIdx(0), mipmapLevel, window, poolSizes, std::function<void()>(), 0.f, &unplannedPulls);
        EXPECT_FALSE(m.any) << "mipmap " << mipmapLevel << ": " << describe(m);
        ASSERT_EQ(poolSizes.size(), unplannedPulls.size());
        for (std::size_t i = 0; i < unplannedPulls.size(); ++i) {
            EXPECT_EQ(0, unplannedPulls[i]) << "mipmap " << mipmapLevel << ", pool " << poolSizes[i];
        }
    }
}

TEST_F(NativeColorspaceTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_COLORSPACE, PLUGIN_MAJOR_NATRON_COLORSPACE, "Colorspace");
}
