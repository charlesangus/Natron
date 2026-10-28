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
#include <cstddef>
#include <functional>
#include <list>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <QString>

#include "BaseTest.h"
#include "NativeGolden.h"
#include "NativeParity.h"
#include "NativePluginIdentity.h"
#include "RenderBothWays.h"

#include "Engine/AppInstance.h"
#include "Engine/EffectInstance.h"
#include "Engine/Format.h"
#include "Engine/ImageLayerDesc.h"
#include "Engine/KnobTypes.h"
#include "Engine/Node.h"
#include "Engine/Nodes/Filter/DirBlur.h"
#include "Engine/Nodes/Generator/Constant.h"
#include "Engine/Nodes/Image/ExtentKnobs.h"
#include "Engine/Nodes/NativeEffectBase.h"
#include "Engine/Project.h"
#include "Engine/RectD.h"
#include "Engine/RectI.h"
#include "Engine/RenderScale.h"
#include "Engine/ViewIdx.h"

NATRON_NAMESPACE_USING

namespace {

const double kTime = 1.;
const int kWidth = 64;
const int kHeight = 48;
const double kCenterX = 32.;
const double kCenterY = 24.;

bool
isNative(const NodePtr& node)
{
    return node && dynamic_cast<NativeEffectBase*>(node->getEffectInstance().get());
}

RectI
pixelRoD(const NodePtr& node)
{
    EffectInstancePtr effect = node->getEffectInstance();
    RectD rod;
    const StatusEnum stat = effect->getRegionOfDefinition(effect->getRenderHash(), kTime, RenderScale::identity, ViewIdx(0), &rod);
    EXPECT_NE(eStatusFailed, stat);

    return rod.toPixelEnclosing(0, 1.);
}

bool
renderRGBA(const NodePtr& node,
           const RectI& window,
           RenderedPlane* plane)
{
    std::vector<RenderedPlane> planes;
    std::string error;
    const bool ok = renderNodePlanesDirect(node, kTime, ViewIdx(0), 0, window, std::list<ImageLayerDesc>(1, ImageLayerDesc::getRGBAComponents()), &planes, &error);
    EXPECT_TRUE(ok) << error;
    if (!ok || (planes.size() != 1) || (planes[0].channels.size() != 4)) {
        return false;
    }
    *plane = planes[0];

    return true;
}

// The float of channel c of pixel (x, y) of an RGBA plane.
float
at(const RenderedPlane& plane,
   int x,
   int y,
   int c)
{
    const RectI& w = plane.window;

    return plane.pixels[((std::size_t)(y - w.y1) * w.width() + (std::size_t)(x - w.x1)) * 4 + c];
}

// The sum of channel c over the pixels of plane that pred accepts.
double
sumWhere(const RenderedPlane& plane,
         int c,
         const std::function<bool(int, int)>& pred)
{
    double sum = 0.;
    const RectI& w = plane.window;
    for (int y = w.y1; y < w.y2; ++y) {
        for (int x = w.x1; x < w.x2; ++x) {
            if (pred(x, y)) {
                sum += at(plane, x, y, c);
            }
        }
    }

    return sum;
}

double
sumAll(const RenderedPlane& plane,
       int c)
{
    return sumWhere(plane, c, [](int, int) {
        return true;
    });
}

GoldenVariant
goldenVariant(const std::string& name,
              bool mask,
              const std::function<bool(const GoldenGraph&)>& apply)
{
    GoldenVariant v;

    v.name = name;
    v.mask = mask;
    v.apply = apply;

    return v;
}

} // namespace

class NativeDirBlurTest
    : public BaseTest {
protected:
    void resetProject()
    {
        ProjectPtr project = getApp()->getProject();

        project->reset(false, true);
        project->setAutoSetProjectFormatEnabled(true);
        project->setOrAddProjectFormat(Format(0, 0, kWidth, kHeight, "nativeDirBlur64x48", 1.));
    }

    // An opaque Constant of value v on RGB, over the project format or over box only.
    NodePtr makeConstant(double v,
                         const RectI* box)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_CONSTANT), PLUGIN_MAJOR_NATRON_CONSTANT);
        EXPECT_TRUE(bool(node));
        if (!node) {
            return node;
        }
        if (box) {
            EXPECT_TRUE(setKnobValue(node, kNativeGeneratorParamExtent, std::string(kNativeGeneratorExtentSize)));
            EXPECT_TRUE(setKnobValues(node, kNativeGeneratorParamBottomLeft, { (double)box->x1, (double)box->y1 }));
            EXPECT_TRUE(setKnobValues(node, kNativeGeneratorParamSize, { (double)box->width(), (double)box->height() }));
        } else {
            EXPECT_TRUE(setKnobValue(node, kNativeGeneratorParamExtent, std::string(kNativeGeneratorExtentProject)));
        }
        EXPECT_TRUE(setKnobValues(node, kConstantParamColor, { v, v, v, 1. }));

        return node;
    }

    // A DirBlur of source in mode, centred on the format's centre.
    NodePtr makeDirBlur(const NodePtr& source,
                        const char* mode)
    {
        NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_DIRBLUR), PLUGIN_MAJOR_NATRON_DIRBLUR);
        EXPECT_TRUE(isNative(node));
        if (!node) {
            return node;
        }
        if (source) {
            connectNodes(source, node, 0, true);
        }
        EXPECT_TRUE(setKnobValue(node, kDirBlurParamMode, std::string(mode)));
        EXPECT_TRUE(setKnobValues(node, kDirBlurParamCenter, { kCenterX, kCenterY }));

        return node;
    }

    // The source's and the node's renders over the node's region of definition.
    void renderPair(const NodePtr& source,
                    const NodePtr& node,
                    RenderedPlane* in,
                    RenderedPlane* out)
    {
        const RectI rod = pixelRoD(node);
        ASSERT_FALSE(rod.isNull());
        ASSERT_TRUE(renderRGBA(source, rod, in));
        ASSERT_TRUE(renderRGBA(node, rod, out));
        ASSERT_EQ(in->pixels.size(), out->pixels.size());
    }
};

TEST_F(NativeDirBlurTest, RegisteredIdentity)
{
    expectNativeIdentity(PLUGINID_NATRON_DIRBLUR, PLUGIN_MAJOR_NATRON_DIRBLUR, "DirBlur");
}

TEST_F(NativeDirBlurTest, InputsAndKnobs)
{
    resetProject();
    NodePtr node = createNode(QString::fromUtf8(PLUGINID_NATRON_DIRBLUR), PLUGIN_MAJOR_NATRON_DIRBLUR);
    ASSERT_TRUE(isNative(node));
    EXPECT_EQ(std::string("Source"), node->getInputLabel(0));
    EXPECT_EQ(std::string("Mask"), node->getInputLabel(1));
    EXPECT_TRUE(node->getEffectInstance()->isInputMask(1));
    EXPECT_TRUE(bool(node->getKnobByName("enableMask_Mask")));
    EXPECT_TRUE(bool(node->getKnobByName(kOfxMixParamName)));
    // A filter: no (un)premult-by knob.
    EXPECT_FALSE(bool(node->getKnobByName("hostUnPremultBy")));

    const struct {
        const char* name;
        const char* label;
    } labels[] = {
        { kDirBlurParamMode, "Mode" },
        { kDirBlurParamLength, "Length" },
        { kDirBlurParamCenter, "Center" },
        { kDirBlurParamDirection, "Direction" },
        { kDirBlurParamSamples, "Samples" },
        { kDirBlurParamCentered, "Centered" },
        { kDirBlurParamFalloff, "Falloff" },
        { kDirBlurParamFromColor, "From Color" },
        { kDirBlurParamToColor, "To Color" },
    };
    for (const auto& l : labels) {
        KnobIPtr knob = node->getKnobByName(l.name);
        ASSERT_TRUE(bool(knob)) << l.name;
        EXPECT_EQ(std::string(l.label), knob->getLabel()) << l.name;
    }

    KnobChoicePtr mode = std::dynamic_pointer_cast<KnobChoice>(node->getKnobByName(kDirBlurParamMode));
    ASSERT_TRUE(bool(mode));
    EXPECT_EQ((int)eDirBlurModeZoom, mode->getValue());
    KnobDoublePtr length = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kDirBlurParamLength));
    ASSERT_TRUE(bool(length));
    EXPECT_EQ(20., length->getValue());
    KnobIntPtr samples = std::dynamic_pointer_cast<KnobInt>(node->getKnobByName(kDirBlurParamSamples));
    ASSERT_TRUE(bool(samples));
    EXPECT_EQ(32, samples->getValue());
    // The centre defaults to the middle of the format.
    KnobDoublePtr center = std::dynamic_pointer_cast<KnobDouble>(node->getKnobByName(kDirBlurParamCenter));
    ASSERT_TRUE(bool(center));
    EXPECT_DOUBLE_EQ(kCenterX, center->getValue(0));
    EXPECT_DOUBLE_EQ(kCenterY, center->getValue(1));
    KnobColorPtr from = std::dynamic_pointer_cast<KnobColor>(node->getKnobByName(kDirBlurParamFromColor));
    ASSERT_TRUE(bool(from));
    EXPECT_EQ(4, from->getDimension());
}

// Sample positions are midpoints of the blur, spread both ways when centred, and the factors of
// white tints sum to one whatever the falloff.
TEST_F(NativeDirBlurTest, SamplesAndWeights)
{
    DirBlurParams params;
    params.mode = eDirBlurModeDirectional;
    params.samples = 4;
    params.centered = false;
    std::vector<double> t = DirBlur::samplePositions(params);
    ASSERT_EQ(4u, t.size());
    EXPECT_DOUBLE_EQ(0.125, t[0]);
    EXPECT_DOUBLE_EQ(0.875, t[3]);
    params.centered = true;
    t = DirBlur::samplePositions(params);
    EXPECT_DOUBLE_EQ(-0.75, t[0]);
    EXPECT_DOUBLE_EQ(0.75, t[3]);

    for (double falloff : { 0., 1., 3. }) {
        params.falloff = falloff;
        const std::vector<DirBlurSample> samples = DirBlur::canonicalSamples(params, 40.);
        double sum[4] = { 0., 0., 0., 0. };
        for (const DirBlurSample& s : samples) {
            for (int c = 0; c < 4; ++c) {
                sum[c] += s.factor[c];
            }
        }
        for (int c = 0; c < 4; ++c) {
            EXPECT_NEAR(1., sum[c], 1e-6) << "falloff " << falloff << " channel " << c;
        }
    }
}

// Length 0 returns the source exactly, in every mode, whatever the tint.
TEST_F(NativeDirBlurTest, LengthZeroIsIdentity)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 0, 0);
    for (const char* mode : { kDirBlurParamModeDirectional, kDirBlurParamModeRadial, kDirBlurParamModeZoom }) {
        NodePtr node = makeDirBlur(source, mode);
        ASSERT_TRUE(bool(node));
        ASSERT_TRUE(setKnobValues(node, kDirBlurParamLength, { 0. }));
        ASSERT_TRUE(setKnobValues(node, kDirBlurParamFromColor, { 0.2, 0.5, 2., 1. }));
        ASSERT_EQ(pixelRoD(source), pixelRoD(node)) << mode;
        RenderedPlane in, out;
        ASSERT_NO_FATAL_FAILURE(renderPair(source, node, &in, &out));
        for (std::size_t i = 0; i < in.pixels.size(); ++i) {
            ASSERT_EQ(in.pixels[i], out.pixels[i]) << mode << " float " << i;
        }
    }
}

// A point smears along Direction only, and its energy is kept exactly: every sample is a
// translation, whose bilinear weights sum to one.
TEST_F(NativeDirBlurTest, DirectionalSmearsAlongDirection)
{
    resetProject();
    const RectI point(20, 20, 21, 21);
    NodePtr source = makeConstant(1., &point);
    ASSERT_TRUE(bool(source));

    NodePtr right = makeDirBlur(source, kDirBlurParamModeDirectional);
    ASSERT_TRUE(bool(right));
    ASSERT_TRUE(setKnobValues(right, kDirBlurParamDirection, { 0. }));
    ASSERT_TRUE(setKnobValues(right, kDirBlurParamLength, { 8. }));
    ASSERT_TRUE(setKnobValues(right, kDirBlurParamSamples, { 8. }));
    const RectI rod = pixelRoD(right);
    EXPECT_LE(rod.x1, 20);
    EXPECT_GE(rod.x2, 29);
    RenderedPlane in, out;
    ASSERT_NO_FATAL_FAILURE(renderPair(source, right, &in, &out));
    for (int c = 0; c < 4; ++c) {
        EXPECT_NEAR(sumAll(in, c), sumAll(out, c), 1e-5) << "channel " << c;
        EXPECT_NEAR(0., sumWhere(out, c, [](int x, int y) {
                        return (y != 20) || (x < 20);
                    }),
                    1e-6)
            << "channel " << c;
    }
    for (int x = 21; x < 28; ++x) {
        EXPECT_GT(at(out, x, 20, 0), 0.f) << "x " << x;
    }

    // 90 degrees smears upwards instead.
    NodePtr up = makeDirBlur(source, kDirBlurParamModeDirectional);
    ASSERT_TRUE(bool(up));
    ASSERT_TRUE(setKnobValues(up, kDirBlurParamDirection, { 90. }));
    ASSERT_TRUE(setKnobValues(up, kDirBlurParamLength, { 8. }));
    ASSERT_TRUE(setKnobValues(up, kDirBlurParamSamples, { 8. }));
    ASSERT_NO_FATAL_FAILURE(renderPair(source, up, &in, &out));
    EXPECT_NEAR(sumAll(in, 0), sumAll(out, 0), 1e-5);
    EXPECT_NEAR(0., sumWhere(out, 0, [](int x, int y) {
                    return (x != 20) || (y < 20);
                }),
                1e-6);
    for (int y = 21; y < 28; ++y) {
        EXPECT_GT(at(out, 20, y, 0), 0.f) << "y " << y;
    }

    // Centred, it smears both ways.
    ASSERT_TRUE(setKnobValues(right, kDirBlurParamCentered, { 1. }));
    ASSERT_NO_FATAL_FAILURE(renderPair(source, right, &in, &out));
    EXPECT_NEAR(sumAll(in, 0), sumAll(out, 0), 1e-5);
    EXPECT_GT(at(out, 14, 20, 0), 0.f);
    EXPECT_GT(at(out, 26, 20, 0), 0.f);
}

// A point smears along the circle around Center, counter-clockwise for a positive length, and
// keeps its energy up to the bilinear weights of the rotated grid.
TEST_F(NativeDirBlurTest, RadialSmearsAroundCenter)
{
    resetProject();
    const RectI square(41, 22, 45, 26);
    NodePtr source = makeConstant(1., &square);
    ASSERT_TRUE(bool(source));
    NodePtr node = makeDirBlur(source, kDirBlurParamModeRadial);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kDirBlurParamLength, { 40. }));
    ASSERT_TRUE(setKnobValues(node, kDirBlurParamSamples, { 64. }));
    RenderedPlane in, out;
    ASSERT_NO_FATAL_FAILURE(renderPair(source, node, &in, &out));

    const double total = sumAll(out, 0);
    EXPECT_NEAR(sumAll(in, 0), total, 0.03 * sumAll(in, 0));
    // Everything stays within the square's radii of the centre.
    const double rMin = std::hypot(41. - kCenterX, 0.) - 1.5;
    const double rMax = std::hypot(45. - kCenterX, 26. - kCenterY) + 1.5;
    EXPECT_NEAR(0., sumWhere(out, 0, [&](int x, int y) {
                    const double r = std::hypot(x + 0.5 - kCenterX, y + 0.5 - kCenterY);

                    return (r < rMin) || (r > rMax);
                }),
                1e-6);
    // Counter-clockwise: the energy moves up from the square, nothing goes below it.
    EXPECT_NEAR(0., sumWhere(out, 0, [](int, int y) {
                    return y < 21;
                }),
                1e-6);
    EXPECT_GT(sumWhere(out, 0, [](int, int y) {
                  return y >= 26;
              }),
              0.5 * total);
}

// A point smears along the line through Center: towards it for a positive length, away from it
// for a negative one.
TEST_F(NativeDirBlurTest, ZoomSmearsThroughCenter)
{
    resetProject();
    const RectI point(52, 24, 53, 25);
    NodePtr source = makeConstant(1., &point);
    ASSERT_TRUE(bool(source));
    NodePtr node = makeDirBlur(source, kDirBlurParamModeZoom);
    ASSERT_TRUE(bool(node));
    ASSERT_TRUE(setKnobValues(node, kDirBlurParamLength, { 10. }));
    RenderedPlane in, out;
    ASSERT_NO_FATAL_FAILURE(renderPair(source, node, &in, &out));
    EXPECT_GT(sumAll(out, 0), 0.);
    EXPECT_NEAR(0., sumWhere(out, 0, [](int x, int y) {
                    return (x >= 53) || (y < 23) || (y > 25);
                }),
                1e-6);
    EXPECT_GT(sumWhere(out, 0, [](int x, int) {
                  return x < 51;
              }),
              0.);

    ASSERT_TRUE(setKnobValues(node, kDirBlurParamLength, { -10. }));
    ASSERT_NO_FATAL_FAILURE(renderPair(source, node, &in, &out));
    EXPECT_GT(sumAll(out, 0), 0.);
    EXPECT_NEAR(0., sumWhere(out, 0, [](int x, int y) {
                    return (x < 52) || (y < 23) || (y > 25);
                }),
                1e-6);
    EXPECT_GT(sumWhere(out, 0, [](int x, int) {
                  return x > 54;
              }),
              0.);
}

// The weights are normalised: a flat field keeps its value inside, in every mode and with
// falloff; a tint ramp scales it by the mean tint.
TEST_F(NativeDirBlurTest, FlatFieldIsKept)
{
    resetProject();
    NodePtr source = makeConstant(0.5, NULL);
    ASSERT_TRUE(bool(source));
    const RectI inner(24, 18, 40, 30);
    for (const char* mode : { kDirBlurParamModeDirectional, kDirBlurParamModeRadial, kDirBlurParamModeZoom }) {
        for (double falloff : { 0., 2. }) {
            NodePtr node = makeDirBlur(source, mode);
            ASSERT_TRUE(bool(node));
            ASSERT_TRUE(setKnobValues(node, kDirBlurParamLength, { 6. }));
            ASSERT_TRUE(setKnobValues(node, kDirBlurParamCentered, { 1. }));
            ASSERT_TRUE(setKnobValues(node, kDirBlurParamFalloff, { falloff }));
            RenderedPlane out;
            ASSERT_TRUE(renderRGBA(node, inner, &out));
            for (int y = inner.y1; y < inner.y2; ++y) {
                for (int x = inner.x1; x < inner.x2; ++x) {
                    ASSERT_NEAR(0.5, at(out, x, y, 0), 1e-5) << mode << " falloff " << falloff << " at (" << x << "," << y << ")";
                    ASSERT_NEAR(1., at(out, x, y, 3), 1e-5) << mode << " falloff " << falloff << " alpha at (" << x << "," << y << ")";
                }
            }
        }
    }

    // Red fading to blue: the uniform midpoint weights average the ramp to half of each.
    NodePtr tinted = makeDirBlur(source, kDirBlurParamModeDirectional);
    ASSERT_TRUE(bool(tinted));
    ASSERT_TRUE(setKnobValues(tinted, kDirBlurParamLength, { 6. }));
    ASSERT_TRUE(setKnobValues(tinted, kDirBlurParamFromColor, { 1., 0., 0., 1. }));
    ASSERT_TRUE(setKnobValues(tinted, kDirBlurParamToColor, { 0., 0., 1., 1. }));
    RenderedPlane out;
    ASSERT_TRUE(renderRGBA(tinted, inner, &out));
    EXPECT_NEAR(0.25, at(out, 32, 24, 0), 1e-5);
    EXPECT_NEAR(0., at(out, 32, 24, 1), 1e-6);
    EXPECT_NEAR(0.25, at(out, 32, 24, 2), 1e-5);
    EXPECT_NEAR(1., at(out, 32, 24, 3), 1e-5);
}

// A tile of the render equals the same pixels of the full render, bit for bit, in every mode.
TEST_F(NativeDirBlurTest, TileMatchesFullFrame)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 0, 0);
    for (const char* mode : { kDirBlurParamModeDirectional, kDirBlurParamModeRadial, kDirBlurParamModeZoom }) {
        NodePtr node = makeDirBlur(source, mode);
        ASSERT_TRUE(bool(node));
        ASSERT_TRUE(setKnobValues(node, kDirBlurParamLength, { 12. }));
        ASSERT_TRUE(setKnobValues(node, kDirBlurParamSamples, { 9. }));
        ASSERT_TRUE(setKnobValues(node, kDirBlurParamCentered, { 1. }));
        ASSERT_TRUE(setKnobValues(node, kDirBlurParamFalloff, { 1. }));
        ASSERT_TRUE(setKnobValues(node, kDirBlurParamToColor, { 0.5, 0.8, 1., 1. }));
        const RectI rod = pixelRoD(node);
        RenderedPlane whole;
        ASSERT_TRUE(renderRGBA(node, rod, &whole));
        const RectI tiles[] = { RectI(30, 6, 52, 30), RectI(rod.x1, rod.y1, rod.x1 + 9, rod.y1 + 7), RectI(rod.x2 - 5, rod.y2 - 11, rod.x2, rod.y2) };
        for (const RectI& tile : tiles) {
            RenderedPlane part;
            ASSERT_TRUE(renderRGBA(node, tile, &part));
            for (int y = tile.y1; y < tile.y2; ++y) {
                for (int x = tile.x1; x < tile.x2; ++x) {
                    for (int c = 0; c < 4; ++c) {
                        ASSERT_EQ(at(whole, x, y, c), at(part, x, y, c)) << mode << " at (" << x << "," << y << ") channel " << c;
                    }
                }
            }
        }
    }
}

// Legacy and task-graph renders agree bit for bit, in each mode, and with a mask and mix.
TEST_F(NativeDirBlurTest, SameBothWays)
{
    resetProject();
    NodePtr source = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(source));
    setParitySourceOrigin(source, 0, 0);
    NodePtr node = makeDirBlur(source, kDirBlurParamModeZoom);
    ASSERT_TRUE(bool(node));

    const std::function<void(const std::string&)> expectSame = [&](const std::string& caseName) {
        std::vector<int> poolSizes;
        poolSizes.push_back(1);
        poolSizes.push_back(4);
        for (unsigned mipmapLevel = 0; mipmapLevel <= 1; ++mipmapLevel) {
            EffectInstancePtr effect = node->getEffectInstance();
            RectD rod;
            ASSERT_NE(eStatusFailed, effect->getRegionOfDefinition(effect->getRenderHash(), kTime, RenderScale::fromMipmapLevel(mipmapLevel), ViewIdx(0), &rod));
            const RectI window = rod.toPixelEnclosing(mipmapLevel, 1.);
            const RenderMismatch m = renderBothWaysDirect(node, kTime, ViewIdx(0), mipmapLevel, window, poolSizes);
            EXPECT_FALSE(m.any) << caseName << ", mipmap " << mipmapLevel << ": " << describe(m);
        }
    };
    expectSame("zoom");
    ASSERT_TRUE(setKnobValue(node, kDirBlurParamMode, std::string(kDirBlurParamModeDirectional)));
    ASSERT_TRUE(setKnobValues(node, kDirBlurParamCentered, { 1. }));
    expectSame("directional-centered");
    ASSERT_TRUE(setKnobValue(node, kDirBlurParamMode, std::string(kDirBlurParamModeRadial)));
    ASSERT_TRUE(setKnobValues(node, kDirBlurParamFalloff, { 2. }));
    expectSame("radial-falloff");

    NodePtr mask = createNodeAtMajor(getApp(), kTestPluginIDParitySource, -1);
    ASSERT_TRUE(bool(mask));
    setParitySourceOrigin(mask, 8, 4);
    connectNodes(mask, node, 1, true);
    ASSERT_TRUE(setKnobValues(node, "enableMask_Mask", { 1. }));
    ASSERT_TRUE(setKnobValues(node, kOfxMixParamName, { 0.5 }));
    expectSame("radial-mask-mix");
}

// Regression fixtures: each mode, centred with falloff, a tint ramp, and through a mask and mix.
TEST_F(NativeDirBlurTest, Golden)
{
    GoldenNodeSpec spec;
    spec.name = "DirBlur";
    spec.pluginID = PLUGINID_NATRON_DIRBLUR;
    spec.major = PLUGIN_MAJOR_NATRON_DIRBLUR;
    spec.maskInput = "Mask";

    std::vector<GoldenVariant> v;
    v.push_back(goldenVariant("default", false, [](const GoldenGraph&) {
        return true;
    }));
    v.push_back(goldenVariant("directional", false, [](const GoldenGraph& g) {
        return setKnobValue(g.node, kDirBlurParamMode, std::string(kDirBlurParamModeDirectional)) && setKnobValues(g.node, kDirBlurParamLength, { 15. }) && setKnobValues(g.node, kDirBlurParamDirection, { 30. });
    }));
    v.push_back(goldenVariant("radialCenteredFalloff", false, [](const GoldenGraph& g) {
        return setKnobValue(g.node, kDirBlurParamMode, std::string(kDirBlurParamModeRadial)) && setKnobValues(g.node, kDirBlurParamLength, { 25. }) && setKnobValues(g.node, kDirBlurParamCentered, { 1. }) && setKnobValues(g.node, kDirBlurParamFalloff, { 2. });
    }));
    v.push_back(goldenVariant("zoomTint", false, [](const GoldenGraph& g) {
        return setKnobValues(g.node, kDirBlurParamLength, { 30. }) && setKnobValues(g.node, kDirBlurParamFromColor, { 1., 0.9, 0.6, 1. }) && setKnobValues(g.node, kDirBlurParamToColor, { 0.2, 0.3, 1., 0.5 });
    }));
    v.push_back(goldenVariant("maskmix", true, [](const GoldenGraph& g) {
        return setKnobValues(g.node, kOfxMixParamName, { 0.5 }) && setKnobValues(g.node, kOfxMaskInvertParamName, { 1. });
    }));
    runGoldenCases(getApp(), spec, v);
}
